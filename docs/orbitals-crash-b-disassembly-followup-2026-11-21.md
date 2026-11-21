# Orbitals Crash B: generated-object disassembly follow-up (21/11/2026)

**Scope:** static review of the supplied disassembly excerpts from `setup-work/obj/main_main_0184.o`, correlated with the latest `GetTlsValue` crash log. This is a diagnostic report only: no runtime code, build configuration, or generated game code was changed.

## Executive summary

The new object disassembly adds useful structure around the generated function `f_7001759b20` and the fault at object `.text+0x1952f`.

It establishes two distinct collection layouts used by this code:

- A collection rooted at object offsets `+0x468` (pointer) and `+0x470` (32-bit count), whose iteration records are addressed with a `0x50`-byte stride.
- A separate collection rooted at `+0x158`, whose elements are addressed with a `0xE8`-byte stride.

These layouts must not be conflated. The excerpts do not prove that either collection is itself the SDK TLS slot table.

The crash path also exposes a potentially unsafe fallback: after the lookup count is zero, control reaches code that loads a pointer from the generated function's local context at `[rsi+0xd8]`, dereferences it, and later accesses a field through the resulting base. The local context slot `[rsi+0xd8]` is reused for different values in other paths (including the immediate `0xE8`), so its meaning must be tracked along the exact control-flow path rather than assigned one global semantic name.

## Runtime evidence from the latest crash log

The latest supplied log reports:

- Guest PC: `sdk+0x16b730`, `nn::os::GetTlsValue(TlsSlot)`.
- Guest thread: `0x2e1c6d9d880`.
- `TPIDR_EL0 = 0xa2003c740`.
- `x27 = 0x840180400`, with the first qword at `[x27]` equal to zero.
- The TLS trace reports: `GetTlsValue entry: no entry captured for thread=0x2e1c6d9d880`.
- The host exception is a read access violation at effective address `0x104`; the host instruction bytes are `42 8b 4c 01 5c`, with host `RCX=0` and `R8=0xa8`.

The nonzero `TPIDR_EL0` does not rule out a missing or null intermediate pointer in the SDK/game TLS lookup. The missing trace entry and the zero qword at `[x27]` are observations for this thread/run; they do not, by themselves, identify which component should have populated that qword.

## Mapping the host fault to the generated object

The faulting generated function is `f_7001759b20`:

- Symbol start in the COFF `.text`: `0x189a0`.
- Fault offset within the lifted function: `0xb8f`.
- Resulting object `.text` offset: `0x1952f`.

The disassembly around the failing block is:

```asm
00019459: test    rcx, rcx
0001945c: je      0x19500
00019462: mov     rcx, qword ptr [rsi + 0xa8]
00019469: movsxd  rax, dword ptr [rcx + 0x470]
00019470: test    rax, rax
00019473: je      0x19507
00019479: mov     r8, qword ptr [rcx + 0x468]
...
00019507: mov     rax, qword ptr [rsi + 0xd8]
0001950e: mov     rcx, qword ptr [rax]
...
00019524: lea     r9, [rcx + r8]
00019528: mov     qword ptr [rsi + 0xc8], r9
0001952f: mov     ecx, dword ptr [rcx + r8 + 0x5c]
```

The reported host register values explain the exact faulting address for the final load:

`RCX + R8 + 0x5c = 0 + 0xa8 + 0x5c = 0x104`.

Thus the immediate failure is a load through a null base at this point in the generated code. The preceding instructions show that this base is obtained by loading `[rsi+0xd8]` and then loading the first qword from that pointer. This identifies the failing dereference chain, but does not yet identify the owner or initializer of the object.

The zero-count branch at `0x19473` reaches `0x19507`. This makes the empty-collection/fallback path a high-priority control-flow path to understand. It is not yet proof that the zero count is itself erroneous: the fallback may be valid under an invariant that is currently not being met.

## Collection layout evidence

### Collection at `+0x468 / +0x470`

The following sequence loads the collection pointer and count into an iterator-like context:

```asm
0004ae79: mov     rax, qword ptr [rsi + 0x40]
0004ae7d: mov     rcx, qword ptr [rbx + 0x468]
0004ae84: mov     edx, dword ptr [rbx + 0x470]
0004ae8a: mov     qword ptr [rsi + 8], rdx
0004ae8e: mov     qword ptr [rsi], rcx
```

A separate loop uses the count at `+0x470` and the pointer at `+0x468`, then computes record positions using a `0x50` stride:

```asm
00018be5: movsxd  r8, dword ptr [rcx + 0x470]
00018bf5: mov     rcx, qword ptr [rcx + 0x468]
...
00018bff: shl     r9, 4
00018c03: lea     rdx, [r9 + r9*4]
00018c07: add     rdx, -0x50
```

