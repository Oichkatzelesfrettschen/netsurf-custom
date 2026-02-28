"""
bnd_parser.py -- State-machine parser for nsgenbind .bnd files.

WHY: .bnd files are neither valid C nor valid WebIDL; they use a custom
     DSL with class{}, init, fini, getter, setter, method, prologue keywords.
     A regex-only approach cannot reliably track brace depth for body
     classification. This state machine tracks depth explicitly.

Status classification:
  done    -- body contains non-trivial code (not just "return 0;" or empty)
  stub    -- declaration present, body is empty or "return 0;" only
  absent  -- not in any .bnd file (detected externally via UnimplementedJavascript.md)
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

@dataclass
class BndDecl:
    """One parsed declaration from a .bnd file."""
    class_name: str          # e.g. "History"
    member: str              # e.g. "back"
    kind: str                # method | getter | setter | init | fini | class
    bnd_file: str            # relative path, e.g. "content/.../History.bnd"
    status: str = "stub"     # stub | done | absent
    body_lines: int = 0      # non-blank lines in the %{ ... %} body
    line_no: int = 0         # declaration line in file


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Patterns for declaration keywords (line-level detection)
_DECL_RE = re.compile(
    r"^\s*"
    r"(?P<kind>getter|setter|method|init|fini)\s+"
    r"(?P<class_>\w+)"
    r"(?:::(?P<member>\w+))?"
    r"\s*\(",
    re.ASCII,
)

_CLASS_RE = re.compile(r"^\s*class\s+(?P<name>\w+)\s*\{", re.ASCII)
_BODY_OPEN = re.compile(r"%\{")
_BODY_CLOSE = re.compile(r"%\}")

# Lines that count as "trivial" (stub body) when they are the ONLY content
_TRIVIAL_LINE = re.compile(
    r"^\s*("
    r"return\s+0\s*;"
    r"|/\*.*\*/"            # single-line comment
    r"|//.*"
    r"|\s*"                  # blank
    r")\s*$"
)


def _is_trivial_body(lines: list[str]) -> bool:
    """Return True if body lines are all trivial (empty, comment, return 0)."""
    meaningful = [l for l in lines if l.strip() and not l.strip().startswith("/*")
                  and not l.strip().startswith("//") and not l.strip().endswith("*/")]
    if not meaningful:
        return True
    if len(meaningful) == 1 and _TRIVIAL_LINE.match(meaningful[0]):
        return True
    return False


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

def parse_bnd_file(path: Path, repo_root: Path) -> list[BndDecl]:
    """Parse a single .bnd file and return all BndDecl objects found."""
    rel = str(path.relative_to(repo_root))
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    decls: list[BndDecl] = []
    current_class = ""

    i = 0
    while i < len(lines):
        line = lines[i]

        # Track current class context
        m = _CLASS_RE.match(line)
        if m:
            current_class = m.group("name")
            i += 1
            continue

        # Match a declaration keyword
        dm = _DECL_RE.match(line)
        if dm:
            kind = dm.group("kind")
            cls = dm.group("class_")
            member = dm.group("member") or cls  # init/fini use class as member
            if not member:
                member = cls
            decl_line = i + 1  # 1-based

            # Advance to find body: look for %{ on this or following lines
            body_lines_text: list[str] = []
            has_body = False
            j = i
            # Scan ahead up to 5 lines for %{
            while j < min(i + 6, len(lines)):
                if _BODY_OPEN.search(lines[j]):
                    has_body = True
                    # Collect body until %}
                    j += 1
                    depth = 1
                    while j < len(lines):
                        if _BODY_CLOSE.search(lines[j]):
                            depth -= 1
                            if depth == 0:
                                break
                        elif _BODY_OPEN.search(lines[j]):
                            depth += 1
                        body_lines_text.append(lines[j])
                        j += 1
                    i = j + 1
                    break
                # If we see a semicolon before %{, declaration has no body
                stripped = lines[j].strip()
                if stripped.endswith(";") and j > i:
                    has_body = False
                    i = j + 1
                    break
                j += 1
            else:
                i += 1

            if has_body:
                trivial = _is_trivial_body(body_lines_text)
                status = "stub" if trivial else "done"
            else:
                # Declaration without a body is always a stub
                status = "stub"

            non_blank = len([l for l in body_lines_text if l.strip()])

            decls.append(BndDecl(
                class_name=cls,
                member=member,
                kind=kind,
                bnd_file=rel,
                status=status,
                body_lines=non_blank,
                line_no=decl_line,
            ))
            continue

        i += 1

    return decls


def parse_all_bnd_files(bnd_dir: Path, repo_root: Path) -> list[BndDecl]:
    """Walk bnd_dir/*.bnd and return all declarations."""
    all_decls: list[BndDecl] = []
    for bnd_path in sorted(bnd_dir.glob("*.bnd")):
        if bnd_path.name == "netsurf.bnd":
            continue  # master registry file, not a binding itself
        all_decls.extend(parse_bnd_file(bnd_path, repo_root))
    return all_decls
