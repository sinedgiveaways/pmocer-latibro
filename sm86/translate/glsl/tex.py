"""SM86 -> GLSL, glsl.core.Builder methods for textures and images. A mixin: it uses the Builder's operand helpers (src_u,
dst...) and state."""

from typing import ClassVar

from program import Insn

from glsl.common import PT, RZ, Unsupported, reg


class TexOps:
    TEX_VARIANT = 16   # texture key word index: bits above it number the sampler pairings of one word (tex_key)

    def tex_key(self, i: Insn, hreg: int | None = None) -> tuple[int, int]:
        """(bank, word index) of the texture or image handle: c[bank5][tsPtrIdx * 4] in the bound forms. The forms with
        the handle in a uniform register (classes ending _urc_, Unreal's programs in Orbitals: 56 of them failed with a
        KeyError on bank5, 17/11/2026) take it from a ULDC of a constant word (mem.py uldc, ur_const), which names the
        same binding: the handle that word holds. Another source is not traced."""
        if i.has("bank5"):
            return i.v("bank5"), i.v("tsPtrIdx")
        for n in ("URc", "URb"):
            if i.has(n) and i.v(n) in self.ur_const:
                bank, byte = self.ur_const[i.v(n)]
                return bank, byte // 4
            if i.has(n) and i.v(n) in self.ur_or:
                return self.or_key(i, self.ur_or[i.v(n)])
            # written by a guarded ULOP3 (@UPg): taken when this TEX runs only where UPg held, that is UPg not
            # negated and either made by VOTEU.ANY from the predicate this TEX is guarded by, or copied into that
            # predicate by a PLOP3 (mem.py track_ssbo)
            if i.has(n) and i.v(n) in self.ur_or_g:
                pair, (up, up_not) = self.ur_or_g[i.v(n)]
                g = self.pred_of(i)
                implied = g is not None and (self.up_any.get(up) == g or (not g[1] and self.p_is_up.get(g[0]) == up))
                if not up_not and implied:
                    return self.or_key(i, pair)
        # tex_scr_b_noConst_: the handle in a general register, written under the guard this instruction runs under
        # (or unguarded): another guard may have left another texture's handle there (mem.py track_handle)
        g = self.pred_of(i)
        h = hreg if hreg is not None else i.v("Rb") if i.has("Rb") else None   # the bindless forms: tex() says where
        if h is not None and h in self.r_const and self.r_const[h][1] in (None, g):
            bank, byte = self.r_const[h][0]
            return bank, byte // 4
        if h is not None and h in self.r_or and self.r_or[h][1] in (None, g):
            return self.or_key(i, self.r_or[h][0])
        raise Unsupported(f"{i.cls}: texture handle in a register not traced to a constant bank")

    def or_key(self, i: Insn, pair: tuple[tuple[int, int], tuple[int, int]]) -> tuple[int, int]:
        """The texture key of a handle OR'ed from two words of one bank: the runtime reads both and ORs them (reflection
        "handle2"). One texture word OR'ed with several sampler words (a texture sampled two ways: 8 Orbitals programs,
        17/11/2026) gets a binding per pair, told apart by a variant number above bit TEX_VARIANT of the word index; the
        reflection gives the word itself (head.py)."""
        (b1, y1), (b2, y2) = pair
        if b1 != b2:
            raise Unsupported(f"{i.cls}: texture handle OR'ed from banks {b1} and {b2}")
        for k in range(64):
            key = (b1, y1 // 4 | k << self.TEX_VARIANT)
            if self.tex_h2.setdefault(key, y2 // 4) == y2 // 4:
                return key
        raise Unsupported(f"{i.cls}: texture word {(b1, y1 // 4)} OR'ed with more than 64 words")

    def set_tex(self, key: tuple[int, int], sampler: str, keep: bool = False) -> None:
        """The sampler type of a handle a sampling instruction reads; keep: TLD and TMML, which keep the type a TEX or
        TXD gave it (a shadow sampler), but not the guess of a TXQ (txq_guess)."""
        if keep and key in self.textures and key not in self.txq_guess:
            return
        self.textures[key] = sampler
        self.txq_guess.discard(key)

    def txq(self, i: Insn) -> str:
        """TXQ TEX_HEADER_DIMENSION: width, height, depth/layers, levels of the texture at lod Ra."""
        if i.sym("query") != "TEX_HEADER_DIMENSION":
            raise Unsupported(f"TXQ {i.sym('query')}")
        key = self.tex_key(i)
        # TXQ does not name a dimension: sampler2D is a guess (txq_guess) that a later TEX/TLD/TMML of the handle
        # replaces, and that head.py drops for the image of the same handle (a compute program's bounds check on its
        # RWTexture3D: as a sampler2D its depth read 1 and FMoveWholeIndirectionTextureCS wrote only slice 0 of the
        # volumetric lightmap's indirection, the colony dark, 25/11/2026)
        if key not in self.textures:
            self.textures[key] = "sampler2D"
            self.txq_guess.add(key)
        # the sampler's type is final only after the whole program (a later TEX may make it an array): the size comes
        # from txqsz_<bank>_<idx>, written with the declarations, as an ivec3 padded with 1
        self.txq_tex.add(key)
        size = f"txqsz_{key[0]}_{key[1]}(int({self.src_u(i, 'a')}))"
        comps = [f"uint(sz[{k}])" for k in range(3)] + [f"uint(txqlv_{key[0]}_{key[1]}())"]
        rd, rd2 = i.v("Rd"), i.v("Rd2")
        dests = [rd, rd + 1 if rd != RZ else RZ, rd2, rd2 + 1 if rd2 != RZ else RZ]
        outs, comp, mask = [], 0, i.v("wmsk", 0xF)
        for c in range(4):
            if mask >> c & 1:
                d = dests[comp]
                comp += 1
                if d != RZ:
                    self.regs.add(d)
                    outs.append(f"r{d} = {comps[c]};")
                    if c == 3:
                        self.txq_lv.add(key)
        return f"{{ ivec3 sz = {size}; {' '.join(outs)} }}" if outs else ""

    # Images: bound like textures (handle at c[bank5][tsPtrIdx * 4], named i<bank>_<idx>), declared without a format
    # (shaderStorageImageRead/WriteWithoutFormat) and as float images (hypothesis: an integer image would need its
    # format from the descriptor). Coordinates are integers from Ra, the array layer after the coordinates.
    # 1D_BUFFER: a buffer texture (NVN_TEXTURE_TARGET_BUFFER) as an imageBuffer, bound as a storage texel buffer (the
    # reflection's type 2, vk_pipe_layout.c descriptor_type). Orbitals: 10 programs store to one and 2 load from one
    # (failed.tsv of 17/11/2026, "SUST 1D_BUFFER IGN", "SULD 1D_BUFFER IGN").
    IMG: ClassVar[dict[str, tuple[str, int]]] = {"1D": ("1D", 1), "2D": ("2D", 2), "3D": ("3D", 3),
                                                 "1D_ARRAY": ("1DArray", 2), "2D_ARRAY": ("2DArray", 3),
                                                 "1D_BUFFER": ("Buffer", 1)}  # fmt: skip

    def image(self, i: Insn) -> tuple[str, str, int]:
        dim = i.sym("dim")
        if dim not in self.IMG or i.sym("clamp") not in ("IGN", ""):
            raise Unsupported(f"{i.op} {dim} {i.sym('clamp')}")
        name, n = self.IMG[dim]
        a = i.v("Ra")
        if a != RZ:
            self.regs.update(range(a, a + n))
        coords = [f"int({reg(a + k if a != RZ else RZ)})" for k in range(n)]
        vec = coords[0] if n == 1 else f"ivec{n}({', '.join(coords)})"
        key = self.tex_key(i)
        # Atomics need an integer image with a declared format (r32ui). An image also loaded or stored is declared so
        # too, its loads and stores going through imld_/imst_ (head.py), which take and give the texels' bits as the
        # registers hold them, whichever the final declaration (32 Orbitals programs, 17/11/2026).
        cur = self.images.get(key)
        if cur is not None and cur.removeprefix("u") != f"image{name}":
            raise Unsupported(f"{i.op}: image {key} used as image{name} and as {cur}")
        atomic = i.op in ("SURED", "SUATOM") or (cur or "").startswith("uimage")
        self.images[key] = f"{'uimage' if atomic else 'image'}{name}"
        return f"i{key[0]}_{key[1]}", vec, {"R": 1, "RG": 2, "RGBA": 4}.get(i.sym("rgba"), 0)

    def sust(self, i: Insn) -> str:
        img, vec, n = self.image(i)
        if not n:
            raise Unsupported(f"SUST.{i.sym('rgba')}")
        b = i.v("Rb")
        vals = [reg(b + k if b != RZ else RZ) for k in range(n)] + ["0u"] * (4 - n)
        if b != RZ:
            self.regs.update(range(b, b + n))
        self.img_st.add(img)
        return f"imst_{img}({vec}, uvec4({', '.join(vals)}));"

    IMG_ATOM: ClassVar[dict[str, str]] = {"ADD": "imageAtomicAdd", "MIN": "imageAtomicMin", "MAX": "imageAtomicMax",
                                          "AND": "imageAtomicAnd", "OR": "imageAtomicOr", "XOR": "imageAtomicXor"}  # fmt: skip

    def sured(self, i: Insn) -> str:
        """SURED: an atomic on a 32-bit texel with no result (Ryujinx SuRed); the image as r32ui (hypothesis: the
        texture is a 32-bit integer one; a float one would need its bits)."""
        op = self.IMG_ATOM.get(i.sym("op"))
        if op is None or i.sym("sz") not in ("32", "U32"):
            raise Unsupported(f"{i.op}.{i.sym('op')}.{i.sym('sz')}")
        img, vec, _ = self.image(i)
        b = i.v("Rb")
        if b != RZ:
            self.regs.add(b)
        return f"{op}({img}, {vec}, {reg(b)});"

    def suatom(self, i: Insn) -> str:
        """SUATOM: SURED that also writes the texel's old value to Rd (Ryujinx SuAtom), 32-bit only."""
        if i.v("Pu", PT) != PT:
            raise Unsupported("SUATOM with predicate out")
        call = self.sured(i).removesuffix(";")
        d = i.v("Rd")
        if d == RZ:
            return f"{call};"
        self.regs.add(d)
        return f"r{d} = {call};"

    def suld(self, i: Insn) -> str:
        if i.v("Pu", PT) != PT:
            raise Unsupported("SULD with predicate")
        img, vec, n = self.image(i)
        if not n:
            raise Unsupported(f"SULD.{i.sym('rgba')}")
        d, out = i.v("Rd"), []
        for k in range(n):
            if d != RZ:
                self.regs.add(d + k)
                out.append(f"r{d + k} = iv[{k}];")
        self.img_ld.add(img)
        return f"{{ uvec4 iv = imld_{img}({vec}); {' '.join(out)} }}" if out else ""

    # ---- textures
    DIMS: ClassVar[dict[str, tuple[str, int, bool]]] = {
        "1D": ("1D", 1, False), "2D": ("2D", 2, False), "3D": ("3D", 3, False), "CUBE": ("Cube", 3, False),
        "ARRAY_1D": ("1DArray", 1, True), "ARRAY_2D": ("2DArray", 2, True), "ARRAY_CUBE": ("CubeArray", 3, True),
    }  # fmt: skip

    def tex(self, i: Insn) -> str:
        """TEX on a bound texture (handle at c[bank5][tsPtrIdx * 4], named t<bank>_<idx>). Source registers as Mesa
        NAK's nak_nir_lower_tex.c (MIT) packs them for SM70+: group 0 = [array index (u32)] + coordinates in Ra...,
        group 1 = [lod or bias] + [depth reference] in Rb...; the scalar form (.SCR) puts the whole list (at most 4)
        as 1 -> Ra; 2 -> Ra, Rb; 3 -> Ra, Ra+1, Rb; 4 -> Ra, Ra+1, Rb, Rb+1. For .SCR with a lod NAK has no case; the
        same split is used (hypothesis until checked on a picture). Results: the components set in wmsk, in order, to
        Rd, Rd+1, Rd2, Rd2+1."""
        dim = i.sym("paramA")
        aoffi = i.sym("aoffi") == "AOFFI"
        gather = i.op == "TLD4"
        ptp = gather and i.sym("toff") == "PTP"
        # TLD4.AOFFI: one offset for the whole gather, x and y as 6-bit signed fields at bits 0 and 8 of a source
        # register (Ryujinx's Tld4; TEX packs 4 bits per axis). 2 Orbitals pixel programs, sampled each frame in the
        # level (18/11/2026).
        goff = gather and i.sym("toff") == "AOFFI"
        if dim not in self.DIMS or i.sym("toff") not in ("notoff", "", *(("PTP", "AOFFI") if gather else ())) or (
            (aoffi or ptp or goff) and dim.startswith("CUBE")
        ):
            raise Unsupported(f"{i.op} {dim} {i.sym('aoffi')}{i.sym('toff')}")
        name, ncoord, is_array = self.DIMS[dim]
        lod, dc = i.sym("lodlc"), i.sym("dc") == "DC"
        g0 = (["idx"] if is_array else []) + [f"c{k}" for k in range(ncoord)]
        # the second group: lod or bias, the packed offsets (AOFFI), the depth reference (Ryujinx's TEX order)
        g1 = (["lod"] if lod in ("LL", "LB") else []) + (["off"] if aoffi or goff else []) + (["p0", "p1"] if ptp else [])
        g1 += ["ref"] if dc else []
        # Bindless (classes _b_): the handle is a source, the first of the second group, after the coordinates. Left
        # out, the .SCR forms took a coordinate for the handle (an array's y: 84 Orbitals programs, 17/11/2026) and
        # the next sources from one register early.
        bindless = "_b_" in i.cls
        if bindless:
            g1 = ["h"] + g1
        regs: dict[str, str] = {}
        a, b = i.v("Ra"), i.v("Rb")
        if i.sym("scr") == "SCR":
            seq = g0 + g1
            if len(seq) > 4:
                raise Unsupported(f"TEX.SCR with {len(seq)} sources")
            spots = {1: [a], 2: [a, b], 3: [a, a + 1, b], 4: [a, a + 1, b, b + 1]}[len(seq)]
            regs = dict(zip(seq, spots, strict=True))
        else:
            regs = {n: a + k for k, n in enumerate(g0)} | {n: b + k for k, n in enumerate(g1)}
        for r in regs.values():
            if r != RZ:
                self.regs.add(r)

        def f(n: str) -> str:
            return f"uintBitsToFloat({reg(regs[n])})"

        coords = [f(f"c{k}") for k in range(ncoord)]
        key = self.tex_key(i, regs["h"] if bindless else None)
        t = f"t{key[0]}_{key[1]}"
        if aoffi:  # 4-bit signed texel offsets per axis; GLSL wants them constant, so they move the coordinates
            lvl = f"int({f('lod')})" if lod == "LL" else "0"
            for k in range(ncoord):
                off = f"float(bitfieldExtract(int({reg(regs['off'])}), {4 * k}, 4))"
                coords[k] = f"({coords[k]} + {off} / float(textureSize({t}, {lvl}).{'xyz'[k]}))"
        if is_array:
            coords.append(f"float({reg(regs['idx'])})")
        if dc:
            coords.append(f("ref"))
        vec = coords[0] if len(coords) == 1 else f"vec{len(coords)}({', '.join(coords)})"
        sampler = f"sampler{name}{'Shadow' if dc else ''}"
        self.set_tex(key, sampler)
        if dc and lod == "LL":
            self.uses_shadow_lod = True  # textureLod on array and cube shadow samplers
        if ptp:
            # One offset per texel: 4 signed bytes x, y per texel in two registers (Mesa NAK nak_nir_lower_tex.c:309-316).
            # Texel k is the (i0, j0) texel (.w) of a gather at offset k, as GLSL's textureGatherOffsets defines it;
            # one gather each, since the offsets are not constant here.
            ch = "" if dc else f", {({'R': 0, 'G': 1, 'B': 2, 'A': 3}[i.sym('comp') or 'R'])}"
            args = self.split_ref(coords) if dc else vec
            offs = [
                f"ivec2(bitfieldExtract(int({reg(regs[f'p{k // 2}'])}), {16 * (k % 2)}, 8), "
                f"bitfieldExtract(int({reg(regs[f'p{k // 2}'])}), {16 * (k % 2) + 8}, 8))"
                for k in range(4)
            ]
            call = f"vec4({', '.join(f'textureGatherOffset({t}, {args}, {o}{ch}).w' for o in offs)})"
        elif gather and goff:
            ch = {"R": 0, "G": 1, "B": 2, "A": 3}[i.sym("comp") or "R"]
            o = (f"ivec2(bitfieldExtract(int({reg(regs['off'])}), 0, 6), "
                 f"bitfieldExtract(int({reg(regs['off'])}), 8, 6))")  # fmt: skip
            call = (f"textureGatherOffset({t}, {vec}, {o}, {ch})" if not dc
                    else f"textureGatherOffset({t}, {self.split_ref(coords)}, {o})")  # fmt: skip
        elif gather:
            ch = {"R": 0, "G": 1, "B": 2, "A": 3}[i.sym("comp") or "R"]
            call = f"textureGather({t}, {vec}, {ch})" if not dc else f"textureGather({t}, {self.split_ref(coords)})"
        elif lod == "LZ":
            call = f"textureLod({t}, {vec}, 0.0)" if not dc else f"textureGrad({t}, {vec}, {self.zero_grad(name)})"
        elif lod == "LL":
            call = f"textureLod({t}, {vec}, {f('lod')})"
        elif lod == "LB":
            call = f"texture({t}, {vec}, {f('lod')})"
        elif lod in ("nolodlc", ""):
            call = f"texture({t}, {vec})"
        else:
            raise Unsupported(f"TEX lod mode {lod}")
        if dc and not gather:
            call = f"vec4({call}, 0.0, 0.0, 1.0)"
        outs = self.tex_outs(i)
        return f"{{ vec4 tv = {call}; {' '.join(outs)} }}" if outs else ""

    def txd(self, i: Insn) -> str:
        """TXD: a sample with explicit derivatives, textureGrad. Sources as Ryujinx's TexD (MIT): Ra..., the array index
        (u32) then the coordinates; Rb..., the derivatives interleaved, d/dx then d/dy of each coordinate (hypothesis
        until checked on a picture). 11 Orbitals programs, 17/11/2026 (txd_urc_)."""
        dim = i.sym("paramA")
        if dim not in ("1D", "2D", "ARRAY_1D", "ARRAY_2D") or i.sym("aoffi") == "AOFFI":
            raise Unsupported(f"TXD {dim} {i.sym('aoffi')}")
        name, ncoord, is_array = self.DIMS[dim]
        a, b = i.v("Ra"), i.v("Rb")
        n0 = ncoord + (1 if is_array else 0)
        self.regs.update(r for r in range(a, a + n0) if a != RZ)
        self.regs.update(r for r in range(b, b + 2 * ncoord) if b != RZ)

        def fr(r: int) -> str:
            return f"uintBitsToFloat({reg(r if r != RZ else RZ)})"

        base = a + (1 if is_array else 0)
        coords = [fr(base + k if a != RZ else RZ) for k in range(ncoord)]
        if is_array:
            coords.append(f"float({reg(a)})")
        dx = [fr(b + 2 * k if b != RZ else RZ) for k in range(ncoord)]
        dy = [fr(b + 2 * k + 1 if b != RZ else RZ) for k in range(ncoord)]

        def vec(v: list[str]) -> str:
            return v[0] if len(v) == 1 else f"vec{len(v)}({', '.join(v)})"

        key = self.tex_key(i)
        t = f"t{key[0]}_{key[1]}"
        self.set_tex(key, f"sampler{name}")
        outs = self.tex_outs(i)
        call = f"textureGrad({t}, {vec(coords)}, {vec(dx)}, {vec(dy)})"
        return f"{{ vec4 tv = {call}; {' '.join(outs)} }}" if outs else ""

    def tex_outs(self, i: Insn) -> list[str]:
        """The components set in wmsk, in order, from tv to Rd, Rd+1, Rd2, Rd2+1. With .F16 (f16rm) two components
        share a register as packed halves (the first in the low half): the menus' text sampled the font's alpha with
        TEX.F16 and read it with HADD2, and a 32-bit float there discarded every glyph (31/10/2026)."""
        rd, rd2 = i.v("Rd"), i.v("Rd2")
        dests = [rd, rd + 1 if rd != RZ else RZ, rd2, rd2 + 1 if rd2 != RZ else RZ]
        comps = [c for c in range(4) if i.v("wmsk", 0xF) >> c & 1]
        if i.v("f16rm", 0):
            # (c0, c1) to Rd, (c2, c3) to Rd2, or Rd+1 without one (eden-nx2 lower_to_maxwell.h:1532-1543): Rd+1 for
            # both dropped the alpha of the button icons next to the title's «Pulsa», read from Rd2 (01/11/2026).
            dests = [rd, rd2 if rd2 != RZ else dests[1]]
            pairs = [comps[k : k + 2] for k in range(0, len(comps), 2)]
            vals = [f"packHalf2x16(vec2(tv[{p[0]}], {f'tv[{p[1]}]' if len(p) > 1 else '0.0'}))" for p in pairs]
        else:
            vals = [f"floatBitsToUint(tv[{c}])" for c in comps]
        outs = []
        for d, v in zip(dests, vals, strict=False):
            if d != RZ:
                self.regs.add(d)
                outs.append(f"r{d} = {v};")
        return outs

    @staticmethod
    def split_ref(coords: list[str]) -> str:
        """textureGather on a shadow sampler takes the coordinates and the reference as separate arguments."""
        p = coords[:-1]
        return f"{p[0] if len(p) == 1 else f'vec{len(p)}({chr(44).join(p)})'}, {coords[-1]}"

    def tld(self, i: Insn) -> str:
        """TLD (texelFetch): integer coordinates and array index, same register packing as TEX; .LZ lod 0, .LL lod
        in group 1. .MS: the sample index last in group 1 (Mesa NAK nak_nir_lower_tex.c:377-378), on a 2D or 2D array
        multisample texture, whose fetch takes the sample instead of a lod. Offsets are not handled."""
        dim = i.sym("paramA")
        ms = i.sym("ms") == "MS"
        if dim not in self.DIMS or i.sym("aoffi") == "AOFFI" or "Cube" in self.DIMS[dim][0] or (ms and "2D" not in dim):
            raise Unsupported(f"TLD {dim} {i.sym('ms')} {i.sym('aoffi')}")
        name, ncoord, is_array = self.DIMS[dim]
        lod = i.sym("lodlc")
        if ms:
            name = name.replace("2D", "2DMS")
            if lod == "LL":
                raise Unsupported("TLD.MS with a lod")
        g0 = (["idx"] if is_array else []) + [f"c{k}" for k in range(ncoord)]
        g1 = (["lod"] if lod == "LL" else []) + (["ms"] if ms else [])
        a, b = i.v("Ra"), i.v("Rb")
        if i.sym("scr") == "SCR":
            seq = g0 + g1
            if len(seq) > 4:
                raise Unsupported("TLD.SCR with more than 4 sources")
            regs = dict(zip(seq, {1: [a], 2: [a, b], 3: [a, a + 1, b], 4: [a, a + 1, b, b + 1]}[len(seq)], strict=True))
        else:
            regs = {n: a + k for k, n in enumerate(g0)} | {n: b + k for k, n in enumerate(g1)}
        self.regs.update(r for r in regs.values() if r != RZ)
        ints = [f"int({reg(regs[f'c{k}'])})" for k in range(ncoord)] + (
            [f"int({reg(regs['idx'])})"] if is_array else []
        )
        vec = ints[0] if len(ints) == 1 else f"ivec{len(ints)}({', '.join(ints)})"
        key = self.tex_key(i)
        self.set_tex(key, f"sampler{name}", keep=True)
        lv = f"int({reg(regs['ms' if ms else 'lod'])})" if ms or lod == "LL" else "0"
        call = f"texelFetch(t{key[0]}_{key[1]}, {vec}, {lv})"
        if name == "1D":
            # The handle may name a buffer texture (NVN_TEXTURE_TARGET_BUFFER): the console's TLD reads either from the
            # descriptor, and Unreal reads its typed buffers so (Orbitals: 32 slots of target 10 sampled through 1D
            # bindings, all zero stand-ins, 17/11/2026). head.py declares a samplerBuffer twin of the binding.
            self.tld1d.add(key)
            call = f"tld1d_{key[0]}_{key[1]}({vec}, {lv})"
        outs = self.tex_outs(i)
        return f"{{ vec4 tv = {call}; {' '.join(outs)} }}" if outs else ""

    def tmml(self, i: Insn) -> str:
        """TMML as Ryujinx InstEmitTexture.cs EmitTmml (MIT): sources [array index] + coordinates from Ra; component
        0 is textureQueryLod().y and 1 is .x (the instruction's order is GLSL's reversed), each * 256 as an integer."""
        dim = i.sym("paramA")
        if dim not in self.DIMS:
            raise Unsupported(f"TMML {dim}")
        name, ncoord, is_array = self.DIMS[dim]
        a = i.v("Ra")
        idx = 1 if is_array else 0
        coords = [f"uintBitsToFloat({reg(a + idx + k)})" for k in range(ncoord)]
        self.regs.update(r for r in range(a, a + idx + ncoord) if r != RZ)
        vec = coords[0] if ncoord == 1 else f"vec{ncoord}({', '.join(coords)})"
        key = self.tex_key(i)
        self.set_tex(key, f"sampler{name}", keep=True)
        t = f"t{key[0]}_{key[1]}"
        rd, rd2 = i.v("Rd"), i.v("Rd2")
        dests = [rd, rd + 1 if rd != RZ else RZ, rd2, rd2 + 1 if rd2 != RZ else RZ]
        outs, comp, mask = [], 0, i.v("wmsk", 0xF)
        for c in range(4):
            if mask >> c & 1:
                d = dests[comp]
                comp += 1
                if d == RZ:
                    continue
                self.regs.add(d)
                outs.append(f"r{d} = {'uint(int(lq[' + str(c ^ 1) + '] * 256.0))' if c < 2 else '0u'};")
        return f"{{ vec2 lq = textureQueryLod({t}, {vec}); {' '.join(outs)} }}" if outs else ""

    @staticmethod
    def zero_grad(name: str) -> str:
        n = {"1D": 1, "2D": 2, "3D": 3, "Cube": 3, "1DArray": 1, "2DArray": 2, "CubeArray": 3}[name]
        z = "0.0" if n == 1 else f"vec{n}(0.0)"
        return f"{z}, {z}"
