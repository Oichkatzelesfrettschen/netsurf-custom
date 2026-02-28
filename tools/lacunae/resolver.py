"""
resolver.py -- Generate copy-paste-ready resolution outlines per gap.

WHY: Context-switching from gap analysis to implementation is expensive.
     Providing a skeleton that compiles and matches NetSurf conventions
     reduces the time-to-first-commit for each gap.
"""

from __future__ import annotations

from pathlib import Path

try:
    from jinja2 import Environment, FileSystemLoader
    _JINJA_AVAILABLE = True
except ImportError:
    _JINJA_AVAILABLE = False

from lacunae.gap_matrix import GapEntry


_TEMPLATE_DIR = Path(__file__).parent / "templates"

_KIND_C_HINTS = {
    "getter": {
        "return_type": "duk_ret_t",
        "example_push": "duk_push_undefined(ctx);",
        "return_count": "1",
        "note": "Push the value onto the Duktape stack and return 1.",
    },
    "setter": {
        "return_type": "duk_ret_t",
        "example_push": "/* read via duk_get_X(ctx, 0) then store */",
        "return_count": "0",
        "note": "Read the new value from ctx[0] and store it.",
    },
    "method": {
        "return_type": "duk_ret_t",
        "example_push": "/* perform operation; push result if any */",
        "return_count": "0",
        "note": "Perform the operation. Push result and return 1, or return 0.",
    },
    "init": {
        "return_type": "void",
        "example_push": "/* initialize priv->field here */",
        "return_count": "N/A",
        "note": "Called once when the JS object is created. Return type is void.",
    },
    "fini": {
        "return_type": "void",
        "example_push": "/* release priv->field here */",
        "return_count": "N/A",
        "note": "Called once when the JS object is destroyed.",
    },
}


def resolve_gap(entry: GapEntry) -> str:
    """Return a formatted resolution outline for one gap entry."""
    if _JINJA_AVAILABLE:
        return _render_jinja(entry)
    return _render_plain(entry)


def _render_jinja(entry: GapEntry) -> str:
    env = Environment(
        loader=FileSystemLoader(str(_TEMPLATE_DIR)),
        autoescape=False,
        trim_blocks=True,
        lstrip_blocks=True,
    )
    if entry.status == "absent":
        tmpl = env.get_template("resolve_bnd.j2")
    else:
        tmpl = env.get_template("resolve_stub.c.j2")
    hints = _KIND_C_HINTS.get(entry.kind, _KIND_C_HINTS["method"])
    return tmpl.render(entry=entry, hints=hints)


def _render_plain(entry: GapEntry) -> str:
    hints = _KIND_C_HINTS.get(entry.kind, _KIND_C_HINTS["method"])
    bnd_fname = entry.class_name + ".bnd"
    lines = [
        "# Resolution outline for " + entry.id + ": " + entry.class_name + "::" + entry.member,
        "# Status : " + entry.status,
        "# Kind   : " + entry.kind,
        "# Impact : " + str(entry.impact) + "/10   Effort: " + str(entry.effort) + "/10   Score: " + str(entry.score),
        "# Notes  : " + (entry.notes or ""),
        "",
    ]

    if entry.status == "absent":
        lines += [
            "## Step 1: Create " + bnd_fname,
            "",
            "class " + entry.class_name + " {",
            "    /* TODO: add private fields if needed */",
            "};",
            "",
            "init " + entry.class_name + "(/* TODO: add C type argument */)",
            "%{",
            "    /* TODO: initialize priv fields */",
            "%}",
            "",
            "fini " + entry.class_name + "()",
            "%{",
            "    /* TODO: release priv fields */",
            "%}",
            "",
            entry.kind + " " + entry.class_name + "::" + entry.member + "()",
            "%{",
            "    " + hints["example_push"],
            "    return " + hints["return_count"] + ";",
            "%}",
            "",
            "## Step 2: Add to netsurf.bnd",
            "",
            '#include "' + bnd_fname + '"',
            "",
            "## Step 3: Wire C implementation",
            "",
            "/* In content/handlers/javascript/duktape/" + entry.class_name.lower() + ".c */",
            "/* TODO: implement the underlying C function */",
        ]
    else:
        lines += [
            "## .bnd body for " + entry.class_name + "::" + entry.member,
            "",
            entry.kind + " " + entry.class_name + "::" + entry.member + "()",
            "%{",
            "    /* WHY: " + (entry.notes or "implement this stub") + " */",
            "    " + hints["example_push"],
            "    /* " + hints["note"] + " */",
            "    return " + hints["return_count"] + ";",
            "%}",
            "",
            "## C helper (if needed)",
            "",
            "/* In a new or existing .c file: */",
            "/* " + hints["return_type"] + " " + entry.class_name.lower() + "_" + entry.member + "(...) { ... } */",
        ]

    return "\n".join(lines)
