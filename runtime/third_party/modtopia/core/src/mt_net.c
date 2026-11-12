/* The network for mods (API 0.4, SPEC section 20). A mod reaches only the hosts its mod.toml names. The host's backend does the request
   off the game's thread and answers from any thread; the answers are handed to the mods at the start of a frame, on the game's thread. */
#include "mt_loader.h"

#include <stdlib.h>
#include <string.h>

#define NET_BODY_MAX ((size_t)64 << 10)
#define NET_ANSWER_MAX ((size_t)256 << 10)
#define NET_WAITING_MAX 4

typedef struct NetReq {
    struct NetReq *next;
    MtLoader *L;   /* NULL once its loader is gone: the answer is dropped */
    Mod *mod;
    MtNetFn fn;
    void *user;
    int done, status;
    char *body;
    size_t n;
} NetReq;

/* one list for every loader: an answer may come after its loader was freed */
static MtLock g_lock;
static int g_ready;
static NetReq *g_reqs;

void mt_net_init(void) {
    if (!g_ready) mt_lock_init(&g_lock), g_ready = 1;
}
static void unlink_req(NetReq *r) {
    for (NetReq **p = &g_reqs; *p; p = &(*p)->next)
        if (*p == r) { *p = r->next; return; }
}
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* a host as mod.toml's network list names it: letters, digits, dots and dashes only (no scheme, port or path) */
int mt_net_host_valid(const char *h) {
    size_t n = strlen(h);
    if (!n || n > 253 || h[0] == '.' || h[0] == '-') return 0;
    for (size_t i = 0; i < n; i++) {
        int c = lower((unsigned char)h[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return 0;
    }
    return 1;
}
/* 1 when url is https://<host>[:port][/...] with a host of the list; http:// only for localhost and 127.0.0.1 (a mod's own tests) */
int mt_net_url_allowed(const MtStrs *hosts, const char *url) {
    int plain = !strncmp(url, "http://", 7);
    if (!plain && strncmp(url, "https://", 8)) return 0;
    const char *h = url + (plain ? 7 : 8);
    size_t n = strcspn(h, ":/?#");
    if (!n || memchr(h, '@', n)) return 0;
    for (int i = 0; i < hosts->n; i++) {
        const char *a = hosts->v[i];
        if (strlen(a) != n) continue;
        size_t k = 0;
        while (k < n && lower((unsigned char)a[k]) == lower((unsigned char)h[k])) k++;
        if (k < n) continue;
        return !plain || !strcmp(a, "localhost") || !strcmp(a, "127.0.0.1");
    }
    return 0;
}

/* the backend's answer: from any thread */
static void net_done(void *token, int status, const char *body, size_t n) {
    NetReq *r = token;
    char *copy = NULL;
    if (!body) n = 0;
    if (n > NET_ANSWER_MAX) status = 0, n = 0;   /* too long an answer counts as none */
    if (n && (copy = malloc(n + 1))) memcpy(copy, body, n), copy[n] = 0;
    else n = 0;
    mt_lock(&g_lock);
    if (!r->L) {
        unlink_req(r);
        free(copy), free(r);
    } else r->status = status, r->body = copy, r->n = n, r->done = 1;
    mt_unlock(&g_lock);
}

int mt_net_request(MtMod *m, const char *method, const char *url, const char *body, MtNetFn done, void *user) {
    MtLoader *L = m->L;
    if (!L->be.net_request) return MT_ERR_VERSION;
    if (!method || !url || (strcmp(method, "GET") && strcmp(method, "POST"))) return MT_ERR;
    size_t n = body ? strlen(body) : 0;
    if (n > NET_BODY_MAX) return MT_ERR;
    if (!mt_net_url_allowed(&m->m.network, url)) {
        mt_log(L, MT_LOG_WARN, "%s asked for an address its mod.toml does not name (network = [...]): refused", m->m.id);
        return MT_ERR_STATE;
    }
    NetReq *r = calloc(1, sizeof *r);
    if (!r) return MT_ERR;
    r->L = L, r->mod = m, r->fn = done, r->user = user;
    int waiting = 0;
    mt_lock(&g_lock);
    for (NetReq *q = g_reqs; q; q = q->next) waiting += q->mod == m && q->L == L;
    if (waiting < NET_WAITING_MAX) r->next = g_reqs, g_reqs = r;
    mt_unlock(&g_lock);
    if (waiting >= NET_WAITING_MAX) { free(r); return MT_ERR_STATE; }
    int rc = L->be.net_request(L->be.user, method, url, body, n, net_done, r);
    if (rc != MT_OK) {   /* not started: the backend does not answer it */
        mt_lock(&g_lock);
        unlink_req(r);
        mt_unlock(&g_lock);
        free(r);
    }
    return rc;
}

/* the answers that have come, to their mods (the game's thread, before FrameStart) */
void mt_net_frame(MtLoader *L) {
    if (!g_reqs) return;
    NetReq *ready = NULL, **tail = &ready;
    mt_lock(&g_lock);
    for (NetReq **p = &g_reqs; *p;) {
        NetReq *r = *p;
        if (r->L == L && r->done) *p = r->next, r->next = NULL, *tail = r, tail = &r->next;
        else p = &r->next;
    }
    mt_unlock(&g_lock);
    while (ready) {
        NetReq *r = ready;
        ready = r->next;
        if (r->fn && r->mod->info.started) r->fn(r->mod, r->status, r->body ? r->body : "", r->n, r->user);
        free(r->body), free(r);
    }
}

/* the loader is going: its answers are dropped when they come */
void mt_net_forget(MtLoader *L) {
    if (!g_ready) return;
    mt_lock(&g_lock);
    for (NetReq **p = &g_reqs; *p;) {
        NetReq *r = *p;
        if (r->L != L) { p = &r->next; continue; }
        if (r->done) { *p = r->next; free(r->body), free(r); continue; }
        r->L = NULL, r->mod = NULL;
        p = &r->next;
    }
    mt_unlock(&g_lock);
}
