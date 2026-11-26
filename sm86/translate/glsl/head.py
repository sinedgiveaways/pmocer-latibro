"""SM86 -> GLSL: the declarations before main() (version, extensions, helper functions, inputs and outputs,
resource bindings and their reflection) and the task/mesh wrappers around the translated body."""

from glsl.common import (HMMA_FN, LDSM_FN, LOP3_FN, MOVM_FN, NMINMAX_FN, PRMT_FN, SHFL_FN, SMEM_PORTABLE_WORDS,
                         SMEM_WRAP_SENTINEL)  # fmt: skip
from glsl.thread import ThreadOps


def interp(b, stage: str, attr: int) -> str:
    """A PS input's interpolation from the SPH input map, as eden-nx2 translate_program.cpp:78-87: constant -> flat,
    screen linear -> noperspective. Declared smooth, the flat inputs that carry integers as float bits (the race
    bushes' material index in in_attr1.w) were interpolated and broke (01/11/2026); a component mix never occurs (census
    of the 7 262 PS: 992 flat, 2 439 linear attributes)."""
    if stage != "ps" or not b.sph:
        return ""
    byte = (b.sph.words[6 + (attr >> 2)] >> ((attr & 3) * 8)) & 0xFF
    modes = {(byte >> (k * 2)) & 3 for k in range(4)} - {0}
    return {1: "flat ", 3: "noperspective "}.get(modes.pop(), "") if len(modes) == 1 else ""

def sampler_type(glsl_type: str) -> int:
    """Ryujinx SamplerType (gfx_shader.c GfxShaderRes.type): 1 1D, 2 buffer, 3 2D, 4 3D, 5 cube | 0x100 array, 0x200
    multisample, 0x400 shadow."""
    base = glsl_type.removeprefix("sampler").removeprefix("u").removeprefix("image")
    if base == "Buffer":
        return 2
    ms = "MS" in base
    code = {"1D": 1, "2D": 3, "3D": 4, "Cube": 5}[base.removesuffix("Shadow").removesuffix("Array").replace("MS", "")]
    return code | (0x100 if "Array" in base else 0) | (0x200 if ms else 0) | (0x400 if base.endswith("Shadow") else 0)


