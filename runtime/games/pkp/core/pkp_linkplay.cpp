// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, Link Play "Interact everywhere" (pkp_linkplay.h). Based on pokopia-native (MIT) windows/src/coop/coop_interact.c and
// windows/docs/coop-interact.md @555813a.
//
// The game keeps a session's permissions in a 36-bit mask (GameMgr+0x8c; bits 32-35, the byte at +0x90, are online / host / visitor) and
// rebuilds it in one function, 0xb0f60 of the 2.0.1 main, when hosting starts, when a town loads and when the session ends. Offline every
// bit but 31 is set. Online in a story area (stage type 1) the player always gets the Spectator pattern (bits 23-26 and 31: "certain moves
// and items can't be used"), whatever the host chose; online in Palette Town with Spectator Mode Off the normal one (bits 1-28 and 30). At
// 0xb1030, `movk x13, #1, lsl #32` in the story-area online block, x13 holds the pattern's bits 0-31 (0x87800000, set at 0xb1020) and the
// movk adds bit 32; x22 is the session's Spectator flag (w3 of the call, clear in every story area). The midhook there (recomp/games/pkp/
// main.sites) swaps the pattern for the normal one while the flag is clear: the mask is then exactly Palette Town's with Spectator Mode Off
// (pokopia-native measured fe ff ff 5f 03 for a host, fe ff ff 5f 05 for a visitor). Palette Town, the other type-3 towns, offline play, the
// host's Spectator setting, the session flag and the advertise data are not touched. Known gaps (pokopia-native): the visitor's join notice
// and the host's settings row still say Spectator Mode; a visitor's bag goes home with it as in Palette Town.
#include "core/pkp_linkplay.h"
#include "core/pkp_settings.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/mods.h"
#include "nxrt/services_api.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";

// Pokopia 2.0.1 main (build 76a489f3...): the mask builder and its story-area online block.
constexpr InteractSite kSites[] = {
    {"76a489f3bd3642fa5e18bd83d32c98ba", 0xb0f60, 0xb1030, 0xf2c0002du},
};
constexpr uint64_t kSpectatorFlagReg = 22, kPatternReg = 13, kHostArgReg = 20;
constexpr uint64_t kNormalPattern = 0x5ffffffeull;   // bits 1-28 and 30: Palette Town, Spectator Mode Off
constexpr uint64_t kStoryBit = 1;                    // bit 0: the story system (the per-frame story update 0xba660, quests, events)

struct State {
    uint64_t base = 0;
    HookSet* hooks = nullptr;              // the registry's set for Link Play (pkp_feature.h)
    uint32_t hook = 0;                     // 0: not installed
    uint32_t story_hooks[kStorySiteCount] = {};   // all 0: Story co-op off or not installed
    std::atomic<bool> story_armed{false};
    std::atomic<bool> story_host{false};   // this player hosts the session in a story area (set by the mask builder)
    std::atomic<bool> logged_story{false};
    std::atomic<bool> logged_story_host{false};
    std::atomic<bool> armed{false};        // main checked: the hook may change the mask
    std::atomic<bool> checked{false};      // the frame callback made its decision
    std::atomic<bool> logged_change{false};
    std::atomic<uint64_t> changes{0};
};
// The co-op mods fingerprint's names (services ldnapi::set_coop_mods): our own features first (Story co-op while armed), then the Modtopia
// gameplay mods that are on, "id@version" sorted (nxrt/mods.h; Modtopia HOST.md section 3, Link Play). Without mods: our list alone.
std::vector<std::string> coop_mods(bool story) {
    std::vector<std::string> names;
    if (story) names.emplace_back(kStoryName);
    const std::vector<std::string> mods = nxrt::mods::gameplay_mods();
    names.insert(names.end(), mods.begin(), mods.end());
    return names;
}

State& st() {
    static State s;
    return s;
}

