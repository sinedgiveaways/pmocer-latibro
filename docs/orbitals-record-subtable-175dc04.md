# Orbitals: second null sub-table read at main+0x175dc04

## Finding

The crash in `orbitals(20261123-225851).log` is a second null sub-table read, separate from the existing `rec-null-guard` at `main+0x175a030`.

The matching AArch64 code in the uploaded `exefs/main` NSO is:

```asm
main+0x175dbd0: ldr   x9, [x0, #0xd0]
main+0x175dbd4: add   x8, x0, #0x10
main+0x175dbd8: mov   w11, #0x38
main+0x175dbdc: cmp   x9, #0
main+0x175dbe0: csel  x8, x8, x9, eq
main+0x175dbe4: sxtw  x9, w1
main+0x175dbe8: add   x10, x8, x9, lsl #6
main+0x175dbec: ldr   x8, [x0]
main+0x175dbf0: mov   w9, #2
main+0x175dbf4: sub   w9, w9, w1
main+0x175dbf8: ldr   s2, [x10, #0xc]
main+0x175dbfc: umaddl x11, w9, w11, x8
main+0x175dc00: ldr   w9, [x0, #0xd8]
main+0x175dc04: ldr   d17, [x11, #0x28]
```

The caller at `main+0x175dd90` passes a record address in `x0` and a selector in `w1`. The helper treats `[x0]` as the base of 0x38-byte entries and selects entry `2 - w1`. In the crash snapshot:

- `x0 = 0x84017c400`, `x1 = 0`.
- The host instruction bytes `48 8b 4c 02 28` are the lifted `ldr d17, [x11, #0x28]`.
- Host `rax = 0`, `rdx = 0x70`: the computed guest address is `0 + (2 * 0x38) + 0x28 = 0x98`.

Thus the pointer stored at `[x0]` is null, and the helper dereferences it. This is not the old `main+0x175a030` site. The separate `x27` pointer printed by the TLS-chain diagnostic is not `x0`; its dump does not establish the state of this record.

## Patch

`rec-null-guard-vector` replaces the load at `main+0x175dbec` with a branchless null-to-zero-page fallback:

- If `[x0]` is non-null, `x8` remains unchanged.
- If `[x0]` is null, `x8` becomes `RT_PATCH_PAGE`.
- The subsequent indexed reads are bounded: the largest access is below `RT_PATCH_PAGE + 0x100`, inside the committed 4 KiB page.
- `x14` and `x15` are scratch registers unused by the remainder of this helper. The replacement does not modify NZCV.
- The patch page is committed by `rt_patches_init`; Windows uses `VirtualAlloc(..., MEM_COMMIT, PAGE_READWRITE)`, which supplies zero-filled memory.

The helper then computes with zero sub-table values instead of faulting. This is a containment fallback, not proof that zero values are the game's intended result. The current log does not dump this `x0` record's `+0x50` state, so whether the missing sub-table is a valid empty record or a construction race remains unresolved.

The generator checks the original word at `main+0x175dbec` (`f9400008`) against the player's ExeFS before emitting the patch. The new switch is `ORBITALS_PATCH_REC_NULL_GUARD_VECTOR`, default on; it can be disabled independently for an A/B test.