def declarations(b, stage: str, shape: tuple[int, int, int, int] | None) -> list[str]:
    """Lines before main() for Builder b once the body is translated; sets b.reflection."""
    head = [
        "#version 460",
        "#extension GL_ARB_gpu_shader_int64 : require",
        # jumps or loop passes before giving up: a wrong loop must not hang the GPU. Graphics stages run per pixel or
        # vertex, so their cap is lower: 65536 passes in every pixel of a 1280x720 target take the GPU past amdgpu's
        # timeout (run93 reset the GPU, cause not pinned down; 4096 is a safety bound, not a measured need)
        f"#define LOOP_CAP {'0x10000u' if stage == 'cs' else '0x1000u'}",
    ]
    if b.uses_subgroup:
        head += [
            "#extension GL_KHR_shader_subgroup_basic : require",
            "#extension GL_KHR_shader_subgroup_ballot : require",
            "#extension GL_KHR_shader_subgroup_vote : require",
            "#extension GL_KHR_shader_subgroup_shuffle : require",
        ]
        if b.uses_arith:
            head.append("#extension GL_KHR_shader_subgroup_arithmetic : require")  # fmt: skip
    if stage == "cs":
        # Workgroup and shared memory size from the program's control section (compute_shape). They were
        # specialization constants that no back end set: every dispatch ran 1x1x1 groups until 31/10/2026.
        x, y, z, _ = shape or (1, 1, 1, 0)
        head += [f"layout(local_size_x = {x}, local_size_y = {y}, local_size_z = {z}) in;"]
    if b.uses_shared:
        words = (shape[3] + 3) // 4 if shape and shape[3] else 16384
        head += [f"const uint SMEM_WORDS = {words}u;", "shared uint smem[SMEM_WORDS];"]
        if words > SMEM_PORTABLE_WORDS:
            # Over 32 KiB, more than some GPUs have (Adreno): the runtime cuts smem there and sets SMEM_WRAP (a
            # sentinel constant it looks for) to the words kept, so each workgroup's window wraps inside the cut array
            # instead of reaching past it; where the array fits it sets a power of two, a mask after compilation
            # (vk_spirv_patch.c spirv_clamp_shared).
            head += [f"const uint SMEM_WRAP = {SMEM_WRAP_SENTINEL:#x}u;", "#define SMEM(i) smem[(i) % SMEM_WRAP]"]
        else:
            head.append("#define SMEM(i) smem[i]")
        if b.uses_ldsm:
            head.append(LDSM_FN)
    if b.uses_hmma:
        head.append(HMMA_FN)
    if b.uses_movm:
        head.append(MOVM_FN)
    if b.lmem_bytes:
        sph_bytes = b.sph.words[1] & 0xFFFFFF if b.sph else 0
        head += [f"const uint LMEM_WORDS = {(max(b.lmem_bytes, sph_bytes) + 3) // 4}u;", "uint lmem[LMEM_WORDS];"]
    if b.uses_lop3:
        head.append(LOP3_FN)
    if b.uses_shfl:
        head.append(SHFL_FN)
    if b.uses_prmt:
        head.append(PRMT_FN)
    if getattr(b, "uses_fmz", False):
        head.append("float fmzmul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }")
        head.append("float fmzfma(float a, float b, float c) { return (a == 0.0 || b == 0.0) ? c + 0.0 : fma(a, b, c); }")
    if getattr(b, "uses_nminmax", False):
        head.insert(2, "#extension GL_EXT_spirv_intrinsics : require")
        head.append(NMINMAX_FN)
    if b.images:
        head.insert(2, "#extension GL_EXT_shader_image_load_formatted : require")
    if b.uses_shadow_lod:
        head.insert(2, "#extension GL_EXT_texture_shadow_lod : require")
    if getattr(b, "uses_demote", False):
        head.insert(2, "#extension GL_EXT_demote_to_helper_invocation : require")
    if stage == "gs":   # per-vertex inputs, as arrays (glsl/geom.py)
        head += b.gs_head()
        head += [f"layout(location = {a}) in vec4 in_attr{a}[];" for a in sorted(b.in_attrs)]
    else:
        head += [f"layout(location = {a}) {interp(b, stage, a)}in vec4 in_attr{a};" for a in sorted(b.in_attrs)]
    if stage == "ps":
        head += [f"layout(location = {t}) out vec4 out_color{t};" for t in sorted(b.out_attrs)]
    else:
        head += [f"layout(location = {t}) out vec4 out_attr{t};" for t in sorted(b.out_attrs)]
        if getattr(b, "clip", 0):
            head.append(f"out float gl_ClipDistance[{b.clip}];")
        if stage == "vs" and not (b.task or b.mesh):
            # NVN's point size (SetPointSize) for the programs that write none; Vulkan leaves gl_PointSize undefined on
            # point lists (VUID topology-08773). Ryujinx's SupportBuffer.PointSize (MIT); the back end fills it.
            head.append("layout(std140, binding = 0) uniform Support { vec4 pad[87]; float point_size; } support;")
    # Bindings from 1 (0 is the back end's support buffer), set 0, in this order: constant banks, storage buffers,
    # textures, images; the reflection names each the way runtime/gfx/nvn/host/gfx_shader.c add_res() reads it.
    refl: dict = {"stage": "task" if b.task else "mesh" if b.mesh else stage, "cbufs": [], "sbufs": [], "textures": [], "images": []}
    nb = 1
    for n in sorted(b.cbufs):
        head.append(f"layout(std140, binding = {nb}) uniform CB{n} {{ uvec4 d[4096]; }} cb{n};")
        refl["cbufs"].append({"binding": nb, "slot": n})
        nb += 1
        if n in b.cbufs_w:
            # The same bank as words for dynamic addresses (mem.cb_word), through a read-only storage view of its own
            # binding (flags 2: the back end binds the bank's range to it as a storage buffer) and bounded by that
            # range: a word past it reads 0, as the console's constant bank. Until 13/11/2026 this was a second uniform
            # block on the bank's own binding (`scalar`, uint w[16384]), the only aliased binding of the translation,
            # and NVIDIA (616-621, Turing to Blackwell) lost the device with "instruction pointer invalid" on draws of
            # vertex shaders whose only difference from a sound twin was one more such read (issue #22, programs
            # c74a0d4e / 852f48d6); the same pattern faulted in another port of the first game ("four reads of c9 at a computed index").
            head.append(f"layout(std430, binding = {nb}) readonly buffer CB{n}W {{ uint w[]; }} cb{n}w;")
            head.append(f"uint cb{n}w_at(uint i) {{ return i < uint(cb{n}w.w.length()) ? cb{n}w.w[i] : 0u; }}")
            refl["cbufs"].append({"binding": nb, "slot": n, "flags": 2})
            nb += 1
    for o in sorted(b.ssbos):
        head.append(f"layout(std430, binding = {nb}) buffer SB{o:x} {{ uint d[]; }} sb{o:x};")
        # flags 1: the shader stores into it. The replay copies such a range back to guest memory and keeps the CPU's
        # writes out of it until then; every buffer marked so hid the HUD's glyph records (01/11/2026).
        # o: mem.py sb_key, the bank in the bits above 16 (0: the driver's c0 table; 3..: a user uniform block)
        refl["sbufs"].append({"binding": nb, "slot": len(refl["sbufs"]), "cb_slot": o >> 16, "cb_offset": (o & 0xFFFF) // 4,
                              "flags": 1 if o in b.ssbos_written else 0})  # fmt: skip
        nb += 1
    # A handle only TXQ reads that is also an image is the image's: its size comes from imageSize, with no sampler
    # binding (a 3D storage image bound as a sampler2D gave a depth of 1, tex.py txq).
    img_txq = {k for k in b.txq_guess if k in b.images}
    for (bank, idx), t in sorted(b.textures.items()):
        if (bank, idx) in img_txq:
            continue
        head.append(f"layout(binding = {nb}) uniform {t} t{bank}_{idx};")
        refl["textures"].append({"binding": nb, "type": sampler_type(t), "format": "Unknown", "cbuf_slot": bank,
                                 "handle": idx & 0xFFFF, "handle2": b.tex_h2.get((bank, idx), -1), "array": 1,
                                 "separate": False, "flags": 0})  # fmt: skip
        nb += 1
    # A 1D texture that TLD reads may be a buffer texture: its twin, a samplerBuffer on the same handle (reflection
    # type 2, a uniform texel buffer the back end binds as a view of the buffer's memory, vk_exec_bind.c texel_view).
    # For a buffer slot the 1D binding gets the 1x1 stand-in and the twin the buffer; for a 1D texture the twin gets
    # the zero view. A width of at most 1 picks the twin (a real 1D texture one texel wide reads 0: none seen).
    twins = {k for k in b.tld1d if b.textures.get(k) == "sampler1D"}
    for bank, idx in sorted(b.tld1d):
        t = f"t{bank}_{idx}"
        if (bank, idx) not in twins:
            head.append(f"vec4 tld1d_{bank}_{idx}(int x, int l) {{ return texelFetch({t}, x, l); }}")
            continue
        head.append(f"layout(binding = {nb}) uniform samplerBuffer tb{bank}_{idx};")
        refl["textures"].append({"binding": nb, "type": 2, "format": "Unknown", "cbuf_slot": bank, "handle": idx & 0xFFFF,
                                 "handle2": b.tex_h2.get((bank, idx), -1), "array": 1, "separate": False,
                                 "flags": 0})  # fmt: skip
        nb += 1
        head.append(f"vec4 tld1d_{bank}_{idx}(int x, int l) {{ return textureSize({t}, 0) <= 1 ? "
                    f"texelFetch(tb{bank}_{idx}, x) : texelFetch({t}, x, l); }}")
    for (bank, idx), t in sorted(b.images.items()):
        fmt = "r32ui, " if (bank, idx) in b.img_atomic else ""
        head.append(f"layout({fmt}binding = {nb}) uniform {t} i{bank}_{idx};")
        # SULD/SUST as the texels' bits (tex.py image): straight on an r32ui image, through the float views otherwise
        im, u = f"i{bank}_{idx}", t.startswith("uimage")
        pt = {"1D": "int", "Buffer": "int", "2D": "ivec2", "1DArray": "ivec2"}.get(t.removeprefix("u").removeprefix("image"), "ivec3")
        if im in b.img_ld:
            head.append(f"uvec4 imld_{im}({pt} p) {{ return {'imageLoad(' + im + ', p)' if u else 'floatBitsToUint(imageLoad(' + im + ', p))'}; }}")
        if im in b.img_st:
            head.append(f"void imst_{im}({pt} p, uvec4 v) {{ imageStore({im}, p, {'v' if u else 'uintBitsToFloat(v)'}); }}")
        # flags: 4 image store (Ryujinx TextureUsageFlags), 0x10 an integer image without a format (its view: the UINT
        # format of the texture's layout, vk_exec_bind.c texel_view)
        refl["images"].append({"binding": nb, "type": sampler_type(t), "format": "R32Uint" if fmt else "Unknown", "cbuf_slot": bank,
                               "handle": idx & 0xFFFF, "handle2": b.tex_h2.get((bank, idx), -1), "array": 1, "separate": False,
                               "flags": 4 | (0x10 if u and not fmt else 0)})  # fmt: skip
        nb += 1
    for bank, idx in sorted(b.txq_tex):
        if (bank, idx) in img_txq:
            # imageSize has no level: an image view has one (vk_exec_view.c), so level l is level 0 halved l times
            dim = b.images[(bank, idx)].removeprefix("u").removeprefix("image")
            im = f"i{bank}_{idx}"
            v = {"1D": f"ivec3(max(imageSize({im}) >> l, 1), 1, 1)", "Buffer": f"ivec3(imageSize({im}), 1, 1)",
                 "2D": f"ivec3(max(imageSize({im}) >> l, ivec2(1)), 1)", "3D": f"max(imageSize({im}) >> l, ivec3(1))",
                 "1DArray": f"ivec3(max(imageSize({im}).x >> l, 1), imageSize({im}).y, 1)",
                 "2DArray": f"ivec3(max(imageSize({im}).xy >> l, ivec2(1)), imageSize({im}).z)"}[dim]  # fmt: skip
            head.append(f"ivec3 txqsz_{bank}_{idx}(int l) {{ return {v}; }}")
            if (bank, idx) in b.txq_lv:
                head.append(f"int txqlv_{bank}_{idx}() {{ return 1; }}")
            continue
        if (bank, idx) in b.txq_lv:
            head.append(f"int txqlv_{bank}_{idx}() {{ return textureQueryLevels(t{bank}_{idx}); }}")
        n = {"sampler1D": 1, "sampler2D": 2, "sampler3D": 3, "samplerCube": 2, "sampler2DArray": 3,
             "samplerCubeArray": 3, "sampler1DArray": 2}.get(b.textures[(bank, idx)].replace("Shadow", ""), 2)  # fmt: skip
        sz = f"textureSize(t{bank}_{idx}, l)"
        if (bank, idx) in twins:   # a buffer texture's size is its twin's
            sz = f"(textureSize(t{bank}_{idx}, 0) <= 1 ? textureSize(tb{bank}_{idx}) : {sz})"
        v = sz if n == 3 else f"ivec3({sz}, 1)" if n == 2 else f"ivec3({sz}, 1, 1)"
        head.append(f"ivec3 txqsz_{bank}_{idx}(int l) {{ return {v}; }}")
    # Specialization constants of the fixed-address constant words (Builder.const); "spec" lists each k's [bank, byte].
    for (bank, byte), k in b.spec.items():
        head.append(f"layout(constant_id = {2 * k}) const bool SCU{k} = false;")
        head.append(f"layout(constant_id = {2 * k + 1}) const uint SCV{k} = 0u;")
    refl["spec"] = [[bank, byte] for (bank, byte), _ in sorted(b.spec.items(), key=lambda e: e[1])]
    b.reflection = refl
    return head


