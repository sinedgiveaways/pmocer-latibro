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

A cross-check against the current `docs/roadmap.md` places the affected guest function `main+0x1759b20` in Unreal's render-graph path, near a loop at `main+0x1759d9c` that processes records of sizes `0xe8` and `0x40`. **Correction from the later disassembly audit:** `main+0xbf040` is a dispatcher through the current global manager object's virtual slot `+0x50`; it is not itself a direct `FMemory::Free`, `GMalloc`, or libc-free implementation. Some manager implementations reached through this slot use TLS bookkeeping and a mutex before delegating to an inner object, but the ultimate operation depends on the runtime vtable and has not been identified conclusively.

The roadmap's current working diagnosis is that the render-graph structure was corrupted earlier and that the code which reads/pops it is behaving correctly. This means the TLS lookup may be where the process finally faults while a manager-mediated cleanup/release path processes an already-corrupted object; it is not, by itself, proof that TLS initialization caused the original corruption. Keep the immediate access-violation evidence above, but do not treat the null pointer in the lookup path as the established root cause.

The roadmap also records these avenues as already tested and ruled out by A/B runs of 3–4 starts each: GPU write-backs (`ORBITALS_WB_MERGE=none`), asynchronous preemption (`ORBITALS_ASYNC_PREEMPT=0`), page freezing (`ORBITALS_FREEZE_IDLE=0`), VRAM mirror mode (`ORBITALS_MEMORY=import`), and release-order handling of `STLR` (fixed in `7b4a312`, with the crash persisting). It also says the game library was rebuilt and its lifted code grew by 4,544 bytes.

### Revised investigation order

1. Audit kernel synchronization for a path that permits two guest threads to enter a supposedly exclusive section concurrently: mutex arbitration, condition variables, and events in `runtime/core/kernel/`.
2. Then audit the SDK thread/TLS functions replaced by the runtime, including the observed `AllocateTlsSlot` startup call and the `GetTlsValue` / `SetTlsValue` call paths.
3. Only after those checks, return to the render-graph array's allocation, ownership, and mutation history to identify the first invalid write or double access.

This order follows the repository roadmap and is more informative than changing TLS allocation sizes or adding a null check at the final fault. No runtime fix is proposed by this report update.


## Kernel synchronization audit (20/11/2026)

The current Windows runtime synchronization paths were reviewed in `runtime/core/kernel/kernel.c`, `kernel_svc.c`, `kernel_int.h`, and `runtime/probes/kernel_probe.c`.

### Invariants observed in the implementation

- The process-wide mutex/condition-variable wait list, thread handles, event state, and wake transitions are serialized by the exclusive kernel lock `G`.
- `WaitProcessWideKeyAtomic` (SVC `0x1C`) registers the condition-variable key and sets it before handing off the user mutex, while still holding `G`. This prevents the documented lost-wakeup window where a signaler could observe a cleared key before the waiter is registered.
- `SignalProcessWideKey` (SVC `0x1D`) moves signaled waiters either directly to ownership of the user mutex or to its mutex-wait queue; the eventual unlock performs the handoff.
- `WaitForAddress` / `SignalToAddress` use a per-address queue lock when `ORBITALS_ADDR_QUEUES` is enabled. Enqueue, wake, timeout removal, and the compared memory word are handled under that queue lock.
- Event signaled state, reference counts, and handle-table changes are performed under `G`; waiters are woken while that lock is held.

No definite missing-lock or double-wake path was established by this source review.

### Concrete issue found: unsynchronized lazy initialization

Two process-wide configuration caches are initialized on first use without synchronization:

- `g_aq_on` in `addr_queue()` (`kernel.c`): concurrent first calls to `WaitForAddress` can both read the initial negative value and write the same selected setting.
- `g_wait_ns` in `wait_woken()` (`kernel.c`): concurrent first timed waits can both initialize the cached `WAIT_NS` setting.

These are C-level data races even though the writers normally store the same value. They should be initialized once before guest threads start, or changed to a proper atomic/once initialization. The current evidence does not connect either race to the render-graph corruption, and neither has been changed in this diagnostic pass.

### Existing coverage

`runtime/probes/kernel_probe.c` already exercises mutex handoff order with five waiters, condition-variable wake order and mutex reacquisition, a deliberately broken lost-wakeup ordering as a negative control, address-arbiter wait/signal cases, and event synchronization. The source review does not replace running those probes on the current Windows build.

### Next discriminating work

