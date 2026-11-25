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

### What to inspect in the next log

The probe runs automatically when the patched build reaches this call. The first 32 calls should produce `vcall probe #N` lines. The decisive fields are:
- `saved_target` and `slot+8`: must match; the name resolved by `ldr_describe` identifies the virtual method.
- `receiver` and `vptr`: identify the dynamic object type/vtable.
- `object`, `+30`, and `+58`: identify the parent callback and optional receiver selection.
- `returned x0`: confirms the original virtual method was invoked and returned through the probe.

The interception preserves the original `blr x8` instruction and its return address. The vtable target is saved in caller-saved `x16`; the probe restores `x8` before invoking the original target. The original descriptor/stack state is not rewritten by the probe.


## 24/11/2026 follow-up: crash after the vcall probe

The log `orbitals(20261124-034253).log` contains the first run with the targeted vcall probe enabled.

### Vcall probe result

The probe logged 32 invocations at `main+0x1d761d0`. Across the captured calls:
- receiver vptr: `0x7007f32450`
- vtable slot `+8` target: `main+0x2d730`
- saved target and slot target agree
- each intercepted call returned normally

This rules out the intercepted virtual call itself as the immediate fault site in this run. It does not prove that the called method cannot mutate unrelated state indirectly.

### Fatal access violation

The run then terminates with:

- exception: `0xc0000005`
- host PC: `orbitalgame+0x1b5a90f1`
- lifted-function context: `f_7001759b20 = main+0x1759b20`
- host instruction bytes: `8b 52 34 89 93 38 ff ff ff 48 8b 96 d8 00 00 00`
- attempted read: `0xff2d8ffd8`
- host `RDX`: `0x0ff2d8ffa4`; the first host instruction reads `[RDX+0x34]`, producing the reported address.

The log's record diagnostic identifies:
- records allocation/base: `0xff2d90000` (allocation logged as `0x24000` bytes)
- `x27 = 0xff2d90000`, described by the logger as `records[0]`
- `[x27] = 0x70026cea0`
- `x21 = 0xb40012810`
- reported count: `3942779978392`
- reported capacity: `18446744073709551615`

The attempted read is exactly `0x28` bytes before the records allocation base. This is a confirmed out-of-allocation read in this run. The unusually large reported count and maximum 64-bit capacity are additional suspicious metadata, but their field layout and validity still need to be checked against the guest structure before treating either as the root cause.

### Correction to the preliminary interpretation

The fatal report gives a lifted-function context, not a guest PC/instruction address. The host instruction bytes alone do not establish that the guest instruction is at `main+0x1759d8c`, nor do they prove the proposed `X25 + X19` producer chain. Those earlier conversational deductions are **not confirmed** and must not be used as patch targets.

### Next diagnostic required

Before adding a guard or changing game data, capture the guest-side instruction and address calculation responsible for the access:
1. identify the guest PC corresponding to the host faulting block;
2. log the relevant guest registers, especially `x21`, `x27`, the record index, and the base/count/capacity values at the access;
3. trace the instruction that computes the address `0xff2d8ffd8` and verify the records structure's actual field offsets;
4. only then patch the producer or bounds calculation.

Do not add a null-only guard: the failing address is non-null and lies before the allocation. The vcall probe already produced its useful 32-call sample; it should not be treated as the crash fix.

**Build impact:** this update documents the new log only. It changes no runtime code, patch table, generated guest code, or build inputs; no rebuild is required for this documentation commit.


## 24/11/2026 follow-up: exact guest instruction and source chain for the 03:42 crash

The uploaded `main(3)` NSO (build ID `d6efcc0a51c1c792`) was decompressed and disassembled directly. This resolves the guest instruction behind the host access violation.

### Exact faulting instruction

At `main+0x1759d8c`, the NSO contains word `0xb8434328`:

```asm
main+0x1759d8c: LDUR W8, [X25, #0x34]
```

