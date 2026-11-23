"""Control-flow-aware fences for Ampere warp-synchronous shared-memory scans.

Ampere shaders sometimes communicate between lanes through STS/LDS without BAR or
MEMBAR. A compiler may move a lane's LDS ahead of its earlier STS because the
addresses do not alias within that lane. Place a memory-only workgroup fence at
each LDS/LDSM reachable from an STS without an intervening fence. This is not
an execution barrier and is therefore safe in divergent control flow.
"""

from collections import deque


def _predicated(insn) -> bool:
    return (
        insn.v("Pg", 7) != 7
        or bool(insn.v("Pg@not", 0))
        or insn.v("UPg", 7) != 7
        or bool(insn.v("UPg@not", 0))
        or insn.v("Pp", 7) != 7
        or bool(insn.v("Pp@not", 0))
    )


def shared_load_fences(insns, brx_targets=()):
    """Return instruction PCs needing groupMemoryBarrier() before their shared load.

    A two-state forward data-flow analysis follows the actual branch graph. The
    pending bit means that at least one path reaches this instruction after an
    STS and before a MEMBAR/BAR. Loops are handled by the worklist fixed point.
    """
    if not insns:
        return set()

    pc_to_index = {insn.pc: n for n, insn in enumerate(insns)}
    all_indices = set(range(len(insns)))
    targets = set(brx_targets)
    successors = [set() for _ in insns]
    return_sites = {
        pc_to_index[insn.pc + 16]
        for insn in insns
        if insn.op == "CALL" and insn.pc + 16 in pc_to_index
    }

    def add_pc(edges, pc):
        if pc in pc_to_index:
            edges.add(pc_to_index[pc])

    for n, insn in enumerate(insns):
        op = insn.op
        fallthrough = n + 1
        conditional = _predicated(insn)

        if op == "BRA":
            add_pc(successors[n], insn.pc + 16 + 4 * insn.sv("sImm"))
            if conditional and fallthrough < len(insns):
                successors[n].add(fallthrough)
        elif op in ("BRX", "BRXU"):
            for pc in targets:
                add_pc(successors[n], pc)
            if conditional and fallthrough < len(insns):
                successors[n].add(fallthrough)
            if not successors[n]:
                successors[n].update(all_indices)
        elif op == "CALL":
            offset = insn.sv("Sa")
            add_pc(successors[n], insn.pc + 16 + 4 * offset)
            if conditional and fallthrough < len(insns):
                successors[n].add(fallthrough)
        elif op == "RET":
            successors[n].update(return_sites)
        elif op == "EXIT":
            if conditional and fallthrough < len(insns):
                successors[n].add(fallthrough)
        elif fallthrough < len(insns):
            successors[n].add(fallthrough)

    # Each node may be visited once for each incoming pending state.
    seen = [set() for _ in insns]
    work = deque([(0, False)])
    fences = set()
    while work:
        n, pending = work.popleft()
        if pending in seen[n]:
            continue
        seen[n].add(pending)
        insn = insns[n]
        op = insn.op

        if op in ("MEMBAR", "BAR"):
            # A predicated barrier may be skipped by some lanes; keep the
            # pending state conservatively in that case.
            if not _predicated(insn):
                pending = False
        elif op == "STS":
            pending = True
        elif op in ("LDS", "LDSM"):
            if pending:
                fences.add(insn.pc)
                # A predicated load does not fence lanes which skip it.
                if not _predicated(insn):
                    pending = False

        for nxt in successors[n]:
            work.append((nxt, pending))

    return fences
