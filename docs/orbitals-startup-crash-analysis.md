# Orbitals startup crash: the game's record walk over an unpopulated sub-table

**Status:** root cause established by the 22/11/2026 instrumented crash log (build b7f50da / PR #14).
**Scope:** the sections dated 20-22/11/2026 below keep the earlier draft analysis; the newest section supersedes its conclusions.

## Root cause confirmed (22/11/2026)

The diagnostic build's record dump settled which of the three candidate causes the fault is.

**Not GetTlsValue, not TPIDR.** The `nn::os::GetTlsValue` name in the crash report was the nearest symbol on the faulting page, a red herring (the lifted-symbols note in "What is established" below stands, but its framing was wrong). The faulting code is game code in `f_7001759b20 = main+0x1759b20`.

**What the dump showed.** At the crash, `x27` pointed at a 0xE8-byte record that is alive and well formed, not an unconstructed one:

- `x27 = 0x84014d000`; the back pointer at the record's +0x38 matches `x21 = 0xb40012810`.
- +0x20/+0x28 hold the owner object's corner (`0x119454520`, twice); +0xb8 points at the next record (`0x84014d100`), so the record sits in a live list.
- The record's sub-table pointer at +0x00 is null, and its state field at +0x50 is 0xFFFFFFFF (-1): the record exists but has no sub-table allocated, i.e. a valid-but-empty element.
- The faulting sequence is `x27 = [x21+0x158] + idx*0xE8`; `[x27] = 0`; `x25 = [x27] + (2 - w8) * 0x38` with `w8 = -1`, so `x25 = 0xa8`; `ldr w9, [x25, #0x5c]` reads address **0x104** from a null base.
- The generated code right after the load already tests the loaded word for zero (`testq %rcx,%rcx` and a conditional jump back): only the sub-table pointer itself lacks a guard.

So: not a race of initialization (the record is constructed), not a stale hint (the list entry is current), but a valid-but-empty element read without the guard its state field calls for. The type of fix therefore is a guard on the null sub-table pointer (or the -1 state) in that one lifted site, through the generator's code patch table (`recomp/patches.py`: a switchable `site` patch, checked against the original instruction word), not a generic change in `lifter.py`. The deeper question, whether the runtime should have had that sub-table pointer populated for a freshly created thread (compare the `SetTlsValue` site at `main+0x1ba10`), stays open and should be answered before or alongside the patch.

**One caveat on the 21/11 extension:** the guessed count/capacity header offsets printed garbage (`count=0x400000002 cap=0x600000003`, bitmap 0) on a record known to be valid, so +0x160/+0x168/+0x178 are not the right fields of the owner object. The record dump itself is what settled the diagnosis; the presence-bitmap fields need the real object layout before being reused.

## Draft analysis (20-22/11/2026), historical

The rest of this document keeps the earlier draft: its register evidence, the runtime cross-check and the generated-object disassembly remain valid; its "GetTlsValue" framing is superseded by the section above.

## Summary

Orbitals terminates with guest exception `0xC0000005` (access violation, read) while executing Nintendo SDK's `nn::os::GetTlsValue(nn::os::TlsSlot)`.

- Guest PC: `sdk+0x16b730` (`GetTlsValue`).
- Caller return address: `main+0x175a16c`.
- Guest stack also contains `sdk+0x169630` and `sdk+0x170b78` below the application frames.
- Faulting thread: `0x147345FF650`.
- Guest `TPIDR_EL0` snapshot: `0xa2003c740` (nonzero).
- Guest `x27`: `0x9e02031d0` (nonzero).

The evidence points to a null pointer in the TLS-related lookup structure reached by the generated code for `GetTlsValue`. It does **not** support the simpler diagnosis that the faulting thread's `TPIDR_EL0` register itself was zero.

## Faulting instruction and pointer chain

The host-side exception maps to the generated function `f_7001759b20` in `setup-work/obj/main_main_0184.o`. The object disassembly places the faulting instruction inside that function (object offset `0x1952f`, function start `0x189a0`):

```asm
19507: 48 8b 86 d8 00 00 00  movq 0xd8(%rsi), %rax
1950e: 48 8b 08              movq (%rax), %rcx
19511: 48 c7 46 58 38 00 00 00 movq $0x38,0x58(%rsi)
19519: b8 02 00 00 00        movl $0x2,%eax
1951e: 29 d0                 subl %edx,%eax
19520: 4c 6b c0 38           imulq $0x38,%rax,%r8
19524: 4e 8d 0c 01           leaq (%rcx,%r8),%r9
19528: 4c 89 8e c8 00 00 00  movq %r9,0xc8(%rsi)
1952f: 42 8b 4c 01 5c        movl 0x5c(%rcx,%r8),%ecx
19534: 48 85 c9              testq %rcx,%rcx
```

The first load reads the `Cpu` register slot at offset `0xd8`, which is `Cpu.x[27]`. In the crash snapshot that register is `0x9e02031d0`, so the address used for the next load is nonzero. The next instruction dereferences that address to obtain a second pointer. The following table-entry load faults at effective address `0x104`.

The observed effective address is consistent with a null table base plus the calculated index displacement and `0x5c` field offset. The host exception register context was not independently captured in the notes, so this arithmetic is an interpretation of the disassembly and reported fault address, not a directly logged host-register dump.

## TLS register evidence

The faulting-thread snapshot records `TPIDR_EL0 = 0xa2003c740`. This rules out describing the crash as a direct null-`TPIDR_EL0` dereference.

The separate TPIDR trace previously examined belongs to another thread (`0x14734aa2030`), not the faulting thread. A search of that trace for the faulting thread found no events. Therefore that trace cannot establish the faulting thread's earlier TPIDR history.

In the current runtime implementation, the generated MRS/MSR handling uses `Cpu.tpidr` directly when TPIDR tracing is disabled; the trace helpers record reads/writes but do not synchronize a separate host TLS facility. This is relevant context, but it does not by itself explain why the pointer loaded through `x27` is null.

## What is established vs. still unknown

### Established by the available dump/disassembly

1. The fatal guest exception is a read access violation in `nn::os::GetTlsValue`.
2. The crashing guest thread has a nonzero `TPIDR_EL0` snapshot.
3. The generated code reaches the lookup through nonzero guest `x27`.
4. The next pointer loaded from the structure reached through `x27` is consistent with a null base for the subsequent indexed access at `0x104`.
5. The matching generated object contains this exact instruction sequence within `f_7001759b20`; it is not merely an unrelated nearest-symbol guess.
6. `GetTlsValue` and `SetTlsValue` were not found as explicit HLE/runtime implementations in the source paths searched. The progress metadata classifies `GetTlsValue` as `pure`, and the lifter has no dedicated intrinsic for it.

### Not yet established

- Which allocation or initialization routine is responsible for the null pointer.
- Whether the missing pointer is intended to be initialized by the game, the SDK emulation, thread creation, or another runtime path.
- Whether `x27` points to a TLS table directly or to an intermediate per-thread structure; the exact semantic name of the pointee has not yet been confirmed from source.
- Whether the fault is caused by missing initialization, incorrect layout/offset, or an earlier corruption.

## Next investigation

Trace the producer and lifetime of the pointer chain used by the generated `GetTlsValue` body:

1. Inspect the generated C around guest function `main+0x1759b20` and identify the guest instruction/source operation that populates `x27`.
2. Follow writes to the memory at `0x9e02031d0` (or the corresponding allocation/layout in the guest address space) back to its initializer.
3. Compare the thread setup path with the SDK's expected TLS slot-table layout and initialization order.
4. Check the `SetTlsValue` call site at `main+0x1ba10` against the failing `GetTlsValue` path to see whether the same structure is initialized or bypassed.

Do not change TPIDR handling or add a speculative null check as a fix until the owner and expected layout of this pointer chain are identified.


## Runtime source cross-check (20/11/2026)

The runtime source was checked after the dump analysis:

- `thr_new()` in `runtime/core/kernel/kernel.c` allocates a per-thread TLR block (`0x200 + 0x400` bytes) and a separate TLS block (`TLS_SIZE = 0x2000`). It sets `Cpu.tpidrro` to the TLR and `Cpu.tpidr` to the TLS block, then attaches the `RtThread` to `Cpu.thread`.
- `mem_kernel_alloc()` in `runtime/core/memory/memory.c` zeroes every newly allocated block with `memset(p, 0, size)`. Thus the runtime's TLS allocation is present and starts zeroed; it is not an unallocated or uninitialized host pointer.
- `thread_main()` runs guest entry code with that thread's `Cpu`. The main guest thread also goes through `thr_new()`.
- Fiber switching copies `tpidr` and `tpidrro` from the outgoing Cpu to the target fiber Cpu, and reattaches the current `RtThread`. A newly initialized fiber Cpu is zeroed, then receives these values on its first switch.

This narrows the investigation but does not identify the fault. The crashing thread's nonzero TPIDR snapshot is consistent with the runtime's normal setup. The null pointer observed by the generated lookup is therefore not evidence that the runtime failed to allocate the thread's TLS block. It may instead be an SDK/game-owned pointer or substructure expected to be populated inside/alongside that block, a bad register value, or a layout/ownership mismatch. The current dump does not identify which.

### Updated next step

Instrument the faulting path to capture, for the **same guest thread and at the failing call**:

1. `Cpu.tpidr`, `Cpu.tpidrro`, `Cpu.x[27]`, and the 8-byte value at guest address `Cpu.x[27]`.
2. The memory map/region ownership for `Cpu.x[27]` and the first pointer it contains.
3. The caller's guest PC/LR and the `TlsSlot` argument passed to `GetTlsValue`.

This distinguishes a valid per-thread TLS base from a separate null slot-table pointer, and tells whether the failure comes from a bad argument/register, missing SDK initialization, or a layout mismatch. Do not alter TLS allocation sizes or fiber copying until those values are captured.

## Build note (21/11/2026)

The lifter's TPIDR_EL0 hook (`rt_tpidr_trace_enabled() ? rt_tpidr_read(...)`, every guest TLS register access a call
into the executable) is taken out again: it forced a full game recompile and slowed every TLS access, for a trace that
showed nothing on the faulting thread. `ORBITALS_TRACE_TPIDR` therefore records nothing from lifted code; the crash
report's host context, TLS chain and `GetTlsValue` entry dump stay. Put the two lifter lines back to trace again.