The crash log's host instruction begins with `8b 52 34`, which is the corresponding 32-bit load from host register `RDX+0x34`. At the exception, `RDX=0x0ff2d8ffa4`, so the attempted address is `0x0ff2d8ffd8`. This is now an instruction-level match, not an inference from the nearest lifted-function symbol.

### Static producer chain

The same NSO establishes the incoming pointer chain:

```asm
main+0x1750524  LDR X25, [X22, #8]
main+0x175052c  MOV W19, #0x3810
main+0x1750630  ADD X0, X25, X19
main+0x175064c  BL  main+0x17508e0
main+0x1750908  MOV X25, X0
main+0x1750b80  MOV X0, X25
main+0x1750b84  BL  main+0x1759b20
...
main+0x1759d8c  LDUR W8, [X25, #0x34]
```

Therefore, statically, the pointer used by the failing load is derived from `*[X22+8] + 0x3810` in the caller. The runtime value of `X22` and the pointer loaded from its +8 field still need to be captured to decide whether the source field is already wrong or the offset/owner is wrong.

### Important correction to the previous log interpretation

The crash handler's `Cpu.x[]` fields are a snapshot, not a guaranteed live copy of every guest register at an arbitrary host instruction. The generated code keeps guest registers in host-side locals between call/dispatch boundaries.

In particular, `main+0x1759d8c` occurs **before** this function computes its current `X27` record pointer at `main+0x1759df0`. Therefore the reported `X27=0xff2d90000`, record-array base, count, and capacity from the generic crash dump cannot be treated as live values at the faulting load. The earlier statement that the fault address is exactly 0x28 before the current records array is **not established** and is withdrawn.

Likewise, the printed `pc=main+0x151b350` is not the exact faulting guest PC. The generator updates `Cpu.pc` at dispatch/indirect-branch boundaries; direct calls to lifted functions do not publish every intervening instruction address. The exact faulting instruction is instead established by matching the NSO word at `main+0x1759d8c` to the host fault bytes.

### New targeted source probe

A diagnostic patch set `orbitals-record-source-probe` is added at the two existing indirect-call sites `main+0x17505b8` and `main+0x17505e0`, immediately after the original `LDR X25,[X22,#8]`. Each replacement saves the original vtable target in `X16`, loads the registered host probe, and leaves the original `BLR X8` in place. The probe logs:
- live `X22` and the value at `[X22+8]`;
- live `X25`;
- the derived pointer `X25+0x3810`;
- whether the later field address `X25+0x3810+0x34` is readable, and its value if so;
- the original callback target, which it invokes through `rt_call` to preserve behavior.

The probe does not change the pointer calculation or callback arguments. It logs the first 32 calls. These sites are conditional reference-count callbacks, so the probe only emits lines if one of those callbacks is actually reached. It is enabled by default and can be disabled with `ORBITALS_TRACE_RECORD_SOURCE=0`.

**Next step:** build once with this patch, run until the same crash, and inspect the `record-source probe` lines. They will show whether `[X22+8]` is already wrong or whether the fixed `+0x3810` calculation produces the bad pointer. If no probe line appears, neither instrumented callback was taken and the capture point must move to another existing call boundary.

**Build impact:** this adds two generated-code patch sites and one runtime host probe. It requires one code-generation/compile pass for the changed patch table and runtime; it does not require ROMFS extraction or a full game-file preparation cycle. Reuse the existing game setup and cached assets.


## 24/11/2026 follow-up: invalid memcpy arguments traced to the game-side caller

This section records the additional dump/disassembly findings supplied on 24/11. The exact dump and object files are available in the `sinedgiveaways/tool` `analysis-data` release; the observations below should be rechecked against those raw artifacts before treating every register/dataflow interpretation as independently verified.

### Host-side copy path

The reported path is:

