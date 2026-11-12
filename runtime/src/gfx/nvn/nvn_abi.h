// SPDX-License-Identifier: GPL-3.0-or-later
// Typed entry points for the NVN back end (S4d).
//
// An NVN function is written as an ordinary C++ function with the types of the NVN C header (third_party/nvn/nvn.h):
//
//     static void nvnBlendStateSetDefaults(NVNblendState* s) { ... }
//     ...
//     NVN_REG(nvnBlendStateSetDefaults);       // inside a register_xxx() function
//
// NVN_REG builds the AArch64 thunk that unpacks the guest registers into the C++ arguments (integers, enums, bools and
// pointers from x0.. in order, float and double from v0.. in order), calls the function and writes the result to x0 / v0.
// The argument classes of the C++ signature are compared with the generated table (nvn_table.inc); a mismatch is
// logged as an error and counted (abi::mismatches(), asserted to be 0 by the gfx_nvn_impl test).
//
// The same signature is used the other way round by the tests: abi::Caller<R(A...)> marshals a C++ call into guest
// registers and runs the thunk the way the generated code's call_indirect does, so a test calls NVN exactly like a game.
#pragma once
#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

#include "nvn_api_stats.h"
#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/nvn.h"
#include <nvn/nvn.h>

namespace nxrt::nvn::abi {

// ---- argument classes ---------------------------------------------------------------------------------------------------
template <class T>
constexpr char class_of() {
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<U, void>) return 'v';
    else if constexpr (std::is_same_v<U, float>) return 'f';
    else if constexpr (std::is_same_v<U, double>) return 'd';
    else if constexpr (std::is_pointer_v<U> || std::is_null_pointer_v<U>) return 'p';
    else if constexpr (std::is_enum_v<U>) return sizeof(U) > 4 ? 'l' : 'i';
    else if constexpr (std::is_integral_v<U>) return sizeof(U) > 4 ? 'l' : 'i';
    else if constexpr (std::is_class_v<U> && sizeof(U) == 8 && std::is_trivially_copyable_v<U>) return 'l';   // e.g. NVNseparateTextureHandle {uint64_t}
    else return 'S';   // any other struct by value: not supported by the thunk builder (static_assert in the reader)
}

// 'p' / 'i' / 'l' are all passed in integer registers; the table is not exact about 'p' versus 'l', so they compare equal.
constexpr bool classes_compatible(char table, char impl) {
    auto norm = [](char c) { return c == 'p' || c == 'S' ? 'l' : c; };   // 'S' (struct by value) is an 8-byte struct or, in the reconstructed header, a pointer
    return norm(table) == norm(impl);
}

// ---- reading the guest registers --------------------------------------------------------------------------------------
struct ArgReader {
    ::Ctx* c;
    int ni = 0, nf = 0, nstack = 0;
    uint64_t next_stack() {
        const uint64_t* st = reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(c->x[31]));
        return st[nstack++];
    }
    uint64_t next_int() {
        if (ni < 8) return c->x[ni++];
        return next_stack();
    }
    template <class T>
    T next() {
        using U = std::remove_cv_t<T>;
        static_assert(class_of<U>() != 'S', "NVN functions taking a struct by value are not supported by the thunk builder");
        if constexpr (std::is_same_v<U, float>) {
            if (nf < 8) return c->v[nf++].f32[0];
            const uint64_t raw = next_stack();   // AAPCS64: a float past v7 goes to the stack even while integer registers are free
            float f;
            std::memcpy(&f, &raw, sizeof f);
            return f;
        } else if constexpr (std::is_same_v<U, double>) {
            if (nf < 8) return c->v[nf++].f64[0];
            const uint64_t raw = next_stack();
            double d;
            std::memcpy(&d, &raw, sizeof d);
            return d;
        } else if constexpr (std::is_pointer_v<U>) {
            return reinterpret_cast<U>(static_cast<uintptr_t>(next_int()));
        } else if constexpr (std::is_same_v<U, bool>) {
            return (next_int() & 0xff) != 0;
        } else if constexpr (std::is_enum_v<U>) {
            using Under = std::underlying_type_t<U>;
            if constexpr (sizeof(Under) > 4) return static_cast<U>(static_cast<Under>(next_int()));
            else return static_cast<U>(static_cast<Under>(static_cast<int32_t>(static_cast<uint32_t>(next_int()))));
        } else if constexpr (std::is_class_v<U>) {
            const uint64_t raw = next_int();
            U u;
            std::memcpy(&u, &raw, sizeof u);
            return u;
        } else if constexpr (std::is_integral_v<U>) {
            const uint64_t raw = next_int();
            if constexpr (sizeof(U) == 8) return static_cast<U>(raw);
            else if constexpr (sizeof(U) == 4) return static_cast<U>(static_cast<uint32_t>(raw));
            else if constexpr (sizeof(U) == 2) return static_cast<U>(static_cast<uint16_t>(raw));
            else return static_cast<U>(static_cast<uint8_t>(raw));
        }
    }
};

