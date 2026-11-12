// SPDX-License-Identifier: GPL-3.0-or-later
// nx_catch_call: the catch frames of lifted functions that contain a catch clause (recomp ctx.h; ssbu-recomp's nro_catch_call is the
// model, only its name and idea are used). The lifter enters such a function as nx_catch_call(c, u_<entry>). This runs body(c, 0)
// under a setjmp frame keyed by the guest SP at entry (= the function's CFA). When the unwinder lands in that function it stores the
// landing pad in the frame and longjmps here, discarding every host frame above; the loop then calls body(c, pad), which resumes the
// lifted function at the pad. The same function may be landed in again (a try inside a loop), hence the loop.
#include "eh_internal.h"
#include "nxrt/dispatch.h"
#include "nxrt/eh.h"
#include "../diag/hle_census.h"

extern "C" void nx_catch_call(Ctx* c, void (*body)(Ctx*, uint64_t)) {
    using namespace nxrt::eh::detail;
    CatchRec rec;
    rec.cfa = c->x[31];
    catch_stack().push_back(&rec);
    // State that dispatch_call / host_import set up and restore on the way out; a longjmp skips that, so put it back by hand.
    const char* const saved_hle = nxrt::hle_current_name();
    const char* const saved_in_import = c->in_import;
    const uint64_t saved_import_from = c->import_from;
    const uint32_t saved_cb_flags = nxrt::host_callback_flags();   // a host bit set inside a guest callback the exception left
    const uint32_t saved_census = nxrt::diag::census::depth();   // [diag] hle_census scopes the longjmp skips
    volatile uint64_t pad = 0;
    for (;;) {
        if (setjmp(rec.jb) == 0) {
            body(c, pad);
            break;
        }
        pad = rec.pad;                                   // landed here by an exception
        nxrt::hle_restore_current(saved_hle);
        c->in_import = saved_in_import;
        c->import_from = saved_import_from;
        nxrt::host_callback_restore(saved_cb_flags);
        nxrt::diag::census::restore(saved_census);
    }
    auto& st = catch_stack();
    for (size_t i = st.size(); i-- > 0;)
        if (st[i] == &rec) { st.resize(i); break; }
}