```
orbitalgame.dll+0x1B5A939A
  -> wrapper orbitalgame.dll+0x1C11DF00
  -> synthetic import slot 0x700900A008
  -> native helper orbitals-recomp.exe+0x40CE0
  -> thunk orbitals-recomp.exe+0x70C430
  -> ucrtbase.dll memcpy (invalid read)
```

The helper and thunk are transport/dispatch layers. The three arguments are reportedly already invalid when they reach the native helper. This does not support changing import resolution or applying a generic `memcpy` workaround.

### Candidate 0x38-byte index calculation

At `orbitalgame.dll+0x1B5A8FF0`, the disassembly is reported as:

```asm
mov  eax, 2
sub  eax, edx
imul rcx, rax, 0x38
add  rcx, r9
```

This computes a signed relative offset in entries of `0x38` bytes. If the live `EDX` value is 5, the resulting index is -3 (byte offset `-0xA8`). A value of -3 was also observed in object `0x0CE01F2C00`. These are correlated clues only: the runtime value of EDX and the identity/dataflow of that object must still be verified at this exact site.

### Guest caller and import wrapper

The crash stack reportedly contains two occurrences of guest address `0x7001759B20` (`f_7001759b20`) in the same path as descriptor `0x1C92EAB0540`, buffer `0x11C51F7A0`, copy size `0x298`, and wrapper return address `orbitalgame.dll+0x1C11DF58`.

The generated object `setup-work/obj/main_main_0184.o` contains calls to `f_70070c1de0` and `f_70070c1df0`; the stack contains `0x70070C1DE0`. The wrapper definition in `setup-work/obj/main_main_0768.o` uses synthetic import slot `0x700900A008`, identifying this boundary as the `memcpy` import wrapper. The wrapper forwards through the dispatcher; it does not itself explain how the invalid pointer value was created.

This makes `f_7001759b20` and the caller-side preparation of the descriptor/buffer the important investigation target. Do not mistake the import wrapper for the bad-argument producer.

### Established vs. still open

Reported by the new dump/object inspection:
- the host copy reaches `ucrtbase!memcpy` through the synthetic import and native helper;
- the invalid arguments predate the native helper;
- the guest stack links the copy path to `f_7001759b20`, descriptor `0x1C92EAB0540`, buffer `0x11C51F7A0`, and size `0x298`;
- the object `main_main_0768.o` contains the wrapper using the `memcpy` import slot.

Still to prove from the raw artifacts:
- which of destination, source, and size is invalid, and their exact values at the call boundary;
- which descriptor fields feed those arguments;
- whether EDX is actually 5 at `+0x1B5A8FF0`, and whether its -3 index reaches object `0x0CE01F2C00`;
- the first caller-side instruction that produces or propagates the bad value.

### Next steps (read-only; no full prepare/recompile)

1. Disassemble the complete caller window from `orbitalgame.dll+0x1B5A8FF0` through the call at `+0x1B5A939A`, including branches and register definitions.
2. Map the wrapper's argument registers through `orbitalgame.dll+0x1C11DF00`, the import slot, and the generated wrapper in `main_main_0768.o`.
3. Capture destination/source/size at the call boundary and trace each backwards to its last write. Inspect descriptor `0x1C92EAB0540` and buffer `0x11C51F7A0`; do not assume the `0x298` size is wrong.
4. Verify the live EDX value and the -3 / `-0xA8` calculation against the actual object layout and the `0x0CE01F2C00` observation.
5. Patch only the verified caller-side producer or index/bounds calculation. Do not modify `memcpy`, import resolution, or the generic dispatcher.

These steps are analysis of existing dump and object artifacts. They do not require `prepare.py`, ROMFS extraction, game code generation, or a full rebuild.


## 24/11/2026 follow-up: inspect the saved call block and ov_memcpy implementation

