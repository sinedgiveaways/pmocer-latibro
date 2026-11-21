# Orbitals Crash B follow-up: COFF disassembly audit (2026-11-21)

**Scope:** static analysis of the user's completed scan of `setup-work/obj/main_main_0184.o` (AMD64 COFF object). This is a follow-up to `docs/orbitals-startup-crash-analysis.md`, not a replacement for the earlier runtime evidence. No game run, rebuild, regeneration, or runtime code change was performed for this analysis.

## Executive summary

The new scan strengthens the structural interpretation of two fields in the generated code, but it does not establish the root cause of Crash B.

- `object + 0x468` and `object + 0x470` are used together as a collection base pointer and 32-bit element count in multiple iteration paths. One observed loop walks records with a `0x50`-byte stride.
- `object + 0x158` is used as the base of a separate indexed collection whose observed element stride is `0xE8`.
- The generated function's scratch/context slot `[rsi+0xD8]` is reused for different values in different paths. It must not be treated as a globally stable pointer field without proving the exact reaching definition for the crashing path.
- The crash path still contains a potentially unsafe fallback/dereference sequence. The new scan does not prove why its required value is absent, nor whether the collection state is the original cause or merely the path through which an earlier invalid state is exposed.

## Object and fault mapping

The scan concerns `setup-work/obj/main_main_0184.o`, section `.text`, with the reported section file offset `0x17c`. The generated function symbol is `f_7001759b20`, at section-relative offset `0x189a0`.

The previously recorded host fault maps to section-relative object offset `0x1952f` (`0x189a0 + 0xb8f`). The reported instruction bytes are:

```asm
0x1952f: 42 8b 4c 01 5c    mov ecx, dword ptr [rcx + r8 + 0x5c]
```

With the previously reported host register values `RCX=0` and `R8=0xA8`, the effective address is `0x104`, matching the recorded read access violation. This confirms the immediate null-base-plus-offset arithmetic for that host snapshot; it does not identify the earlier producer of the null base.

## Collection at offsets +0x468 / +0x470

Several independent excerpts treat these offsets as a pointer/count pair.

### Iteration over 0x50-byte records

At `0x18be5-0x18c0b`, the code loads the signed count from `[rcx+0x470]`, tests it, loads the base pointer from `[rcx+0x468]`, and calculates a record position using a `0x50` stride:

```asm
0x18be5: movsxd r8, dword ptr [rcx + 0x470]
0x18bec: test r8, r8
0x18bef: je   0x18c99
0x18bf5: mov  rcx, qword ptr [rcx + 0x468]
...
0x18c03: lea  rdx, [r9 + r9*4]
0x18c07: add  rdx, -0x50
```

Other paths read the same count/base pair, including `0x19469-0x19479`, `0x19eee-0x19f27`, `0x1a0f7`, and `0x4ae79-0x4ae8e`. The last excerpt copies the base and count into an iterator-like context.

**Supported conclusion:** these fields are used as the base and count of a collection in the observed code. The exact semantic type/name of that collection is not established by this scan.

## Separate indexed collection at +0x158

The excerpts at `0x18f4a-0x18f5c` and `0x4c0a3-0x4c0c3` load a base pointer from an object at offset `+0x158`, multiply an index by `0xE8`, and form an element address:

```asm
0x18f4a: mov  rax, qword ptr [r9 + 0x158]
0x18f51: imul r8, r8, 0xe8
0x18f58: lea  rcx, [rax + r8]
0x18f5c: mov  qword ptr [rsi + 0xd8], rcx
```

```asm
0x4c0a3: mov    rdx, qword ptr [rsi + 0xa8]
0x4c0aa: mov    r8, qword ptr [rdx + 0x158]
0x4c0b1: movsxd rdx, dword ptr [rsi + 0xb8]
0x4c0b8: imul   rdx, rdx, 0xe8
0x4c0bf: lea    r9, [rdx + r8]
0x4c0c3: mov    qword ptr [rsi + 0xe0], r9
```

**Supported conclusion:** the observed code indexes a separate `+0x158`-based collection with `0xE8)-byte elements. This should not be conflated with the `+0x468/+0x470` collection, whose observed loop uses `0x50)-byte records.

