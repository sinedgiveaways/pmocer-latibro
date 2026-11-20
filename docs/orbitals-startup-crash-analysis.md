# Orbitals startup crash: TLS lookup in GetTlsValue

**Status:** diagnosis of the immediate fault; the initialization root cause is not yet established.  
**Scope:** analysis of the Windows crash dump, guest ARM64 register/stack snapshot, and disassembly from the matching generated object and DLL. No source or runtime code was changed as part of this investigation.

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


## Further allocator cross-check

The allocator implementation in `runtime/core/memory/memory.c` confirms that `mem_kernel_alloc(size, align)` zeroes exactly the requested allocation with `memset(p, 0, size ? size : 1)` before returning it. `thr_new()` obtains its TLS block through this allocator and assigns that address to `Cpu.tpidr`.

This makes an important distinction in interpreting the dump:

- The faulting thread's recorded `TPIDR_EL0` is `0xa2003c740`.
- The guest `x27` value used by the generated code is `0x9e02031d0`.
- These values are different. Therefore the observed `x27` pointer must not be casually identified with the runtime's `t->tls` allocation from this snapshot.
- The allocator's zeroing behavior establishes that a newly allocated runtime TLS block begins zeroed, but it does not establish that the separate object reached through `x27` is that allocation or that the SDK expects its first word to be nonzero.

The exact owner and provenance of `x27` remain open. In particular, the prior pointer-chain interpretation (a null second-level pointer) is consistent with the disassembly and effective address, but the dump does not yet identify which allocator or guest initialization routine owns that object.

### Data needed to close the gap

The next useful capture is a same-thread, same-call snapshot at entry to `sdk+0x16b730` (or in the exception handler): all guest GPRs `x0-x30`, `Cpu.tpidr`, `Cpu.tpidrro`, the current guest PC/LR, and safely-read 8-byte values at `x27` and `x27+0x8`. Also record the active `RtThread` pointer and its `tls`/ `tlr` addresses. Comparing those addresses will tell whether `x27` is a runtime-owned block, an SDK-owned structure, or a stale/corrupted guest pointer.

No fix is justified from the current evidence alone.


## Roadmap cross-check: distinguish the failing lookup from the earlier corruption (20/11/2026)

A cross-check against the current `docs/roadmap.md` changes the investigation priority. The roadmap identifies the affected guest function `main+0x1759b20` as Unreal's render-graph path that pops an element from an internal array (the nearby loop is at `main+0x1759d9c`, with element sizes `0xe8` and `0x40`) and frees its allocation through `FMemory::Free` (`main+0xbf040`). That free reaches `GMalloc`'s vtable slot `0x50`, which reads thread TLS.

The roadmap's current working diagnosis is that the render-graph structure was corrupted earlier and that the code which reads/pops it is behaving correctly. This means the TLS lookup may be where the process finally faults while freeing an already-corrupted object; it is not, by itself, proof that TLS initialization caused the original corruption. Keep the immediate access-violation evidence above, but do not treat the null pointer in the lookup path as the established root cause.

The roadmap also records these avenues as already tested and ruled out by A/B runs of 3–4 starts each: GPU write-backs (`ORBITALS_WB_MERGE=none`), asynchronous preemption (`ORBITALS_ASYNC_PREEMPT=0`), page freezing (`ORBITALS_FREEZE_IDLE=0`), VRAM mirror mode (`ORBITALS_MEMORY=import`), and release-order handling of `STLR` (fixed in `7b4a312`, with the crash persisting). It also says the game library was rebuilt and its lifted code grew by 4,544 bytes.

### Revised investigation order

1. Audit kernel synchronization for a path that permits two guest threads to enter a supposedly exclusive section concurrently: mutex arbitration, condition variables, and events in `runtime/core/kernel/`.
2. Then audit the SDK thread/TLS functions replaced by the runtime, including the observed `AllocateTlsSlot` startup call and the `GetTlsValue` / `SetTlsValue` call paths.
3. Only after those checks, return to the render-graph array's allocation, ownership, and mutation history to identify the first invalid write or double access.

This order follows the repository roadmap and is more informative than changing TLS allocation sizes or adding a null check at the final fault. No runtime fix is proposed by this report update.