## Phenomenology (23/11/2026)

Player-side launch pattern: logo x3 (no audio) -> loading -> random crash -> menu -> character select ->
loading -> random crash -> cutscene -> gameplay. Four launches out of five run clean; back-to-back relaunches
can crash every time. Both known faulting sites sit in the game-side loading windows.

This random, load-correlated timing keeps a parallel-initialization race on the table, and it is still compatible
with the record dump: the record is fully formed except its sub-table pointer at +0x00, exactly the footprint of a
record another thread is still constructing (pointer stored last) while the walker reads it. The rec-null-guard
site patch covers that read window too: the null pointer becomes the patch page, the word reads 0 and the
function's own cbz skips the element, so a walker racing the constructor soft-lands instead of faulting on 0x104.

To tell the race from a genuinely missing sub-table:

- when it crashes again, capture whether the faulting site is always the same 0x104 / main+0x1759b20 read or the
  crash moves (a moving fault argues for a separate second bug, the GPU device-lost profile of the 20/11 log);
- the per-record state field (the -1 seen at +0x50 in the one crash we have) and a dump of the same record a few
  frames later on a clean run would settle whether +0x00 gets filled in later by another thread.


## 24/11: the follow-up abort is a free of the thread's own stack

The build with rec-null-guard v2 no longer access-violates: the record walk proceeds and the run dies at the
loader/character-select (~15 s, reproduced in the 23/11 and 24/11 logs) in an engine abort,
"FMallocBinned3 Attempt to free an unrecognized pointer %p", from main+0x1f9878 over the same record-walker
chain (main+0x175a16c -> main+0xb9214 -> main+0x181cc8). The stack-hex dump at the abort (svc.c) captures the
value: the 24/11 log frees 0x1183f9f1c while the aborting thread's sp is 0x1183f9e00, so the pointer is
sp+0x11c, an address in the caller's own stack frame. The 23/11 run's x1 (0x115eaef1c) fits the same shape.
So the record entry the walk lands on holds a stack-resident garbage value (uninitialized field) instead of a
heap pointer; the engine's Free then refuses it. Next step: the exact instruction words around
main+0x1f9878 / main+0xb9214 / main+0x181cc8 (recomp/peek_words.py against the local extracted NSO) to state
the patch site, same process as rec-null-guard.