## Important correction: [rsi+0xD8] is a reused scratch/context slot

The scan contains different assignments to `[rsi+0xd8]`:

- At `0x18f5c`, it receives an element pointer computed from the `+0x158` collection.
- At `0x1491e`, it receives the immediate value `0xE8`, used as a stride/size value in that code path.
- At `0x4708e-0x47095`, another path loads `[rsi+0xd8]` and dereferences it, after checking `[rsi+0xe0]`.

These are not interchangeable contexts. The presence of a pointer load/dereference at `0x4708e` does **not** prove that the same field has the same meaning or that the same guard is missing in the Crash B path. The reaching definition of `[rsi+0xd8]` must be followed within the exact control-flow path that reaches `0x19507-0x1952f`.

This corrects any overly broad reading of the earlier report that described `[rsi+0xd8]` simply as a pointer. It can hold a pointer in a particular path, but the slot is reused and its meaning is path-dependent.

## Crash-path control flow to prioritize

The scan shows the following relevant branch structure around the failing function:

```asm
0x19459: test rcx, rcx
0x1945c: je   0x19500
0x19462: mov  rcx, qword ptr [rsi + 0xa8]
0x19469: movsxd rax, dword ptr [rcx + 0x470]
0x19470: test rax, rax
0x19473: je   0x19507
0x19479: mov  r8, qword ptr [rcx + 0x468]
...
```

The zero-count branch reaches `0x19507`; the faulting instruction is at `0x1952f`. The complete path must be audited for the reaching definition and validity of every pointer and index used between those addresses. In particular, the value loaded into `rcx` for the final indexed access must be traced back to its actual producer, not inferred solely from the slot offset.

The excerpt at `0x47085-0x47095`:

```asm
0x47085: cmp qword ptr [rsi + 0xe0], 0
0x4708c: je  0x470e6
0x4708e: mov rax, qword ptr [rsi + 0xd8]
0x47095: mov rax, qword ptr [rax]
```

is a useful example of a guarded dereference in another path, but it is not evidence by itself that the Crash B path has the same data structure or omitted check.

## What this scan establishes

1. The immediate faulting memory operand at object offset `0x1952f` is consistent with the recorded read at `0x104` when the host base register is null and the index register is `0xA8`.
2. Offsets `+0x468/+0x470` are used as a collection base/count pair; one observed traversal uses a `0x50)-byte record stride.
3. Offset `+0x158` is used as the base of a distinct indexed collection with a `0xE8)-byte stride.
4. The local context slot `[rsi+0xd8]` is reused for values of different kinds in different paths; its meaning must be established by data-flow analysis at the exact crash path.

## What remains unproven

- The semantic identity of either collection (including whether either is a TLS slot table).
- The exact reaching definition of the pointer/base register used by the final access at `0x1952f`.
- Why the relevant pointer is null at runtime: missing initialization, wrong index/argument, stale state, layout mismatch, or earlier corruption.
- Whether this TLS lookup is the original defect or only the point where an earlier invalid state becomes fatal.
- Whether the observed code at `0x4708e` is related to the same object/context as the crash path.

## Next static-analysis steps (no rebuild or game relaunch required)

1. Extract a contiguous disassembly window covering `0x19430-0x19550` from the same object, preserving all instructions and branch targets.
2. Build a def-use trace for the `[rsi+0xd8]` slot and the register that becomes `rcx` at `0x1952f`, restricted to paths reaching the fault.
3. Identify the call sites and callers that initialize the object referenced by `[rsi+0xa8]`; inspect how its `+0x468/+0x470` collection is populated and when its count can be zero.
4. Compare the zero-count fallback at `0x19507` with the nonzero-count iteration path, including all writes to the scratch/context fields before the branch.
5. Keep the investigation focused on Crash B. Do not pivot back to the separate allocator/GOT/PLT Crash A unless new evidence links the two.

No fix is proposed by this static scan. Do not add a speculative null check, change TLS allocation sizes, or alter TPIDR handling until the pointer's owner and initialization path are identified.