This analysis uses files already present in the conversation workspace: `orbitals_memcpy_callers.zip`, `orbitals_memcpy_runtime.zip`, `orbitals_references_objet.zip`, `orbitals_extraits_crash.zip`, and the saved `orbitals-recomp.exe`. No new upload or rebuild was needed.

### Correct the native thunk identification

Disassembly of the saved `orbitals-recomp.exe` (PE x64, ImageBase `0x140000000`) establishes:

- `orbitals-recomp.exe+0x40CE0` is `ov_memcpy`.
- `orbitals-recomp.exe+0x70C420` is the `memcpy` import thunk.
- `orbitals-recomp.exe+0x70C430` is the `memmove` import thunk.

The body of `ov_memcpy` is:

```asm
mov  rsi, rcx
mov  rdi, [rcx]
mov  rbx, [rcx+0x08]
mov  r14, [rcx+0x10]
...
mov  rcx, rdi
mov  rdx, rbx
mov  r8,  r14
call orbitals-recomp.exe+0x70C430 ; memmove
```

Therefore the helper receives a pointer to a three-qword argument block, reads destination/source/size from offsets 0, 8 and 0x10, and forwards them to `memmove`. The earlier description of the final thunk as `memcpy` must be corrected: it is `memmove`. The import slot `0x700900A008` is still the synthetic memcpy override slot; the override implementation uses memmove internally.

### Reconstructed argument block in the saved dump

The saved object-reference report contains a 264-byte stack window beginning at VA `0x19AFFFF160`. Its qwords are:

| Offset | Value |
|---:|---:|
| +0x00 | `0x01460149013D0140` |
| +0x08 | `0x0000000005240500` |
| +0x10 | `0x0000000005240500` |
| +0x28 | `0x00000070070C1DE0` |
| +0x38 | `0x00000012E1A8000C` |
| +0x40 | `0x298` |
| +0x60 | `0x000001C92EAB0540` |
| +0x80 | `0x000000700900A008` |
| +0x88 | `0x00007FFA60A8DF58` |
| +0x98 | `0x0000007001759B20` |
| +0xA0 | `0x000001C92EAB0540` |
| +0xA8 | `0x000000011C51F7A0` |
| +0xC0 | `0x298` |
| +0xC8 | `0x00007FFA5FF1939F` |
| +0xF0 | `0x000000011C51F8A0` |

The memcpy wrapper at `orbitalgame.dll+0x1C11DF00` stores the synthetic slot at context+`0x80` and its internal resolver return at context+`0x88`. Those exact values occur at `0x19AFFFF1E0` and `0x19AFFFF1E8`, respectively. This ties the context base to `0x19AFFFF160`, rather than treating the 0x298/buffer metadata later in the same stack window as the three memcpy arguments.

Applying the `ov_memcpy` field layout to that block gives the candidate actual arguments:

- destination = `0x01460149013D0140`
- source = `0x05240500`
- size = `0x05240500` (86,246,656 bytes)

These are the first three qwords of the context block passed through the memcpy override. They are not `buffer=0x11C51F7A0, size=0x298`; those values are separate trace/frame metadata at offsets +0xA8 and +0xC0.

### Strong source clue: descriptor field +0x40

The same object-reference report shows descriptor `0x1C92EAB0540` with:

- descriptor+`0x40` = `0x01460149013D0140`
- descriptor+`0x48` = `0x000000011C51F6F0`
- descriptor+`0x50` = `0xFF`

The invalid-looking first qword in the memcpy argument block exactly matches descriptor+`0x40`. This strongly suggests the first argument was loaded from or copied from that descriptor field. It is not yet proven which caller instruction performed that load, nor whether the candidate `(2-EDX)*0x38` calculation selects this exact field.

### Important limitation in the older runtime report

The older `orbitals_memcpy_runtime.zip` reports exception-context registers such as RIP and RSP as nonsensical values (RSP=0, RIP outside declared modules), despite the exception stream identifying `ucrtbase.dll`. Do not use those parsed register values as reliable evidence for the faulting address or read/write direction until the exception-context extraction is corrected. The saved stack block and the executable disassembly are more reliable for the argument reconstruction above.

