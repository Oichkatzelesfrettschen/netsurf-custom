"""
report_terminal.py -- Rich terminal tables and panels for lacunae output.

WHY: A color-coded terminal view gives immediate feedback during development
     without needing to open a browser or parse JSON.
"""

from __future__ import annotations

from lacunae.gap_matrix import GapEntry

try:
    from rich.console import Console
    from rich.table import Table
    from rich.panel import Panel
    from rich.text import Text
    from rich import box
    _RICH_AVAILABLE = True
except ImportError:
    _RICH_AVAILABLE = False


_STATUS_COLOR = {
    "absent":  "red",
    "stub":    "yellow",
    "partial": "cyan",
    "done":    "green",
}


def print_summary(entries: list[GapEntry]) -> None:
    """Print a summary panel with counts by status."""
    counts = {}
    for e in entries:
        counts[e.status] = counts.get(e.status, 0) + 1

    total = len(entries)
    if _RICH_AVAILABLE:
        console = Console()
        lines = []
        for status in ("absent", "stub", "partial", "done"):
            n = counts.get(status, 0)
            pct = 100 * n / total if total else 0
            color = _STATUS_COLOR.get(status, "white")
            lines.append(f"[{color}]{status:8s}[/{color}]  {n:4d}  ({pct:5.1f}%)")
        lines.append(f"{'total':8s}  {total:4d}")
        console.print(Panel("\n".join(lines), title="Gap Summary", expand=False))
    else:
        print("=== Gap Summary ===")
        for status in ("absent", "stub", "partial", "done"):
            n = counts.get(status, 0)
            pct = 100 * n / total if total else 0
            print(f"  {status:8s}  {n:4d}  ({pct:5.1f}%)")
        print(f"  {'total':8s}  {total:4d}")


def print_top_gaps(entries: list[GapEntry], n: int = 20, status_filter: str | None = None) -> None:
    """Print a ranked table of the top N gaps."""
    filtered = entries
    if status_filter:
        filtered = [e for e in entries if e.status == status_filter]
    # entries already sorted by score descending
    top = filtered[:n]

    if _RICH_AVAILABLE:
        console = Console()
        table = Table(
            title=f"Top {n} Gaps" + (f" (status={status_filter})" if status_filter else ""),
            box=box.SIMPLE,
            show_lines=False,
        )
        table.add_column("ID", style="dim", width=10)
        table.add_column("Class", width=22)
        table.add_column("Member", width=22)
        table.add_column("Kind", width=8)
        table.add_column("Status", width=8)
        table.add_column("Score", justify="right", width=7)
        table.add_column("Impact", justify="right", width=7)
        table.add_column("Effort", justify="right", width=7)
        table.add_column("WPT", justify="right", width=5)

        for e in top:
            color = _STATUS_COLOR.get(e.status, "white")
            table.add_row(
                e.id,
                e.class_name,
                e.member,
                e.kind,
                Text(e.status, style=color),
                f"{e.score:.2f}",
                str(e.impact),
                str(e.effort),
                str(e.wpt_tests),
            )
        console.print(table)
    else:
        header = f"{'ID':10}  {'Class':22}  {'Member':22}  {'Kind':8}  {'Status':8}  {'Score':7}  {'WPT':5}"
        print(header)
        print("-" * len(header))
        for e in top:
            print(f"{e.id:10}  {e.class_name:22}  {e.member:22}  {e.kind:8}  {e.status:8}  {e.score:7.2f}  {e.wpt_tests:5}")
