// SPDX-License-Identifier: GPL-3.0-or-later
// ldn:u: picks the backend per service object (ldn_backend.h; [net] protocol: native = ldn_native.cpp, linkplay = ldn_linkplay.cpp) and
// holds the service's entry points (the IUserServiceCreator, ldn_reset, ldnapi). Shared types: ldn_types.h; shared helpers: ldn_internal.h.
#include "ldn.h"

#include <memory>
#if !defined(_WIN32)
#include "nx_os.h"   // _wfopen on POSIX: the wide path is converted to UTF-8
#endif

#include "nxrt/log.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "ldn_backend.h"
#include "ldn_native.h"
#include "services_internal.h"

namespace nxrt::services {
using namespace ldn_impl;
namespace {

// [net] protocol picks the backend (ldn_backend.h): native (the default; the offline answers with the co-op module off) or linkplay. The
// service object only forwards: the backend adds the command table and the session it returns is shut down on close.
LdnBackend& selected_backend() { return native_protocol() ? native_backend() : linkplay_backend(); }

class LdnService final : public ipc::Service {
public:
    LdnService() : ipc::Service("nn::ldn::detail::IUserLocalCommunicationService") { session_ = selected_backend().open(*this); }
    void on_close() override {
        if (session_) session_->shutdown();
    }

private:
    std::shared_ptr<LdnSession> session_;
};

}  // namespace

std::shared_ptr<ipc::Service> make_ldn_user_creator() {
    return make_iface("nn::ldn::detail::IUserServiceCreator", [](ipc::Service& s) {
        s.add_cmd(0, "CreateUserLocalCommunicationService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            note(1000, "IUserServiceCreator CreateUserLocalCommunicationService");
            rsp.push_object(std::make_shared<LdnService>());
            return 0;
        });
        s.add_cmd(1, "CreateClientProcessMonitor", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            note(1001, "IUserServiceCreator CreateClientProcessMonitor");
            rsp.push_object(make_iface("nn::ldn::detail::IClientProcessMonitor", [](ipc::Service& m) {
                m.add_cmd(0, "RegisterClient", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
            }));
            return 0;
        });
    });
}

coop_node* ldn_native_node() { return native().node.load(); }

void ldn_reset() {
    {
        std::lock_guard<std::mutex> lk(g_seen_mu);
        g_seen.clear();
    }
    ldnapi::set_session_variant(0, "");
    ldnapi::set_coop_mods({});
    for (LdnBackend* b : {&linkplay_backend(), &native_backend()}) b->reset();
}

namespace ldnapi {
void set_session_variant(uint8_t v, const char* what) {
    v &= kVariantMask >> kVariantShift;
    Variant& s = variant();
    std::lock_guard<std::mutex> lk(s.mu);
    s.value = v;
    s.what = what ? what : "";
}
uint8_t session_variant() { return own_variant(); }
void set_coop_mods(const std::vector<std::string>& names) {
    Variant& s = variant();
    std::lock_guard<std::mutex> lk(s.mu);
    s.mods = names;
}
std::vector<std::string> coop_mods() {
    std::vector<std::string> m;
    own_variant(nullptr, &m);
    return m;
}
bool native_protocol() { return services::native_protocol(); }
}  // namespace ldnapi

}  // namespace nxrt::services