## 24/11 follow-up: exact source of the bad Free argument

The local NSO `main(1)` supplied with the 24/11 log was decompressed with the repository's `recomp/nso.py`; the .text segment is 0x70c5b20 bytes. The following instruction words were read directly from that NSO (not inferred from a different build):

```
main+0x1757e0  f9408a79   ldr x25, [x19, #0x110]
main+0x1757ec  f90043f9   str x25, [sp, #0x80]
main+0x175800  f9008a7f   str xzr, [x19, #0x110]
...
main+0x175a160 f94043e0   ldr x0, [sp, #0x80]
main+0x175a164 b4ffe1c0   cbz x0, main+0x1759d? (skip null)
main+0x175a168 97a593b6   bl main+0xbf040
main+0x175a16c 17ffff0c   b main+0x1759...
```

This establishes the local dataflow in the cleanup routine: it reads the object field `[x19+0x110]` into x25, saves that value in the stack local `[sp+0x80]`, clears the object field, and later reloads the saved value into x0 for `FMemory::Free`. The 24/11 fatal log's `%p` value is `0x1183f9f1c`, which is inside the aborting thread's stack (`SP=0x1183f9e00`, therefore `SP+0x11c`). The bad value therefore reaches Free through this field/local path.

The relevant return/call words in the fatal-reporting chain are also confirmed in the same NSO:

```
main+0x1f9870 f9400908
main+0x1f9874 d63f0100
main+0x1f9878 aa1503e0
main+0x1f987c 97f869d1   bl main+0x13fc0
main+0xb9210  9403227a   bl main+0x181bf8
main+0xb9214  aa1f03e8
main+0x181cc4 9401deaf   bl main+0x1f9780
main+0x181cc8 a9514ffc
```

The stack frames at `main+0x1f9878`, `main+0x181cc8`, and `main+0xb9214` are on the allocator's fatal-reporting/logging path. They are not the correct place to patch the pointer. The actual Free call is the direct `main+0x175a168 -> main+0xbf040` call shown above.

**Still open:** which producer wrote the stack address into the object field `[x19+0x110]`, and whether the field is meant to hold an owned heap allocation or a borrowed/inline temporary. Do not add a blind null check or suppress Free yet. The next step is to trace writes/initialization of this field for the same object type and identify the task/queue lifetime that placed a stack address there. No Crash A patch is confirmed at this point.


## Instruction-level follow-up (24/11/2026, extracted NSO supplied in chat)

The supplied `main` NSO was decompressed with the repository's `recomp/nso.py` logic (text size `0x70c5b20`). The instruction words below were read directly from its decompressed .text; they are not inferred from host addresses.

### Exact free path

```
main+0x1759e38  f85f0109   LDUR X9, [X8, #-16]
main+0x1759e3c  b5000089   CBNZ X9, +0x10
main+0x1759e40  3cde0100   (128-bit load from [X8, #-32])
main+0x1759e44  f9401fea   LDR X10, [SP, #0x38]
main+0x1759e48  3d800140   (128-bit store through X10)
main+0x1759e4c  f90043e9   STR X9, [SP, #0x80]
main+0x1759e50  f81f011f   STUR XZR, [X8, #-16]
...
main+0x175a160  f94043e0   LDR X0, [SP, #0x80]
main+0x175a164  b4ffe1c0   CBZ X0, loop-back
main+0x175a168  97a593b6   BL main+0xbf040 (FMemory::Free wrapper)
main+0x175a16c  17ffff0c   branch back to loop
```

This corrects the earlier loose description: the local slot is exactly `[SP,#0x80]` (not +0x88). It is populated by copying X9, which was loaded from the field at `[X8-0x10]`; that source field is then cleared. The later loop loads that saved value and passes it to FMemory::Free. Therefore the allocator is not manufacturing the stack address: it is receiving a value already present in the object/record field `[X8-0x10]`.

### Return-address chain cross-check

```
main+0x1f9874  d63f0100   BLR X8
main+0x1f9878  aa1503e0   MOV X0, X21
main+0x1f987c  97f869d1   BL main+0x13fc0
main+0x181cc4  9401deaf   BL main+0x1f9780
main+0xb9210   9403227a   BL main+0x181bf8
main+0x175a168 97a593b6   BL main+0xbf040
```

The log's return address main+0x1f9878 is consistent with the indirect call at main+0x1f9874. The other logged return addresses likewise sit immediately after the calls shown above.

### What is established / next step