### Remaining work

1. Confirm the exact caller instruction that writes the first argument at context+`0`, and prove whether its source address is descriptor+`0x40`.
2. Obtain/disassemble the missing host-code window around `orbitalgame.dll+0x1B5A8FF0`; the existing `orbitals_memcpy_callers.zip` starts its nearest window at `+0x1B5A929F` and does not include `+0x1B5A8FF0`.
3. Verify the source and size qwords (`0x05240500`, `0x05240500`) against the exact argument-block producer. Do not assume `0x298` is the memcpy size.
4. Only after the producer and field semantics are proven, patch that caller-side load/index/bounds logic. Do not patch the import resolver or generic memcpy/memmove implementation.

No `prepare.py`, ROMFS extraction, guest regeneration, or full recompilation is needed for these analysis steps.


## 24/11/2026 follow-up: disassembly of the caller window already extracted

The existing `orbitals_memcpy_callers.zip` was reopened and its 384-byte window at VA `0x00007FFA5FF1929F` was disassembled. This is the window containing the call whose return address is `orbitalgame.dll+0x1B5A939F`.

Verified instructions:

```asm
; first memcpy-wrapper call
1B5A9328  mov  [rsi+0x98], r13
1B5A932F  mov  rcx, rsi
1B5A9332  call orbitalgame.dll+0x1C11DF00
1B5A9337  mov  r8,  [rsi+0x40]
...
; second memcpy-wrapper call
1B5A938C  mov  [rsi], rdx
1B5A938F  mov  [rsi+0x40], r8
1B5A9393  mov  [rsi+0x48], rcx
1B5A9397  mov  rcx, rsi
1B5A939A  call orbitalgame.dll+0x1C11DF00
1B5A939F  mov  rcx, [rsi+0x48]
```

This proves the second call passes the address in RSI as the wrapper's context pointer and writes the qwords at offsets 0, 0x40 and 0x48 immediately before the call. It does **not** show the writes to context offsets +8 and +0x10, which are needed to reconstruct all three values consumed by `ov_memcpy`; those may have been set earlier in the function.

The same window also contains a branch that derives values from `[rsi+0xD0]` and writes fields at +8/+0x10, but it is not safe to equate those with the failing call's arguments without the missing earlier control-flow and the runtime RSI value at the call.

### The requested earlier window is still not present in the saved extracted files

The existing caller archive has a complete window `[0x1B5A929F, 0x1B5A941F)`, but no bytes for `orbitalgame.dll+0x1B5A8FF0`. The original dump is a 8,798,186,414-byte minidump; its release asset is a 1,711,267,101-byte ZIP. The ZIP is listed in the public `analysis-data` release, but its binary contents are not available in the mounted workspace in this session. The existing partial extraction cannot reconstruct omitted bytes.