1. Remove or prove harmless the two lazy-init data races, then run the kernel probe and CI.
2. Add a stress probe for concurrent mutex/condition-variable handoff and for two threads entering/leaving the same critical section, checking a guarded counter and canaries.
3. If those pass, instrument the game-side render-graph array's allocation and mutation/ownership path; the present crash dump alone cannot show the first corrupting write.

No runtime code change or game run was made for this audit.


## Follow-up: race-free kernel knob initialization and contention probe (20/11/2026)

The two unsynchronized lazy configuration caches identified in the previous audit have now been changed in `runtime/core/kernel/kernel.c`:

- `g_wait_ns` is initialized by `init_wait_ns()` through a static `PlatOnce`.
- `g_aq_on` is initialized by `init_addr_queues()` through a separate static `PlatOnce`.

The platform layer already provides `plat_once()` in `runtime/platform/common.c`; it uses acquire/release atomic state and a shared wait mechanism so exactly one caller runs the initializer and other callers wait for completion. The values are read only after `plat_once()` returns. This removes the C data races on these two caches without changing the selected knob values or wait/queue behavior.

A new `--probe-kernel` stress case was added in `runtime/probes/kernel_probe.c`: eight guest threads contend on the same SDK-style user mutex, each performs 2,000 guarded increments, and the probe checks the exact final count (16,000), guard values, and that the lock word is clear after all joins. Workers are allowed to run on any application core.

**Validation status:** source changes and the probe are committed, but no Windows build or execution of `--probe-kernel` was available in this session. The stress case is therefore added, not yet passed. The original Orbitals crash has not been reproduced or fixed by this change. The next required validation is a Windows build, `--probe-kernel`, then a game launch/reproduction run; if the probe passes and the crash remains, trace the first invalid write to the render-graph array.


## Separate crash: Unreal allocator rejects a pointer (20/11/2026)

This is a **different failure mode** from the earlier 0xC0000005 access violation in nn::os::GetTlsValue documented above. Do not merge their stack traces or assume they share a root cause.

### Runtime observation

For orbitals(10).log (build 0.2.0-g3732230, Windows, RTX 4080 / Vulkan 1.4.351):

- The process runs for about 49 seconds before the diagnostic.
- The failing thread is 0x1021, named RenderThread 0.
- Unreal's allocator reports: FMallocBinned3 Attempt to free an unrecognized pointer %p.
- The process then takes the Nintendo SDK diagnostic-abort path (nn::diag::Abort).
- Stack offsets reported in main include main+0x175a16c, main+0x1750b88, and main+0x1750650.
- Stack-memory strings include FRDGBuilder::AddCommandListSetupTask and FRDGBuilder::AsyncSetupQueue. These are contextual strings, **not proof of symbolic stack frames**.
- Reported system RAM use was about 16,565 / 32,581 MiB; this does not suggest ordinary system-memory exhaustion.
- GPU breadcrumbs were enabled; ORBITALS_GPU_CRUMBS=0 disables them.

The diagnostic means the allocator was asked to free a pointer it did not recognize as one of its allocations. The rejected pointer value itself was not captured in the supplied excerpt. Possible explanations include a stale pointer, duplicate release, pointer from a different allocator, or earlier heap/object corruption. None is established yet.

### Disassembly findings

The following are findings from the player's disassembly of the matching main NSO. They describe dispatch and object-management paths; they do not identify the rejected pointer.

1. **The global manager dispatchers are not free functions.**
   - main+0xbf040 loads the global manager at the cell based on main+0x8ff7a80, reads its vtable entry at +0x50, and branches indirectly.
   - main+0xbef90 similarly dispatches through vtable entry +0x30.
   - main+0xc7a60 and main+0xc7850 dispatch through other manager vtable entries (+0x68 and +0x40 respectively), with lazy initialization where applicable.
   - Therefore labels such as free attached to main+0x13fc0 in the disassembler output are misleading: 0x13fc0 branches to 0xbf040, which is another virtual dispatch. The same applies to main+0x156b0.

2. **The generic wrapper serializes calls to an inner manager.**
   - The wrapper created by main+0xc7460 has a mutex and an inner-object pointer.
   - Its methods at main+0xf0880, main+0xf08f0, main+0xf0970, and main+0xf09d0 lock the wrapper mutex, load the inner object, and dispatch to its corresponding virtual method.
   - main+0xc7460 initializes the global manager and may also construct specialized manager objects of sizes 0x170 and 0x140, depending on global feature flags. Their initializers are main+0xbe280 and main+0xbf0f0; main+0xbd7e0 initializes common fields and synchronization primitives.