template <class R>
inline void write_result(::Ctx* c, R v) {
    using U = std::remove_cv_t<R>;
    if constexpr (std::is_same_v<U, float>) {
        c->v[0].f32[0] = v;
    } else if constexpr (std::is_same_v<U, double>) {
        c->v[0].f64[0] = v;
    } else if constexpr (std::is_pointer_v<U>) {
        c->x[0] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(v));
    } else if constexpr (std::is_enum_v<U>) {
        c->x[0] = static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(v)));
    } else if constexpr (std::is_same_v<U, bool>) {
        c->x[0] = v ? 1 : 0;
    } else if constexpr (std::is_class_v<U>) {
        uint64_t raw = 0;
        std::memcpy(&raw, &v, sizeof v);
        c->x[0] = raw;
    } else if constexpr (std::is_integral_v<U>) {
        if constexpr (sizeof(U) == 8) c->x[0] = static_cast<uint64_t>(v);
        else c->x[0] = static_cast<uint64_t>(static_cast<uint32_t>(v));
    }
}

// ---- the thunk ---------------------------------------------------------------------------------------------------------
// Set when a recording ran out of the command or control memory the game added (api_cmdbuf.cpp): the thunk calls the game's
// memory callback after the NVN function returns, with no lock held, on the recording thread.
inline thread_local bool t_cmdmem_pending = false;
void cmdmem_run_pending(::Ctx* c);

template <auto Fn>
struct Thunk;

template <class R, class... A, R (*Fn)(A...)>
struct Thunk<Fn> {
    static constexpr char kRet = class_of<R>();
    static constexpr char kArgs[sizeof...(A) + 1] = {class_of<A>()..., '\0'};
    static void call(::Ctx* c) {
        const uint64_t t0 = api_stats::on() ? ft::tsc() : 0;   // [prof] render_split: time inside the NVN function (nvn_api_stats.h)
        ArgReader rd{c};
        if constexpr (std::is_void_v<R>) {
            std::apply(Fn, std::tuple<A...>{rd.template next<A>()...});
        } else {
            R r = std::apply(Fn, std::tuple<A...>{rd.template next<A>()...});
            write_result<R>(c, r);
        }
        if (t0) api_stats::note_call(ft::tsc() - t0);          // before the game's memory callback: that is game code
        if (t_cmdmem_pending) cmdmem_run_pending(c);
    }
};

// Registers `fn` as the implementation of `name`, after comparing its classes with the table entry. Returns false when the
// name is not an NVN entry point or the signature does not match (both are logged and counted).
bool register_checked(const char* name, HleFn fn, char ret, const char* args);
// How many registrations failed the signature comparison so far.
unsigned mismatches();
void reset_mismatches();

#define NVN_REG(fn) ::nxrt::nvn::abi::register_checked(#fn, &::nxrt::nvn::abi::Thunk<&fn>::call, ::nxrt::nvn::abi::Thunk<&fn>::kRet, ::nxrt::nvn::abi::Thunk<&fn>::kArgs)