Accordingly, no claim is made that the earlier `1B5A8FF0` window has been extracted. To finish that exact extraction, the dump asset must be downloaded/materialized into the analysis runtime (or the requested 4-KiB window extracted on the user's PC and supplied). This is a data-access blocker, not a need to regenerate or rebuild the game.

Do not apply a patch yet. The current caller window is useful evidence, but does not establish the producer of the failing source/size values.


## 24/11/2026: exact caller window extracted from the published dump

The GitHub Actions run `sinedgiveaways/tool#45475145880` successfully downloaded the published 1.71-GB ZIP, extracted the minidump, and produced a fully captured 4-KiB code window (4096/4096 bytes) for `orbitalgame.dll`. Module base in this dump: `0x7FFA44970000`; target VA for RVA `0x1B5A8FF0`: `0x7FFA5FF18FF0`.

The exact instruction sequence at the reported index calculation is:

```asm
7FFA5FF18FD7  mov  r8,  [rsi+0xD8]
7FFA5FF18FDE  mov  r9,  [r8]
7FFA5FF18FE1  mov  qword ptr [rsi+0x58], 0x38
7FFA5FF18FE9  mov  eax, 2
7FFA5FF18FEE  sub  eax, edx
7FFA5FF18FF0  imul rcx, rax, 0x38
7FFA5FF18FF4  add  rcx, r9
7FFA5FF18FF7  mov  [rsi+0xC8], rcx
```

This definitively establishes the calculation:
- base pointer = `*(uint64_t *)(RSI+0xD8)`, then dereference once more to obtain R9;
- signed 32-bit index = `2 - EDX` (because EAX is 32-bit and SUB writes EAX);
- byte offset = sign-extended result multiplied by `0x38` in RCX;
- result stored at `[RSI+0xC8]`.

If (and only if) EDX=5 at this point, the effective offset is `-3 * 0x38 = -0xA8`. The code bytes prove the formula, but the saved code-window artifact alone does not prove the live EDX value or the contents of the base table. Do not call the -3/object-`0x0CE01F2C00` match proven yet.

The control-flow paths into `0x7FFA5FF18FD7` are also visible:
- `0x7FFA5FF18F43` branches there when the signed value loaded from `[RCX+0x470]` is zero;
- `0x7FFA5FF18F74` branches there when the computed scan-size value equals `0x50`.
The register EDX is not assigned on these two short branch edges, so its reaching definition is earlier in the function and must be traced through the preceding basic blocks / call-clobber behavior.

The same 4-KiB window is fully captured and disassembled in artifact `orbitals-caller-1B5A8FF0` from run `45475145880`. This is real dump-derived code, not a guessed reconstruction.

### What this resolves and what remains

Resolved: the missing caller window has been obtained from the GitHub release; the exact index formula and the two visible branch conditions are now known. No further extraction of that code window is needed.

Remaining: obtain a trustworthy exception CONTEXT for the failing thread and correlate its EDX, RSI, R8/R9, and stack to the branch at `0x7FFA5FF18FD7`. Then read the memory at `[RSI+0xD8]` and the resulting table entry at the computed signed offset. The first exception-context extractor revision used the wrong field within the context-location descriptor and was corrected; its follow-up run is `45475825616`. Until that run succeeds and yields validated context bytes, do not claim a runtime value for EDX or propose a patch.


## 24/11/2026 correction: exact memcpy arguments at the crashing call site

The complete 4-KiB host-code window extracted from the original minidump has now been inspected at the actual call return address `orbitalgame.dll+0x1B5A939F` (call instruction at RVA `0x1B5A939A`). This corrects the earlier interpretation of the stack qword window at `0x19AFFFF160`: that window was not proven to be the three-argument block for this call, and its first three qwords must **not** be reported as the actual memcpy destination/source/size.

### Exact producer sequence

At the call site (module VA based on dump ImageBase `0x7FFA44970000`):

```asm
1B5A9347  mov  rdx, [rsi+0xD0]     ; save old D0 in RDX
1B5A934E  mov  r9,  [rsi+0xE0]
1B5A9355  mov  [rsi+0xD0], r9     ; update D0, RDX still holds old D0
1B5A935C  and  r13, 4             ; length becomes exactly 0 or 4
1B5A9362  mov  rax, [rsi+0xD8]
1B5A9369  mov  r9,  [rsi+0xC8]
1B5A9370  add  r9, rax
1B5A9373  mov  [rsi+0x08], r9     ; source = C8 + D8
1B5A9377  mov  [rsi+0x10], r13    ; size = (old R13 & 4)
1B5A937B  add  rdx, rax
1B5A938C  mov  [rsi], rdx         ; destination = old D0 + D8
1B5A9397  mov  rcx, rsi
1B5A939A  call orbitalgame.dll+0x1C11DF00
1B5A939F  ...                    ; return address recorded in crash
```

Therefore, for this specific failing call, the caller-side argument expressions are:

- destination = **old** `[RSI+0xD0]` + `[RSI+0xD8]`
- source = `[RSI+0xC8]` + `[RSI+0xD8]`
- size = `R13 & 4` (only 0 or 4)

The `[RSI+0xD0]` value used for destination is read into RDX before the subsequent write replaces `[RSI+0xD0]` with `[RSI+0xE0]`. This ordering matters.

### Consequences / corrected conclusions

1. The previously inferred `0x01460149013D0140`, `0x05240500`, `0x05240500` triple from the stack window at `0x19AFFFF160` is **not established as this call's argument triple**. The earlier assertion that descriptor `0x1C92EAB0540+0x40` directly supplies the failing destination is withdrawn pending a dataflow proof.
2. The `0x298` value is not the size argument at this particular call site. The static caller code computes the size from `R13 & 4`.
3. The `0x38` index calculation at RVA `0x1B5A8FF0` is a separate earlier path in the same function window; it must not be conflated with the argument construction at `0x1B5A939A`.

### What remains to finish the root cause

The remaining task is now sharply bounded: recover the crash-time values of `RSI+0xC8`, `RSI+0xD0` (before overwrite), `RSI+0xD8`, and `R13`, or reconstruct them from the saved caller frame. Then calculate the exact destination/source and validate the two address ranges against captured minidump memory. If the length is 0, the reported invalid read may instead come from a different call or from the helper's argument/context interpretation; if it is 4, inspect those exact 4-byte ranges. Do not patch the descriptor field or the generic memcpy override based on the earlier, now-withdrawn qword mapping.

The original dump's exception stream/context extraction is being published as a separate artifact; the previous attempt's artifact upload path was wrong, so the run's extraction step completed but its output was not attached. The workflow path has since been corrected.


## 24/11/2026: crash-time native arguments recovered from the real minidump

The corrected exception-context extraction from the original 8.8-GB minidump has now been downloaded and inspected. This is the first direct register-level confirmation at the actual AV in `ucrtbase.dll`.

### Exception and faulting instruction

- Exception thread: `0x5B10`
- Exception code: `0xC0000005` (access violation)
- Exception RIP: `ucrtbase.dll+0xAE44D` (`0x7FFB2D4AE44D`)
- Faulting instruction: `vmovdqu ymm0, ymmword ptr [rdx]`
- Crash-time registers:
  - `RCX = 0x12E1A8000C`
  - `RDX = 0x01460149013D0140`
  - `R8  = 0x05240500` (86,246,656 bytes)
  - `RSI = 0x1C92EAB0540`
  - `RDI = 0x12E1A8000C`
  - `RBX = 0x01460149013D0140`
  - `R14 = 0x05240500`

At the faulting instruction, UCRT's vectorized `memmove` is reading from `[RDX]`. Thus the immediate bad operand is the **source pointer `0x01460149013D0140`**. The requested copy length is also highly suspicious: 86,246,656 bytes (about 82.25 MiB), which selects the large-copy path. The destination `0x12E1A8000C` has captured memory at its address in the dump; the source address has no captured memory range in the extracted register-pointed-memory report.

### Descriptor contents confirm argument ordering

The exception context has `RSI=0x1C92EAB0540`. The first qwords at that address are:

| Descriptor offset | Value | Meaning confirmed by native registers |
|---:|---:|---|
| `+0x00` | `0x12E1A8000C` | destination (`RCX`) |
| `+0x08` | `0x01460149013D0140` | source (`RDX`) |
| `+0x10` | `0x05240500` | size (`R8`) |
| `+0x40` | `0x01460149013D0140` | duplicate of source |

This resolves the earlier ambiguity: descriptor+`0x40` is a duplicate of the **source**, not the destination. The earlier interpretation that the qwords at stack base `0x19AFFFF160` were the actual argument triple is withdrawn. The authoritative argument values are the native register state plus the descriptor at `RSI`.

### Important consistency warning

The exception record's two access-violation parameters were parsed as `(0, 0xFFFFFFFFFFFFFFFF)`, which does not match the effective address implied by the faulting instruction and `RDX`. The instruction bytes and register context agree with each other, but the exception-information address field is inconsistent and must be treated as suspect until the raw exception-stream bytes are independently checked. Do not claim the AV parameter address is confirmed.

The caller disassembly around return address `orbitalgame.dll+0x1B5A939F` is useful for finding the producer, but its current static interpretation of the `R13 & 4` branch does not reconcile with the runtime size `0x05240500`. The exact wrapper/context field mapping and the relevant branch path must be revalidated before attributing the size to that instruction sequence. The runtime register/descriptor values above take precedence over an unverified static mapping.

### Next root-cause target

1. Trace the store that populates descriptor+`0x08` (source) and descriptor+`0x10` (size), and identify why source becomes `0x01460149013D0140` while size becomes `0x05240500`.
2. Inspect the descriptor's duplicated source at +`0x40` and its neighboring fields to identify the owning structure and any base/offset calculation.
3. Validate the full destination allocation size around `0x12E1A8000C` against the 86,246,656-byte request.
4. Resolve the exception-information mismatch from the raw stream.
5. Patch only the proven descriptor producer or length calculation. Do not patch `memcpy`/`memmove` globally.

The exception-context artifact is `orbitals-caller-1B5A8FF0` from workflow run `45476215717`. Its corrected upload succeeded.


## 25/11/2026: exact crash descriptor fields and the caller/helper contradiction

The latest successful extraction includes the 0x200-byte descriptor at `RSI=0x1C92EAB0540` and confirms the four fields needed to follow the caller's address construction:

| Field | Crash-time value |
|---|---:|
| `RSI+0xC8` | `0x0000000001490140` |
| `RSI+0xD0` | `0x000000011C51F8A8` |
| `RSI+0xD8` | `0x0000000CE01F2C00` |
| `RSI+0xE0` | `0x000000011C51F8A8` |

The caller's call site at RVA `0x1B5A939A` is preceded by:

- `mov [rsi+0x10], r13` at `+0x1B5A9343`
- `and r13, 4` at `+0x1B5A935C`
- conditional branch `je +0x1B5A93A7` at `+0x1B5A9360`
- only on the nonzero-bit path, `mov [rsi+0x10], r13` at `+0x1B5A9377`, then the call at `+0x1B5A939A`.

Thus the call at `+0x1B5A939A` is reached through the path where bit 2 is set and the caller should have overwritten the size field with 4. Yet the crash-time descriptor +0x10 and native `R8` both contain `0x05240500` (86,246,656). The crash-time `R13` also shows `0x05240500`, but that is a callee-time register and must not be treated as definitive caller state without ABI-preservation proof.

Likewise, the crash-time `RSI+0xC8/D8` values do not arithmetically produce the descriptor source `RSI+0x08`. The runtime descriptor is internally inconsistent with the expected caller-side stores.

**This contradiction moves the next investigation boundary downstream of the caller's source code:** inspect the native wrapper/dispatcher path between `orbitalgame.dll+0x1C11DF00`, `orbitals-recomp.exe+0x40CE0`, and `orbitals-recomp.exe+0x70C430`. Specifically determine whether the helper copies/mutates the context, misreads its field offsets, or passes a different descriptor to UCRT. A new dump extraction job now disassembles both native addresses directly. Do not patch the guest caller based solely on the static call-site sequence; do not patch UCRT memcpy/memmove globally.

The pointer source `0x01460149013D0140` remains the actual invalid source operand at the AV instruction. The exception stream's access-violation parameter address remains inconsistent (`0xFFFFFFFFFFFFFFFF`) and is not used as evidence.