3. **The specialized manager methods maintain per-thread activity state.**
   - main+0xbd950 (slot +0x30), main+0xbdb40 (slot +0x40), and main+0xbdd70 (slot +0x50) first query an inner-object capability/status method at vtable offset +0x108.
   - On the direct path they delegate to the corresponding inner-object slot.
   - On the managed path they update a per-thread counter obtained through nn::os::GetTlsValue, use a mutex at manager offset +0x18, call inner-object methods (including slots +0x30, +0x40, +0x50, and +0x70), and then undo the TLS counter update.
   - This is evidence of thread-aware manager bookkeeping. It is **not** enough to name the inner operations as allocate/free without examining the actual runtime vtable target.

4. **Cleanup paths release or return stored resources through the manager interface.**
   - main+0xec990 and main+0xecad0 reset stored-pointer counters and pass non-null stored pointers to the global manager's slot +0x50.
   - main+0xf43f0 and main+0xf4740 similarly reset structures and route pointer fields through slot +0x50.
   - main+0xec770 resets object state and finalizes its reader/writer lock and mutex; it does not directly prove that the outer allocation is freed.
   - main+0xecaa0 and main+0xecc08 call cleanup then main+0x13fc0; that final call is still the manager dispatch described above, not a verified direct allocator free.

5. **A thread-local block allocator/pool is present, but its ownership semantics remain unproven.**
   - main+0x170aa0 obtains per-thread state using GetTlsValue; when it has no current block it requests 0x10000 bytes through main+0xbef90 and lays out chunk metadata in the returned block.
   - main+0x5df20 manipulates global block metadata with atomic compare-and-swap operations and TLS lookups.
   - main+0x131b60 performs recursive reference-count release and routes object cleanup through the manager interface.
   - These functions make the allocator/manager path a worthwhile investigation target, but do not prove that it produced the pointer rejected by FMallocBinned3.

### Static-image data check

The player inspected the first 0x80 bytes at main+0x94beda0 and main+0x9072210; all displayed qwords were zero in the NSO image. These addresses are associated with accessors/global initialization paths (including main+0x89660 and main+0x14a30). This is a **file-image observation**, not a live-process memory capture. Zero-filled BSS/global storage and runtime initialization/relocation mean these values cannot establish that the corresponding runtime objects have null vtables or are invalid. Do not use these static zeros as evidence of a bad live object.

### What is established and what is not

Established:

- The new failure is an Unreal allocator diagnostic followed by SDK abort, not the older TLS read access violation.
- The failing thread is the render thread, and the stack context contains render-graph-related strings.
- The disassembled manager path contains virtual dispatch, TLS activity counters, mutex serialization, cleanup of stored pointers, and a thread-local block/pool path.
- Several apparent free labels in the disassembly are not direct free implementations.

Not established:

- The exact pointer rejected by FMallocBinned3.
- The concrete inner vtable implementation reached by the failing release call at runtime.
- Whether the invalid pointer results from double release, stale ownership, allocator mismatch, or earlier memory corruption.
- Whether the render-graph strings correspond to the exact active stack frames.
- Whether the manager/pool path is causal or merely nearby.

### Next discriminating capture

On the next instrumented run, capture the **actual rejected pointer** at the FMallocBinned3 diagnostic and the call stack with symbols/return addresses. Also capture, immediately before the relevant virtual +0x50 call, the global manager pointer, its vtable pointer and slot +0x50 target, the pointer argument, and the allocation's recorded owner/size if available. This should be correlated on the same thread with the allocator diagnostic. A live-process memory read is required; another static NSO dump will not answer the question.

Do not change TLS sizes, manager dispatch, or allocator behavior based only on the current evidence. No root cause or fix is claimed.


## Disassembly follow-up: allocation backtrace and manager metadata tables (20/11/2026)

The next NSO disassembly pass examined `main+0x19f40`, `main+0xbdfe0`, `main+0xf4b70`, `main+0xf4c80`, `main+0xf4de0`, `main+0xf53c0`, `main+0xf5020`, `main+0xf524c`, `main+0xf555c`, `main+0xf5600`, `main+0xf5730`, `main+0xbd7e0`, `main+0xbe280`, and `main+0xbf0f0`.

### Backtrace capture and deduplication

