"""
report_html.py -- Jinja2 HTML report generator.

WHY: A self-contained HTML report (no CDN) can be opened offline, shared
     via artifact upload, and viewed without a server.
"""

from __future__ import annotations

from pathlib import Path

try:
    from jinja2 import Environment, FileSystemLoader
    _JINJA_AVAILABLE = True
except ImportError:
    _JINJA_AVAILABLE = False

from lacunae.gap_matrix import GapEntry
from lacunae.resolver import resolve_gap
from lacunae.report_json import load_json


_TEMPLATE_DIR = Path(__file__).parent / "templates"


def write_html_report(
    entries: list[GapEntry],
    generated: str,
    commit: str,
    out_dir: Path,
) -> Path:
    """Write index.html and per-gap detail pages. Returns out_dir."""
    out_dir.mkdir(parents=True, exist_ok=True)

    counts = {}
    for e in entries:
        counts[e.status] = counts.get(e.status, 0) + 1
    for s in ("absent", "stub", "partial", "done"):
        counts.setdefault(s, 0)
    total = len(entries)

    if _JINJA_AVAILABLE:
        env = Environment(
            loader=FileSystemLoader(str(_TEMPLATE_DIR)),
            autoescape=True,
            trim_blocks=True,
            lstrip_blocks=True,
        )
        # Index page
        tmpl = env.get_template("report.html.j2")
        index_html = tmpl.render(
            gaps=entries,
            counts=counts,
            total=total,
            generated=generated,
            commit=commit,
        )
        (out_dir / "index.html").write_text(index_html, encoding="utf-8")

        # Detail pages
        detail_tmpl = env.get_template("gap_detail.html.j2")
        for e in entries:
            resolution = resolve_gap(e)
            detail_html = detail_tmpl.render(entry=e, resolution=resolution)
            num = e.id[4:]  # strip "GAP-"
            (out_dir / f"GAP-{num}.html").write_text(detail_html, encoding="utf-8")
    else:
        # Minimal fallback without jinja2
        _write_fallback_html(entries, counts, total, generated, commit, out_dir)

    return out_dir


def _write_fallback_html(entries, counts, total, generated, commit, out_dir):
    rows = []
    for e in entries:
        rows.append(
            f"<tr><td>{e.id}</td><td>{e.class_name}</td><td>{e.member}</td>"
            f"<td>{e.kind}</td><td class='{e.status}'>{e.status}</td>"
            f"<td>{e.score:.2f}</td><td>{e.wpt_tests}</td></tr>"
        )
    html = (
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>NetSurf Gap Report</title></head><body>"
        f"<h1>NetSurf JS Binding Gap Report</h1>"
        f"<p>Generated: {generated} | Commit: {commit}</p>"
        f"<p>absent={counts.get('absent',0)} stub={counts.get('stub',0)} "
        f"partial={counts.get('partial',0)} done={counts.get('done',0)} total={total}</p>"
        "<table border='1'><tr><th>ID</th><th>Class</th><th>Member</th>"
        "<th>Kind</th><th>Status</th><th>Score</th><th>WPT</th></tr>"
        + "".join(rows) + "</table></body></html>"
    )
    (out_dir / "index.html").write_text(html, encoding="utf-8")
