"""
git_history.py -- Per-file commit frequency and churn via gitpython.

WHY: High-churn files are unstable. When a binding file is touched frequently,
     the effort estimate for implementing stubs in it should be higher (more
     merge risk, moving target). last_touched informs the report age column.
"""

from __future__ import annotations

import datetime
from pathlib import Path

try:
    from git import Repo
    _GIT_AVAILABLE = True
except ImportError:
    _GIT_AVAILABLE = False


class FileHistory:
    def __init__(self, last_touched: str = "unknown", churn: int = 0):
        self.last_touched = last_touched
        self.churn = churn


def get_file_histories(
    repo_root: Path, file_rel_paths: list[str], days: int = 90
) -> dict[str, FileHistory]:
    """
    Return a dict mapping relative file path -> FileHistory.
    Falls back gracefully if gitpython is unavailable or repo is bare.
    """
    default = FileHistory()

    if not _GIT_AVAILABLE:
        return {p: FileHistory() for p in file_rel_paths}

    try:
        repo = Repo(str(repo_root))
    except Exception:
        return {p: FileHistory() for p in file_rel_paths}

    cutoff = datetime.datetime.utcnow().replace(tzinfo=datetime.timezone.utc)
    cutoff = cutoff - datetime.timedelta(days=days)

    result: dict[str, FileHistory] = {}
    for rel_path in file_rel_paths:
        if not rel_path:
            result[rel_path] = FileHistory()
            continue
        try:
            commits = list(repo.iter_commits(paths=rel_path, max_count=500))
            if not commits:
                result[rel_path] = FileHistory()
                continue

            last_dt = commits[0].committed_datetime
            # Normalize to UTC-aware for comparison
            if last_dt.tzinfo is None:
                last_dt = last_dt.replace(tzinfo=datetime.timezone.utc)
            last_iso = last_dt.date().isoformat()

            recent_count = 0
            for c in commits:
                dt = c.committed_datetime
                if dt.tzinfo is None:
                    dt = dt.replace(tzinfo=datetime.timezone.utc)
                if dt >= cutoff:
                    recent_count += 1

            result[rel_path] = FileHistory(last_touched=last_iso, churn=recent_count)
        except Exception:
            result[rel_path] = FileHistory()

    return result


def enrich_entries_with_git(entries: list, repo_root: Path) -> None:
    """Mutate gap entries in-place to add last_touched and churn."""
    paths = list({e.bnd_file for e in entries if e.bnd_file})
    histories = get_file_histories(repo_root, paths)

    for entry in entries:
        if entry.bnd_file and entry.bnd_file in histories:
            h = histories[entry.bnd_file]
            entry.last_touched = h.last_touched
            entry.churn = h.churn
            # Adjust effort upward for high-churn files (moving target)
            if h.churn >= 5:
                entry.effort = min(entry.effort + 1, 10)