- `main+0x19f40` calls `nn::diag::GetBacktrace(buffer, capacity)`. The caller supplies capacity `0x42`; when fewer frames are returned, the helper writes a null qword at the first unused position. This is a backtrace collector, not a generic buffer initializer.
- `main+0xbdfe0` obtains that backtrace, invokes a callback obtained from global storage to derive a 32-bit key, then consults a read/write-locked table. A candidate entry is checked against the key and the stored trace contents before its associated identifier is reused. If no match exists, it inserts a new record and updates auxiliary slot bookkeeping.
- `main+0xf4c80` confirms the content-comparison behavior for the related table: it walks a bucket chain, compares the 32-bit key, then compares qwords from the stored and supplied trace buffers, stopping on a mismatch or null terminator. On a match it replaces the stored descriptor/payload and updates the auxiliary index/list and bitmap state.
- The strongest interpretation is therefore a deduplicating allocation/backtrace metadata registry: stack traces are fingerprinted and identical traces can share/reuse an identifier. The exact meaning of every returned identifier and the callback at `0x8ff70a68` still depends on its runtime target and call-site context.

### Pointer-keyed metadata tables

- `main+0xf4b70` allocates/copies a record, calls `main+0xf4c80` to detect/update an existing entry, and otherwise inserts it into a hash table. The key is derived from a pointer (shifted right by four and mixed); records have a `0x20`-byte stride.
- `main+0xf4de0` is a related pointer-keyed operation. It copies a small descriptor into a `0x20`-byte record, searches the table, and on an existing key updates the record plus secondary list/bitmap bookkeeping. Its optional byte output indicates whether an existing entry was found/updated.
- `main+0xf53c0` handles a related table with a `0x18`-byte record stride and corresponding hash-bucket and auxiliary-slot management. `main+0xf555c` is its existing-entry update path.
- `main+0xf524c` and the continuation around `main+0xf5730` scan bitmaps/slot words and rebuild bucket links for records; the `0x18` stride is visible in the rehash loop. These are table maintenance/free-slot routines, not direct heap-free implementations.
- `main+0xf5020` is a table reset/growth helper used by the smaller-record table. The supplied disassembly was partial, so its complete reset semantics are not claimed here.

### Manager initialization

- `main+0xbd7e0` initializes common manager state: mutex, reader/writer lock, counters, empty table pointers/counts, and a TLS slot/index used by later activity bookkeeping. It also calls `nn::os::GetTlsValue` and ensures the per-thread slot is initialized/cleared as needed.
- `main+0xbe280` and `main+0xbf0f0` are one-time initialization/growth routines for the two specialized manager layouts created by `main+0xc7460`. They reserve backing storage for several record arrays and hash tables, initialize capacities, set the initialized flag, and publish the manager pointer globally. They are not the per-allocation/free operations themselves.
- The two specialized layouts share the common initialization path but use different table offsets and helper routines, consistent with the distinct record formats observed above.

### Relevance to the allocator rejection

This strengthens the conclusion that the specialized manager is an instrumented allocator/metadata layer which records allocation-site backtraces and maintains pointer-keyed tracking tables under synchronization. It does **not** show that any table is corrupt, that a record is stale, or that this layer passed the pointer rejected by `FMallocBinned3`.

The earlier distinction remains important: the older `GetTlsValue` access violation and the later `FMallocBinned3 Attempt to free an unrecognized pointer` are separate observed failures. The backtrace registry's use of `GetTlsValue` does not establish that TLS caused the allocator rejection.

### Next discriminating disassembly

Inspect the concrete targets of the specialized manager's virtual cleanup/release path (not just the wrapper dispatch), including the callbacks reached through vtable slots `+0x50`, and correlate the actual pointer argument with the pointer printed/rejected by `FMallocBinned3`. Also inspect the remaining portions of `main+0xf5020` and `main+0xf5600` if the table reset/free-slot algorithm itself becomes suspect. No runtime code change, fix, or root cause is claimed by this disassembly pass.


## Allocator bucket registration path (20/11/2026)

A disassembly pass over `main+0xb8760` (range `0xb8760-0xb8b80`) clarified how the base Binned3 manager registers backing regions in its pointer-lookup table:

- The manager's `+0x60` field points to a hash table of buckets; the bucket index is masked by `manager+0x20 - 1`, and each bucket head/record stride is `0x20` bytes.
- The lookup derives a region key from the pointer relative to `manager+0x28`, shifted by the manager's configured granularities. It walks the bucket chain through the link at record `+0x18`, checking the key at record `+0x00` and the region-data pointer at `+0x08`.
- When a key is absent, the routine allocates backing storage, calls `main+0xbc7e0` to initialize region metadata, writes the region pointer into the record, and validates/initializes the metadata marker `0x39431234`.
- It then links a record into the bucket's circular doubly linked list (record links at `+0x10` and `+0x18`). Existing-key and new-key paths both validate marker/state before updating the metadata value.
- The marker `0x39431234` in this routine is distinct from the allocation metadata marker `0x17EA5678` examined by the fallback free path; they must not be conflated.

A caller scan found one direct branch to `main+0xb67a0`, at `main+0xb831c`, within the slow allocation path. The user supplied only this one line of the scan output; the caller sections for `main+0xb8760` and `main+0xb6ad0` have not yet been provided, so their caller counts remain unverified. The current evidence supports that `0xb67a0` is reached from slow allocation, but does not yet prove that `0xb8760` has only one caller or that every freeable region is registered through one path.

### Next discriminating steps

1. Obtain the complete caller-scan output for `0xb8760` and `0xb6ad0` (including their section headers and totals).
2. Inspect the call site around `0xb831c` and the full `0xb67a0` routine to establish its inputs, lock ownership, and region initialization semantics.
3. Compare the record/key calculation in `0xb8760` with the search in `0xb90e0` instruction-for-instruction, including sentinel/list invariants and the exact metadata pointer calculation.
4. Capture the rejected pointer from the live crash, plus the live manager pointer/vtable/slot-`+0x50` target and the allocator region key/bucket chain. Without the actual pointer and runtime object, the disassembly cannot distinguish a wrong-manager free, stale pointer, duplicate free, or prior metadata corruption.

No allocator code change or root-cause claim is made by this update.


## Runtime instrumentation added on `mco2vie/orbital-crash-trace`

The diagnostic branch now captures the investigated SDK lookup at function entry:

- The entry address is resolved from the loaded `sdk` module base plus `0x16b730`, so ASLR does not change the target.
- The generated function-entry macro checks that address and records only calls to `GetTlsValue`; it does not log every guest function.
- A bounded 32-entry lock-free ring stores guest thread identity, `x0` (the `TlsSlot` argument), `x27`, `[x27]` when safely readable, guest LR, TPIDR/TLR, and the memory-region state/end for `x27` and the first pointer.
- It also samples three candidate table words at offsets `+0x5c`, `+0x38+0x5c`, and `+0x70+0x5c`, using guarded process-memory reads. These are diagnostic samples, not an assertion about the final slot-index formula.
- On a crash, the report dumps the ring entries belonging to the faulting guest thread before the existing TPIDR, host-register, instruction-byte, guest-register, and guest-frame diagnostics.
- Windows artifact and zip labels on this branch use the `mco2vie` prefix.

Existing diagnostics remain available: set `ORBITALS_TRACE_TPIDR=1` in the runtime config to record TPIDR reads/writes. Vulkan breadcrumbs are already enabled by default on supported drivers; `ORBITALS_GPU_CRUMBS=0` disables them and `ORBITALS_GPU_CRUMBS_DRAW=1` adds draw-level breadcrumbs.

**Validation status:** source changes are committed to this branch, but they have not yet been compiled or exercised in a Windows game run. The new TLS samples are intended to identify the pointer chain at the failing call; they do not establish the earlier corruption/root cause by themselves.


### Additional Windows crash context

The Windows vectored exception handler now snapshots, before entering the crash reporter:

- The host thread ID.
- Up to 64 pointer-sized words beginning at the exception-time host RSP, copied with `ReadProcessMemory`.
- The host general-purpose registers and bytes at the faulting RIP (already present).

The fatal report prints this bounded raw stack snapshot with offsets from RSP. It is intentionally labelled as raw stack data rather than an unwound call stack: lifted/JIT frames cannot safely be claimed as symbolicated frames from this snapshot alone.

The `GetTlsValue` entry hook is emitted after both generated-function layouts are selected, so it also covers functions that use unresolved-branch or setjmp dispatch layouts. The hook is limited to the one SDK function and does not add a test to every guest function's hot entry macro.

### Build trigger

The Windows workflow now runs on pushes to `mco2vie/orbital-crash-trace` and pull requests targeting `copilot/orbital-copilot-analyse`, as well as the existing `orbitals-port` events. Manual `workflow_dispatch` remains available.