- The bad value is copied from the object field `[X8-0x10]` into the temporary cleanup slot `[SP+0x80]`, then passed to Free.
- The log establishes that the value passed to Free is within the aborting thread's stack (24/11: `0x1183f9f1c = 0x1183f9e00 + 0x11c`). This is not a valid heap allocation.
- The exact writer and semantic type of `[X8-0x10]` are not yet identified. This is now the primary target: find the producer/store to that field and determine why a stack address is being retained there.
- Do not patch the allocator or blindly null the field yet. A safe fix requires identifying the field's owner and expected lifetime, then guarding/fixing the producer at a verified instruction site.



### Correction from the exact uploaded log (20261124-002310)

The stack values for this specific uploaded file are:
- fatal SP = `0x1183f9e00`
- pointer in x1 = `0x1183f9f1c`
- therefore `x1 = SP + 0x11c`.

The previously documented `0x11e2b4...` pair referred to a different capture/was not the address pair in this uploaded log. The relation (pointer inside the current thread's stack) remains the same.

The NSO instruction trace further narrows the dataflow: `main+0x1759e38` loads X9 from `[X8-0x10]`; `main+0x1759e4c` saves X9 into the local cleanup slot `[SP+0x80]`; `main+0x1759e50` clears the source field; `main+0x175a160` reloads the saved value and `main+0x175a168` calls `FMemory::Free`. The producer that originally wrote `[X8-0x10]` is still unknown.


## Record-layout cross-check (24/11/2026)

The instruction-level decode identifies the same 0xE8 record array as the earlier 0x104 failure, not an unrelated allocator path:

- The record base is loaded from `[X21,#344]` (`main+0x1759de4`), and the record address is computed with stride `0xE8` (`main+0x1759df0`). This matches the earlier Crash B calculation `[X21+0x158] + index*0xE8`.
- Within that record, `[X27,#208]` is used as the backing pointer for a nested array, with inline fallback `X27+16`; `[X27,#216]` is its signed count. Its elements have stride `0x40`.
- For an element, `X8 = array_base + index*0x40`; the value at `[X8-0x10]` is therefore the element's field at offset `+0x30`.
- The cleanup path moves that field into `[SP,#0x80]`, clears the original field, and later frees the saved value. The same function also frees the final element's `+0x30` field at `main+0x1759e74 -> main+0x1759e7c`.
- This ties Crash A to the same 0xE8 record family involved in Crash B. It does not prove that the null field at record +0x00 and the invalid pointer at nested-element +0x30 have the same writer or exact semantic owner.

**Current root-cause boundary:** the nested element's +0x30 field contains a thread-stack address when cleanup reaches it. The cleanup code copies and frees that value; it does not create the address. The writer/constructor that first puts a stack address in this field remains to be found. Do not call it simply an uninitialized field until that producer is identified.



## 24/11/2026 follow-up: indirect-call descriptor traced in supplied main(3)

The supplied `main(3)` NSO (80,716,406 bytes; decompressed .text size `0x70c5b20`) was decompressed and the following instructions were read directly from its .text.

### Caller-side sequence

At `main+0x1d76120` the caller prepares a descriptor at its current stack pointer and calls `main+0x15e7420`:

```asm
1d76124  mov x0, sp
1d76128  mov w3, #7
1d7612c  mov x23, sp
1d76130  mov x1, x19
1d76134  bl  main+0x15e7420
...
1d761b4  ldr x23, [x8, #0x30]
...
1d761d0  blr x8
1d761d4  mov x1, sp
1d761d8  sub x2, x29, #0x90
1d761dc  blr x23
```

The callee `main+0x15e7420` uses its incoming `x0` as `x19`, so the descriptor base is the caller's stack pointer saved in `x23`.

### Descriptor layout: important correction

The constructor initializes the first 16 bytes of this descriptor to zero:

```asm
15e74bc  add x22, x19, #0x18
15e74c4  stp xzr, xzr, [x19]
```

Later, it writes a 32-bit value to offset +8, not an 8-byte pointer:

```asm
15e7c98  add w8, w0, #1
15e7ca0  str w8, [x19, #0x8]
```

Thus the second qword loaded by the wrapper below consists of that 32-bit value with the upper half initially zero. It must **not** be described as a proven pointer to a stack object.

### Wrapper sequence

The function at `main+0x1d77d40` does:

```asm
1d77d48  mov x8, x0
1d77d4c  ldp x0, x2, [x1]
1d77d54  mov x1, x8
1d77d58  ldr x9, [x0]
1d77d5c  ldr x9, [x9, #0x48]
1d77d60  blr x9
1d77d64  mov w0, #1
```

The wrapper loads its inner-call `x0` and `x2` from the descriptor's first and second qwords. The caller's stack trace includes return addresses `main+0x1d77d64` and `main+0x1d761e0`, consistent with these call sites.

### Unresolved mismatch — do not infer the target yet

The fatal log also contains `main+0x1718cf8`, and the earlier analysis associated that frame with a call to `main+0x1750480` through `main+0x1718240`. However, the descriptor's +8 field shown above is populated by a 32-bit count-like value, whereas the earlier producer hypothesis treated the corresponding `x2` as a pointer used by `main+0x1750480`. Those claims cannot both be accepted without resolving the actual target of the indirect call at `main+0x1d77d60` and the exact stack-frame interpretation. The DATA reference previously noted at `0x111708` alone does not prove that it is this object's active vtable.

**Current safe conclusion:** the stack descriptor and indirect-call wrapper are now instruction-level identified, but the target/argument mapping to `main+0x1718240` is not yet proven. Consequently, the candidate writer `main+0x17518ec` remains a hypothesis, not a confirmed producer of the invalid free pointer `0x1183f9f1c`. Do not patch this path or the allocator until this mismatch is resolved.

### Next verification

1. Resolve the runtime target stored at the object vtable slot `+0x48` used at `main+0x1d77d5c`.
2. Confirm whether that target is actually `main+0x1718240`; if not, identify the correct method and its argument semantics.
3. Trace the exact value loaded from descriptor +8 and compare it with the register values expected by the target.
4. Only then reconnect this indirect-call path to `main+0x1750480`, `main+0x17518ec`, and the invalid-free cleanup at `main+0x175a168`.



## 24/11/2026: resolve the wrapper's virtual target from the actual DATA slot

The decompressed DATA segment of the supplied `main(3)` was inspected (DATA guest base `0x7f2c000`). This resolves the earlier slot-offset ambiguity:

- The pointer table begins at DATA offset `0x1116c0` (guest address `0x803d6c0`), not `0x111690`.
- Its entry at table offset `+0x48` is stored at DATA offset `0x111708` (guest address `0x803d708`) and contains `0x1718240`.
- This is exactly the slot read by the wrapper at `main+0x1d77d5c` (`ldr x9,[x9,#0x48]`). Therefore, **if the object's vptr is `0x803d6c0`, the wrapper's inner virtual call targets `main+0x1718240`**. The fatal stack's return address `main+0x1d77d64` is consistent with the call at `main+0x1d77d60`.

The full caller sequence is:

```asm
1d761b4  ldr x23, [x8, #0x30]     ; outer callback/wrapper
1d761d0  blr x8                   ; preceding virtual call
1d761d4  mov x1, sp
1d761d8  sub x2, x29, #0x90
1d761dc  blr x23                  ; enters wrapper (return 1d761e0)
...
1d77d4c  ldp x0, x2, [x1]
1d77d54  mov x1, x8
1d77d58  ldr x9, [x0]
1d77d5c  ldr x9, [x9, #0x48]
1d77d60  blr x9                   ; inner virtual call (return 1d77d64)
```

### Argument-layout inconsistency still needs resolution

The wrapper loads the inner call's `x0` and `x2` from the first two qwords at the stack descriptor passed in `x1`. The constructor called at `main+0x1d76134` receives `x0=sp`; in `main+0x15e7420`, `x19=x0`, the descriptor's first 16 bytes are zeroed, and the later instruction `str w8,[x19,#8]` writes a 32-bit count-like value to its second qword. This appears inconsistent with `main+0x1718240`, which saves incoming `x2` in `x26` and later passes it to `main+0x1750480`, whose entry dereferences `[x2,#0x38]`.

Therefore the virtual target is now statically identified, but the actual runtime contents of the descriptor at the moment of the wrapper call are not. A preceding call or descriptor mutation may explain the apparent mismatch; it is not yet proven. Do not describe `[sp+8]` as a pointer, and do not claim the `x22+0x7c` writer is the producer of the bad free until the descriptor value and argument mapping are reconciled.

### Immediate next checks

1. Disassemble the full body and all stores affecting the descriptor between `main+0x15e7420` and `main+0x1d761dc`, including side effects of the call at `main+0x1d761d0`.
2. Determine the runtime vptr and first two qwords at the wrapper entry from the crash log or existing instrumentation.
3. Reconcile the observed `x2` with the pointer dereferenced at `main+0x1750480`; only then confirm or reject the proposed invalid-free producer chain.



## 24/11/2026 follow-up: preceding virtual call does not receive the descriptor directly

The caller around `main+0x1d761d0` was decoded one level further. The important distinction is between the virtual call at `1d761d0` and the wrapper call at `1d761dc`:

```asm
1d761b4  ldr x8, [x21, #8]
1d761b8  ldr x23, [x8, #0x30]    ; wrapper saved for the next call
1d761bc  add x9, x8, #0x40
1d761c0  ldr x8, [x8, #0x58]
1d761c4  cmp x8, #0
1d761c8  csel x0, x9, x8, eq    ; select subobject / optional pointer
1d761cc  ldr x8, [x0]            ; selected object's vptr
1d761d0  ldr x8, [x8, #8]        ; virtual slot +8
1d761d4  blr x8                  ; preceding virtual method
1d761d8  mov x1, sp              ; descriptor passed only after this returns
1d761dc  sub x2, x29, #0x90
1d761e0  blr x23                 ; descriptor wrapper
```

This corrects the previous shorthand that placed `blr x8` at `1d761d0`: the actual slot load is at `1d761d0`, and the call is at `1d761d4`; the wrapper call is at `1d761e0`.

**New conclusion:** the preceding virtual method does not receive the stack descriptor directly in either `x1` or `x2` from this caller sequence. Those registers are explicitly prepared for the wrapper only after the virtual method returns. The method's direct receiver (`x0`) is selected from the object rooted at `[x21+8]`: either its `+0x40` subobject or the pointer stored at `+0x58`, depending on whether that pointer is null. Its target is the function pointer at offset `+8` in that selected object's vtable.

It could still mutate the descriptor indirectly through a pointer reachable from its receiver or global state, but the current instructions do not show such an alias. Therefore it is **not** a direct descriptor writer on the evidence available. This makes it less likely to explain the count/pointer mismatch by a simple argument overwrite.

**Still missing to identify the method:** the runtime value of the selected receiver's vptr (and thus the actual target loaded from vtable slot +8), plus the receiver's relevant fields. The static DATA table at `0x803d6c0` is not enough to prove this preceding call uses that table.

### Targeted vcall probe added

The main NSO confirms the instruction words at this site:
- `main+0x1d761c8`: `ldr x8, [x0]` (`0xf9400008`)
- `main+0x1d761cc`: `ldr x8, [x8, #8]` (`0xf9400508`)
- `main+0x1d761d0`: `blr x8` (`0xd63f0100`)

A targeted, switchable patch is now defined in `recomp/patches-v1.txt` under `ORBITALS_TRACE_VCALL`. At main+0x1d761cc it replaces the vtable-target load with a load into x16 (preserving the original target), then loads a registered runtime probe from patch-page slot +0x100 into x8. The original BLR at main+0x1d761d0 remains unchanged and calls the probe. The probe logs the receiver, vptr, vtable slot+8 target, x1/x2, x21/x23, SP/LR, the object rooted at [x21+8], and that object's +0x30/+0x58 fields. It then restores x8 and invokes the original virtual target through `rt_call`. The first 32 invocations are logged to avoid flooding.

The runtime side is in `runtime/core/boot/patches.c`; it registers the probe before the dispatch table is sealed and writes its synthetic host-function address into the committed patch page. The patch is enabled by default, and can be disabled with `ORBITALS_TRACE_VCALL=0`.

**Build impact:** this patch changes the generated guest-code patch table, so it must be included in the next code-generation/build pass once. It does not alter the NSO or require a full setup/ROMFS preparation cycle. Subsequent runs use the same built artifact; no regeneration is needed per run.

### Required runtime capture at the call site

At `main+0x1d761d4` (immediately before `blr x8`), capture guest registers `x0`, `x1`, `x2`, `x8`, `x21`, `x23`, `sp`, and the qwords at:
- `[x0]` (receiver vptr),
- `[[x0]+8]` (actual virtual target),
- `[sp]` and `[sp+8]` (descriptor values before the wrapper),
- `[x21+8]`, `[x21+8]+0x30`, `[x21+8]+0x58`.

This single snapshot identifies the method, tests whether its receiver can alias the descriptor, and shows whether the descriptor changes between the constructor return and wrapper entry. No rebuild or speculative patch is justified yet.
