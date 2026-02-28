"""
cli.py -- Typer CLI for the lacunae gap analysis tool.

Commands:
  scan    -- parse .bnd files, mine git, write lacunae-gaps.json
  report  -- render HTML + terminal reports from lacunae-gaps.json
  gaps    -- print ranked gap table to terminal
  resolve -- print resolution outline for one gap
  diff    -- compare current vs baseline JSON; exit 1 on regressions
  graph   -- render .bnd dependency graph to SVG/DOT
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional

try:
    import typer
    _TYPER = True
except ImportError:
    _TYPER = False

if _TYPER:
    app = typer.Typer(
        name="lacunae",
        help="NetSurf JS binding gap analysis and resolution tool.",
        add_completion=False,
    )
else:
    # Minimal fallback app when typer is not installed
    class _FallbackApp:
        def command(self, *a, **kw):
            return lambda f: f
        def __call__(self, *a, **kw):
            _fallback_main()
    app = _FallbackApp()


def _find_repo_root() -> Path:
    """Walk up from CWD to find the git repo root."""
    p = Path.cwd()
    for _ in range(10):
        if (p / ".git").exists():
            return p
        p = p.parent
    return Path.cwd()


def _load_entries(gaps_json: Path):
    """Load gap entries from a JSON file."""
    import dataclasses
    import json
    from lacunae.gap_matrix import GapEntry

    data = json.loads(gaps_json.read_text(encoding="utf-8"))
    field_names = {f.name for f in dataclasses.fields(GapEntry)}
    entries = []
    for g in data.get("gaps", []):
        entries.append(GapEntry(**{k: g[k] for k in field_names if k in g}))
    return entries, data.get("generated", ""), data.get("commit", "")


# ---------------------------------------------------------------------------
# scan
# ---------------------------------------------------------------------------

if _TYPER:
    @app.command()
    def scan(
        repo_root: Optional[Path] = typer.Option(None, "--repo-root", help="Repo root (default: auto-detect)"),
        out: Optional[Path] = typer.Option(None, "--out", help="Output JSON path (default: <repo>/lacunae-gaps.json)"),
        skip_git: bool = typer.Option(False, "--skip-git", help="Skip git history enrichment"),
        skip_cppcheck: bool = typer.Option(False, "--skip-cppcheck", help="Skip cppcheck enrichment"),
    ):
        """Parse all .bnd files, mine git history, write lacunae-gaps.json."""
        _do_scan(repo_root, out, skip_git, skip_cppcheck)
else:
    def scan(repo_root=None, out=None, skip_git=False, skip_cppcheck=False):
        _do_scan(repo_root, out, skip_git, skip_cppcheck)


def _do_scan(repo_root, out, skip_git, skip_cppcheck):
    from lacunae.gap_matrix import build_gap_matrix
    from lacunae.scorer import score_all
    from lacunae.git_history import enrich_entries_with_git
    from lacunae.static_analysis import enrich_entries_with_cppcheck
    from lacunae.report_json import write_json

    if repo_root is None:
        repo_root = _find_repo_root()
    repo_root = Path(repo_root)

    print(f"[lacunae] Scanning {repo_root} ...")
    entries = build_gap_matrix(repo_root)
    print(f"[lacunae] Found {len(entries)} gap entries before scoring.")

    if not skip_git:
        print("[lacunae] Enriching with git history ...")
        enrich_entries_with_git(entries, repo_root)

    if not skip_cppcheck:
        print("[lacunae] Running cppcheck enrichment ...")
        enrich_entries_with_cppcheck(entries, repo_root)

    entries = score_all(entries)

    out_path = write_json(entries, repo_root, out_path=out)
    print(f"[lacunae] Wrote {len(entries)} gaps to {out_path}")

    # Print quick summary
    from lacunae.report_terminal import print_summary
    print_summary(entries)


# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------

if _TYPER:
    @app.command()
    def report(
        repo_root: Optional[Path] = typer.Option(None, "--repo-root"),
        gaps_json: Optional[Path] = typer.Option(None, "--gaps-json", help="Input JSON (default: lacunae-gaps.json)"),
        out_dir: Optional[Path] = typer.Option(None, "--out-dir", help="Output dir (default: lacunae-report/)"),
    ):
        """Render HTML + terminal reports from lacunae-gaps.json."""
        _do_report(repo_root, gaps_json, out_dir)
else:
    def report(repo_root=None, gaps_json=None, out_dir=None):
        _do_report(repo_root, gaps_json, out_dir)


def _do_report(repo_root, gaps_json, out_dir):
    from lacunae.report_html import write_html_report
    from lacunae.report_terminal import print_summary, print_top_gaps

    if repo_root is None:
        repo_root = _find_repo_root()
    repo_root = Path(repo_root)

    if gaps_json is None:
        gaps_json = repo_root / "lacunae-gaps.json"
    if not gaps_json.exists():
        print(f"[lacunae] ERROR: {gaps_json} not found. Run 'lacunae scan' first.")
        sys.exit(1)

    if out_dir is None:
        out_dir = repo_root / "lacunae-report"

    entries, generated, commit = _load_entries(Path(gaps_json))
    print_summary(entries)
    write_html_report(entries, generated, commit, Path(out_dir))
    print(f"[lacunae] HTML report written to {out_dir}/index.html")


# ---------------------------------------------------------------------------
# gaps
# ---------------------------------------------------------------------------

if _TYPER:
    @app.command()
    def gaps(
        repo_root: Optional[Path] = typer.Option(None, "--repo-root"),
        gaps_json: Optional[Path] = typer.Option(None, "--gaps-json"),
        top: int = typer.Option(20, "--top", "-n", help="Number of gaps to show"),
        status: Optional[str] = typer.Option(None, "--status", help="Filter by status (absent|stub|partial|done)"),
    ):
        """Print ranked gap table to terminal."""
        _do_gaps(repo_root, gaps_json, top, status)
else:
    def gaps(repo_root=None, gaps_json=None, top=20, status=None):
        _do_gaps(repo_root, gaps_json, top, status)


def _do_gaps(repo_root, gaps_json, top, status):
    from lacunae.report_terminal import print_summary, print_top_gaps

    if repo_root is None:
        repo_root = _find_repo_root()
    repo_root = Path(repo_root)

    if gaps_json is None:
        gaps_json = repo_root / "lacunae-gaps.json"
    if not gaps_json.exists():
        print(f"[lacunae] ERROR: {gaps_json} not found. Run 'lacunae scan' first.")
        sys.exit(1)

    entries, _, _ = _load_entries(Path(gaps_json))
    print_summary(entries)
    print_top_gaps(entries, n=top, status_filter=status)


# ---------------------------------------------------------------------------
# resolve
# ---------------------------------------------------------------------------

if _TYPER:
    @app.command()
    def resolve(
        gap_id: str = typer.Argument(help="Gap ID, e.g. GAP-0001"),
        repo_root: Optional[Path] = typer.Option(None, "--repo-root"),
        gaps_json: Optional[Path] = typer.Option(None, "--gaps-json"),
    ):
        """Print a copy-paste-ready resolution outline for one gap."""
        _do_resolve(gap_id, repo_root, gaps_json)
else:
    def resolve(gap_id, repo_root=None, gaps_json=None):
        _do_resolve(gap_id, repo_root, gaps_json)


def _do_resolve(gap_id, repo_root, gaps_json):
    from lacunae.resolver import resolve_gap

    if repo_root is None:
        repo_root = _find_repo_root()
    repo_root = Path(repo_root)

    if gaps_json is None:
        gaps_json = repo_root / "lacunae-gaps.json"
    if not gaps_json.exists():
        print(f"[lacunae] ERROR: {gaps_json} not found. Run 'lacunae scan' first.")
        sys.exit(1)

    entries, _, _ = _load_entries(Path(gaps_json))
    # Normalize ID
    gap_id_upper = gap_id.upper()
    if not gap_id_upper.startswith("GAP-"):
        gap_id_upper = "GAP-" + gap_id_upper

    for e in entries:
        if e.id == gap_id_upper:
            print(resolve_gap(e))
            return

    print(f"[lacunae] ERROR: gap ID '{gap_id}' not found.")
    sys.exit(1)


# ---------------------------------------------------------------------------
# diff
# ---------------------------------------------------------------------------

if _TYPER:
    @app.command()
    def diff(
        repo_root: Optional[Path] = typer.Option(None, "--repo-root"),
        gaps_json: Optional[Path] = typer.Option(None, "--gaps-json"),
        baseline: Optional[Path] = typer.Option(None, "--baseline", help="Baseline JSON path"),
    ):
        """Compare current gaps vs baseline; exit 1 on regressions."""
        _do_diff(repo_root, gaps_json, baseline)
else:
    def diff(repo_root=None, gaps_json=None, baseline=None):
        _do_diff(repo_root, gaps_json, baseline)


def _do_diff(repo_root, gaps_json, baseline):
    from lacunae.diff import diff_gaps

    if repo_root is None:
        repo_root = _find_repo_root()
    repo_root = Path(repo_root)

    if gaps_json is None:
        gaps_json = repo_root / "lacunae-gaps.json"
    if baseline is None:
        baseline = repo_root / "test" / "lacunae-baseline.json"

    if not Path(gaps_json).exists():
        print(f"[lacunae] ERROR: {gaps_json} not found. Run 'lacunae scan' first.")
        sys.exit(1)
    if not Path(baseline).exists():
        print(f"[lacunae] No baseline at {baseline}; treating as no regressions.")
        sys.exit(0)

    rc = diff_gaps(Path(gaps_json), Path(baseline))
    sys.exit(rc)


# ---------------------------------------------------------------------------
# graph
# ---------------------------------------------------------------------------

if _TYPER:
    @app.command()
    def graph(
        repo_root: Optional[Path] = typer.Option(None, "--repo-root"),
        gaps_json: Optional[Path] = typer.Option(None, "--gaps-json"),
        out_dir: Optional[Path] = typer.Option(None, "--out-dir"),
        fmt: str = typer.Option("svg", "--format", help="Output format: svg | png | pdf | dot"),
    ):
        """Render the .bnd dependency graph to SVG/DOT."""
        _do_graph(repo_root, gaps_json, out_dir, fmt)
else:
    def graph(repo_root=None, gaps_json=None, out_dir=None, fmt="svg"):
        _do_graph(repo_root, gaps_json, out_dir, fmt)


def _do_graph(repo_root, gaps_json, out_dir, fmt):
    from lacunae.graph import render_graph

    if repo_root is None:
        repo_root = _find_repo_root()
    repo_root = Path(repo_root)

    if gaps_json is None:
        gaps_json = repo_root / "lacunae-gaps.json"

    if out_dir is None:
        out_dir = repo_root / "lacunae-report"

    entries = []
    if Path(gaps_json).exists():
        entries, _, _ = _load_entries(Path(gaps_json))

    out_path = Path(out_dir) / f"binding-graph.{fmt}"
    result = render_graph(repo_root, entries, out_path=out_path, fmt=fmt)
    print(f"[lacunae] Graph written to {result}")


# ---------------------------------------------------------------------------
# Fallback main for when typer is absent
# ---------------------------------------------------------------------------

def _fallback_main():
    import argparse

    parser = argparse.ArgumentParser(prog="lacunae")
    sub = parser.add_subparsers(dest="cmd")

    sub.add_parser("scan")
    sub.add_parser("report")
    p_gaps = sub.add_parser("gaps")
    p_gaps.add_argument("--top", type=int, default=20)
    p_gaps.add_argument("--status", default=None)
    p_resolve = sub.add_parser("resolve")
    p_resolve.add_argument("gap_id")
    sub.add_parser("diff")
    sub.add_parser("graph")

    args = parser.parse_args()
    if args.cmd == "scan":
        scan()
    elif args.cmd == "report":
        report()
    elif args.cmd == "gaps":
        gaps(top=args.top, status=args.status)
    elif args.cmd == "resolve":
        resolve(args.gap_id)
    elif args.cmd == "diff":
        diff()
    elif args.cmd == "graph":
        graph()
    else:
        parser.print_help()