# Task programs: 32 invocations a workgroup (a warp: the programs use lane masks and ballots), the payload as a uint
# array, the mesh count in shared memory; EmitMeshTasksEXT runs once, after every invocation has finished the program
# (on the console the last lane writes the count and exits).
TASK_MAIN = """void main() {
    if (gl_LocalInvocationIndex == 0u) task_count = 0u;
    barrier();
    sm_main();
    barrier();
    EmitMeshTasksEXT(task_count, 1u, 1u);
}
"""


# Mesh programs: 32 invocations, outputs kept in shared memory while the program runs (MV_WORDS words in the SKEW
# layout, index bytes, the primitive count), then emitted: 32 vertices with the position and generics 0-15, triangles.
# The outputs start at zero: shared memory keeps the previous workgroup's bytes on AMD, and an index or vertex the
# program leaves unwritten drew long triangles across the shadow map (Hookshot ae4fb30, RX 7700S; GPL: the idea only).
MESH_GENERICS = 16
MESH_ZERO = """    for (uint k = gl_LocalInvocationIndex; k < MV_WORDS; k += 32u) mv[k] = 0u;
    for (uint k = gl_LocalInvocationIndex; k < MESH_PRIMS * 3u; k += 32u) mib[k] = 0u;
"""
MESH_MAIN = (
    """void main() {
    if (gl_LocalInvocationIndex == 0u) mprims = 0u;
"""
    + MESH_ZERO
    + """    barrier();
    sm_main();
    barrier();
    uint np = min(mprims, MESH_PRIMS), v = gl_LocalInvocationIndex;
    SetMeshOutputsEXT(32u, np);
    gl_MeshVerticesEXT[v].gl_Position = uintBitsToFloat(uvec4(mv[v], mv[32u + v], mv[64u + v], mv[96u + v]));
"""
    + "".join(f"    out_attr{g}[v] = uintBitsToFloat(uvec4(mv[{(4 + 4 * g) * 32}u + v], mv[{(5 + 4 * g) * 32}u + v], "
              f"mv[{(6 + 4 * g) * 32}u + v], mv[{(7 + 4 * g) * 32}u + v]));\n" for g in range(MESH_GENERICS))
    + """    for (uint p = v; p < np; p += 32u) gl_PrimitiveTriangleIndicesEXT[p] = uvec3(mib[3u * p], mib[3u * p + 1u], mib[3u * p + 2u]);
}
"""
)


