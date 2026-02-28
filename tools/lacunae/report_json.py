"""
report_json.py -- Write lacunae-gaps.json to the repo root.

WHY: Machine-readable output lets CI pipelines and the diff command compare
     snapshots across commits without re-parsing the source every time.
"""

from __future__ import annotations

import dataclasses
import datetime
import json
import subprocess
from pathlib import Path
from typing import Any

from lacunae.gap_matrix import GapEntry


def _git_commit(repo_root: Path) -> str:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, cwd=repo_root, check=True,
        )
        return result.stdout.strip()
    except Exception:
        return "unknown"


def entry_to_dict(e: GapEntry) -> dict[str, Any]:
    return dataclasses.asdict(e)


def write_json(entries: list[GapEntry], repo_root: Path, out_path: Path | None = None) -> Path:
    """Write gaps.json and return the output path."""
    if out_path is None:
        out_path = repo_root / "lacunae-gaps.json"

    payload = {
        "generated": datetime.datetime.utcnow().isoformat(timespec="seconds") + "Z",
        "commit": _git_commit(repo_root),
        "gaps": [entry_to_dict(e) for e in entries],
    }
    out_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return out_path


def load_json(path: Path) -> dict[str, Any]:
    """Load a previously written gaps.json file."""
    return json.loads(path.read_text(encoding="utf-8"))