int hook_mask(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (!s.armed.load(std::memory_order_acquire)) return 0;
    bool keep_story = false;
    if (s.story_armed.load(std::memory_order_acquire) && !(c->x[kSpectatorFlagReg] & 1)) {
        keep_story = c->x[kHostArgReg] & 1;   // the builder's host argument
        s.story_host.store(keep_story, std::memory_order_release);
        if (keep_story && !s.logged_story_host.exchange(true)) NXLOG_I(kTag, "Story co-op: the host's story keeps going in this session");
    }
    if (interact_apply(c->x, keep_story)) {
        s.changes.fetch_add(1, std::memory_order_relaxed);
        if (!s.logged_change.exchange(true))
            NXLOG_I(kTag, "Interact everywhere: this story area's Link Play session has Palette Town's permissions (Spectator Mode lifted)");
    }
    return 0;
}

int hook_talk(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (!s.story_armed.load(std::memory_order_acquire)) return 0;
    if (!(c->x[0] & 1) && !s.logged_story.exchange(true)) NXLOG_I(kTag, "Story co-op: a visitor's talk takes the host's path");
    c->x[0] = 1;   // IsHost: yes
    return 0;
}

// The mask builder's first instruction (0xb0f74, `mov x19, x0`): a rebuild forgets the role; the story-area site sets it again.
int hook_role(::Ctx*, uint64_t, void*) {
    st().story_host.store(false, std::memory_order_release);
    return 0;
}

// Talk conditions 0x19 and 0x60, right after their IsOnline call: the host's answer is "not online".
int hook_alone(::Ctx* c, uint64_t, void*) {
    if (st().story_host.load(std::memory_order_acquire)) c->x[0] = 0;
    return 0;
}

// Talk condition 0x22 (0x4d04fc, `b IsOnline`): for the host it returns "not online" itself (nothing is pushed yet, x0 is the result).
int hook_alone_tail(::Ctx* c, uint64_t, void*) {
    if (!st().story_host.load(std::memory_order_acquire)) return 0;
    c->x[0] = 0;
    return 1;
}

// Pokopia 2.0.1 main: Story co-op's sites (pokopia-native coop_interact.c g_story @9cb7f07).
struct StorySite {
    uint64_t site;
    uint32_t word;
    SiteHookFn fn;
    bool entry = false;   // an entry hook (the site is a function's first instruction), else a midhook
};
constexpr StorySite kStorySites[kStorySiteCount] = {
    {0x4c06d8, 0x2a000328u, &hook_talk},         // talk handler 0x4c024c: `orr w8, w25, w0`, w0 = IsHost's answer
    {0xb0f74, 0xaa0003f3u, &hook_role},          // the mask builder 0xb0f60 starts: `mov x19, x0`
    {0x4cfe20, 0x52800028u, &hook_alone},        // condition 0x19 (not online) 0x4cfe10: `mov w8, #1` after IsOnline
    {0x4d4860, 0x37001da0u, &hook_alone},        // condition 0x60 (not online and more) 0x4d4844: `tbnz w0, #0` after IsOnline
    {0x4d04fc, 0x17ed40adu, &hook_alone_tail, true},   // condition 0x22 (online): `b 0x207b0`, IsOnline
};

const char* check_str(InteractCheck c) {
    switch (c) {
        case InteractCheck::Ok: return "ok";
        case InteractCheck::UnknownBuild: return "main is not a supported build (2.0.1 only)";
        case InteractCheck::NotLifted: return "the lifted game libraries lack the hook site (rebuild them)";
        case InteractCheck::WordDiffers: return "the instruction at the site is not the expected one";
    }
    return "?";
}

}  // namespace

const InteractSite* interact_site(const std::string& build_id) {
    for (const InteractSite& s : kSites)
        if (build_id.compare(0, std::strlen(s.build), s.build) == 0) return &s;
    return nullptr;
}

InteractCheck interact_check(const std::string& build_id, bool site_lifted, uint32_t word, const InteractSite** site) {
    const InteractSite* s = interact_site(build_id);
    if (site) *site = s;
    if (!s) return InteractCheck::UnknownBuild;
    if (!site_lifted) return InteractCheck::NotLifted;
    if (word != s->word) return InteractCheck::WordDiffers;
    return InteractCheck::Ok;
}

bool interact_apply(uint64_t* x, bool keep_story) {
    if (x[kSpectatorFlagReg] & 1) return false;           // the session's own Spectator flag: left as the game has it
    const uint64_t want = keep_story ? kNormalPattern | kStoryBit : kNormalPattern;
    if (x[kPatternReg] == want) return false;
    x[kPatternReg] = want;                                // the movk that follows adds bit 32 (online)
    return true;
}

