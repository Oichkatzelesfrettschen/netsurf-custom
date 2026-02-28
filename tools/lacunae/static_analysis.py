"""
static_analysis.py -- Ingest cppcheck and clang-tidy results into gap annotations.

WHY: Static analysis tools may flag issues in binding C files. Attaching
     hit counts to gap entries tells us which gaps also have associated
     code-quality problems in their surrounding code.
"""

from __future__ import annotations

import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

from lacunae.gap_matrix import GapEntry


# ---------------------------------------------------------------------------
# cppcheck
# ---------------------------------------------------------------------------

def run_cppcheck(bnd_dir: Path, repo_root: Path) -> dict[str, int]:
    """
    Run cppcheck on generated binding C files and return a dict
    mapping filename -> hit count.

    WHY: nsgenbind generates C from .bnd files. cppcheck catches issues
         that would otherwise only appear at runtime.
    """
    # Look for generated binding C in build dir, fall back to source
    search_dirs = [
        repo_root / "build",
        repo_root / "content" / "handlers" / "javascript" / "duktape",
    ]
    c_files = []
    for d in search_dirs:
        if d.exists():
            c_files.extend(d.rglob("*.c"))

    if not c_files:
        return {}

    try:
        result = subprocess.run(
            ["cppcheck", "--xml", "--quiet", "--enable=all", "--"]
            + [str(f) for f in c_files[:50]],  # cap at 50 to avoid timeout
            capture_output=True, text=True, timeout=60,
        )
        return _parse_cppcheck_xml(result.stderr)
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return {}


def _parse_cppcheck_xml(xml_text: str) -> dict[str, int]:
    """Parse cppcheck XML output and return filename -> error count."""
    counts: dict[str, int] = {}
    try:
        root = ET.fromstring(xml_text)
        for error in root.iter("error"):
            for location in error.findall("location"):
                fname = location.get("file", "")
                if fname:
                    key = Path(fname).name
                    counts[key] = counts.get(key, 0) + 1
    except ET.ParseError:
        pass
    return counts


# ---------------------------------------------------------------------------
# clang-tidy (YAML output)
# ---------------------------------------------------------------------------

def parse_clang_tidy_yaml(yaml_path: Path) -> dict[str, int]:
    """
    Parse a clang-tidy YAML diagnostics file and return filename -> hit count.
    WHY: CI runs clang-tidy separately; this ingests its results for the report.
    """
    counts: dict[str, int] = {}
    if not yaml_path.exists():
        return counts
    try:
        import yaml  # type: ignore
        data = yaml.safe_load(yaml_path.read_text())
        for diag in data.get("Diagnostics", []):
            loc = diag.get("DiagnosticMessage", {}).get("FilePath", "")
            if loc:
                key = Path(loc).name
                counts[key] = counts.get(key, 0) + 1
    except Exception:
        pass
    return counts


# ---------------------------------------------------------------------------
# Enrichment
# ---------------------------------------------------------------------------

def enrich_entries_with_cppcheck(entries: list[GapEntry], repo_root: Path) -> None:
    """Add cppcheck hit counts to gap entries in-place."""
    bnd_dir = repo_root / "content" / "handlers" / "javascript" / "duktape"
    counts = run_cppcheck(bnd_dir, repo_root)
    for entry in entries:
        if entry.bnd_file:
            fname = Path(entry.bnd_file).stem + ".c"
            entry.cppcheck_hits = counts.get(fname, 0)
