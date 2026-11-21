# Orbitals Crash B: control-flow and field-access scan (2026-11-21)

**Scope:** follow-up to `docs/orbitals-crash-b-followup-2026-11-21.md`, based on the complete output pasted from Orbitals Crash Analyzer for `setup-work/obj/main_main_0184.o`. This is static analysis only. No game launch, rebuild, regeneration, recompilation, or runtime code modification was performed.

## Executive summary

The complete scan narrows the immediate failure to a two-level pointer lookup in generated function `f_7001759b20`:

1. The code loads a value from `[rsi+0xd8]` into `rax`.
2. It successfully reads the first qword at that address into `rcx`.
3. At `0x1952f`, it reads `dword ptr [rcx+r8+0x5c]`.
4. The captured host state has `rcx=0` and `r8=0xa8`, producing the failing address `0x104`.

Because execution reached the final instruction, the earlier load `mov rcx,[rax]` did not itself fault. Therefore, the immediate null value is the **first qword stored at the address loaded from `[rsi+0xd8]`**, not necessarily the value of `[rsi+0xd8]` itself.

The branch scan identifies two routes to the fallback block at `0x19507`: an empty collection and a non-empty collection for which the scan finds no matching record. The static scan does not establish which route was taken in the captured run.

## Faulting sequence

Object-relative disassembly:

```asm
0x19507: mov rax, qword ptr [rsi + 0xd8]
0x1950e: mov rcx, qword ptr [rax]
0x19511: mov qword ptr [rsi + 0x58], 0x38
0x19519: mov eax, 2
0x1951e: sub eax, edx
0x19520: imul r8, rax, 0x38
0x19524: lea r9, [rcx + r8]
0x19528: mov qword ptr [rsi + 0xc8], r9
0x1952f: mov ecx, dword ptr [rcx + r8 + 0x5c] ; FAULT
```

For the recorded host registers `RCX=0` and `R8=0xA8`, the effective address is `0 + 0xA8 + 0x5C = 0x104`. This matches the recorded read access violation.

The fact that `0x1950e` completed is significant: `rax` was a readable address at that point, while the qword loaded from it was zero. The owner, type, and initialization contract of the object at `rax` remain unknown.

## Branches into the fallback

The analyzer reported these branches in the crash window:

```asm
0x19435: jmp 0x1987e
0x19453: jg  0x18ed0
0x1945c: je  0x19500
0x19473: je  0x19507
0x19495: jne 0x194a0
0x1949a: jmp 0x18ed0
0x194a4: je  0x19507
0x194c0: jne 0x194a0
0x194c2: jmp 0x18ed0
0x194cb: je  0x197a1
0x194d5: jne 0x197ac
0x194eb: jne 0x194f4
0x194fb: jmp 0x1987e
0x19502: jmp 0x18ed0
0x19537: je  0x18ed0
```

The previously inspected loop reads the collection count at `[rcx+0x470]` and its base at `[rcx+0x468]`. It traverses records with a `0x50` stride and compares a key. The branches to `0x19507` correspond to the zero-count case and the no-match fallback after scanning. Consequently, the fallback does **not** prove that the collection was empty.

## Reaching-definition evidence for [rsi+0xd8]

The complete access list supplied by the analyzer includes the following references in the function's relevant address range:

```asm
0x18f5c: mov qword ptr [rsi + 0xd8], rcx
0x19001: mov rcx, qword ptr [rsi + 0xd8]
0x190f0: mov r8, qword ptr [rsi + 0xd8]
0x1915d: mov rax, qword ptr [rsi + 0xd8]
0x191e0: mov r8, qword ptr [rsi + 0xd8]
0x19200: mov r10, qword ptr [rsi + 0xd8]
0x19507: mov rax, qword ptr [rsi + 0xd8]
```

The earlier disassembly excerpt for `0x18f4a-0x18f5c` shows that the write at `0x18f5c` stores an element address computed from a separate collection:

```asm
0x18f4a: mov  rax, qword ptr [r9 + 0x158]
0x18f51: imul r8, r8, 0xe8
0x18f58: lea  rcx, [rax + r8]
0x18f5c: mov  qword ptr [rsi + 0xd8], rcx
```

No other write to `[rsi+0xd8]` appears in the analyzer's supplied access list between `0x18f5c` and `0x19507`. This makes `0x18f5c` the leading **linear-address candidate** for the value read at `0x19507`.

However, the analyzer explicitly reports static references, not path-sensitive reaching definitions. This candidate must not be promoted to a proven reaching definition until the complete control-flow graph and all paths into `0x19507` are checked. Also, the slot `[rsi+0xd8]` is reused elsewhere in the object for unrelated values (including immediates such as `0xe8`); its meaning is local to the relevant execution path.

## Interpretation of the null pointer

The fault sequence distinguishes two different pointers:

- `rax`: address read from `[rsi+0xd8]`; it must be readable because the subsequent load at `0x1950e` completed.
- `rcx`: first qword read from `[rax]`; it is zero in the captured host fault context and becomes the base of the failing indexed access.

Thus the immediate failure is consistent with a valid outer object whose first qword (or pointer field at offset zero) is null, followed by an unchecked access into a table/array expected to be reachable through that qword. The exact data structure and field semantics are not yet established.

The `+0xA8`, `+0xB8`, `+0xC0`, `+0xC8`, and `+0xE0` scans are broad displacement inventories across the object file. They include different base registers, stack references, and reused context slots. They should not be treated as stable semantic field definitions without local instruction and control-flow context.

## Established vs. unproven

### Established by the supplied dump and static scan

1. The faulting instruction is at object offset `0x1952f` in `f_7001759b20`.
2. The reported host registers `RCX=0` and `R8=0xA8` produce the failing address `0x104`.
3. The preceding load from `[rax]` completed, so the immediate null base is the qword loaded into `rcx`, not necessarily the outer address in `rax`.
4. Two branches lead to the fallback at `0x19507`: zero collection count and scan/no-match fallback.
5. The write at `0x18f5c` is the leading linear-address candidate for the `[rsi+0xd8]` value read at `0x19507`, but path-sensitive proof is still missing.

### Not established

- Which branch was taken in the actual crashing run.
- Whether `0x18f5c` is the true reaching definition on that path.
- The semantic type and owner of the object addressed by `rax`.
- Why its first qword is null: expected empty state, missing initialization, wrong index/argument, layout mismatch, stale state, or prior memory corruption.
- Whether this lookup is the original defect or merely where earlier invalid state becomes fatal.

## Next static-analysis work (no rebuild or game relaunch)

1. Extract the complete contiguous disassembly for `0x18e80-0x19550`, including every instruction and all branch targets, from the existing object.
2. Build a path-sensitive control-flow graph for both predecessors of `0x19507`; determine whether `0x18f5c` executes on each path and identify any other reaching definitions.
3. Trace the value used to compute `rcx` at `0x18f5c`: the source of `r9`, the index in `r8`, the base pointer loaded from `[r9+0x158]`, and the initialization/population of that collection.
4. Trace the first qword at the resulting element address: identify all writes to that field and the expected initialization invariant before the fallback dereference.
5. Compare the empty-collection and no-match paths separately. Do not assume they have identical object state.

Keep the investigation focused on Crash B. Do not change TLS allocation sizes, TPIDR handling, add a speculative null check, or pivot to the separate allocator/GOT/PLT Crash A without new evidence. No fix or root cause is claimed by this static scan.