namespace {

// Registers the midhook at main+site when [pkp] link_interact_everywhere (or link_story_coop) is on (main is not loaded yet, so nothing
// is checked here) and logs the choice. The hook does nothing until linkplay_arm() accepted main.
void install(const FeatureInstall& in) {
    State& s = st();
    if (s.hook) return;
    bool on = settings::get_bool(settings::Key::LinkInteractEverywhere);
    const bool story = settings::get_bool(settings::Key::LinkStoryCoop);
    const uint64_t base = in.base;
    s.base = base;
    s.hooks = &in.hooks;
    s.armed.store(false);
    s.checked.store(false);
    s.logged_change.store(false);
    s.changes.store(0);
    s.story_armed.store(false);
    s.story_host.store(false);
    s.logged_story.store(false);
    s.logged_story_host.store(false);
    services::ldnapi::set_session_variant(0, kInteractName);
    services::ldnapi::set_coop_mods(coop_mods(false));
    bool want_story = story;
    if (want_story && !services::ldnapi::native_protocol()) {
        NXLOG_W(kTag, "Story co-op ([pkp] link_story_coop): ignored, it needs [net] protocol = native (the relay Link Play has no such setting)");
        want_story = false;
    }
    if (want_story && !on) {
        NXLOG_I(kTag, "Story co-op ([pkp] link_story_coop): turns Interact everywhere on with it, as pokopia-native's does");
        on = true;
    }
    if (!on) {
        NXLOG_I(kTag, "Interact everywhere ([pkp] link_interact_everywhere): off, Link Play outside Palette Town keeps the game's Spectator Mode");
        return;
    }
    s.hook = in.hooks.add_mid(base + kSites[0].site, &hook_mask);
    NXLOG_I(kTag, "Interact everywhere ([pkp] link_interact_everywhere): on, hook at main+0x%llx, main is checked once the game runs",
            static_cast<unsigned long long>(kSites[0].site));
    if (want_story) {
        for (size_t i = 0; i < kStorySiteCount; i++) {
            const StorySite& ss = kStorySites[i];
            s.story_hooks[i] = ss.entry ? in.hooks.add_entry(base + ss.site, ss.fn) : in.hooks.add_mid(base + ss.site, ss.fn);
        }
        NXLOG_I(kTag, "Story co-op ([pkp] link_story_coop): on, %zu hooks (talk at main+0x%llx), main is checked once the game runs",
                kStorySiteCount, static_cast<unsigned long long>(kStorySites[0].site));
    }
}

}  // namespace

InteractCheck story_arm(const std::string& build_id, const StorySiteFacts (&facts)[kStorySiteCount]) {
    State& s = st();
    if (!s.story_hooks[0]) return InteractCheck::Ok;
    const std::string shown = build_id.substr(0, 32);
    InteractCheck c = InteractCheck::Ok;
    size_t bad = 0;   // the first site that fails
    if (!s.armed.load() || build_id.compare(0, std::strlen(kStoryBuild), kStoryBuild) != 0) {
        c = InteractCheck::UnknownBuild;   // Story co-op builds on an armed Interact everywhere
    } else {
        for (; bad < kStorySiteCount; bad++) {
            if (!facts[bad].lifted) c = InteractCheck::NotLifted;
            else if (facts[bad].word != kStorySites[bad].word) c = InteractCheck::WordDiffers;
            if (c != InteractCheck::Ok) break;
        }
    }
    if (c == InteractCheck::Ok) {
        services::ldnapi::set_coop_mods(coop_mods(true));
        s.story_armed.store(true, std::memory_order_release);
        NXLOG_I(kTag, "Story co-op: armed, %zu sites (build %s); Link Play sessions are refused to players without it (mods fingerprint)",
                kStorySiteCount, shown.c_str());
        return c;
    }
    s.story_armed.store(false);
    s.story_host.store(false);
    for (uint32_t& h : s.story_hooks) {
        s.hooks->remove(h);
        h = 0;
    }
    if (c == InteractCheck::WordDiffers)
        NXLOG_W(kTag, "Story co-op: not applied: the word at main+0x%llx is %08x, expected %08x (build %s)",
                static_cast<unsigned long long>(kStorySites[bad].site), facts[bad].word, kStorySites[bad].word, shown.c_str());
    else if (c == InteractCheck::NotLifted)
        NXLOG_W(kTag, "Story co-op: not applied: %s, main+0x%llx (build %s)", check_str(c),
                static_cast<unsigned long long>(kStorySites[bad].site), shown.c_str());
    else
        NXLOG_W(kTag, "Story co-op: not applied: %s (build %s)", s.armed.load() ? check_str(c) : "Interact everywhere is not armed",
                shown.empty() ? "unknown" : shown.c_str());
    return c;
}

