"""Tests for control-flow-aware implicit shared-memory fences."""

import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sm86/translate"))
from glsl.shared_fence import shared_load_fences


@dataclass
class I:
    pc: int
    op: str
    fields: dict

    def v(self, name, default=None):
        return self.fields.get(name, default)

    def sv(self, name):
        return self.fields[name]


def prog(*ops):
    return [I(n * 16, op, fields) for n, (op, fields) in enumerate(ops)]


def test_downsweep_fences_each_store_load_pair():
    code = prog(("STS", {}), ("LDS", {}), ("STS", {}), ("LDS", {}), ("EXIT", {}))
    assert shared_load_fences(code) == {16, 48}


def test_one_fence_covers_a_run_of_stores_and_loads():
    code = prog(("STS", {}), ("STS", {}), ("LDS", {}), ("LDS", {}), ("EXIT", {}))
    assert shared_load_fences(code) == {32}


def test_explicit_membar_and_bar_cover_the_load():
    for barrier in ("MEMBAR", "BAR"):
        code = prog(("STS", {}), (barrier, {}), ("LDS", {}), ("EXIT", {}))
        assert shared_load_fences(code) == set()


def test_store_at_bottom_of_loop_reaches_load_at_top():
    # LDS; STS; BRA back to LDS; EXIT
    code = prog(("LDS", {}), ("STS", {}), ("BRA", {"sImm": -2}), ("EXIT", {}))
    assert shared_load_fences(code) == {0}


def test_store_on_one_branch_reaches_join_load():
    # BRA.P0 to the join; STS; BRA to join; LDS
    code = prog(
        ("BRA", {"sImm": 2, "Pg": 0}),
        ("STS", {}),
        ("BRA", {"sImm": 1}),
        ("LDS", {}),
        ("EXIT", {}),
    )
    assert shared_load_fences(code) == {48}


def test_predicated_load_does_not_clear_pending_store():
    code = prog(("STS", {}), ("LDS", {"Pg": 0}), ("LDS", {}), ("EXIT", {}))
    assert shared_load_fences(code) == {16, 32}


def test_load_before_store_and_store_at_end_need_no_fence():
    code = prog(("LDS", {}), ("STS", {}), ("EXIT", {}))
    assert shared_load_fences(code) == set()


def test_ldsm_is_a_shared_load():
    code = prog(("STS", {}), ("LDSM", {}), ("EXIT", {}))
    assert shared_load_fences(code) == {16}