def mesh_head(head: list[str]) -> list[str]:
    out = [head[0], "#extension GL_EXT_mesh_shader : require", *head[1:]]
    out += ["layout(local_size_x = 32) in;", "layout(triangles, max_vertices = 32, max_primitives = 64) out;",
            "#define WG_INDEX gl_WorkGroupID.x",
            "const uint MESH_PRIMS = 64u;", f"const uint MV_WORDS = {(4 + 4 * MESH_GENERICS) * 32}u;",
            f"taskPayloadSharedEXT uint tp[{ThreadOps.TASK_WORDS}];", "shared uint mv[MV_WORDS];",
            "shared uint mib[MESH_PRIMS * 3u];", "shared uint mprims;"]  # fmt: skip
    out = [x for x in out if not x.startswith("layout(location") and "gl_ClipDistance" not in x]
    return out + [f"layout(location = {g}) out vec4 out_attr{g}[];" for g in range(MESH_GENERICS)]


def task_head(head: list[str]) -> list[str]:
    out = [head[0], "#extension GL_EXT_mesh_shader : require", *head[1:]]
    out += ["layout(local_size_x = 32) in;", f"taskPayloadSharedEXT uint tp[{ThreadOps.TASK_WORDS}];",
            "shared uint task_count;", "#define WG_INDEX gl_WorkGroupID.x"]  # fmt: skip
    return [x for x in out if not x.startswith("layout(location") and "gl_ClipDistance" not in x]