// Same for an NVN2-only proc (a name that is not in the NVN1 table): proc_address hands the thunk out for that name.
#define NVN2_REG(fn) ::nxrt::nvn::register_extra_impl(#fn, &::nxrt::nvn::abi::Thunk<&fn>::call)

// ---- calling a thunk from C++ (tests, and host-side callers that behave like a game) -----------------------------------
template <class Sig>
class Caller;

template <class R, class... A>
class Caller<R(A...)> {
public:
    explicit Caller(uint64_t addr) : addr_(addr) {}
    explicit operator bool() const { return addr_ != 0; }
    uint64_t address() const { return addr_; }
    R operator()(A... args) const {
        ::Ctx c{};
        static thread_local unsigned char fake_stack[64];
        c.x[31] = reinterpret_cast<uint64_t>(fake_stack + 32);
        c.x[30] = 0x7100000100ull;   // a plausible guest return address, shown as the caller in logs
        int ni = 0, nf = 0;
        (put(&c, ni, nf, args), ...);
        dispatch_call(&c, addr_);
        if constexpr (!std::is_void_v<R>) return get_result(&c);
    }

private:
    template <class T>
    static void put(::Ctx* c, int& ni, int& nf, T v) {
        using U = std::remove_cv_t<T>;
        if constexpr (std::is_same_v<U, float>) c->v[nf++].f32[0] = v;
        else if constexpr (std::is_same_v<U, double>) c->v[nf++].f64[0] = v;
        else if constexpr (std::is_pointer_v<U>) c->x[ni++] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(v));
        else if constexpr (std::is_enum_v<U>) c->x[ni++] = static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(v)));
        else if constexpr (std::is_same_v<U, bool>) c->x[ni++] = v ? 1 : 0;
        else if constexpr (std::is_class_v<U>) {
            uint64_t raw = 0;
            std::memcpy(&raw, &v, sizeof v);
            c->x[ni++] = raw;
        } else if constexpr (std::is_integral_v<U>) {
            if constexpr (std::is_signed_v<U> && sizeof(U) < 8) c->x[ni++] = static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(v)));
            else c->x[ni++] = static_cast<uint64_t>(v);
        } else {
            static_assert(sizeof(U) == 0, "unsupported argument type");
        }
    }
    static R get_result(::Ctx* c) {
        using U = std::remove_cv_t<R>;
        if constexpr (std::is_same_v<U, float>) return c->v[0].f32[0];
        else if constexpr (std::is_same_v<U, double>) return c->v[0].f64[0];
        else if constexpr (std::is_pointer_v<U>) return reinterpret_cast<U>(static_cast<uintptr_t>(c->x[0]));
        else if constexpr (std::is_enum_v<U>) return static_cast<U>(static_cast<int32_t>(static_cast<uint32_t>(c->x[0])));
        else if constexpr (std::is_same_v<U, bool>) return (c->x[0] & 0xff) != 0;
        else if constexpr (std::is_class_v<U>) {
            U u;
            std::memcpy(&u, &c->x[0], sizeof u);
            return u;
        } else if constexpr (std::is_integral_v<U>) return static_cast<U>(sizeof(U) == 8 ? c->x[0] : (std::is_signed_v<U> ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(c->x[0]))) : (c->x[0] & 0xffffffffull)));
        else return U{};
    }
    uint64_t addr_;
};

// Deduces the signature from a PFN-style function pointer type: `proc<PFNNVNDEVICEINITIALIZEPROC>("nvnDeviceInitialize")`.
template <class Pfn>
struct PfnTraits;
template <class R, class... A>
struct PfnTraits<R (*)(A...)> {
    using Sig = R(A...);
};
template <class Pfn>
Caller<typename PfnTraits<Pfn>::Sig> proc(const char* name) {
    return Caller<typename PfnTraits<Pfn>::Sig>(hle_address(name));
}

}  // namespace nxrt::nvn::abi