This supports identifying `+0x468` as the collection's data pointer and `+0x470` as its count. The loop's record stride is `0x50`.

### Separate collection at `+0x158)

The excerpts repeatedly fetch a base pointer from `+0x158` and index records with a `0xE8)-byte stride:

```asm
00018f4a: mov     rax, qword ptr [r9 + 0x158]
00018f51: imul    r8, r8, 0xe8
00018f58: lea     rcx, [rax + r8]
00018f5c: mov     qword ptr [rsi + 0xd8], rcx
```

A later path independently performs the same element-address calculation:

```asm
0004c0a3: mov     rdx, qword ptr [rsi + 0xa8]
0004c0aa: mov     r8, qword ptr [rdx + 0x158]
0004c0b1: movsxd  rdx, dword ptr [rsi + 0xb8]
0004c0b8: imul    rdx, rdx, 0xe8
0004c0bf: lea     r9, [rdx + r8]
0004c0c3: mov     qword ptr [rsi + 0xe0], r9
```

This is a distinct indexed collection. Its `0xE8)-byte records are not the `0x50)-byte records traversed through `+0x468`.

## Important qualification: local context slot `[rsi+0xd8]` is reused

The object disassembly shows different assignments to this local context slot:

- At `0x1491e`, it is assigned the immediate value `0xE8`.
- At `0x18f5c`, it is assigned the address of an element in the `+0x158` collection.
- At `0x19507`, it is consumed as a pointer and dereferenced.

Therefore, `[rsi+0xd8]` is a reused local/context field, not a stable global object field whose meaning can be inferred from its offset alone. The exact reaching definition on the failing branch must be established from the complete control flow and the caller/context setup.

There is another guarded dereference in a different path:

```asm
00047085: cmp     dword ptr [rsi + 0xe0], 0
0004708c: je      0x470e6
0004708e: mov     rax, qword ptr [rsi + 0xd8]
00047095: mov     rax, qword ptr [rax]
```

This proves that this other path checks a condition before dereferencing the slot. It does **not** prove that the same guard is missing from the failing path: the context fields are reused, and the paths may have different invariants.

## What this establishes

1. The current runtime snapshot has a nonzero `TPIDR_EL0`, but `x27` points to an object whose first qword is zero.
2. The latest trace reports no captured TLS entry for the crashing guest thread.
3. The host fault maps exactly to `.text+0x1952f` in `f_7001759b20`, and the reported host registers produce the reported effective address `0x104`.
4. The failing block can be reached from a zero-count branch on the `+0x468/+0x470` collection.
5. The `+0x468/+0x470` collection uses `0x50)-byte records in the inspected loop.
6. The separate `+0x158` collection uses `0xE8)-byte records.
7. The generated local context slot at `[rsi+0xd8]` is reused and must be analyzed by reaching definition on the exact failing path.

## What remains unknown

- Which guest object `x27 = 0x840180400` belongs to and which code is expected to populate its first qword.
- Whether the missing TLS entry is the direct cause of the zero pointer or a correlated symptom.
- Which exact caller state and reaching definition feed `[rsi+0xd8]` at `0x19507`.
- Why the zero-count branch reaches a fallback that expects a valid pointer.
- Whether the empty collection is legitimate and the fallback invariant is broken, or whether the collection state was corrupted earlier.
- Whether either collection is directly part of the SDK TLS slot manager. The observed strides and offsets alone do not establish that semantic identity.

## Next static-analysis steps

Keep the investigation on Crash B and avoid unrelated GOT/PLT work.

1. Recover the complete basic-block/control-flow context from `0x19459` through `0x19534`, including all predecessors of `0x19507` and the reaching definitions of `rsi`, `rsi+0xa8`, and `rsi+0xd8`.
2. Trace every write to the local context slot `[rsi+0xd8]` along the path that reaches `0x19507`; do not treat writes in other paths as interchangeable.
3. Identify the object loaded through `[rsi+0xa8]` at `0x19462`, then establish the owner and initialization rules for its `+0x468/+0x470` collection.
4. Correlate the guest `x27` value and its first qword with the exact `GetTlsValue` argument and the SDK's `AllocateTlsSlot` / `SetTlsValue` setup path, using same-thread evidence.
5. Determine whether the zero-count fallback is an expected empty case or an invariant violation before proposing any code change.

No null-check patch, TLS allocation-size change, or speculative runtime fix is justified by this disassembly alone. The next pass should remain static and should not require regenerating or recompiling the game.
