"""
cross_ref.py -- Cross-reference index: which .bnd files include or reference which.

WHY: Understanding dependency relationships between bindings helps prioritize
     implementation order. A class that many others depend on is higher value.
"""

from __future__ import annotations

import re
from pathlib import Path


_INCLUDE_RE = re.compile(r'#include\s+"(\w+)\.bnd"')
_INIT_RE = re.compile(r'^\s*init\s+(\w+)\s*\(', re.MULTILINE)


def build_cross_ref(bnd_dir: Path) -> dict[str, set[str]]:
    """
    Return a dict: class_name -> set of class names it references.
    WHY: Enables graph rendering and topological ordering of gaps.
    """
    refs: dict[str, set[str]] = {}

    # Parse netsurf.bnd for #include and bare init declarations
    netsurf_bnd = bnd_dir / "netsurf.bnd"
    registered: set[str] = set()
    if netsurf_bnd.exists():
        text = netsurf_bnd.read_text(encoding="utf-8", errors="replace")
        for m in _INCLUDE_RE.finditer(text):
            registered.add(m.group(1))
        for m in _INIT_RE.finditer(text):
            registered.add(m.group(1))

    # For each .bnd file, find what it includes or init-declares
    for bnd_path in sorted(bnd_dir.glob("*.bnd")):
        if bnd_path.name == "netsurf.bnd":
            continue
        cls_name = bnd_path.stem
        text = bnd_path.read_text(encoding="utf-8", errors="replace")
        included = set()
        for m in _INCLUDE_RE.finditer(text):
            included.add(m.group(1))
        # init X(... ::parent) pattern implies parent dependency
        for m in re.finditer(r'init\s+\w+\([^)]*::(\w+)\)', text):
            included.add(m.group(1))
        refs[cls_name] = included

    return refs


def in_degree(refs: dict[str, set[str]]) -> dict[str, int]:
    """Return how many classes each class is referenced by (in-degree)."""
    counts: dict[str, int] = {}
    for targets in refs.values():
        for t in targets:
            counts[t] = counts.get(t, 0) + 1
    return counts
