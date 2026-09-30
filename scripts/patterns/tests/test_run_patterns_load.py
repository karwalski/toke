"""131.65: bench/patterns/run_patterns.py stamps load_warning against the protocol floor.

`--max-load` decides whether a run may start; it must not decide whether the
results file admits the machine was busy.  Load is sampled at start, middle and
end, and the peak is what is judged.
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "bench", "patterns"))
import run_patterns as rp  # noqa: E402


def test_floor_is_two_and_independent_of_max_load():
    assert rp.LOAD_WARNING_FLOOR == 2.0
    # the 131.25 case: --max-load 8, load ~5 -> the old code stamped false
    stamp = rp.load_stamp({"start": 5.0, "mid": 5.0, "end": 5.0}, max_load=8.0)
    assert stamp["load_warning"] is True
    assert stamp["max_load"] == 8.0 and stamp["max_load_override"] is True


def test_mid_run_storm_is_not_invisible():
    # quiet at start and end, loaded in the middle: a start-only sample missed it
    stamp = rp.load_stamp({"start": 0.8, "mid": 9.5, "end": 1.1}, max_load=2.0)
    assert stamp["load_warning"] is True
    assert stamp["loadavg_1min_max"] == 9.5
    assert stamp["loadavg_1min"] == 0.8                 # start sample, for older readers
    assert stamp["loadavg_1min_samples"] == {"start": 0.8, "mid": 9.5, "end": 1.1}
    assert stamp["max_load_override"] is False


def test_quiet_run_is_clean_even_with_a_high_max_load():
    stamp = rp.load_stamp({"start": 1.2, "mid": 1.9, "end": 2.0}, max_load=8.0)
    assert stamp["load_warning"] is False                # 2.0 is not above the floor
    assert stamp["max_load_override"] is True