InteractCheck linkplay_arm(const std::string& build_id, bool site_lifted, uint32_t word) {
    State& s = st();
    if (!s.hook) return InteractCheck::Ok;
    s.checked.store(true);
    const InteractSite* site = nullptr;
    const InteractCheck c = interact_check(build_id, site_lifted, word, &site);
    const std::string shown = build_id.substr(0, 32);
    if (c == InteractCheck::Ok) {
        services::ldnapi::set_session_variant(kInteractVariant, kInteractName);
        s.armed.store(true, std::memory_order_release);
        NXLOG_I(kTag, "Interact everywhere: armed at main+0x%llx (build %s); Link Play sessions are refused to players without it (session variant %u)",
                static_cast<unsigned long long>(site->site), shown.c_str(), kInteractVariant);
        return c;
    }
    s.armed.store(false);
    s.hooks->remove(s.hook);
    s.hook = 0;
    services::ldnapi::set_session_variant(0, kInteractName);
    if (c == InteractCheck::WordDiffers)
        NXLOG_W(kTag, "Interact everywhere: not applied: the word at main+0x%llx is %08x, expected %08x (build %s): Link Play keeps the game's rules",
                static_cast<unsigned long long>(site->site), word, site->word, shown.c_str());
    else
        NXLOG_W(kTag, "Interact everywhere: not applied: %s (build %s): Link Play keeps the game's rules", check_str(c),
                shown.empty() ? "unknown" : shown.c_str());
    return c;
}

namespace {

// Once per frame (play mode's frame: main is loaded): the first call checks main (its build id, the lifted site, the instruction word)
// and arms the hook or removes it; logs once either way.
void frame(bool, double, play::GameFrame*) {
    State& s = st();
    if (!s.hook || s.checked.load(std::memory_order_relaxed)) return;
    const Module* m = module_at(s.base);
    const std::string build = m ? m->build_id() : std::string();
    const InteractSite* site = interact_site(build);
    bool lifted = false;
    uint32_t word = 0;
    if (site) {
        const uint64_t a = s.base + site->site;
        lifted = fn_lookup(a) != nullptr;   // a midhook makes its address a lifted function of its own
        if (mem::is_mapped(a, 4)) std::memcpy(&word, reinterpret_cast<const void*>(a), 4);
    }
    linkplay_arm(build, lifted, word);
    if (s.story_hooks[0]) {
        StorySiteFacts facts[kStorySiteCount];
        for (size_t i = 0; i < kStorySiteCount; i++) {
            const uint64_t a = s.base + kStorySites[i].site;
            facts[i].lifted = fn_lookup(a) != nullptr;
            if (mem::is_mapped(a, 4)) std::memcpy(&facts[i].word, reinterpret_cast<const void*>(a), 4);
        }
        story_arm(build, facts);
    }
}

void remove() {   // the registry has removed the hooks
    State& s = st();
    s.armed.store(false);
    s.story_armed.store(false);
    s.story_host.store(false);
    for (uint32_t& h : s.story_hooks) h = 0;
    services::ldnapi::set_coop_mods({});
    s.hook = 0;
    s.hooks = nullptr;
    s.checked.store(false);
    services::ldnapi::set_session_variant(0, kInteractName);
}

}  // namespace

const Feature kLinkplayFeature = {.name = "linkplay", .install = &install, .remove = &remove, .frame = &frame};

}  // namespace nxrt::games::pkp
