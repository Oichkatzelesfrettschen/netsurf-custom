#!/usr/bin/env python3
"""
lacunae.py -- NetSurf JS binding gap analysis (zero-dep consolidation).

WHY: The original tools/lacunae/ package had 15 modules, ~1,763 lines, and
     9 pip dependencies. The core value is ~300 lines: parse .bnd files,
     classify stubs, score, emit JSON, diff baseline.

Subcommands:
  scan  -- parse .bnd files, write lacunae-gaps.json
  gaps  -- print ranked gap table from lacunae-gaps.json
  diff  -- compare current vs baseline; exit 1 on regressions
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime
import json
import math
import re
import subprocess
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class BndDecl:
    class_name: str
    member: str
    kind: str
    bnd_file: str
    status: str = "stub"
    body_lines: int = 0
    line_no: int = 0


@dataclasses.dataclass
class IdlDecl:
    interface_name: str
    member_name: str
    kind: str          # "attribute", "readonly_attribute", "method", "constructor"
    spec_file: str


@dataclasses.dataclass
class GapEntry:
    id: str
    class_name: str
    member: str
    kind: str
    bnd_file: str
    status: str
    impact: int
    effort: int
    coverage: float
    score: float
    wpt_tests: int
    notes: str


# ---------------------------------------------------------------------------
# .bnd parser (ported from bnd_parser.py lines 44-167)
# ---------------------------------------------------------------------------

_DECL_RE = re.compile(
    r"^\s*(?P<kind>getter|setter|method|init|fini)\s+"
    r"(?P<class_>\w+)(?:::(?P<member>\w+))?\s*\(",
    re.ASCII,
)
_CLASS_RE = re.compile(r"^\s*class\s+(?P<name>\w+)\s*\{", re.ASCII)
_BODY_OPEN = re.compile(r"%\{")
_BODY_CLOSE = re.compile(r"%\}")
_TRIVIAL_LINE = re.compile(
    r"^\s*(return\s+0\s*;|/\*.*\*/|//.*|\s*)\s*$"
)


def _is_trivial_body(lines: list[str]) -> bool:
    meaningful = [
        l for l in lines
        if l.strip()
        and not l.strip().startswith("/*")
        and not l.strip().startswith("//")
        and not l.strip().endswith("*/")
    ]
    if not meaningful:
        return True
    return len(meaningful) == 1 and bool(_TRIVIAL_LINE.match(meaningful[0]))


def _parse_bnd_file(path: Path, repo_root: Path) -> list[BndDecl]:
    rel = str(path.relative_to(repo_root))
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    decls: list[BndDecl] = []

    i = 0
    while i < len(lines):
        line = lines[i]

        m = _CLASS_RE.match(line)
        if m:
            i += 1
            continue

        dm = _DECL_RE.match(line)
        if dm:
            kind = dm.group("kind")
            cls = dm.group("class_")
            member = dm.group("member") or cls
            decl_line = i + 1

            body_text: list[str] = []
            has_body = False
            j = i
            while j < min(i + 6, len(lines)):
                if _BODY_OPEN.search(lines[j]):
                    has_body = True
                    j += 1
                    depth = 1
                    while j < len(lines):
                        if _BODY_CLOSE.search(lines[j]):
                            depth -= 1
                            if depth == 0:
                                break
                        elif _BODY_OPEN.search(lines[j]):
                            depth += 1
                        body_text.append(lines[j])
                        j += 1
                    i = j + 1
                    break
                stripped = lines[j].strip()
                if stripped.endswith(";") and j > i:
                    i = j + 1
                    break
                j += 1
            else:
                i += 1

            status = "stub"
            if has_body and not _is_trivial_body(body_text):
                status = "done"

            decls.append(BndDecl(
                class_name=cls, member=member, kind=kind,
                bnd_file=rel, status=status,
                body_lines=len([l for l in body_text if l.strip()]),
                line_no=decl_line,
            ))
            continue

        i += 1

    return decls


def _parse_all_bnd(bnd_dir: Path, repo_root: Path) -> list[BndDecl]:
    decls: list[BndDecl] = []
    for p in sorted(bnd_dir.glob("*.bnd")):
        if p.name == "netsurf.bnd":
            continue
        decls.extend(_parse_bnd_file(p, repo_root))
    return decls


# ---------------------------------------------------------------------------
# Gap matrix (ported from gap_matrix.py)
# ---------------------------------------------------------------------------

ABSENT_APIS: list[dict] = [
    {"class": "XMLHttpRequest",   "member": "open",            "kind": "method", "impact": 9,  "effort": 8,  "wpt": 142, "notes": "No C implementation. Requires async fetch integration."},
    {"class": "XMLHttpRequest",   "member": "send",            "kind": "method", "impact": 9,  "effort": 8,  "wpt": 98,  "notes": "No C implementation."},
    {"class": "XMLHttpRequest",   "member": "setRequestHeader","kind": "method", "impact": 7,  "effort": 6,  "wpt": 45,  "notes": "No C implementation."},
    {"class": "XMLHttpRequest",   "member": "abort",           "kind": "method", "impact": 6,  "effort": 5,  "wpt": 30,  "notes": "No C implementation."},
    {"class": "XMLHttpRequest",   "member": "response",        "kind": "getter", "impact": 8,  "effort": 6,  "wpt": 112, "notes": "No C implementation."},
    {"class": "XMLHttpRequest",   "member": "status",          "kind": "getter", "impact": 8,  "effort": 4,  "wpt": 88,  "notes": "No C implementation."},
    {"class": "XMLHttpRequest",   "member": "readyState",      "kind": "getter", "impact": 8,  "effort": 4,  "wpt": 76,  "notes": "No C implementation."},
    {"class": "fetch",            "member": "fetch",           "kind": "method", "impact": 10, "effort": 10, "wpt": 320, "notes": "Requires ServiceWorker/Fetch integration."},
    {"class": "localStorage",     "member": "setItem",         "kind": "method", "impact": 8,  "effort": 7,  "wpt": 55,  "notes": "No persistent storage backend."},
    {"class": "localStorage",     "member": "getItem",         "kind": "method", "impact": 8,  "effort": 7,  "wpt": 55,  "notes": "Absent."},
    {"class": "localStorage",     "member": "removeItem",      "kind": "method", "impact": 6,  "effort": 5,  "wpt": 30,  "notes": "Absent."},
    {"class": "localStorage",     "member": "clear",           "kind": "method", "impact": 5,  "effort": 4,  "wpt": 20,  "notes": "Absent."},
    {"class": "sessionStorage",   "member": "setItem",         "kind": "method", "impact": 7,  "effort": 7,  "wpt": 40,  "notes": "Absent."},
    {"class": "sessionStorage",   "member": "getItem",         "kind": "method", "impact": 7,  "effort": 7,  "wpt": 40,  "notes": "Absent."},
    {"class": "MutationObserver", "member": "observe",         "kind": "method", "impact": 8,  "effort": 9,  "wpt": 180, "notes": "Requires libdom MutationObserver hooks."},
    {"class": "MutationObserver", "member": "disconnect",      "kind": "method", "impact": 6,  "effort": 7,  "wpt": 45,  "notes": "Absent."},
    {"class": "IntersectionObserver","member": "observe",      "kind": "method", "impact": 7,  "effort": 9,  "wpt": 95,  "notes": "Requires layout integration."},
    {"class": "ResizeObserver",   "member": "observe",         "kind": "method", "impact": 6,  "effort": 8,  "wpt": 60,  "notes": "Requires layout integration."},
    {"class": "WebSocket",        "member": "send",            "kind": "method", "impact": 7,  "effort": 9,  "wpt": 110, "notes": "No WebSocket backend."},
    {"class": "WebSocket",        "member": "close",           "kind": "method", "impact": 6,  "effort": 7,  "wpt": 50,  "notes": "Absent."},
    {"class": "crypto",           "member": "getRandomValues", "kind": "method", "impact": 6,  "effort": 4,  "wpt": 25,  "notes": "Could wrap /dev/urandom."},
    {"class": "performance",      "member": "mark",            "kind": "method", "impact": 4,  "effort": 4,  "wpt": 18,  "notes": "Absent. performance.now/timeOrigin done."},
    {"class": "requestAnimationFrame","member": "requestAnimationFrame","kind": "method","impact": 8,"effort": 6,"wpt": 85,"notes": "Needs scheduler integration."},
    {"class": "cancelAnimationFrame","member": "cancelAnimationFrame","kind": "method","impact": 6,"effort": 4,"wpt": 30,"notes": "Absent."},
    {"class": "queueMicrotask",   "member": "queueMicrotask", "kind": "method", "impact": 5,  "effort": 4,  "wpt": 20,  "notes": "Needs micro-task queue."},
    {"class": "Notification",     "member": "Notification",   "kind": "method", "impact": 5,  "effort": 8,  "wpt": 35,  "notes": "Requires OS notification backend."},
    {"class": "ServiceWorker",    "member": "register",        "kind": "method", "impact": 6,  "effort": 10, "wpt": 150, "notes": "XL effort -- service worker lifecycle."},
    {"class": "Cache",            "member": "match",           "kind": "method", "impact": 5,  "effort": 9,  "wpt": 80,  "notes": "Requires Cache API + storage."},
    {"class": "IndexedDB",        "member": "open",            "kind": "method", "impact": 6,  "effort": 10, "wpt": 200, "notes": "XL effort -- full IDB spec."},
    {"class": "FileReader",       "member": "readAsText",      "kind": "method", "impact": 6,  "effort": 6,  "wpt": 45,  "notes": "Needs Blob/File integration."},
    {"class": "Blob",             "member": "text",            "kind": "method", "impact": 6,  "effort": 5,  "wpt": 38,  "notes": "Absent."},
    {"class": "FormData",         "member": "append",          "kind": "method", "impact": 7,  "effort": 5,  "wpt": 60,  "notes": "Needed for XHR form submission."},
    {"class": "URLSearchParams",  "member": "append",          "kind": "method", "impact": 6,  "effort": 4,  "wpt": 30,  "notes": "Declared in URL.bnd but no body."},
]

_CATEGORY_DEFAULTS: dict[str, dict] = {
    "HTMLElement":          {"impact": 4, "effort": 3, "wpt": 5},
    "HTMLInputElement":     {"impact": 5, "effort": 3, "wpt": 8},
    "HTMLSelectElement":    {"impact": 5, "effort": 3, "wpt": 6},
    "HTMLFormElement":      {"impact": 5, "effort": 3, "wpt": 7},
    "HTMLAnchorElement":    {"impact": 5, "effort": 3, "wpt": 6},
    "HTMLImageElement":     {"impact": 4, "effort": 3, "wpt": 5},
    "Element":              {"impact": 6, "effort": 4, "wpt": 12},
    "Node":                 {"impact": 5, "effort": 4, "wpt": 10},
    "Document":             {"impact": 6, "effort": 4, "wpt": 14},
    "Window":               {"impact": 7, "effort": 5, "wpt": 20},
    "CSSStyleDeclaration":  {"impact": 6, "effort": 5, "wpt": 15},
    "CSSRule":              {"impact": 4, "effort": 4, "wpt": 8},
    "CSSStyleSheet":        {"impact": 5, "effort": 5, "wpt": 10},
    "KeyboardEvent":        {"impact": 6, "effort": 3, "wpt": 12},
    "MouseEvent":           {"impact": 6, "effort": 4, "wpt": 14},
    "Navigator":            {"impact": 5, "effort": 3, "wpt": 8},
    "Location":             {"impact": 6, "effort": 3, "wpt": 10},
    "History":              {"impact": 7, "effort": 4, "wpt": 18},
    "URL":                  {"impact": 6, "effort": 4, "wpt": 12},
    "URLSearchParams":      {"impact": 5, "effort": 3, "wpt": 8},
    "Storage":              {"impact": 7, "effort": 5, "wpt": 25},
    "Event":                {"impact": 5, "effort": 3, "wpt": 10},
    "default":              {"impact": 3, "effort": 3, "wpt": 3},
}

_KNOWN_DONE: set[tuple[str, str]] = {
    ("History", "back"),
    ("History", "forward"),
    ("History", "go"),
    ("History", "pushState"),
    ("History", "replaceState"),
    ("History", "length"),
    ("Element", "firstElementChild"),
    ("Element", "lastElementChild"),
    ("Element", "innerHTML"),
    ("TextDecoder", "decode"),
    ("TextDecoder", "encoding"),
    ("URL", "href"),
    ("URL", "origin"),
    ("URL", "protocol"),
    ("URL", "hostname"),
    ("URL", "port"),
    ("URL", "host"),
    ("URL", "pathname"),
    ("URL", "search"),
    ("URL", "hash"),
    ("URL", "toString"),
    ("CustomEvent", "detail"),
    # Round 6 additions
    ("Performance", "now"),
    ("Performance", "timeOrigin"),
    ("Element", "tagName"),
    ("Element", "localName"),
    ("HTMLElement", "hidden"),
    ("HTMLElement", "tabIndex"),
    ("HTMLInputElement", "accept"),
    ("HTMLInputElement", "align"),
    ("HTMLInputElement", "alt"),
    ("HTMLInputElement", "checked"),
    ("HTMLInputElement", "defaultChecked"),
    ("HTMLInputElement", "defaultValue"),
    ("HTMLInputElement", "disabled"),
    ("HTMLInputElement", "maxLength"),
    ("HTMLInputElement", "name"),
    ("HTMLInputElement", "readOnly"),
    ("HTMLInputElement", "size"),
    ("HTMLInputElement", "src"),
    ("HTMLInputElement", "type"),
    ("HTMLInputElement", "useMap"),
    ("HTMLInputElement", "value"),
    ("HTMLButtonElement", "disabled"),
    ("HTMLButtonElement", "name"),
    ("HTMLButtonElement", "value"),
    ("HTMLSelectElement", "disabled"),
    ("HTMLSelectElement", "multiple"),
    ("HTMLSelectElement", "name"),
    ("HTMLSelectElement", "type"),
    ("HTMLSelectElement", "value"),
}


def _parse_unimplemented_md(md_path: Path) -> list[dict]:
    _RE = re.compile(
        r"^(?P<kind>method|getter|setter)\s*\|\s*"
        r"(?P<class_>\w+)::(?P<member>\w+)"
    )
    entries = []
    if not md_path.exists():
        return entries
    for line in md_path.read_text(encoding="utf-8").splitlines():
        m = _RE.match(line.strip())
        if m:
            entries.append({
                "kind": m.group("kind"),
                "class": m.group("class_"),
                "member": m.group("member"),
            })
    return entries


def _build_gap_matrix(repo_root: Path) -> list[GapEntry]:
    bnd_dir = repo_root / "content" / "handlers" / "javascript" / "duktape"
    unimp_md = repo_root / "docs" / "UnimplementedJavascript.md"

    parsed = _parse_all_bnd(bnd_dir, repo_root)
    unimp = _parse_unimplemented_md(unimp_md)

    seen: set[tuple[str, str]] = set()
    raw: list[dict] = []

    # From .bnd files
    for d in parsed:
        key = (d.class_name, d.member)
        if key in seen:
            continue
        seen.add(key)
        status = "done" if key in _KNOWN_DONE else d.status
        cat = _CATEGORY_DEFAULTS.get(d.class_name, _CATEGORY_DEFAULTS["default"])
        raw.append({
            "class": d.class_name, "member": d.member, "kind": d.kind,
            "bnd_file": d.bnd_file, "status": status,
            "impact": cat["impact"], "effort": cat["effort"],
            "wpt": cat["wpt"], "body_lines": d.body_lines, "notes": "",
        })

    # From UnimplementedJavascript.md
    for e in unimp:
        key = (e["class"], e["member"])
        if key in seen:
            continue
        seen.add(key)
        cat = _CATEGORY_DEFAULTS.get(e["class"], _CATEGORY_DEFAULTS["default"])
        bnd_guess = f"content/handlers/javascript/duktape/{e['class']}.bnd"
        if not (repo_root / bnd_guess).exists():
            bnd_guess = ""
        raw.append({
            "class": e["class"], "member": e["member"], "kind": e["kind"],
            "bnd_file": bnd_guess, "status": "stub",
            "impact": cat["impact"], "effort": cat["effort"],
            "wpt": cat["wpt"], "body_lines": 0,
            "notes": "Listed in UnimplementedJavascript.md",
        })

    # Absent APIs (manually curated)
    absent_keys: set[tuple[str, str]] = set()
    for a in ABSENT_APIS:
        key = (a["class"], a["member"])
        absent_keys.add(key)
        if key in seen:
            continue
        seen.add(key)
        raw.append({
            "class": a["class"], "member": a["member"], "kind": a["kind"],
            "bnd_file": "", "status": "absent",
            "impact": a["impact"], "effort": a["effort"],
            "wpt": a["wpt"], "body_lines": 0, "notes": a["notes"],
        })

    # Spec-driven discovery: if tools/spec-idl/ contains .idl files,
    # parse them and add any (interface, member) not yet seen as "absent"
    spec_dir = repo_root / "tools" / "spec-idl"
    if spec_dir.exists() and list(spec_dir.glob("*.idl")):
        idl_decls = _parse_all_idl(spec_dir)
        for iface_name, decls in idl_decls.items():
            if iface_name not in _RELEVANT_INTERFACES:
                continue
            for decl in decls:
                if _is_event_handler(decl.member_name):
                    continue
                key = (iface_name, decl.member_name)
                if key in seen:
                    continue
                seen.add(key)
                # Use manual scores if in ABSENT_APIS, else category defaults
                if key in absent_keys:
                    aa = next(a for a in ABSENT_APIS
                             if (a["class"], a["member"]) == key)
                    raw.append({
                        "class": iface_name, "member": decl.member_name,
                        "kind": decl.kind, "bnd_file": "", "status": "absent",
                        "impact": aa["impact"], "effort": aa["effort"],
                        "wpt": aa["wpt"], "body_lines": 0, "notes": aa["notes"],
                    })
                else:
                    cat = _CATEGORY_DEFAULTS.get(
                        iface_name, _CATEGORY_DEFAULTS["default"])
                    raw.append({
                        "class": iface_name, "member": decl.member_name,
                        "kind": decl.kind, "bnd_file": "",
                        "status": "absent",
                        "impact": cat["impact"], "effort": cat["effort"],
                        "wpt": cat["wpt"], "body_lines": 0,
                        "notes": f"Discovered from {decl.spec_file}",
                    })

    # Build scored entries
    entries: list[GapEntry] = []
    for idx, r in enumerate(raw, start=1):
        if r["status"] == "done":
            cov = 1.0
        elif r["status"] == "absent":
            cov = 0.0
        elif r.get("body_lines", 0) > 3:
            cov = 0.3
        else:
            cov = 0.0

        entries.append(GapEntry(
            id=f"GAP-{idx:04d}",
            class_name=r["class"], member=r["member"], kind=r["kind"],
            bnd_file=r["bnd_file"], status=r["status"],
            impact=r["impact"], effort=r["effort"],
            coverage=cov, score=0.0,
            wpt_tests=r["wpt"], notes=r["notes"],
        ))

    # Score
    for e in entries:
        if e.status == "done":
            e.score = 0.0
        else:
            w = 1.0 + math.log2(1.0 + e.wpt_tests)
            e.score = round((e.impact * w) / max(e.effort, 1), 3)

    entries.sort(key=lambda e: e.score, reverse=True)
    return entries


# ---------------------------------------------------------------------------
# JSON I/O
# ---------------------------------------------------------------------------

def _git_short_hash(repo_root: Path) -> str:
    try:
        r = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, cwd=repo_root, check=True,
        )
        return r.stdout.strip()
    except Exception:
        return "unknown"


def _write_json(entries: list[GapEntry], repo_root: Path, out: Path) -> None:
    payload = {
        "generated": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "commit": _git_short_hash(repo_root),
        "gaps": [dataclasses.asdict(e) for e in entries],
    }
    out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------
# Diff (ported from diff.py lines 20-95)
# ---------------------------------------------------------------------------

_STATUS_RANK = {"done": 0, "partial": 1, "stub": 2, "absent": 3}


def _diff_gaps(current_path: Path, baseline_path: Path) -> int:
    current = json.loads(current_path.read_text(encoding="utf-8"))
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))

    key_fn = lambda g: g["class_name"] + "::" + g["member"]
    cur_gaps = {key_fn(g): g for g in current.get("gaps", [])}
    base_gaps = {key_fn(g): g for g in baseline.get("gaps", [])}

    new_gaps = []
    regressions = []
    improvements = []
    resolved = []

    for key, gap in cur_gaps.items():
        if key not in base_gaps:
            if gap.get("status") in ("absent", "stub"):
                new_gaps.append(gap)
        else:
            base = base_gaps[key]
            cr = _STATUS_RANK.get(gap.get("status", "stub"), 2)
            br = _STATUS_RANK.get(base.get("status", "stub"), 2)
            if cr > br:
                regressions.append((base, gap))
            elif cr < br:
                if gap.get("status") == "done":
                    resolved.append(gap)
                else:
                    improvements.append((base, gap))

    print("=== lacunae diff ===")
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
            print(f"  {g.get('id','?'):10}  "
                  f"{g.get('class_name','?')}::{g.get('member','?')}  "
                  f"[{g.get('status','?')}]")
        if len(new_gaps) > 10:
            print(f"  ... and {len(new_gaps) - 10} more")

    if regressions:
        print("\nRegressions (status got worse):")
        for base, cur in regressions[:10]:
            print(f"  {cur.get('id','?'):10}  "
                  f"{cur.get('class_name','?')}::{cur.get('member','?')}  "
                  f"{base.get('status','?')} -> {cur.get('status','?')}")

    if resolved:
        print("\nResolved:")
        for g in resolved[:10]:
            print(f"  {g.get('id','?'):10}  "
                  f"{g.get('class_name','?')}::{g.get('member','?')}")

    if new_gaps or regressions:
        print("\nFAIL: regressions found.")
        return 1
    print("\nOK: no regressions.")
    return 0


# ---------------------------------------------------------------------------
# WebIDL parser
# ---------------------------------------------------------------------------

# Matches: interface X { ... }; / interface X : Y { ... };
# Also:    partial interface X { ... };
# Also:    interface mixin X { ... };
_IDL_IFACE_RE = re.compile(
    r"^\s*(?:partial\s+)?interface\s+(?:mixin\s+)?(\w+)"
    r"(?:\s*:\s*\w+)?\s*\{",
    re.ASCII,
)
# Matches: namespace X { ... };
_IDL_NS_RE = re.compile(r"^\s*namespace\s+(\w+)\s*\{", re.ASCII)

# Matches: [readonly] attribute TYPE name;
_IDL_ATTR_RE = re.compile(
    r"^\s*(?:\[[\w=,\s\"\'()]*\]\s*)*"
    r"(readonly\s+)?attribute\s+.+?\s+(\w+)\s*;",
    re.ASCII,
)

# Matches: RETURNTYPE name(ARGS);  (but not "attribute ...")
_IDL_METHOD_RE = re.compile(
    r"^\s*(?:\[[\w=,\s\"\'()]*\]\s*)*"
    r"(?!attribute\b)(?!readonly\b)(?!const\b)"
    r"(?:static\s+)?(?:\w[\w<>\?,\s]*?)\s+(\w+)\s*\(",
    re.ASCII,
)

# Matches: constructor(ARGS);
_IDL_CTOR_RE = re.compile(r"^\s*(?:\[[\w=,\s\"\'()]*\]\s*)*constructor\s*\(", re.ASCII)

# Matches: X includes Y;
_IDL_INCLUDES_RE = re.compile(r"^\s*(\w+)\s+includes\s+(\w+)\s*;", re.ASCII)

# Skip: const, dictionary, callback, typedef, enum
_IDL_SKIP_RE = re.compile(
    r"^\s*(?:const\s|dictionary\s|callback\s|typedef\s|enum\s)",
    re.ASCII,
)


def _parse_idl_file(path: Path) -> tuple[dict[str, list[IdlDecl]], list[tuple[str, str]]]:
    """Parse a single .idl file.

    Returns:
        (interfaces: {name: [IdlDecl, ...]}, includes: [(target, mixin), ...])
    """
    spec = path.name
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    interfaces: dict[str, list[IdlDecl]] = {}
    includes: list[tuple[str, str]] = []
    current_iface: str | None = None
    brace_depth = 0

    for line in lines:
        stripped = line.strip()

        # Skip empty, comments, dictionaries, etc.
        if not stripped or stripped.startswith("//"):
            continue
        if _IDL_SKIP_RE.match(stripped):
            continue

        # Check includes statement (outside interface bodies)
        m = _IDL_INCLUDES_RE.match(stripped)
        if m:
            includes.append((m.group(1), m.group(2)))
            continue

        # Check interface/namespace opening
        if brace_depth == 0:
            m = _IDL_IFACE_RE.match(stripped)
            if not m:
                m = _IDL_NS_RE.match(stripped)
            if m:
                current_iface = m.group(1)
                if current_iface not in interfaces:
                    interfaces[current_iface] = []
                brace_depth = stripped.count("{") - stripped.count("}")
                continue

        # Track braces
        if brace_depth > 0:
            brace_depth += stripped.count("{") - stripped.count("}")
            if brace_depth <= 0:
                current_iface = None
                brace_depth = 0
                continue

            # Only parse members at depth 1
            if current_iface is None:
                continue

            # Constructor
            if _IDL_CTOR_RE.match(stripped):
                interfaces[current_iface].append(IdlDecl(
                    interface_name=current_iface,
                    member_name=current_iface,
                    kind="constructor",
                    spec_file=spec,
                ))
                continue

            # Attribute
            am = _IDL_ATTR_RE.match(stripped)
            if am:
                readonly = bool(am.group(1))
                name = am.group(2)
                interfaces[current_iface].append(IdlDecl(
                    interface_name=current_iface,
                    member_name=name,
                    kind="readonly_attribute" if readonly else "attribute",
                    spec_file=spec,
                ))
                continue

            # Method
            mm = _IDL_METHOD_RE.match(stripped)
            if mm:
                name = mm.group(1)
                # Skip special IDL keywords parsed as methods
                if name in ("getter", "setter", "deleter", "stringifier",
                            "iterable", "maplike", "setlike", "inherit"):
                    continue
                interfaces[current_iface].append(IdlDecl(
                    interface_name=current_iface,
                    member_name=name,
                    kind="method",
                    spec_file=spec,
                ))

    return interfaces, includes


def _parse_all_idl(spec_dir: Path) -> dict[str, list[IdlDecl]]:
    """Parse all .idl files and resolve mixin includes."""
    all_ifaces: dict[str, list[IdlDecl]] = {}
    all_includes: list[tuple[str, str]] = []

    for p in sorted(spec_dir.glob("*.idl")):
        ifaces, includes = _parse_idl_file(p)
        for name, decls in ifaces.items():
            all_ifaces.setdefault(name, []).extend(decls)
        all_includes.extend(includes)

    # Resolve includes: "HTMLElement includes GlobalEventHandlers"
    for target, mixin in all_includes:
        if mixin in all_ifaces and target in all_ifaces:
            existing = {(d.member_name, d.kind) for d in all_ifaces[target]}
            for decl in all_ifaces[mixin]:
                if (decl.member_name, decl.kind) not in existing:
                    all_ifaces[target].append(IdlDecl(
                        interface_name=target,
                        member_name=decl.member_name,
                        kind=decl.kind,
                        spec_file=decl.spec_file,
                    ))

    # Deduplicate within each interface (partial interfaces merge)
    for name in all_ifaces:
        seen: set[tuple[str, str]] = set()
        deduped: list[IdlDecl] = []
        for d in all_ifaces[name]:
            key = (d.member_name, d.kind)
            if key not in seen:
                seen.add(key)
                deduped.append(d)
        all_ifaces[name] = deduped

    return all_ifaces


# ---------------------------------------------------------------------------
# Cross-reference engine
# ---------------------------------------------------------------------------

# Interfaces we care about (ones that map to .bnd files or are in ABSENT_APIS)
_RELEVANT_INTERFACES = {
    "Event", "CustomEvent", "EventTarget",
    "Node", "Element", "Document", "DocumentFragment",
    "HTMLElement", "HTMLHtmlElement", "HTMLHeadElement", "HTMLBodyElement",
    "HTMLDivElement", "HTMLSpanElement", "HTMLParagraphElement",
    "HTMLAnchorElement", "HTMLImageElement", "HTMLFormElement",
    "HTMLInputElement", "HTMLButtonElement", "HTMLSelectElement",
    "HTMLTextAreaElement", "HTMLLabelElement", "HTMLOptionElement",
    "HTMLScriptElement", "HTMLStyleElement", "HTMLLinkElement",
    "HTMLTableElement", "HTMLTableRowElement", "HTMLTableCellElement",
    "HTMLBRElement", "HTMLHRElement", "HTMLPreElement",
    "HTMLHeadingElement", "HTMLUListElement", "HTMLOListElement",
    "HTMLLIElement", "HTMLCanvasElement", "HTMLMediaElement",
    "HTMLVideoElement", "HTMLAudioElement", "HTMLSourceElement",
    "HTMLIFrameElement", "HTMLObjectElement", "HTMLMetaElement",
    "HTMLTitleElement",
    "Window", "History", "Location", "Navigator",
    "Performance",
    "URL", "URLSearchParams",
    "XMLHttpRequest", "FormData",
    "Request", "Response", "Headers",
    "UIEvent", "MouseEvent", "KeyboardEvent", "WheelEvent",
    "CSSStyleDeclaration", "CSSRule", "CSSStyleSheet",
    "console",
    "Storage",
    "DOMRect", "DOMRectReadOnly",
    "MutationObserver",
    "AbortController", "AbortSignal",
    "TextDecoder", "TextEncoder",
}


def _is_event_handler(name: str) -> bool:
    return name.startswith("on") and len(name) > 2 and name[2:3].islower()


def _cross_reference(
    idl_decls: dict[str, list[IdlDecl]],
    bnd_decls: list[BndDecl],
    skip_events: bool = False,
) -> list[dict]:
    """Cross-reference spec IDL against .bnd implementations.

    Returns list of dicts: {interface, member, kind, spec_file, status}
    """
    # Build .bnd lookup: (class_name, member_name) -> BndDecl
    bnd_lookup: dict[tuple[str, str], BndDecl] = {}
    for d in bnd_decls:
        key = (d.class_name, d.member)
        if key not in bnd_lookup or d.status == "done":
            bnd_lookup[key] = d

    # Build ABSENT_APIS lookup
    absent_lookup: set[tuple[str, str]] = set()
    for a in ABSENT_APIS:
        absent_lookup.add((a["class"], a["member"]))

    # Also check _KNOWN_DONE
    results: list[dict] = []

    for iface_name, decls in sorted(idl_decls.items()):
        if iface_name not in _RELEVANT_INTERFACES:
            continue

        for decl in decls:
            if skip_events and _is_event_handler(decl.member_name):
                continue

            # Determine status
            status = "absent"

            if (iface_name, decl.member_name) in _KNOWN_DONE:
                status = "done"
            elif decl.kind in ("attribute", "readonly_attribute"):
                # Check getter
                getter_key = (iface_name, decl.member_name)
                getter = bnd_lookup.get(getter_key)
                has_getter = getter is not None and getter.status == "done"
                has_getter_stub = getter is not None and getter.status == "stub"

                if decl.kind == "readonly_attribute":
                    if has_getter:
                        status = "done"
                    elif has_getter_stub:
                        status = "stub"
                else:
                    # Non-readonly: need getter + setter
                    if has_getter:
                        status = "done"
                    elif has_getter_stub:
                        status = "stub"
            elif decl.kind == "method":
                m_key = (iface_name, decl.member_name)
                m = bnd_lookup.get(m_key)
                if m is not None:
                    status = m.status
            elif decl.kind == "constructor":
                # Check init
                init_key = (iface_name, iface_name)
                init = bnd_lookup.get(init_key)
                if init is not None:
                    status = init.status

            results.append({
                "interface": iface_name,
                "member": decl.member_name,
                "kind": decl.kind,
                "spec_file": decl.spec_file,
                "status": status,
            })

    return results


# ---------------------------------------------------------------------------
# spec-coverage output formatters
# ---------------------------------------------------------------------------

def _spec_coverage_table(results: list[dict], filter_spec: str | None,
                         filter_iface: str | None) -> str:
    # Group by (spec_file, interface)
    groups: dict[tuple[str, str], list[dict]] = {}
    for r in results:
        if filter_spec and r["spec_file"] != filter_spec:
            continue
        if filter_iface and r["interface"] != filter_iface:
            continue
        key = (r["spec_file"], r["interface"])
        groups.setdefault(key, []).append(r)

    lines = []
    lines.append(f"{'Spec':<16}  {'Interface':<28}  {'Total':>5}  {'Done':>4}  "
                 f"{'Stub':>4}  {'Absent':>6}  {'Coverage':>8}")
    lines.append("-" * 88)

    grand = {"total": 0, "done": 0, "stub": 0, "absent": 0}
    for (spec, iface), members in sorted(groups.items()):
        total = len(members)
        done = sum(1 for m in members if m["status"] == "done")
        stub = sum(1 for m in members if m["status"] == "stub")
        absent = sum(1 for m in members if m["status"] == "absent")
        pct = done / total * 100 if total else 0
        lines.append(f"{spec:<16}  {iface:<28}  {total:5d}  {done:4d}  "
                     f"{stub:4d}  {absent:6d}  {pct:7.1f}%")
        grand["total"] += total
        grand["done"] += done
        grand["stub"] += stub
        grand["absent"] += absent

    lines.append("-" * 88)
    gt = grand["total"]
    pct = grand["done"] / gt * 100 if gt else 0
    lines.append(f"{'TOTAL':<16}  {'':<28}  {gt:5d}  {grand['done']:4d}  "
                 f"{grand['stub']:4d}  {grand['absent']:6d}  {pct:7.1f}%")
    return "\n".join(lines)


def _spec_coverage_json(results: list[dict]) -> str:
    by_iface: dict[str, dict] = {}
    for r in results:
        iface = r["interface"]
        if iface not in by_iface:
            by_iface[iface] = {"spec_file": r["spec_file"], "members": []}
        by_iface[iface]["members"].append({
            "name": r["member"],
            "kind": r["kind"],
            "status": r["status"],
        })

    payload = {
        "generated": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "interfaces": by_iface,
        "summary": {
            "total": len(results),
            "done": sum(1 for r in results if r["status"] == "done"),
            "stub": sum(1 for r in results if r["status"] == "stub"),
            "absent": sum(1 for r in results if r["status"] == "absent"),
        },
    }
    return json.dumps(payload, indent=2)


def _spec_coverage_markdown(results: list[dict]) -> str:
    lines = ["# NetSurf Spec Coverage Report", ""]
    lines.append("Generated: " + datetime.datetime.now(
        datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"))
    lines.append("")

    total = len(results)
    done = sum(1 for r in results if r["status"] == "done")
    pct = done / total * 100 if total else 0
    lines.append(f"**Overall: {done}/{total} ({pct:.1f}%)**")
    lines.append("")

    # Group by interface
    groups: dict[str, list[dict]] = {}
    for r in results:
        groups.setdefault(r["interface"], []).append(r)

    lines.append("| Spec | Interface | Total | Done | Stub | Absent | Coverage |")
    lines.append("|------|-----------|------:|-----:|-----:|-------:|---------:|")

    for iface in sorted(groups.keys()):
        members = groups[iface]
        t = len(members)
        d = sum(1 for m in members if m["status"] == "done")
        s = sum(1 for m in members if m["status"] == "stub")
        a = sum(1 for m in members if m["status"] == "absent")
        p = d / t * 100 if t else 0
        spec = members[0]["spec_file"]
        lines.append(f"| {spec} | {iface} | {t} | {d} | {s} | {a} | {p:.1f}% |")

    lines.append("")
    lines.append("---")
    lines.append("*Generated by `python3 tools/lacunae.py spec-coverage`*")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _find_repo_root() -> Path:
    p = Path.cwd()
    for _ in range(10):
        if (p / ".git").exists():
            return p
        p = p.parent
    return Path.cwd()


def _cmd_scan(args: argparse.Namespace) -> int:
    repo = Path(args.repo_root) if args.repo_root else _find_repo_root()
    out = Path(args.out) if args.out else repo / "lacunae-gaps.json"
    entries = _build_gap_matrix(repo)
    _write_json(entries, repo, out)

    total = len(entries)
    done = sum(1 for e in entries if e.status == "done")
    stub = sum(1 for e in entries if e.status == "stub")
    absent = sum(1 for e in entries if e.status == "absent")
    print(f"Scanned: {total} entries  (done={done}, stub={stub}, absent={absent})")
    print(f"Written: {out}")
    return 0


def _cmd_gaps(args: argparse.Namespace) -> int:
    repo = _find_repo_root()
    gaps_path = Path(args.gaps_json) if args.gaps_json else repo / "lacunae-gaps.json"
    if not gaps_path.exists():
        print(f"ERROR: {gaps_path} not found. Run 'scan' first.", file=sys.stderr)
        return 1

    data = json.loads(gaps_path.read_text(encoding="utf-8"))
    gaps = data.get("gaps", [])
    top_n = args.top

    # Filter out done entries and sort by score
    active = [g for g in gaps if g.get("status") != "done"]
    active.sort(key=lambda g: g.get("score", 0), reverse=True)

    print(f"{'Rank':>4}  {'Score':>6}  {'Status':>7}  {'Class':<24}  {'Member':<24}  {'Kind':<8}  Notes")
    print("-" * 105)
    for i, g in enumerate(active[:top_n], start=1):
        print(f"{i:4d}  {g.get('score',0):6.2f}  {g.get('status','?'):>7}  "
              f"{g.get('class_name','?'):<24}  {g.get('member','?'):<24}  "
              f"{g.get('kind','?'):<8}  {g.get('notes','')[:40]}")
    return 0


def _cmd_spec_coverage(args: argparse.Namespace) -> int:
    repo = Path(args.repo_root) if args.repo_root else _find_repo_root()
    spec_dir = Path(args.spec_dir) if args.spec_dir else repo / "tools" / "spec-idl"

    if not spec_dir.exists() or not list(spec_dir.glob("*.idl")):
        print(f"ERROR: No .idl files in {spec_dir}. Run fetch-spec-idl.sh first.",
              file=sys.stderr)
        return 1

    bnd_dir = repo / "content" / "handlers" / "javascript" / "duktape"
    bnd_decls = _parse_all_bnd(bnd_dir, repo)
    idl_decls = _parse_all_idl(spec_dir)
    results = _cross_reference(idl_decls, bnd_decls, skip_events=args.skip_events)

    fmt = args.format
    if fmt == "json":
        out = repo / "spec-coverage.json"
        out.write_text(_spec_coverage_json(results) + "\n", encoding="utf-8")
        print(f"Written: {out}")
    elif fmt == "markdown":
        out = repo / "docs" / "spec-coverage.md"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(_spec_coverage_markdown(results) + "\n", encoding="utf-8")
        print(f"Written: {out}")
    else:
        print(_spec_coverage_table(results, args.filter_spec, args.filter_interface))

    return 0


def _cmd_diff(args: argparse.Namespace) -> int:
    repo = _find_repo_root()
    current = Path(args.gaps_json) if args.gaps_json else repo / "lacunae-gaps.json"
    baseline = Path(args.baseline) if args.baseline else repo / "test" / "lacunae-baseline.json"

    if not current.exists():
        print(f"ERROR: {current} not found. Run 'scan' first.", file=sys.stderr)
        return 1
    if not baseline.exists():
        print(f"ERROR: {baseline} not found.", file=sys.stderr)
        return 1

    return _diff_gaps(current, baseline)


def main() -> int:
    p = argparse.ArgumentParser(
        prog="lacunae",
        description="NetSurf JS binding gap analysis tool",
    )
    sub = p.add_subparsers(dest="command")

    sc = sub.add_parser("scan", help="Parse .bnd files, write lacunae-gaps.json")
    sc.add_argument("--repo-root", default=None, help="Repo root (default: auto-detect)")
    sc.add_argument("--out", default=None, help="Output JSON path")

    sg = sub.add_parser("gaps", help="Print ranked gap table")
    sg.add_argument("--gaps-json", default=None, help="Path to lacunae-gaps.json")
    sg.add_argument("--top", type=int, default=20, help="Show top N gaps (default 20)")

    sd = sub.add_parser("diff", help="Compare current vs baseline; exit 1 on regressions")
    sd.add_argument("--gaps-json", default=None, help="Path to current gaps JSON")
    sd.add_argument("--baseline", default=None, help="Path to baseline JSON")

    ss = sub.add_parser("spec-coverage", help="Cross-reference spec IDL vs .bnd implementations")
    ss.add_argument("--repo-root", default=None, help="Repo root (default: auto-detect)")
    ss.add_argument("--spec-dir", default=None, help="Directory with .idl files")
    ss.add_argument("--filter-spec", default=None, help="Only show one spec file (e.g. html.idl)")
    ss.add_argument("--filter-interface", default=None, help="Only show one interface")
    ss.add_argument("--format", default="table", choices=["table", "json", "markdown"],
                    help="Output format (default: table)")
    ss.add_argument("--skip-events", action="store_true",
                    help="Exclude onXxx event handler attributes")

    args = p.parse_args()
    if args.command is None:
        p.print_help()
        return 1

    cmds = {"scan": _cmd_scan, "gaps": _cmd_gaps, "diff": _cmd_diff,
            "spec-coverage": _cmd_spec_coverage}
    return cmds[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
