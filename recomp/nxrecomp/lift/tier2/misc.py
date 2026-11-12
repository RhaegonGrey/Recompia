# SPDX-License-Identifier: GPL-3.0-or-later
# Ported from SuperMarioWonderRecomp recomp/emit.py (helpers, memory, misc) (MIT License, Copyright (c) 2026 BelmanteGu; see
# recomp/THIRD_PARTY.md for the notice). Changes: the generated code uses nxrecomp's context (Ctx *c, c->x / c->v /
# c->N Z C V) and C++ helpers (nxrecomp/cxx.h); absolute guest addresses instead of module offsets.
import struct

from capstone import arm64_const as A

from .base import B, CC, VAS, Reg, Unhandled, subst, u, s  # noqa: F401


class MiscOps:
    def regop(self, i, o):
        return Reg(i.reg_name(o.reg))

    def shifted(self, i, o, bits):
        """Value of a register/immediate second operand with shift or extend applied, as uint{bits}."""
        if o.type == A.ARM64_OP_IMM:
            v = o.imm
            if o.shift.type == A.ARM64_SFT_LSL:
                v <<= o.shift.value
            return f"{u(bits)}({v & ((1 << bits) - 1):#x}ull)"
        r = self.regop(i, o)
        v = r.r()
        if o.ext:
            e = o.ext
            src = {A.ARM64_EXT_UXTB: "uint8_t", A.ARM64_EXT_UXTH: "uint16_t", A.ARM64_EXT_UXTW: "uint32_t",
                   A.ARM64_EXT_UXTX: "uint64_t", A.ARM64_EXT_SXTB: "int8_t", A.ARM64_EXT_SXTH: "int16_t",
                   A.ARM64_EXT_SXTW: "int32_t", A.ARM64_EXT_SXTX: "int64_t"}[e]
            v = f"{u(bits)}({s(bits) if src.startswith('int') else u(bits)}({src}({v})))"
            if o.shift.type == A.ARM64_SFT_LSL and o.shift.value:
                v = f"{u(bits)}({v} << {o.shift.value})"
            return v
        if o.shift.type and o.shift.value:
            n = o.shift.value
            t = o.shift.type
            if t == A.ARM64_SFT_LSL:
                return f"{u(bits)}({u(bits)}({v}) << {n})"
            if t == A.ARM64_SFT_LSR:
                return f"{u(bits)}({u(bits)}({v}) >> {n})"
            if t == A.ARM64_SFT_ASR:
                return f"{u(bits)}({s(bits)}({v}) >> {n})"
            if t == A.ARM64_SFT_ROR:
                return f"nx::ror{bits}({v}, {n})"
            raise Unhandled("shift")
        return f"{u(bits)}({v})"

    def cond(self, i):
        return CC[i.cc]

    def address(self, i, memo, after):
        """Returns (address expression, statement to run after the access for writeback)."""
        base = Reg(i.reg_name(memo.mem.base))
        bexpr = f"c->x[{base.n}]"
        idx = ""
        if memo.mem.index:
            ir = Reg(i.reg_name(memo.mem.index))
            iv = ir.r()
            if memo.ext in (A.ARM64_EXT_SXTW,):
                iv = f"uint64_t(int64_t(int32_t({iv})))"
            elif memo.ext in (A.ARM64_EXT_UXTW,):
                iv = f"uint64_t(uint32_t({iv}))"
            elif memo.ext in (A.ARM64_EXT_SXTX,):
                iv = f"uint64_t({iv})"
            if memo.shift.type == A.ARM64_SFT_LSL and memo.shift.value:
                iv = f"(uint64_t({iv}) << {memo.shift.value})"
            idx = f" + uint64_t({iv})"
        disp = memo.mem.disp
        post = None
        if after is not None:
            if after.type == A.ARM64_OP_IMM:
                post = f"{after.imm & 0xFFFFFFFFFFFFFFFF:#x}ull"
            elif after.type == A.ARM64_OP_REG:
                post = Reg(i.reg_name(after.reg)).r()
        if post is not None:  # post-index (capstone does not always flag writeback for structured ld/st)
            return bexpr + idx, f"c->x[{base.n}] += {post};"
        a = f"{bexpr}{idx} + {disp & 0xFFFFFFFFFFFFFFFF:#x}ull" if disp else bexpr + idx
        if i.writeback:
            return a, f"c->x[{base.n}] = a;"
        return a, ""

    # ---- function ---------------------------------------------------------------------------------------

    def i_nop(self, f, i, ops):
        return ";"

    i_hint = i_nop
    i_prfm = i_nop
    i_prfum = i_nop
    def i_yield(self, f, i, ops):
        return "nx_spin_hint(0);"

    def i_wfe(self, f, i, ops):
        return "nx_spin_hint(1);"
    i_wfi = i_nop
    i_sev = i_nop
    i_sevl = i_nop
    def i_dc(self, f, i, ops):
        if "zva" in i.op_str:  # zero a 64-byte block (DCZID_EL0 reports 64)
            r = Reg(i.op_str.split(",")[1].strip())
            return f"std::memset(reinterpret_cast<void*>(c->x[{r.n}] & ~63ull), 0, 64);"
        return ";"  # cache maintenance

    i_ic = i_nop
    i_autia1716 = i_nop
    i_autib1716 = i_nop
    i_pacia1716 = i_nop
    i_pacib1716 = i_nop

    def crc(self, i, ops, poly, size):
        d, n, m = (self.regop(i, o) for o in ops[:3])
        return d.w(f"nx::crc32<{size}>({n.r()}, {m.r()}, {poly:#x}u)")

    def i_crc32b(self, f, i, ops):
        return self.crc(i, ops, 0xEDB88320, 1)

    def i_crc32h(self, f, i, ops):
        return self.crc(i, ops, 0xEDB88320, 2)

    def i_crc32w(self, f, i, ops):
        return self.crc(i, ops, 0xEDB88320, 4)

    def i_crc32x(self, f, i, ops):
        return self.crc(i, ops, 0xEDB88320, 8)

    def i_crc32cb(self, f, i, ops):
        return self.crc(i, ops, 0x82F63B78, 1)

    def i_crc32ch(self, f, i, ops):
        return self.crc(i, ops, 0x82F63B78, 2)

    def i_crc32cw(self, f, i, ops):
        return self.crc(i, ops, 0x82F63B78, 4)

    def i_crc32cx(self, f, i, ops):
        return self.crc(i, ops, 0x82F63B78, 8)

    def i_ngc(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{u(d.bits)}({u(d.bits)}(~{self.regop(i, ops[1]).r()}) + c->C)")

    def i_ngcs(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"nx::adds{d.bits}(c, 0, {u(d.bits)}(~{self.regop(i, ops[1]).r()}), c->C)")

    def mem_ops(self, ops):
        """(register operands, mem operand, trailing post-index operand)."""
        regs, mem, after = [], None, None
        for o in ops:
            if o.type == A.ARM64_OP_MEM:
                mem = o
            elif mem is None:
                regs.append(o)
            else:
                after = o
        return regs, mem, after

    def load_into(self, r, addr, size, signed):
        if r.gp:
            if signed:
                return r.w(f"{u(r.bits)}({s(r.bits)}(nx::ld<{s(size * 8)}>({addr})))")
            return r.w(f"nx::ld<{u(size * 8)}>({addr})")
        if size == 16:
            return f"c->v[{r.n}] = nx::ld<V128>({addr});"
        return f"{{ V128 t_ = nx::vzero(); t_.u{size * 8}[0] = nx::ld<{u(size * 8)}>({addr}); c->v[{r.n}] = t_; }}"

    def store_from(self, r, addr, size):
        if r.gp:
            return f"nx::st<{u(size * 8)}>({addr}, {u(size * 8)}({r.r()}));"
        if size == 16:
            return f"nx::st<V128>({addr}, c->v[{r.n}]);"
        return f"nx::st<{u(size * 8)}>({addr}, c->v[{r.n}].u{size * 8}[0]);"

    def ldst(self, i, ops, size=None, signed=False, store=False, pair=False):
        regs, mem, after = self.mem_ops(ops)
        if mem is None:
            # Literal: ldr x0, #addr (the address is an immediate operand, not a register)
            addr = f"{B} + {ops[-1].imm:#x}ull"
            r = self.regop(i, ops[0])
            sz = size or r.bits // 8
            return self.load_into(r, addr, sz, signed)
        rs = [self.regop(i, o) for o in regs]
        sz = size or rs[0].bits // 8
        a, wb = self.address(i, mem, after)
        parts = [f"uint64_t a = {a};"]
        if store:
            for k, r in enumerate(rs):
                parts.append(self.store_from(r, f"a + {k * sz}" if k else "a", sz))
            parts.append(wb)
        else:
            # Load everything first so base-register overlap cannot corrupt the address.
            if pair:
                tmp = []
                for k, r in enumerate(rs):
                    parts.append(f"auto v{k} = nx::ld<{'V128' if sz == 16 else (s(sz * 8) if signed else u(sz * 8))}>(a + {k * sz});")
                    tmp.append(k)
                parts.append(wb)
                for k, r in enumerate(rs):
                    if r.gp:
                        parts.append(r.w(f"{u(r.bits)}({s(r.bits)}(v{k}))" if signed else f"v{k}"))
                    elif sz == 16:
                        parts.append(f"c->v[{r.n}] = v{k};")
                    else:
                        parts.append(f"{{ V128 t_ = nx::vzero(); t_.u{sz * 8}[0] = v{k}; c->v[{r.n}] = t_; }}")
            else:
                parts.append(wb)
                parts.append(self.load_into(rs[0], "a", sz, signed))
                if wb and rs[0].gp and rs[0].n == Reg(i.reg_name(mem.mem.base)).n:
                    raise Unhandled("load writeback to base")
        return "{ " + " ".join(p for p in parts if p) + " }"

    def i_ldr(self, f, i, ops):
        return self.ldst(i, ops)

    i_ldur = i_ldr

    def i_ldrb(self, f, i, ops):
        return self.ldst(i, ops, 1)

    i_ldurb = i_ldrb

    def i_ldrh(self, f, i, ops):
        return self.ldst(i, ops, 2)

    i_ldurh = i_ldrh

    def i_ldrsb(self, f, i, ops):
        return self.ldst(i, ops, 1, signed=True)

    i_ldursb = i_ldrsb

    def i_ldrsh(self, f, i, ops):
        return self.ldst(i, ops, 2, signed=True)

    i_ldursh = i_ldrsh

    def i_ldrsw(self, f, i, ops):
        return self.ldst(i, ops, 4, signed=True)

    i_ldursw = i_ldrsw

    def i_str(self, f, i, ops):
        return self.ldst(i, ops, store=True)

    i_stur = i_str

    def i_strb(self, f, i, ops):
        return self.ldst(i, ops, 1, store=True)

    i_sturb = i_strb

    def i_strh(self, f, i, ops):
        return self.ldst(i, ops, 2, store=True)

    i_sturh = i_strh

    def i_ldp(self, f, i, ops):
        return self.ldst(i, ops, pair=True)

    i_ldnp = i_ldp

    def i_ldpsw(self, f, i, ops):
        return self.ldst(i, ops, 4, signed=True, pair=True)

    def i_stp(self, f, i, ops):
        return self.ldst(i, ops, store=True)

    i_stnp = i_stp

    # acquire / release / exclusive
    # LDAR is not a C acquire load (nx::ld_acq is, and LDAPR in x_ldst.py keeps that): it must not pass an earlier STLR, nor a
    # plain store to another word, so it is nx::ldar (ctx.h ldar*: a full barrier and a seq_cst load, the barrier free on x86).
    def acq(self, i, ops, size):
        regs, mem, _ = self.mem_ops(ops)
        d = self.regop(i, regs[0])
        sz = size or d.bits // 8
        return d.w(f"nx::ldar<{u(sz * 8)}>(c->x[{Reg(i.reg_name(mem.mem.base)).n}])")

    def rel(self, i, ops, size):
        regs, mem, _ = self.mem_ops(ops)
        r = self.regop(i, regs[0])
        sz = size or r.bits // 8
        return f"nx::st_rel<{u(sz * 8)}>(c->x[{Reg(i.reg_name(mem.mem.base)).n}], {u(sz * 8)}({r.r()}));"

    def i_ldar(self, f, i, ops):
        return self.acq(i, ops, None)

    def i_ldarb(self, f, i, ops):
        return self.acq(i, ops, 1)

    def i_ldarh(self, f, i, ops):
        return self.acq(i, ops, 2)

    def i_stlr(self, f, i, ops):
        return self.rel(i, ops, None)

    def i_stlrb(self, f, i, ops):
        return self.rel(i, ops, 1)

    def i_stlrh(self, f, i, ops):
        return self.rel(i, ops, 2)

    def ldx(self, i, ops, size):
        regs, mem, _ = self.mem_ops(ops)
        d = self.regop(i, regs[0])
        sz = size or d.bits // 8
        return d.w(f"nx::ldxr<{u(sz * 8)}>(c, c->x[{Reg(i.reg_name(mem.mem.base)).n}])")

    def stx(self, i, ops, size):
        regs, mem, _ = self.mem_ops(ops)
        st, r = self.regop(i, regs[0]), self.regop(i, regs[1])
        sz = size or r.bits // 8
        return st.w(f"nx::stxr<{u(sz * 8)}>(c, c->x[{Reg(i.reg_name(mem.mem.base)).n}], {u(sz * 8)}({r.r()}))")

    def i_ldaxr(self, f, i, ops):
        return self.ldx(i, ops, None)

    i_ldxr = i_ldaxr

    def i_ldaxrb(self, f, i, ops):
        return self.ldx(i, ops, 1)

    i_ldxrb = i_ldaxrb

    def i_ldaxrh(self, f, i, ops):
        return self.ldx(i, ops, 2)

    i_ldxrh = i_ldaxrh

    def i_stlxr(self, f, i, ops):
        return self.stx(i, ops, None)

    i_stxr = i_stlxr

    def i_stlxrb(self, f, i, ops):
        return self.stx(i, ops, 1)

    i_stxrb = i_stlxrb

    def i_stlxrh(self, f, i, ops):
        return self.stx(i, ops, 2)

    i_stxrh = i_stlxrh
