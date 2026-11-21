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

## Runtime capture update (21/11/2026, Crash B)

This section supersedes older thread IDs and host-register assumptions above where they differ. The earlier values came from a different run.

### Latest correlated fault snapshot

- Faulting guest thread in the latest detailed capture: `0x205b2bde100`.
- Guest PC at the captured SDK entry: `sdk+0x16b730` (`nn::os::GetTlsValue`); LR: `main+0x175a16c`.
- Guest `TPIDR_EL0=0xa2003c740`, `TPIDRRO_EL0=0x7010078000`; `TLR[+0x1f8]=0x7c005a038`. These values are nonzero.
- Guest `x27=0x9e01f7c00`, and the captured first qword at `[x27]` is zero. This alone does not establish that x27 is the TLS table base or that the zero first field is invalid.
- The Windows exception handler captured the host instruction bytes `42 8b 4c 01 5c`, decoded as `mov ecx, [rcx+r8+0x5c]`, with host `RCX=0` and `R8=0xa8`. Its effective read address is therefore exactly `0x104`, matching the access-violation address.
- Guest `x25=0xa8` in the fatal guest snapshot. This is a useful candidate correlation with host R8, but it is **not** proof that host R8 was allocated from guest x25. The host compiler may have transformed, reused, or spilled guest values.

### Important distinction about the two captures

The `main+0x1759b20` records are snapshots taken when that lifted function is entered through dispatch. They are not guaranteed to be the live guest register state at the exact host fault instruction. Do not combine one of those entry snapshots with the later fatal Cpu snapshot as if they were a single atomic register dump.

The Windows VEH already copies all 16 host GPRs, up to 16 instruction bytes, and up to 32 qwords at host RSP before entering the crash reporter. The crash reporter separately prints the current guest Cpu registers. The next step is to correlate these existing snapshots against the generated host function, not to add another high-volume TPIDR trace.

### Trace-retention limitation confirmed in source

The per-thread recorder currently has 32 permanently claimed thread slots and retains 64 events per event type per thread. Slots are not reclaimed when guest threads exit. On a busy process, slot exhaustion and ring wrap are expected; a missing event is not evidence that the call never happened. The large reported ring-overflow counts make absence-based conclusions unsafe.

### Next implementation / analysis steps
The crash reporter now also prints the nearest lifted host-function entry and the byte delta from that entry to the faulting host RIP. This is a runtime-only diagnostic in `runtime/core/boot/main.c`; it makes the next crash log directly searchable against the generated host function's disassembly without tracing every guest call. The Windows runtime-only build is triggered by the branch update.


1. Use the exact host fault RIP and instruction bytes from the fatal record to locate the instruction inside the host function generated for guest `main+0x1759b20`. Use the matching build's generated object or emitted C; do not infer a host-to-guest register map from equal numeric values alone.
2. Reconstruct the host dataflow immediately before the faulting load: where RCX becomes zero, where R8 becomes `0xa8`, and which earlier load or calculation supplied each value.
3. Compare that dataflow with the guest ARM64 instruction sequence and the saved Cpu state to establish whether the null base is a guest-visible value, a lifted temporary, or a translation/lowering error.
4. Only if the existing build artifact cannot answer this, add a narrowly scoped diagnostic that records the relevant host function's instruction window and guest Cpu pointer/thread identity at the exception. Keep it disabled by default and bounded; do not trace every GetTlsValue call.
5. Build the Windows executable with the existing **runtime-only** workflow (`tools/build/build.py --target windows --dist --runtime-only`). This workflow builds the host runtime and package; do not invoke `prepare.py`, `generate.py`, or a game-code rebuild for this investigation.

### Current conclusion

The immediate invalid read is localized: a host load uses a null base plus index `0xa8` and field offset `0x5c`, producing address `0x104`. The thread's captured TPIDR/TLR values are nonzero. The root cause remains open between a bad guest-side lookup value/initialization and an incorrect lifted-code dataflow. The next decisive evidence is the generated host-code dataflow at the fault RIP.
