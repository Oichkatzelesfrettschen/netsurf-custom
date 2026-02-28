"""
scorer.py -- Priority score computation for gap entries.

Formula:
    score = (impact * coverage_weight) / effort
    coverage_weight = 1 + log2(1 + wpt_tests)

WHY: This formula rewards gaps that are high-impact, have many WPT tests
     (meaning we gain visible test coverage by fixing them), and require
     relatively little effort to implement.
"""

from __future__ import annotations

import math
from lacunae.gap_matrix import GapEntry


def compute_score(entry: GapEntry) -> float:
    """Return the priority score for one gap entry."""
    if entry.status == "done":
        return 0.0
    coverage_weight = 1.0 + math.log2(1.0 + entry.wpt_tests)
    effort = max(entry.effort, 1)
    return round((entry.impact * coverage_weight) / effort, 3)


def score_all(entries: list[GapEntry]) -> list[GapEntry]:
    """Compute scores for all entries in-place; return sorted by score descending."""
    for e in entries:
        e.score = compute_score(e)
    return sorted(entries, key=lambda e: e.score, reverse=True)
