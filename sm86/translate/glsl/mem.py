"""SM86 -> GLSL, glsl.core.Builder methods for memory: constant banks, global (storage buffers), shared, local, attributes
and IPA. A mixin: it uses the Builder's operand helpers (src_u, dst...) and state."""

from typing import ClassVar

from program import Insn

from glsl.common import PT, RZ, URZ, Unsupported, multi_load, reg


class MemOps:
    def fold_set(self, i: Insn, state: tuple) -> None:
        n = i.v("Rd")
        if n != RZ and not self.predicated and i.sym("sat") != "SAT":
            self.fold[n] = state
            self.fold_new.add(n)

    def fold_ipa_w(self, i: Insn) -> str | None:
        """(attr * w) * (1 / w) as attr: the console hands a perspective varying times w and the shader multiplies by
        the reciprocal of w (IPA.PASS a[0x7c], MUFU.RCP), two multiplies the driver cannot fold since w * (1 / w) is
        not 1 in floats; the varying is then the value Vulkan interpolated, as on the console up to rounding. The fold
        of smo-recomp mx_translate.c fold_ipa_w and SavageWindRecomp glsl_fold_ipa_w (-8 % ACO instructions there)."""
        if not (i.has("Ra") and i.has("Rb")) or any(
            i.v(f"{r}@negate", 0) or i.v(f"{r}@absolute", 0) for r in ("Ra", "Rb")
        ):
            return None
        a, b = self.fold.get(i.v("Ra")), self.fold.get(i.v("Rb"))
        for x, y in ((a, b), (b, a)):
            if x and y and x[0] == "aw" and y == ("rw",):
                return x[1]
        return None

    def ipa(self, i: Insn) -> str:
        """IPA as Ryujinx InstEmitAttribute.cs (MIT) does it: a perspective input is the interpolated value times
        gl_FragCoord.w (the hardware hands the shader value * w); MULTIPLY then multiplies by Rb."""
        if not i.has("attr"):
            return self.ipa_indexed(i)
        addr = i.v("attr") * 4
        if 0x70 <= addr < 0x80:
            v = f"gl_FragCoord[{(addr - 0x70) >> 2}]"
        elif 0x80 <= addr < 0x280:
            attr, comp = (addr - 0x80) // 16, ((addr - 0x80) >> 2) & 3
            self.in_attrs.add(attr)
            v = plain = f"in_attr{attr}[{comp}]"
            persp = bool(self.sph and self.sph_perspective(attr))
            if persp:
                v = f"({v} * gl_FragCoord.w)"
        elif addr in (0x2E0, 0x2E4):  # PointCoord (Ryujinx AttributeMap.cs)
            v = f"gl_PointCoord[{(addr - 0x2E0) >> 2}]"
        elif addr in (0x60, 0x64, 0x68):
            # PrimitiveId, Layer, ViewportIndex (Ryujinx AttributeConsts): fragment inputs of the built-ins. Layer was
            # refused in 3 Orbitals programs (17/11/2026).
            v = {0x60: "gl_PrimitiveID", 0x64: "gl_Layer", 0x68: "gl_ViewportIndex"}[addr]
            v = f"intBitsToFloat({v})"
        elif addr == 0x3FC:  # FrontFacing, a bool: all ones when true, as Ryujinx stores booleans
            v = "uintBitsToFloat(gl_FrontFacing ? 0xffffffffu : 0u)"
        else:
            raise Unsupported(f"IPA a[{addr:#x}] (system value)")
        op = i.sym("ipaop")
        if op == "MUL" and 0x80 <= addr < 0x280 and persp and self.fold.get(i.v("Rb")) == ("rw",):
            return self.dst_f(i, plain)  # fold_ipa_w
        if op == "MUL":
            v = f"({v} * {self.src_f(i, 'b')})"
        elif i.sym("ipaop") == "CONSTANT" and 0x80 <= addr < 0x280:
            v = f"in_attr{(addr - 0x80) // 16}[{((addr - 0x80) >> 2) & 3}]"  # flat: no perspective factor
        elif i.sym("ipaop") not in ("PASS", "", "CONSTANT"):  # CONSTANT on a system value: the value itself
            raise Unsupported(f"IPA.{i.sym('ipaop')}")
        out = self.dst_f(i, v)
        if op in ("PASS", "") and addr == 0x7C:
            self.fold_set(i, ("w",))
        elif op in ("PASS", "") and 0x80 <= addr < 0x280 and persp:
            self.fold_set(i, ("aw", plain))
        return out

    def ipa_indexed(self, i: Insn) -> str:
        """IPA.PASS a[URa + offset]: an input picked at run time (a loop over a material's inputs: agl_common 230 and
        three more). ipa_dyn, made by ipa_dyn_fns, reads any generic input the SPH marks used, with the same
        perspective factor as a fixed IPA, and FragCoord; any other address reads 0."""
        if not i.has("URa") or i.sym("ipaop") not in ("PASS", "") or not self.sph:
            raise Unsupported(f"{i.cls}: attribute address in a register")
        self.ipa_dyn = True
        off = i.v("URa_offset") * 4
        ua = i.v("URa")
        if ua != URZ:
            self.uregs.add(ua)
        addr = f"(ur{ua} + {off:#x}u)" if ua != URZ else f"{off:#x}u"
        return self.dst_f(i, f"ipa_dyn({addr})")

    def ipa_dyn_fns(self) -> list[str]:
        if not getattr(self, "ipa_dyn", False):
            return []
        cases = [f"    case {0x70 + 4 * c:#x}u: return gl_FragCoord[{c}];" for c in range(4)]
        for a in range(32):
            if not any(self.sph.imap_used(a, c) for c in range(4)):
                continue
            self.in_attrs.add(a)
            w = " * gl_FragCoord.w" if self.sph_perspective(a) else ""
            cases += [f"    case {0x80 + 16 * a + 4 * c:#x}u: return in_attr{a}[{c}]{w};" for c in range(4)]
        return ["float ipa_dyn(uint a) {", "    switch (a) {", *cases, "    }", "    return 0.0;", "}"]

    def sph_perspective(self, attr: int) -> bool:
        """PS input map: the first used component of a generic input says its interpolation (2 = perspective)."""
        byte = (self.sph.words[6 + (attr >> 2)] >> ((attr & 3) * 8)) & 0xFF
        for comp in range(4):
            t = (byte >> (comp * 2)) & 3
            if t:
                return t == 2
        return False

    def uldc(self, i: Insn) -> str:
        size = {"32": 1, "": 1, "64": 2}.get(i.sym("sz"))
        if size is None:
            raise Unsupported(f"ULDC.{i.sym('sz')}")
        n = i.v("URd")
        if n == URZ:
            return ""
        iv = next(v for k, v in i.f.items() if k.startswith("ConstBankAddress"))
        bank, addr = (v for _, v in iv.slot_values)
        out = []
        for k in range(size):   # a new value: no longer what was traced (track_ssbo leaves ULDC to this)
            self.ur_ssbo.pop(n + k, None), self.ur_derived.pop(n + k, None), self.ur_const.pop(n + k, None)
            self.ur_or.pop(n + k, None), self.ur_or_g.pop(n + k, None)
        if i.has("URa") and i.v("URa") != URZ:
            # ULDC URd, c[bank][URa + offset]: a dynamic address. The index was dropped and every read took word
            # `offset`: the race's colour-grading LUT read the same table entry for all 32 slices (no blue, 31/10/2026).
            self.cbufs.add(bank)
            loads = []
            for k in range(size):
                self.uregs.add(n + k)
                loads.append(self.cb_word(bank, self.ureg(i.v("URa")), addr + 4 * k))
            tmp = ", ".join(f"lw{k} = {w}" for k, w in enumerate(loads))
            return f"{{ uint {tmp}; {' '.join(f'ur{n + k} = lw{k};' for k in range(size))} }}"
        sb = size == 2 and (bank == 0 or self.SB_BANKS[0] <= bank < self.SB_BANKS[1])
        for k in range(size):
            self.uregs.add(n + k)
            out.append(f"ur{n + k} = {self.const(bank, addr + 4 * k, spec=not sb)};")
            self.ur_const[n + k] = (bank, addr + 4 * k)   # a texture handle read through URc (tex.py tex_key)
        if sb:
            # A storage buffer address: the driver keeps one per binding in bank 0. A user uniform block may hold one
            # too: Orbitals (Unreal) loads LDG/STG bases with ULDC.64 c[3][...] in 50 programs it dumped (title,
            # menu and character select backgrounds, 17/11/2026); the back end reads it from the block (ubo_ptr).
            self.ur_ssbo[n] = self.sb_key(bank, addr)
        return " ".join(out)

    # ---- storage buffer keys: the c[0] byte offset of a driver binding, or bank << 16 | byte offset for an address
    # held in a user uniform block (banks 3..20 = NVN uniform slots 0..17, vk_exec_bind.c ubo_ptr_res)
    SB_BANKS = (3, 21)

    @staticmethod
    def sb_key(bank: int, byte: int) -> int:
        return bank << 16 | byte

    def sb_word(self, key: int, extra: int = 0) -> str:
        """The bank word at the storage buffer's address (+ extra bytes), plain: the traced bases subtract it."""
        return self.const(key >> 16, (key & 0xFFFF) + extra, spec=False)

    # ---- global memory as storage buffers
    def global_mem(self, i: Insn) -> str:
        """LDG/STG through a storage buffer's address, which the driver keeps in bank 0 (one slot per binding, as
        Ryujinx's GlobalMemoryToStorageBufferPass finds them on Maxwell): a 64-bit uniform base loaded from it, or a
        register pair computed from it (reg_ssbo). The access becomes sb<offset>.d[] at its byte offset in the buffer:
        for a register base, Ra minus the buffer's address (the low words; buffers stay under 4 GiB). A register base the
        tracking lost goes through global_search."""
        try:
            slot, rel = self.global_addr(i)
        except Unsupported:
            if not ("sImmOffset" in i.cls or "RaNonRZ" in i.cls) or i.v("Ra") == RZ:
                raise
            return self.global_search(i)
        if i.op == "STG":
            self.ssbos_written.add(slot)
        sz = i.sym("sz")
        sub = {"U8": (8, False), "S8": (8, True), "U16": (16, False), "S16": (16, True)}.get(sz)
        words = {"32": 1, "64": 2, "128": 4}.get(sz, 1 if sub else None)
        if words is None:
            raise Unsupported(f"{i.op}.{sz}")
        if sub and i.op == "STG":
            # A byte or halfword store into the word holding it: clear its bits, then set them, both atomic, so the
            # other bytes of the word (other invocations may store them) stay as they are.
            bits = sub[0]
            sh, n = f"((({rel}) & 3u) * 8u)", i.v("Rb")
            if n != RZ:
                self.regs.add(n)
            m = f"({(1 << bits) - 1:#x}u << {sh})"
            w = f"sb{slot:x}.d[({rel}) >> 2]"
            return f"atomicAnd({w}, ~{m}); atomicOr({w}, ({reg(n)} << {sh}) & {m});"
        if sub:
            bits, signed = sub
            n = i.v("Rd")
            if n == RZ:
                return ""
            self.regs.add(n)
            w, sh = f"sb{slot:x}.d[{rel} >> 2]", f"(({rel} & 3u) * 8u)"
            e = (
                f"uint(bitfieldExtract(int({w}), int({sh}), {bits}))"
                if signed
                else f"bitfieldExtract({w}, int({sh}), {bits})"
            )
            return f"r{n} = {e};"
        out = []
        if i.op == "LDG":
            n, loads = i.v("Rd"), []
            for k in range(words):
                if n + k != RZ:
                    self.regs.add(n + k)
                    loads.append((n + k, f"sb{slot:x}.d[({rel} >> 2) + {k}u]"))
            return multi_load(loads)
        else:
            n = i.v("Rb")
            for k in range(words):
                self.regs.add(n + k) if n != RZ else None
                out.append(f"sb{slot:x}.d[({rel} >> 2) + {k}u] = {reg(n + k if n != RZ else RZ)};")
        return " ".join(out)

    def global_addr(self, i: Insn) -> tuple[int, str]:
        """(storage buffer's c[0] offset, byte offset in it) of a global access; see global_mem."""
        base = next((i.v(k) for k in ("Ra_URb", "Ra_URc") if i.has(k)), None)
        off = i.sv("Ra_offset") & 0xFFFFFFFF
        ra = i.v("Ra") if i.has("Ra") else RZ
        if base is not None and base in self.ur_ssbo:
            slot = self.ur_ssbo[base]
            rel = f"({self.src_u(i, 'a')} + {off:#x}u)" if i.cls.endswith("Ra32") and ra != RZ else f"{off:#x}u"
        elif base is not None and base in self.ur_derived:
            slot = self.ur_derived[base]
            self.uregs.add(base)
            extra = f"{self.src_u(i, 'a')} + " if i.cls.endswith("Ra32") and ra != RZ else ""
            rel = f"({extra}ur{base} - {self.sb_word(slot)} + {off:#x}u)"
        elif base is None and ("sImmOffset" in i.cls or "RaNonRZ" in i.cls) and ra in self.reg_ssbo:
            slot = self.reg_ssbo[ra]
            self.regs.add(ra)
            rel = f"(r{ra} - {self.sb_word(slot)} + {off:#x}u)"
        else:
            raise Unsupported(f"{i.cls}: base not a traced storage buffer")
        self.ssbos.add(slot)
        return slot, rel

    def global_search(self, i: Insn) -> str:
        """LDG/STG of 32, 64 or 128 bits whose register base the linear tracking lost (a BRX or another path rewrote
        the register in program order: DrivePassShaderDefault 91, missing in 121 runs). The address is looked up at run
        time among the program's storage buffers (gld/gst, made by global_search_fns once they are all known): the
        nearest base below it whose bound range holds it. The idea is eden-nx2's global memory functions
        (src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:942-1000, GPL-3.0: idea only)."""
        words = {"32": 1, "64": 2, "128": 4}.get(i.sym("sz"))
        if words is None:
            raise Unsupported(f"{i.op}.{i.sym('sz')}: base not a traced storage buffer")
        ra, off = i.v("Ra"), i.sv("Ra_offset") & 0xFFFFFFFF
        self.regs.update((ra, ra + 1))
        addr = f"uint gc, gl = uaddCarry(r{ra}, {off:#x}u, gc), gh = r{ra + 1} + gc;"
        if i.op == "LDG":
            self.gsearch_load = True
            n = i.v("Rd")
            if n == RZ:
                return ""
            self.regs.update(range(n, n + words))
            tmp = ", ".join(f"gw{k} = gld(gl, gh, {k}u)" for k in range(words))
            return f"{{ {addr} uint {tmp}; {' '.join(f'r{n + k} = gw{k};' for k in range(words))} }}"
        self.gsearch_store = True
        n = i.v("Rb")
        if n != RZ:
            self.regs.update(range(n, n + words))
        stores = " ".join(f"gst(gl, gh, {k}u, {reg(n + k if n != RZ else RZ)});" for k in range(words))
        return f"{{ {addr} {stores} }}"

    def global_search_fns(self) -> list[str]:
        """gld/gst over the storage buffers the program names (bank 0 base, bound length); every one may be stored
        into when gst is used. No buffer at all: nothing to search."""
        if not (self.gsearch_load or self.gsearch_store or self.gsearch_atom):
            return []
        # Unreal keeps each buffer as {u64 address, u32 size} at a 16-byte row of a user uniform block. A program that
        # reads both address words of such a row, compares its size word as an integer or moves it into a register for
        # that (the bounds check Unreal emits; a row of floats goes straight into FADD/FFMA: without that test 281
        # programs gained 7157 bindings, up to 80 in one) and goes through the search may address that buffer with a
        # register pair
        # loaded from it (MOV R0, c[3][0x10]; MOV R1, c[3][0x14]; STG [R0]), which no traced access names: the buffer
        # joins the search. Orbitals' 565a0b530c43107c resets the record walk's readback count this way at
        # c[3][0x10]; without it the reset was dropped, the count stayed garbage and the CPU popped records below index 0
        # (the memcpy / Free / 0x104 crashes, 25/11/2026).
        for bank, byte in sorted(self.const_reads):
            if self.SB_BANKS[0] <= bank < self.SB_BANKS[1] and byte % 16 == 0 and \
                    (bank, byte + 4) in self.const_reads and (bank, byte + 8) in self.const_icmp:
                self.ssbos.add(self.sb_key(bank, byte))
        if not self.ssbos:
            raise Unsupported("global access with no storage buffer to search")
        if self.gsearch_store or self.gsearch_atom:
            self.ssbos_written |= self.ssbos
        find = []
        for k, o in enumerate(sorted(self.ssbos)):
            lo, hi = self.sb_word(o), self.sb_word(o, 4)
            find.append(f"    r = usubBorrow(lo, {lo}, b); if (hi - {hi} - b == 0u && r < uint(sb{o:x}.d.length()) * 4u "
                        f"&& r < w) {{ w = r; s = {k}; }}")  # fmt: skip
        head = ["uint gfind(uint lo, uint hi, out int s) {", "    uint w = 0xffffffffu, r, b; s = -1;", *find,
                "    return w;", "}"]  # fmt: skip
        pick = " else ".join(f"if (s == {k}) {{X(sb{o:x})}}" for k, o in enumerate(sorted(self.ssbos)))
        out = head
        if self.gsearch_load:
            ld = pick.replace("{X(", "v = ").replace(")}", ".d[(w >> 2) + k];")
            out += ["uint gld(uint lo, uint hi, uint k) {", "    int s; uint w = gfind(lo, hi, s), v = 0u;",
                    f"    {ld}", "    return v;", "}"]  # fmt: skip
        for op in sorted(self.gsearch_atom):
            at = pick.replace("{X(", "o = " + self.ATOM[op] + "(").replace(")}", ".d[w >> 2], v);")
            out += [f"uint gatm_{op}(uint lo, uint hi, uint v) {{", "    int s; uint w = gfind(lo, hi, s), o = 0u;",
                    f"    {at}", "    return o;", "}"]  # fmt: skip
        if self.gsearch_store:
            st = pick.replace("{X(", "").replace(")}", ".d[(w >> 2) + k] = v;")
            out += ["void gst(uint lo, uint hi, uint k, uint v) {", "    int s; uint w = gfind(lo, hi, s);",
                    f"    {st}", "}"]  # fmt: skip
        return out

    ATOM: ClassVar[dict[str, str]] = {"ADD": "atomicAdd", "MIN": "atomicMin", "MAX": "atomicMax", "AND": "atomicAnd",
                                      "OR": "atomicOr", "XOR": "atomicXor", "EXCH": "atomicExchange"}  # fmt: skip

    def atom_global(self, i: Insn) -> str:
        """ATOMG / RED on a storage buffer word: GLSL atomics on its uint array, 32-bit unsigned or ADD/EXCH of any
        32-bit kind (signed MIN/MAX would need an int view: not handled). RED keeps no result."""
        op, sz = i.sym("op"), i.sym("sz")
        if op not in self.ATOM or sz not in ("U32", "32", "S32") or (sz == "S32" and op in ("MIN", "MAX")):
            raise Unsupported(f"{i.op}.{op}.{sz}")
        if i.has("Pu") and i.v("Pu") != PT:
            raise Unsupported(f"{i.op} with predicate out")
        try:
            slot, rel = self.global_addr(i)
        except Unsupported:
            # A register base the tracking lost (RaNonRZ): looked up at run time among the program's storage buffers,
            # as global_search does for LDG/STG (4 Orbitals compute programs, 18/11/2026)
            if "RaNonRZ" not in i.cls or i.v("Ra") == RZ:
                raise
            ra, off = i.v("Ra"), i.sv("Ra_offset") & 0xFFFFFFFF
            self.regs.update((ra, ra + 1))
            self.gsearch_atom.add(op)
            addr = f"uint gc, gl = uaddCarry(r{ra}, {off:#x}u, gc), gh = r{ra + 1} + gc;"
            call = f"gatm_{op}(gl, gh, {self.src_u(i, 'b')})"
            if i.op == "RED" or i.v("Rd", RZ) == RZ:
                return f"{{ {addr} {call}; }}"
            n = i.v("Rd")
            self.regs.add(n)
            return f"{{ {addr} r{n} = {call}; }}"
        self.ssbos_written.add(slot)
        e = f"{self.ATOM[op]}(sb{slot:x}.d[{rel} >> 2], {self.src_u(i, 'b')})"
        return f"{e};" if i.op == "RED" else self.dst(i, e)

    def atoms(self, i: Insn) -> str:
        """ATOMS: an atomic on a 32-bit shared memory word (byte address Ra + offset), Rd the old value. Unsigned and
        bitwise ops map to GLSL's; signed MIN/MAX, INC and DEC (old >= b ? 0 : old + 1; old == 0 || old > b ? b :
        old - 1, as Ryujinx's AtomicDecrement/Increment) go through a compare-and-swap loop."""
        op, sz = i.sym("op"), i.sym("sz")
        if sz not in ("", "U32", "_32", "32", "S32"):
            raise Unsupported(f"ATOMS.{op}.{sz}")
        self.uses_shared = True
        off = (i.sv("Ra_offset") if "NonRZ" in i.cls else i.v("Ra_offset")) & 0xFFFFFFFF
        ra = i.v("Ra") if i.has("Ra") else RZ
        addr = f"({reg(ra)} + {off:#x}u)" if ra != RZ else f"{off:#x}u"
        if ra != RZ:
            self.regs.add(ra)
        b = self.src_u(i, "b")
        word = f"SMEM(({addr}) >> 2)"
        signed = sz == "S32"
        new = {"MIN": "uint(min(int(w), int(bv)))", "MAX": "uint(max(int(w), int(bv)))"} if signed else {}
        new |= {"INC": "(w >= bv ? 0u : w + 1u)", "DEC": "((w == 0u || w > bv) ? bv : w - 1u)"}
        n = i.v("Rd", RZ)
        if n != RZ:
            self.regs.add(n)
        put = f" r{n} = w;" if n != RZ else ""
        if op in new:
            loop = (f"{{ uint ad = {addr}, bv = {b}, o = SMEM(ad >> 2), w; do {{ w = o; "
                    f"o = atomicCompSwap(SMEM(ad >> 2), w, {new[op]}); }} while (o != w);{put} }}")  # fmt: skip
            return loop
        fn = {"ADD": "atomicAdd", "MIN": "atomicMin", "MAX": "atomicMax", "AND": "atomicAnd", "OR": "atomicOr",
              "XOR": "atomicXor", "EXCH": "atomicExchange"}.get(op)  # fmt: skip
        if fn is None:
            raise Unsupported(f"ATOMS.{op}")
        call = f"{fn}({word}, {b})"
        return f"r{n} = {call};" if n != RZ else f"{call};"

    def track_ssbo(self, i: Insn) -> None:
        """reg_ssbo after i: a register written from a bank-0 word that is a storage buffer's address (LEA/IADD3 of
        an index with it, LDC.64 of it), or from a register already holding one, keeps that buffer; any other write
        drops it. Straight-line approximation of Ryujinx's search, enough for the address arithmetic ptxas emits."""
        # up_any: a write of a predicate ends what a vote said of it (Pu/Pv, UPu/UPv are the predicate destinations;
        # 7 is PT/UPT, no write). Then VOTEU.ANY UPu, Pp records UPu <- Pp.
        for f in ("Pu", "Pv"):
            if i.has(f) and i.v(f) != PT:
                p = i.v(f)
                for up in [u for u, (q, _) in self.up_any.items() if q == p]:
                    del self.up_any[up]
                self.p_is_up.pop(p, None)
        for f in ("UPu", "UPv"):
            if i.has(f) and i.v(f) != PT:
                up = i.v(f)
                self.up_any.pop(up, None)
                for p in [p for p, u in self.p_is_up.items() if u == up]:
                    del self.p_is_up[p]
        # PLOP3 Pu = PT & PT & UPr (table 0x80 over two true inputs): Pu holds UPr's value
        if i.op == "PLOP3" and i.has("UPr") and i.v("Pg", PT) == PT and not i.v("Pg@not", 0) \
                and i.v("uimm8", 0) == 0x80 and i.v("Pu", PT) != PT and not i.v("UPr@not", 0) \
                and all(i.v(f, PT) == PT and not i.v(f + "@not", 0) for f in ("Pp", "Pq")):
            self.p_is_up[i.v("Pu")] = i.v("UPr")
        if i.op == "VOTEU" and i.sym("voteop") == "ANY" and i.v("UPu", PT) != PT and i.v("Pg", PT) == PT \
                and not i.v("Pg@not", 0):
            self.up_any[i.v("UPu")] = (i.v("Pp", PT), i.v("Pp@not", 0))
        if i.has("URd") and i.op != "ULDC":   # ULDC keeps uldc's record
            # uniform: a register derived from a ULDC.64 base (ur_ssbo) or from one derived already (ur_derived).
            # Any other write ends both: a base kept after `UIADD3 UR4, UR4, 0x100` lost the offset of the accesses
            # through it (115 programs, among them the cacti's draw arguments; tools/shaders/track_audit.py).
            n = i.v("URd")
            # ULOP3 URd, URa, URb, URZ with the table of a | b over two ULDC words: Orbitals' combined handles,
            # c[3][texture word] | c[3][sampler word] (tools/shaders/handle_trace.py, 56 programs, 17/11/2026)
            # The table is a | b whatever URc is (0xfc), or with URc = URZ any table whose c = 0 half reads a | b: the
            # 6 programs still refused on 18/11/2026 had ULOP3 URd, URa, URb, URc with a non-zero URc (base_origin.py
            # --tex). UMOV copies a word or a combination (3 of them moved the ULDC word first).
            combo = copied = guarded = None
            lut, c = i.v("imm8", 0), i.v("URc", URZ)
            if i.op == "ULOP3" and (lut == 0xFC or (c == URZ and lut & 0x55 == 0x54)):
                a, b = i.v("URa", URZ), i.v("URb", URZ)
                if a in self.ur_const and b in self.ur_const:
                    pair = (self.ur_const[a], self.ur_const[b])
                    if i.v("UPg", PT) == PT and not i.v("UPg@not", 0):
                        combo = pair
                    else:   # @UPg: the pair only where the guard held (tex_key asks for a TEX guard implying it)
                        guarded = (pair, (i.v("UPg"), i.v("UPg@not", 0)))
            if i.op == "UMOV" and i.v("UPg", PT) == PT and not i.v("UPg@not", 0):
                src = next((i.v(r) for r in ("URb", "URa") if i.has(r) and i.v(r) != URZ), None)
                if src is not None and (src in self.ur_const or src in self.ur_or):
                    copied = (self.ur_const.get(src), self.ur_or.get(src))
            self.ur_const.pop(n, None), self.ur_or.pop(n, None), self.ur_or_g.pop(n, None)
            if combo is not None and n != URZ:
                self.ur_or[n] = combo
            if guarded is not None and n != URZ:
                self.ur_or_g[n] = guarded
            if copied is not None and n != URZ:
                if copied[0] is not None:
                    self.ur_const[n] = copied[0]
                if copied[1] is not None:
                    self.ur_or[n] = copied[1]
            src = None
            if i.op in ("UIADD3", "ULEA", "UMOV", "UIMAD"):
                src = next((self.ur_ssbo.get(i.v(r), self.ur_derived.get(i.v(r))) for r in ("URa", "URb", "URc")
                            if i.has(r) and (i.v(r) in self.ur_ssbo or i.v(r) in self.ur_derived)), None)  # fmt: skip
            self.ur_ssbo.pop(n, None), self.ur_derived.pop(n, None)
            if src is not None and n != URZ:
                self.ur_derived[n] = src
            return
        if not i.has("Rd") or i.op == "STG":
            return   # an LDG's destination is a loaded value, a pointer at most: dropped below
        n = i.v("Rd")
        if n == RZ:
            return
        self.track_handle(i, n)
        slot = None
        for k, iv in i.f.items():
            if k.startswith("ConstBankAddress"):
                bank, word = (v for _, v in iv.slot_values)
                if bank == 0 and word % 2 == 0 and word * 4 >= 0x10:  # CS bases from 0x10 (dumped cb84cbdf)
                    slot = word * 4
        if slot is None and i.op in ("IADD3", "LEA", "IMAD", "MOV", "SEL"):
            slot = next((self.reg_ssbo[i.v(r)] for r in ("Ra", "Rb", "Rc") if i.has(r) and i.v(r) in self.reg_ssbo),
                        None)  # fmt: skip
        self.reg_ssbo.pop(n, None)
        # a 32-bit write leaves the next register alone (agl_common: `SHF R7` between `IADD3 R8, c[0x0][0x510]` and
        # `RED [R8]` dropped R8's buffer: 71 programs, 31/10/2026); pairs and quads clear what they cover
        wide = {i.sym(k) for k in ("sz", "wide", "fmt")} & {"64", "128", "WIDE", "U64", "S64"}
        for k in range(1, 4 if "128" in wide else 2 if wide else 1):
            self.reg_ssbo.pop(n + k, None)
        if slot is not None:
            self.reg_ssbo[n] = slot

    @staticmethod
    def const_operand(i: Insn) -> tuple[int, int] | None:
        """(bank, byte) of the instruction's constant bank operand; 14-bit address slots count words, 16-bit ones bytes."""
        for k, iv in i.f.items():
            if k.startswith("ConstBankAddress"):
                (_, bank), (slot, addr) = iv.slot_values
                return bank, addr * 4 if slot.width == 14 else addr
        return None

    @staticmethod
    def pred_of(i: Insn) -> tuple[int, int] | None:
        """The instruction's guard (Pg, negated), None when it always runs."""
        g, neg = i.v("Pg", PT), i.v("Pg@not", 0)
        return None if g == PT and not neg else (g, neg)

    def track_handle(self, i: Insn, n: int) -> None:
        """r_const / r_or after i writes Rd = n: `MOV Rn, c[b][x]` then `LOP3.LUT Rm, Rn, c[b][y], RZ, 0xfc` (a | b), the
        texture | sampler handle words in a general register (tex_scr_b_noConst_; tools/shaders/base_origin.py --tex on
        Orbitals' dump: 8 programs, 17/11/2026). Each entry keeps the guard of its writes: Orbitals picks the texture
        with predicated MOV/LOP3 pairs (@P c[3][0x10], 0x18 or 0x20) and samples it under the same guard, so a TEX takes
        the entry only when its own guard is the same (tex.py tex_key). Any other write ends what n held."""
        g = self.pred_of(i)
        combo = None
        # a | b whatever the table says for c, which is RZ: the bits of c = 0 (0, 2, 4, 6) read 0, 1, 1, 1
        if (i.op == "LOP3" and i.v("imm8", 0) & 0x55 == 0x54 and i.v("Rc", RZ) == RZ and i.v("Ra", RZ) in self.r_const
                and not i.v("Ra@not", 0) and not i.v("Rc@not", 0)):
            c = self.const_operand(i)
            src, sg = self.r_const[i.v("Ra")]
            if c is not None and sg in (None, g):
                combo = (src, c)
        # MOV R, c[b][x], or IMAD.MOV.U32 R, RZ, RZ, c[b][x] (the form ptxas also writes)
        mov = i.op == "MOV" or (i.op == "IMAD" and i.v("Ra", 0) == RZ and i.v("Rb", 0) == RZ and "RRC" in i.cls)
        konst = self.const_operand(i) if mov else None
        wide = {i.sym(k) for k in ("sz", "wide", "fmt")} & {"64", "128", "WIDE", "U64", "S64"}
        for k in range(4 if "128" in wide else 2 if wide else 1):
            self.r_const.pop(n + k, None), self.r_or.pop(n + k, None)
        if konst is not None:
            self.r_const[n] = (konst, g)
        if combo is not None:
            self.r_or[n] = (combo, g)

    def ldc(self, i: Insn) -> str:
        """LDC c[bank][Ra + offset]: a constant read at a dynamic address; .U8/.S8/.U16/.S16 take the byte or halfword
        from the word holding it, zero- or sign-extended."""
        sz = i.sym("sz")
        sub = {"U8": (8, False), "S8": (8, True), "U16": (16, False), "S16": (16, True)}.get(sz)
        words = {"32": 1, "64": 2}.get(sz, 1 if sub else None)
        if words is None:
            raise Unsupported(f"LDC.{sz}")
        iv = next(v for k, v in i.f.items() if k.startswith("ConstBankAddress"))
        bank, off = (v for _, v in iv.slot_values)
        self.cbufs.add(bank)
        addr = f"({self.src_u(i, 'a')} + {off}u)"
        out, n = [], i.v("Rd")
        if sub:
            if n == RZ:
                return ""
            self.regs.add(n)
            word, sh = f"cb{bank}.d[{addr} >> 4][({addr} >> 2) & 3u]", f"int(({addr} & 3u) * 8u)"
            bits, signed = sub
            v = f"uint(bitfieldExtract(int({word}), {sh}, {bits}))" if signed else f"bitfieldExtract({word}, {sh}, {bits})"
            return f"r{n} = {v};"
        for k in range(words):
            if n + k != RZ:
                self.regs.add(n + k)
                out.append((n + k, self.cb_word(bank, self.src_u(i, "a"), off + 4 * k)))
        return multi_load(out)

    def cb_word(self, bank: int, base: str, off: int) -> str:
        """The word at byte base + off of a constant bank, base dynamic and off a constant multiple of 4: through the
        bank's bounded word view (head.py cbNw_at: 0 past the bound range), as (base >> 2) + off / 4 with an
        arithmetic shift, so a negative base still lands where base + off does. Through the std140
        `.d[A >> 4][(A >> 2) & 3]` RADV paid ~4 VALU per load (add, and -16, bfe, lshl_add) that it never folds into
        A & ~3; this way the shift is shared by the loads with one base and the offset goes into the load's immediate
        (pt4)."""
        if off & 3:
            a = f"({base} + {off}u)"
            return f"cb{bank}.d[{a} >> 4][({a} >> 2) & 3u]"
        self.cbufs_w.add(bank)
        return f"cb{bank}w_at(uint(int({base}) >> 2) + {off >> 2}u)"

    def shared_mem(self, i: Insn) -> str:
        """LDS/STS: byte address Ra * stride (X1/X4/X8/X16) [+ URb] + offset into shared memory (words); LDS of 8/16
        bits takes them from the word holding them, zero- or sign-extended."""
        sz = i.sym("sz")
        sub = {"U8": (8, False), "S8": (8, True), "U16": (16, False), "S16": (16, True)}.get(sz)
        words = {"32": 1, "64": 2, "128": 4}.get(sz, 1 if sub and i.op == "LDS" else None)
        if words is None:
            raise Unsupported(f"{i.op}.{sz}")
        self.uses_shared = True
        stride = {"X1": 1, "X4": 4, "X8": 8, "X16": 16, "": 1}.get(i.sym("stride"), 1)
        addr = f"({self.src_u(i, 'a')} * {stride}u + {i.sv('Ra_offset') & 0xFFFFFFFF:#x}u"
        if i.has("Ra_URb"):
            addr += f" + {self.ureg(i.v('Ra_URb'))}"
        addr += ")"
        out = []
        if sub:
            n = i.v("Rd")
            if n == RZ:
                return ""
            self.regs.add(n)
            w, sh, (bits, signed) = f"SMEM({addr} >> 2)", f"int(({addr} & 3u) * 8u)", sub
            v = f"uint(bitfieldExtract(int({w}), {sh}, {bits}))" if signed else f"bitfieldExtract({w}, {sh}, {bits})"
            return f"r{n} = {v};"
        for k in range(words):
            w = f"SMEM(({addr} >> 2) + {k}u)"
            if i.op == "LDS":
                n = i.v("Rd")
                if n != RZ:
                    self.regs.add(n + k)
                    out.append((n + k, w))
            else:
                n = i.v("Rb")
                if n != RZ:
                    self.regs.add(n + k)
                out.append(f"{w} = {reg(n + k if n != RZ else RZ)};")
        return multi_load(out) if i.op == "LDS" else " ".join(out)

    def local_mem(self, i: Insn) -> str:
        """LDL/STL: thread-private memory (register spills, indexed arrays) as a uint array of the size the SPH gives
        (word 1, low 24 bits: 0x10-0x200 bytes in the 22 programs that use it, 31/10/2026, each at least the furthest
        constant access). Byte address Ra + offset; a register index is clamped to the array."""
        words = {"32": 1, "64": 2, "128": 4}.get(i.sym("sz"))
        if words is None:
            raise Unsupported(f"{i.op}.{i.sym('sz')}")
        off = i.sv("Ra_offset") if "sImm" in i.cls else i.v("Ra_offset")
        self.lmem_bytes = max(self.lmem_bytes, off + 4 * words, 4)
        ra, out = i.v("Ra"), []
        for k in range(words):
            if ra == RZ:
                w = f"lmem[{off // 4 + k}]"
            else:
                w = f"lmem[min((({self.src_u(i, 'a')} + {off & 0xFFFFFFFF:#x}u) >> 2) + {k}u, LMEM_WORDS - 1u)]"
            if i.op == "LDL":
                n = i.v("Rd")
                if n != RZ:
                    self.regs.add(n + k)
                    out.append((n + k, w))
            else:
                n = i.v("Rb")
                if n != RZ:
                    self.regs.add(n + k)
                out.append(f"{w} = {reg(n + k if n != RZ else RZ)};")
        return multi_load(out) if i.op == "LDL" else " ".join(out)

    # Attribute addresses (Ryujinx AttributeConsts.cs, MIT): 0x70 position, 0x80 + 16 i generic i, 0x6c point size,
    # 0x2c0 clip distances, 0x2f8 instance id, 0x2fc vertex id.
    def attr_word(self, addr: int, out: bool) -> str:
        if 0x70 <= addr < 0x80:
            if not out:
                raise Unsupported("ALD of the position")
            return f"gl_Position[{(addr - 0x70) >> 2}]"
        if 0x80 <= addr < 0x280:
            g, c = (addr - 0x80) // 16, ((addr - 0x80) >> 2) & 3
            (self.out_attrs if out else self.in_attrs).add(g)
            return f"{'out_attr' if out else 'in_attr'}{g}[{c}]"
        if addr == 0x6C and out:
            return "gl_PointSize"
        if addr == 0x5C and out:
            # Not in Maxwell's map (Ryujinx); eden-nx2 meets it as "reserved attribute 23". Hypothesis: Ampere's
            # per-primitive shading rate, which goes with the first game's VRS calls; VRS is off, so the write is dropped.
            return ""
        if 0x2C0 <= addr < 0x2E0 and out:
            self.clip = max(getattr(self, "clip", 0), ((addr - 0x2C0) >> 2) + 1)
            return f"gl_ClipDistance[{(addr - 0x2C0) >> 2}]"
        if addr == 0x2F8 and not out:
            # Maxwell's InstanceId does not include the draw's base instance; Vulkan's InstanceIndex does (eden-nx2
            # emit_spirv_context_get_set.cpp:346-352, an idea: index minus base). The HUD's text VS indexes its glyph
            # records with it and read past them on draws with a base instance.
            return "intBitsToFloat(gl_InstanceIndex - gl_BaseInstance)"
        if addr == 0x2FC and not out:
            return "intBitsToFloat(gl_VertexIndex)"
        raise Unsupported(f"{'AST' if out else 'ALD'} a[{addr:#x}]")

    def attr_mem(self, i: Insn) -> str:
        """ALD/AST with a constant address (Ra = RZ): consecutive words from Rd / Rb."""
        if i.v("Ra") != RZ or (i.op == "ALD" and i.sym("io") == "O"):
            raise Unsupported(f"{i.op} indexed or of outputs")
        n = {"32": 1, "64": 2, "96": 3, "128": 4}.get(i.sym("sz"))
        if n is None:
            raise Unsupported(f"{i.op}.{i.sym('sz')}")
        base, out = i.v("Ra_offset"), i.op == "AST"
        stmts = []
        for k in range(n):
            target = self.attr_word(base + 4 * k, out)
            if out and not target:
                continue
            if out:
                r = i.v("Rb") + k if i.v("Rb") != RZ else RZ
                if r != RZ:
                    self.regs.add(r)
                stmts.append(f"{target} = uintBitsToFloat({reg(r)});")
            else:
                d = i.v("Rd") + k
                if i.v("Rd") != RZ:
                    self.regs.add(d)
                    stmts.append(f"r{d} = floatBitsToUint({target});")
        return " ".join(stmts)
