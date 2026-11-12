// SPDX-License-Identifier: GPL-3.0-or-later
// The virtual LAN: Link receiver plumbing, HoldLink and the process-wide current link (see vnet.h).
// Lock order: the current-link lock, then a link's receiver lock, then whatever the receiver takes (the socket table's lock).
#include "vnet.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <thread>

#include "nxrt/log.h"

namespace nxrt::vnet {

void Link::set_receiver(Receiver r) {
    std::lock_guard<std::mutex> lk(rx_mu_);
    rx_ = std::move(r);
}

void Link::deliver(const Datagram& d) {
    std::lock_guard<std::mutex> lk(rx_mu_);   // held across the call: set_receiver(nullptr) returning means no call is running
    if (rx_) rx_(d);
}

// ---- HoldLink -------------------------------------------------------------------------------------------------------------------------
// Lock order: the inner link's receiver lock (inbound), then st->mu, then this link's receiver lock (deliver), then the receiver's locks.
// Held datagrams are delivered with st->mu held, so a sender's released datagrams cannot be overtaken by its next one. detach() takes no
// lock of its own across the inner set_receiver (that waits for an inbound call in progress, which may be waiting for st->mu).

struct HoldLink::State {
    using Clock = std::chrono::steady_clock;
    int hold_ms = 0;
    size_t cap = 0;
    std::mutex mu;
    std::condition_variable cv;
    bool stop = false, detached = false;
    std::set<uint32_t> passed;                  // admitted senders
    std::deque<Datagram> held;                  // arrival order, all senders
    std::map<uint32_t, Clock::time_point> due;  // sender -> when its held datagrams go anyway
    std::thread timer;
};

HoldLink::HoldLink(std::shared_ptr<Link> inner, int hold_ms, size_t cap) : inner_(std::move(inner)), st_(std::make_unique<State>()) {
    st_->hold_ms = hold_ms;
    st_->cap = cap ? cap : 1;
    State* st = st_.get();
    st_->timer = std::thread([this, st] {
        log_set_thread_name("vnet-timer");
        std::unique_lock<std::mutex> lk(st->mu);
        while (!st->stop) {
            if (st->due.empty()) {
                st->cv.wait(lk);
                continue;
            }
            auto first = st->due.begin();
            for (auto it = st->due.begin(); it != st->due.end(); ++it)
                if (it->second < first->second) first = it;
            if (State::Clock::now() < first->second) {
                st->cv.wait_until(lk, first->second);
                continue;
            }
            const uint32_t src = first->first;
            NXLOG_D("net", "hold: %s not admitted within %d ms, its datagrams go anyway", ip_str(src).c_str(), st->hold_ms);
            release({src});
        }
    });
    inner_->set_receiver([this, st](const Datagram& d) {
        std::lock_guard<std::mutex> lk(st->mu);
        if (st->detached) return;
        if (st->passed.count(d.src_ip)) {
            deliver(d);
            return;
        }
        if (st->held.size() >= st->cap) {
            const uint32_t old = st->held.front().src_ip;
            NXLOG_D("net", "hold: %zu datagrams held, the oldest (from %s) dropped", st->held.size(), ip_str(old).c_str());
            st->held.pop_front();
            if (std::none_of(st->held.begin(), st->held.end(), [old](const Datagram& h) { return h.src_ip == old; })) st->due.erase(old);
        }
        st->held.push_back(d);
        if (st->due.emplace(d.src_ip, State::Clock::now() + std::chrono::milliseconds(st->hold_ms)).second) {
            NXLOG_D("net", "hold: datagrams from %s held until the game has read a node table listing it", ip_str(d.src_ip).c_str());
            st->cv.notify_all();
        }
    });
}

HoldLink::~HoldLink() { detach(); }

uint32_t HoldLink::local_ip() const { return inner_->local_ip(); }

bool HoldLink::send(const Datagram& d) { return inner_->send(d); }

void HoldLink::release(const std::vector<uint32_t>& srcs) {
    State* st = st_.get();
    for (const uint32_t v : srcs) {
        st->passed.insert(v);
        st->due.erase(v);
    }
    for (auto it = st->held.begin(); it != st->held.end();) {   // one pass: arrival order across the senders released together
        if (!st->passed.count(it->src_ip)) {
            ++it;
            continue;
        }
        deliver(*it);
        it = st->held.erase(it);
    }
}

void HoldLink::admit(const std::vector<uint32_t>& vips) {
    std::lock_guard<std::mutex> lk(st_->mu);
    if (st_->detached) return;
    std::vector<uint32_t> fresh;
    for (const uint32_t v : vips)
        if (v && !st_->passed.count(v)) fresh.push_back(v);
    if (!fresh.empty()) release(fresh);
}

void HoldLink::members(const std::vector<uint32_t>& vips) {
    std::lock_guard<std::mutex> lk(st_->mu);
    for (auto it = st_->passed.begin(); it != st_->passed.end();) {
        if (std::find(vips.begin(), vips.end(), *it) == vips.end()) it = st_->passed.erase(it);
        else ++it;
    }
}

void HoldLink::detach() {
    {
        std::lock_guard<std::mutex> lk(st_->mu);
        if (st_->detached) return;
        st_->detached = true;
        st_->stop = true;
        st_->held.clear();
        st_->due.clear();
        st_->cv.notify_all();
    }
    inner_->set_receiver(nullptr);   // waits for an inbound call in progress (it returns at once: detached)
    if (st_->timer.joinable()) st_->timer.join();
}

size_t HoldLink::held() const {
    std::lock_guard<std::mutex> lk(st_->mu);
    return st_->held.size();
}

// ---- current link ---------------------------------------------------------------------------------------------------------------------

namespace {
struct Current {
    std::mutex mu;
    std::shared_ptr<Link> link;
    Receiver inbound;
};
Current& cur() {
    static Current c;
    return c;
}
}  // namespace

void set_current_link(std::shared_ptr<Link> link) {
    Current& c = cur();
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.link == link) return;
    if (c.link) c.link->set_receiver(nullptr);
    c.link = std::move(link);
    if (c.link) c.link->set_receiver(c.inbound);
}

std::shared_ptr<Link> current_link() {
    Current& c = cur();
    std::lock_guard<std::mutex> lk(c.mu);
    return c.link;
}

void set_inbound(Receiver r) {
    Current& c = cur();
    std::lock_guard<std::mutex> lk(c.mu);
    c.inbound = std::move(r);
    if (c.link) c.link->set_receiver(c.inbound);
}

std::string ip_str(uint32_t a) {
    char b[16];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
    return b;
}

}  // namespace nxrt::vnet