The TLS ring also records the host OS thread ID alongside the guest runtime-thread pointer, plus guest SP and FP (x29) at function entry. This lets the trace be correlated with the Windows exception's thread ID and guest stack state. The fatal report also queries the memory-region state and end address for the exact access-violation address.


### Historical instrumentation (superseded)

An earlier revision traced every instruction in `sdk+0x16b730` (GetTlsValue). That per-instruction ring was removed because Crash B's faulting lifted function is `main+0x1759b20`, not the SDK helper. Only the bounded GetTlsValue entry snapshot remains available as an optional secondary trace (`ORBITALS_TRACE_TPIDR=1`). The active per-instruction trace is the 64-entry Crash B ring described below.


## Crash B: expanded capture on mco2vie/orbital-crash-trace

The earlier instruction ring targeted `sdk+0x16b730` (GetTlsValue), not the faulting function. That per-instruction ring has been removed to avoid tracing an unrelated hot path. The GetTlsValue entry/pointer snapshot remains optional when `ORBITALS_TRACE_TPIDR=1`. The new primary ring targets the lifted function `main+0x1759b20` (`f_7001759b20`), which contains the failing host-side lifted instruction sequence documented in the Crash B control-flow follow-up.

- A 64-entry lock-free ring snapshots guest PC, SP, host TID, guest-thread pointer and all `x0..x30` immediately before each guest instruction in that one function.
- The fatal report prints the Crash B ring first, filtered to the current guest thread. GetTlsValue entry/pointer snapshots and TPIDR read/write history are optional secondary traces enabled with `ORBITALS_TRACE_TPIDR=1`.
- On Windows, an additional DbgHelp minidump is written to `%TEMP%\\OrbitalsCrashDumps\\` for faults handled by the runtime. It includes thread information, handles, unloaded modules, data segments, virtual-memory region metadata, indirectly referenced memory and private read/write memory. It is not a full-memory dump.
- The path to a successfully written dump is printed in the fatal log. The dump may contain process memory and should be treated as private.
- The Windows build links DbgHelp and publishes the runtime linker map as `orbitals-runtime-symbols-mco2vie`. The player setup also emits `orbitalgame.dll.map` when it links the generated game-code DLL.

The minidump is supplementary host-side evidence. The guest ARM64 ring is the key evidence for reconstructing the Crash B branch and the values consumed by the faulting load. Neither trace alone proves the upstream cause.


### Reproduction and collection

1. Download the newest `orbitals-recomp-windows-x64-mco2vie` artifact from the successful run of this branch and install its contents over the existing player files, preserving the game's required data folders.
2. Start the game and allow the generated game-code library to rebuild with the updated recompiler instrumentation. Do not reuse an old `orbitalgame.dll` if the launcher indicates that it must be regenerated.
3. Reproduce the same Crash B scenario.
4. Collect the fatal log and the newest `.dmp` from `%TEMP%\\OrbitalsCrashDumps\\`. The dump may contain private process memory; do not post it publicly.
5. Also keep the `orbitals-runtime-symbols-mco2vie` artifact (linker map for `orbitals-recomp.exe`). A separate `orbitalgame.dll.map` is now emitted alongside a game DLL built by the player build, and should be collected if present.

The next analysis will correlate the trace's guest instruction PCs and `x0..x30` snapshots with the two predecessor branches of the fallback block, then inspect the actual pointer and indexed record in the dump. The run must reproduce the crash with this instrumented build; a successful CI build alone does not validate runtime capture.


### Generated game-code import and symbol validation

The generated `orbitalgame.dll` calls three diagnostic entry points implemented by the runtime: `rt_tls_call_trace_entry`, `rt_crash_b_trace_begin`, and `rt_crash_b_instruction_trace`. They must be present in `RT_EXPORTS`, because the Windows package creates `setup/lib/liborbitals-recomp.dll.a` from that export list and the game-code DLL links against that import library. This was corrected on the diagnostic branch.

The Windows workflow now syntax-checks the recompiler and player build scripts and asserts that all three generated-game diagnostic imports are exported. The player game-code link emits `orbitalgame.dll.map` (including the packaged-CRT/lld-link path and the fallback compiler-link path). The runtime executable map is collected as a separate CI artifact.

CI builds the runtime executable and validates the export list, but does not compile the game's generated C: that requires the user's local game files and generated objects. A successful CI run therefore does not replace the first-launch game-code generation check.