# Task/mesh as compute, for devices without VK_EXT_mesh_shader (the idea from AnyPS5
# #6528, no code): the same translated body, other wrappers. Buffers by address (push constants): trec the task records
# (TASK_REC words a workgroup: the payload, then task_count, at most MESH_PER_TASK: a ballot count in the first game's 7 task
# programs), mmap the mesh workgroups the tasks asked for, packed as task << 5 | index (runtime/shaders/mesh_compact.comp
# between the two passes, which also writes the indirect dispatch and draw), mvo the vertices (MESH_VWORDS words each:
# position and the MESH_GENERICS generics) and mto the triangles as absolute vertex indices, those past the mesh's
# count written degenerate. The draw is indexed, through MESH_CS_VS.
MESH_PER_TASK = 32
MESH_VWORDS = 4 + 4 * MESH_GENERICS
TASK_REC = 1024 + 1
# The three buffers by GPU address in push constants (bufferDeviceAddress, core in Vulkan 1.2): the programs' own set 0
# stays as it is, and the runtime adds only a push-constant range to these layouts.
CS_BUFFERS = [
    "#extension GL_EXT_buffer_reference : require",
    "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require",
    "layout(buffer_reference, std430, buffer_reference_align = 4) buffer U32s { uint d[]; };",
    "layout(push_constant) uniform MeshPass { U32s trec, mvo, mto, map; uint has_task; } mp;",
    "#define trec mp.trec",
    "#define mvo mp.mvo",
    "#define mto mp.mto",
    "#define mmap mp.map",
]

