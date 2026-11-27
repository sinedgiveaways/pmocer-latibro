# sm86/translate/glsl

The SM86 → GLSL 4.6 translator, split by responsibility. Used as `import glsl` (`glsl.translate`,
`glsl.compute_shape`, `glsl.Unsupported`) with `sm86/translate` on `sys.path`.

| File | Contents |
|---|---|
| `__init__.py` | What the package exports (`translate`, `compute_shape`, `Unsupported`) |
| `core.py` | `Builder` (state, operands, dispatch by opcode in `insn`) and `translate()`: branches, structured form (`flow.py`) or a `switch` on the PC, `inline_single_call`, `compute_shape` |
| `alu.py` | Integer, float, predicate and packing instructions (`IADD3`, `IMAD`, `LOP3`, `FSET`, `HADD2`…) |
| `mem.py` | Memory: constant banks (`ULDC`, `LDC`), global as storage buffers (`LDG`/`STG`, atomics, base tracking), shared, local, attributes (`ALD`/`AST`) and `IPA` with its `w` fold |
| `tex.py` | Textures and images (`TEX`, `TLD`, `TLD4`, `TMML`, `TXQ`, `SUST`/`SULD`/`SURED`) |
| `thread.py` | Subgroups (`SHFL`, `VOTE`, `MATCH`, `REDUX`), system values (`S2R`), task/mesh outputs (`ISBEWR`/`ISBERD`) and `EXIT` |
| `geom.py` | Geometry programs (NVN stage 2): per-vertex `ALD` (`gl_in`, `in_attrN[]`), `AST` to the layer and viewport index, `OUT.EMIT`/`CUT`, `ISBERD` and `S2R SR_INVOCATION_INFO` as Ryujinx reads them, the input/output layouts from the SPH (triangles in) |
| `head.py` | Declarations before `main()` (extensions, helper functions, inputs and outputs, bindings and their reflection) and the task/mesh wrappers |
| `subroutines.py` | Subroutines called from several places (CALL/RET): a copy of the body in each call for the structured form, once the return address is proven to be the instruction after the CALL |
| `common.py` | Shared pieces: `Unsupported`, GLSL helper functions, comparison and `LOP3` tables, register and predicate names |
| `shared_fence.py` | Memory-only workgroup fences at each shared-memory load reachable from a store without a fence between them: Ampere's warp-synchronous scans (STS/LDS with no BAR or MEMBAR) keep their order in SPIR-V |

The groups in `alu`, `mem`, `tex`, `thread` and `geom` are mixins of `Builder`: they use its operand helpers (`src_u`, `dst`…)
and its state.
