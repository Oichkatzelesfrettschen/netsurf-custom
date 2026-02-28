"""
diff.py -- Compare current gaps.json vs a baseline to detect regressions.

WHY: CI must be able to fail on new gaps (regressions) and report progress
     (gaps resolved since baseline) without human inspection of JSON diffs.

Exit code:
  0 -- no regressions (new absent/stub gaps relative to baseline)
  1 -- regressions found (new gaps appeared or existing gaps got worse)
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any


def _gap_key(g: dict) -> str:
    return g["class_name"] + "::" + g["member"]


def _gap_status(g: dict) -> str:
    return g.get("status", "stub")


_STATUS_RANK = {"done": 0, "partial": 1, "stub": 2, "absent": 3}


def diff_gaps(current_path: Path, baseline_path: Path) -> int:
    """
    Compare current vs baseline. Print a summary. Return exit code.
    0 = no regressions, 1 = regressions found.
    """
    current = json.loads(current_path.read_text(encoding="utf-8"))
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))

    cur_gaps = {_gap_key(g): g for g in current.get("gaps", [])}
    base_gaps = {_gap_key(g): g for g in baseline.get("gaps", [])}

    new_gaps = []       # present in current, absent in baseline
    regressions = []    # status got worse
    improvements = []   # status got better
    resolved = []       # present in baseline as stub/absent, now done

    for key, gap in cur_gaps.items():
        if key not in base_gaps:
            if gap.get("status") in ("absent", "stub"):
                new_gaps.append(gap)
        else:
            base = base_gaps[key]
            cur_rank = _STATUS_RANK.get(gap.get("status", "stub"), 2)
            base_rank = _STATUS_RANK.get(base.get("status", "stub"), 2)
            if cur_rank > base_rank:
                regressions.append((base, gap))
            elif cur_rank < base_rank:
                if gap.get("status") == "done":
                    resolved.append(gap)
                else:
                    improvements.append((base, gap))

    print(f"=== lacunae diff ===")
    print(f"Current : {current_path}  ({current.get('commit', '?')})")
    print(f"Baseline: {baseline_path}  ({baseline.get('commit', '?')})")
    print()
    print(f"  New gaps      : {len(new_gaps)}")
    print(f"  Regressions   : {len(regressions)}")
    print(f"  Improvements  : {len(improvements)}")
    print(f"  Resolved      : {len(resolved)}")

    if new_gaps:
        print("\nNew gaps:")
        for g in new_gaps[:10]:
            print(f"  {g.get('id','?'):10}  {g.get('class_name','?')}::{g.get('member','?')}  [{g.get('status','?')}]")
        if len(new_gaps) > 10:
            print(f"  ... and {len(new_gaps) - 10} more")

    if regressions:
        print("\nRegressions (status got worse):")
        for base, cur in regressions[:10]:
            print(f"  {cur.get('id','?'):10}  {cur.get('class_name','?')}::{cur.get('member','?')}"
                  f"  {base.get('status','?')} -> {cur.get('status','?')}")

    if resolved:
        print("\nResolved:")
        for g in resolved[:10]:
            print(f"  {g.get('id','?'):10}  {g.get('class_name','?')}::{g.get('member','?')}")

    has_regression = bool(new_gaps or regressions)
    if has_regression:
        print("\nFAIL: regressions found.")
        return 1
    print("\nOK: no regressions.")
    return 0