TASK_MAIN_CS = f"""void main() {{
    if (gl_LocalInvocationIndex == 0u) task_count = 0u;
    barrier();
    sm_main();
    barrier();
    uint base = gl_WorkGroupID.x * {TASK_REC}u;
    for (uint k = gl_LocalInvocationIndex; k < {TASK_REC - 1}u; k += 32u) trec.d[base + k] = tp[k];
    if (gl_LocalInvocationIndex == 0u) trec.d[base + {TASK_REC - 1}u] = min(task_count, {MESH_PER_TASK}u);
}}
"""

MESH_MAIN_CS = (
    f"""void main() {{
    // with a task: the packed workgroup's task and index (runtime/shaders/mesh_compact.comp), launched only as many
    // as the tasks asked for
    uint g = gl_WorkGroupID.x, t = HAS_TASK ? mmap.d[g] >> 5 : 0u, v = gl_LocalInvocationIndex;
    bool live = true;
    if (v == 0u) mprims = 0u;
{MESH_ZERO}    if (HAS_TASK) for (uint k = v; k < {TASK_REC - 1}u; k += 32u) tp[k] = trec.d[t * {TASK_REC}u + k];
    barrier();
    if (live) sm_main();
    barrier();
    uint np = live ? min(mprims, MESH_PRIMS) : 0u, vb = (g * 32u + v) * {MESH_VWORDS}u;
    for (uint k = 0u; k < {MESH_VWORDS}u; k++) mvo.d[vb + k] = mv[k * 32u + v];
    for (uint p = v; p < MESH_PRIMS; p += 32u)
        for (uint k = 0u; k < 3u; k++) mto.d[(g * MESH_PRIMS + p) * 3u + k] = g * 32u + (p < np ? mib[3u * p + k] : 0u);
}}
"""
)

# The vertex shader of every mesh-as-compute draw: the vertex the mesh workgroup wrote, read as MESH_VWORDS / 4
# attributes of one vertex buffer (the vertex buffer is mvo, stride MESH_VWORDS * 4), so the graphics layout needs no
# push constants. A prepared shader like the others, under the fixed key MESH_CS_VS_KEY (runtime gfx_shader.c).
MESH_CS_VS = "\n".join(
    ["#version 460"]
    + [f"layout(location = {a}) in vec4 v{a};" for a in range(MESH_VWORDS // 4)]
    + [f"layout(location = {g}) out vec4 out_attr{g};" for g in range(MESH_GENERICS)]
    + ["void main() {", "    gl_Position = v0;"]
    + [f"    out_attr{g} = v{g + 1};" for g in range(MESH_GENERICS)]
    + ["}", ""]
)
MESH_CS_VS_KEY = "6f72622d6d6573682d63732d76732d31" * 2   # "orb-mesh-cs-vs-1" twice, as hex: not a program key


def mesh_head_cs(head: list[str]) -> list[str]:
    """mesh_head as a compute shader: tp is shared memory filled from the task's record."""
    out = [*head, "layout(local_size_x = 32) in;", "const uint MESH_PRIMS = 64u;",
           f"const uint MV_WORDS = {(4 + 4 * MESH_GENERICS) * 32}u;",
           # the runtime knows whether a task program comes first (the pipeline's pair), the translator does not
           "#define HAS_TASK (mp.has_task != 0u)",
           "#define WG_INDEX (HAS_TASK ? mmap.d[gl_WorkGroupID.x] & 31u : gl_WorkGroupID.x)",
           f"shared uint tp[{ThreadOps.TASK_WORDS}];", "shared uint mv[MV_WORDS];", "shared uint mib[MESH_PRIMS * 3u];",
           "shared uint mprims;", *CS_BUFFERS]  # fmt: skip
    return [x for x in out if not x.startswith("layout(location") and "gl_ClipDistance" not in x]


def task_head_cs(head: list[str]) -> list[str]:
    out = [*head, "layout(local_size_x = 32) in;", f"shared uint tp[{ThreadOps.TASK_WORDS}];", "shared uint task_count;",
           "#define WG_INDEX gl_WorkGroupID.x", *CS_BUFFERS]  # fmt: skip
    return [x for x in out if not x.startswith("layout(location") and "gl_ClipDistance" not in x]
