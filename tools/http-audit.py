#!/usr/bin/env python3
"""
http-audit.py -- audit NetSurf HTTP implementation against RFC 9110/9111/9112.

WHY: Provides objective HTTP protocol compliance metrics by static analysis.
WHAT: Scans curl.c, utils/http/, llcache.c for implemented features.
HOW: python3 tools/http-audit.py [--repo-root DIR] [--format json|markdown|both]

Zero dependencies beyond Python 3.8+ stdlib.
"""

from __future__ import annotations

import argparse
import datetime
import json
import re
import sys
from pathlib import Path


def _find_repo_root() -> Path:
    p = Path.cwd()
    for _ in range(10):
        if (p / ".git").exists():
            return p
        p = p.parent
    return Path.cwd()


def _read(path: Path) -> str:
    if path.exists():
        return path.read_text(encoding="utf-8", errors="replace")
    return ""


# ---------------------------------------------------------------------------
# HTTP Methods (RFC 9110 Section 9)
# ---------------------------------------------------------------------------

_RFC9110_METHODS = [
    "GET", "HEAD", "POST", "PUT", "DELETE", "CONNECT", "OPTIONS", "TRACE", "PATCH",
]

_METHOD_CURL_PATTERNS: dict[str, list[str]] = {
    "GET":     ["CURLOPT_HTTPGET", "CURLOPT_NOBODY.*false"],
    "HEAD":    ["CURLOPT_NOBODY"],
    "POST":    ["CURLOPT_POST", "CURLOPT_POSTFIELDS"],
    "PUT":     ["CURLOPT_PUT", "CURLOPT_UPLOAD", "CUSTOMREQUEST.*PUT"],
    "DELETE":  ["CUSTOMREQUEST.*DELETE"],
    "CONNECT": ["CUSTOMREQUEST.*CONNECT"],
    "OPTIONS": ["CUSTOMREQUEST.*OPTIONS"],
    "TRACE":   ["CUSTOMREQUEST.*TRACE"],
    "PATCH":   ["CUSTOMREQUEST.*PATCH"],
}


def _audit_methods(curl_src: str) -> dict[str, str]:
    results: dict[str, str] = {}
    for method in _RFC9110_METHODS:
        patterns = _METHOD_CURL_PATTERNS.get(method, [])
        found = any(re.search(p, curl_src) for p in patterns)
        results[method] = "supported" if found else "not_implemented"
    return results


# ---------------------------------------------------------------------------
# HTTP Headers (RFC 9110 Section 8)
# ---------------------------------------------------------------------------

# Headers NetSurf explicitly parses or generates
_HEADER_AUDIT = {
    # Response headers with dedicated parsers
    "Cache-Control":            {"file": "utils/http/cache-control.c",   "rfc": "9111"},
    "Content-Type":             {"file": "utils/http/content-type.c",    "rfc": "9110"},
    "Content-Disposition":      {"file": "utils/http/content-disposition.c", "rfc": "6266"},
    "WWW-Authenticate":        {"file": "utils/http/www-authenticate.c", "rfc": "9110"},
    "Strict-Transport-Security":{"file": "utils/http/strict-transport-security.c", "rfc": "6797"},
    # Headers handled in curl.c or llcache.c
    "ETag":                     {"file": "content/llcache.c",            "rfc": "9110"},
    "Last-Modified":            {"file": "content/llcache.c",            "rfc": "9110"},
    "If-None-Match":            {"file": "content/llcache.c",            "rfc": "9110"},
    "If-Modified-Since":        {"file": "content/llcache.c",            "rfc": "9110"},
    "Content-Length":           {"file": "content/fetchers/curl.c",      "rfc": "9110"},
    "Content-Encoding":        {"file": "content/fetchers/curl.c",      "rfc": "9110"},
    "Location":                {"file": "content/fetchers/curl.c",      "rfc": "9110"},
    "Set-Cookie":              {"file": "content/fetchers/curl.c",      "rfc": "6265"},
    "Cookie":                  {"file": "content/fetchers/curl.c",      "rfc": "6265"},
    "Referer":                 {"file": "content/fetchers/curl.c",      "rfc": "9110"},
    "User-Agent":              {"file": "content/fetchers/curl.c",      "rfc": "9110"},
    "Accept":                  {"file": "content/fetchers/curl.c",      "rfc": "9110"},
    "Accept-Language":         {"file": "content/fetchers/curl.c",      "rfc": "9110"},
    # Security headers
    "Content-Security-Policy": {"file": None,                            "rfc": "CSP3"},
    "X-Content-Type-Options":  {"file": None,                            "rfc": "fetch"},
    "X-Frame-Options":         {"file": None,                            "rfc": "7034"},
    "Referrer-Policy":         {"file": None,                            "rfc": "referrer-policy"},
    "Permissions-Policy":      {"file": None,                            "rfc": "permissions-policy"},
    # CORS headers
    "Access-Control-Allow-Origin":  {"file": None,                       "rfc": "fetch"},
    "Access-Control-Allow-Methods": {"file": None,                       "rfc": "fetch"},
    "Access-Control-Allow-Headers": {"file": None,                       "rfc": "fetch"},
    "Access-Control-Max-Age":       {"file": None,                       "rfc": "fetch"},
}


def _audit_headers(repo: Path) -> dict[str, dict]:
    results: dict[str, dict] = {}

    for header, info in _HEADER_AUDIT.items():
        filepath = info["file"]
        if filepath is None:
            results[header] = {
                "status": "not_implemented",
                "rfc": info["rfc"],
                "file": None,
            }
            continue

        full = repo / filepath
        if full.exists():
            src = _read(full)
            # Check if the header name appears in the source
            h_lower = header.lower().replace("-", "[-_]")
            if re.search(h_lower, src, re.IGNORECASE):
                results[header] = {
                    "status": "parsed",
                    "rfc": info["rfc"],
                    "file": filepath,
                }
            else:
                results[header] = {
                    "status": "file_exists_no_match",
                    "rfc": info["rfc"],
                    "file": filepath,
                }
        else:
            results[header] = {
                "status": "file_missing",
                "rfc": info["rfc"],
                "file": filepath,
            }

    return results


# ---------------------------------------------------------------------------
# Status Codes (RFC 9110 Section 15)
# ---------------------------------------------------------------------------

_RFC9110_STATUS_CODES = {
    100: "Continue", 101: "Switching Protocols",
    200: "OK", 201: "Created", 202: "Accepted",
    203: "Non-Authoritative Information", 204: "No Content",
    205: "Reset Content", 206: "Partial Content",
    300: "Multiple Choices", 301: "Moved Permanently", 302: "Found",
    303: "See Other", 304: "Not Modified", 305: "Use Proxy",
    307: "Temporary Redirect", 308: "Permanent Redirect",
    400: "Bad Request", 401: "Unauthorized", 402: "Payment Required",
    403: "Forbidden", 404: "Not Found", 405: "Method Not Allowed",
    406: "Not Acceptable", 407: "Proxy Authentication Required",
    408: "Request Timeout", 409: "Conflict", 410: "Gone",
    411: "Length Required", 412: "Precondition Failed",
    413: "Content Too Large", 414: "URI Too Long",
    415: "Unsupported Media Type", 416: "Range Not Satisfiable",
    417: "Expectation Failed", 418: "I'm a Teapot",
    421: "Misdirected Request", 422: "Unprocessable Content",
    426: "Upgrade Required",
    500: "Internal Server Error", 501: "Not Implemented",
    502: "Bad Gateway", 503: "Service Unavailable",
    504: "Gateway Timeout", 505: "HTTP Version Not Supported",
}


def _audit_status_codes(repo: Path) -> dict[str, str]:
    rc_path = repo / "utils" / "http" / "response-codes.h"
    src = _read(rc_path)

    results: dict[str, str] = {}
    for code, name in sorted(_RFC9110_STATUS_CODES.items()):
        pattern = rf"=\s*{code}\b"
        if re.search(pattern, src):
            results[str(code)] = "defined"
        else:
            results[str(code)] = "not_defined"

    return results


# ---------------------------------------------------------------------------
# Caching (RFC 9111)
# ---------------------------------------------------------------------------

_CACHE_FEATURES = {
    "max-age":               {"pattern": r"max.age",           "files": ["utils/http/cache-control.c", "content/llcache.c"]},
    "no-cache":              {"pattern": r"no.cache",          "files": ["utils/http/cache-control.c", "content/llcache.c"]},
    "no-store":              {"pattern": r"no.store",          "files": ["utils/http/cache-control.c", "content/llcache.c"]},
    "must-revalidate":       {"pattern": r"must.revalidate",   "files": ["utils/http/cache-control.c"]},
    "ETag / If-None-Match":  {"pattern": r"etag|if.none.match","files": ["content/llcache.c"]},
    "Last-Modified / If-Modified-Since": {"pattern": r"last.modified|if.modified.since", "files": ["content/llcache.c"]},
    "Expires":               {"pattern": r"expires",           "files": ["content/llcache.c"]},
    "Age":                   {"pattern": r"\bage\b",           "files": ["content/llcache.c"]},
    "s-maxage":              {"pattern": r"s.maxage",          "files": ["utils/http/cache-control.c"]},
    "private":               {"pattern": r"\bprivate\b",       "files": ["utils/http/cache-control.c"]},
    "public":                {"pattern": r"\bpublic\b",        "files": ["utils/http/cache-control.c"]},
    "304 Not Modified":      {"pattern": r"304|NOT_MODIFIED|NOTMODIFIED", "files": ["content/llcache.c", "content/fetchers/curl.c"]},
}


def _audit_caching(repo: Path) -> dict[str, str]:
    file_cache: dict[str, str] = {}
    results: dict[str, str] = {}

    for feature, info in _CACHE_FEATURES.items():
        found = False
        for f in info["files"]:
            if f not in file_cache:
                file_cache[f] = _read(repo / f)
            if re.search(info["pattern"], file_cache[f], re.IGNORECASE):
                found = True
                break
        results[feature] = "implemented" if found else "not_implemented"

    return results


# ---------------------------------------------------------------------------
# Security Headers
# ---------------------------------------------------------------------------

def _audit_security(repo: Path) -> dict[str, str]:
    results: dict[str, str] = {}

    # HSTS
    hsts_path = repo / "utils" / "http" / "strict-transport-security.c"
    results["HSTS (RFC 6797)"] = "implemented" if hsts_path.exists() else "not_implemented"

    # CORS
    curl_src = _read(repo / "content" / "fetchers" / "curl.c")
    results["CORS"] = "implemented" if re.search(r"access.control", curl_src, re.IGNORECASE) else "not_implemented"

    # CSP
    results["CSP (Content-Security-Policy)"] = "not_implemented"

    # X-Content-Type-Options
    results["X-Content-Type-Options"] = "not_implemented"

    # X-Frame-Options
    results["X-Frame-Options"] = "not_implemented"

    # Referrer-Policy
    results["Referrer-Policy"] = "not_implemented"

    return results


# ---------------------------------------------------------------------------
# Protocol Version
# ---------------------------------------------------------------------------

def _audit_protocol(curl_src: str) -> dict[str, str]:
    results: dict[str, str] = {}

    results["HTTP/1.1"] = "supported"  # libcurl default

    if re.search(r"CURL_HTTP_VERSION_2|HTTP_VERSION_2", curl_src):
        results["HTTP/2"] = "supported"
    else:
        results["HTTP/2"] = "not_supported"

    if re.search(r"CURL_HTTP_VERSION_3|HTTP_VERSION_3", curl_src):
        results["HTTP/3"] = "supported"
    else:
        results["HTTP/3"] = "not_supported"

    return results


# ---------------------------------------------------------------------------
# Output formatters
# ---------------------------------------------------------------------------

def _build_report(repo: Path) -> dict:
    curl_src = _read(repo / "content" / "fetchers" / "curl.c")

    return {
        "generated": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "rfc9110": {
            "methods": _audit_methods(curl_src),
            "headers": _audit_headers(repo),
            "status_codes": _audit_status_codes(repo),
        },
        "rfc9111": {
            "caching": _audit_caching(repo),
        },
        "security": _audit_security(repo),
        "protocol_versions": _audit_protocol(curl_src),
    }


def _count(d: dict, val: str) -> int:
    return sum(1 for v in d.values() if (v == val if isinstance(v, str) else v.get("status") == val))


def _format_markdown(report: dict) -> str:
    lines = ["# NetSurf HTTP Compliance Report", ""]
    lines.append("Generated: " + report["generated"])
    lines.append("")

    # Methods
    methods = report["rfc9110"]["methods"]
    supported = _count(methods, "supported")
    lines.append(f"## HTTP Methods (RFC 9110 Section 9) -- {supported}/{len(methods)}")
    lines.append("")
    lines.append("| Method | Status |")
    lines.append("|--------|--------|")
    for m, s in methods.items():
        lines.append(f"| {m} | {s} |")
    lines.append("")

    # Headers
    headers = report["rfc9110"]["headers"]
    parsed = sum(1 for v in headers.values() if v["status"] == "parsed")
    lines.append(f"## HTTP Headers -- {parsed}/{len(headers)} parsed")
    lines.append("")
    lines.append("| Header | Status | RFC | File |")
    lines.append("|--------|--------|-----|------|")
    for h, v in headers.items():
        lines.append(f"| {h} | {v['status']} | {v['rfc']} | {v.get('file') or 'n/a'} |")
    lines.append("")

    # Status Codes
    codes = report["rfc9110"]["status_codes"]
    defined = _count(codes, "defined")
    lines.append(f"## HTTP Status Codes (RFC 9110 Section 15) -- {defined}/{len(codes)} defined")
    lines.append("")
    lines.append("| Code | Status |")
    lines.append("|------|--------|")
    for c, s in sorted(codes.items(), key=lambda x: int(x[0])):
        lines.append(f"| {c} | {s} |")
    lines.append("")

    # Caching
    caching = report["rfc9111"]["caching"]
    impl = _count(caching, "implemented")
    lines.append(f"## Caching (RFC 9111) -- {impl}/{len(caching)} implemented")
    lines.append("")
    lines.append("| Feature | Status |")
    lines.append("|---------|--------|")
    for f, s in caching.items():
        lines.append(f"| {f} | {s} |")
    lines.append("")

    # Security
    security = report["security"]
    sec_impl = _count(security, "implemented")
    lines.append(f"## Security Headers -- {sec_impl}/{len(security)} implemented")
    lines.append("")
    lines.append("| Feature | Status |")
    lines.append("|---------|--------|")
    for f, s in security.items():
        lines.append(f"| {f} | {s} |")
    lines.append("")

    # Protocol Versions
    proto = report["protocol_versions"]
    lines.append("## Protocol Versions")
    lines.append("")
    lines.append("| Version | Status |")
    lines.append("|---------|--------|")
    for v, s in proto.items():
        lines.append(f"| {v} | {s} |")
    lines.append("")

    lines.append("---")
    lines.append("*Generated by `python3 tools/http-audit.py`*")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(
        prog="http-audit",
        description="Audit NetSurf HTTP implementation against RFC 9110/9111/9112",
    )
    p.add_argument("--repo-root", default=None, help="Repository root (default: auto-detect)")
    p.add_argument("--format", default="both", choices=["json", "markdown", "both"],
                   help="Output format (default: both)")
    args = p.parse_args()

    repo = Path(args.repo_root) if args.repo_root else _find_repo_root()
    report = _build_report(repo)

    if args.format in ("json", "both"):
        out_json = repo / "http-compliance.json"
        out_json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Written: {out_json}")

    if args.format in ("markdown", "both"):
        out_md = repo / "docs" / "http-compliance.md"
        out_md.parent.mkdir(parents=True, exist_ok=True)
        out_md.write_text(_format_markdown(report) + "\n", encoding="utf-8")
        print(f"Written: {out_md}")

    # Print summary
    methods = report["rfc9110"]["methods"]
    headers = report["rfc9110"]["headers"]
    codes = report["rfc9110"]["status_codes"]
    caching = report["rfc9111"]["caching"]
    security = report["security"]

    print(f"\nHTTP Compliance Summary:")
    print(f"  Methods:      {_count(methods, 'supported')}/{len(methods)} supported")
    print(f"  Headers:      {sum(1 for v in headers.values() if v['status'] == 'parsed')}/{len(headers)} parsed")
    print(f"  Status codes: {_count(codes, 'defined')}/{len(codes)} defined")
    print(f"  Caching:      {_count(caching, 'implemented')}/{len(caching)} features")
    print(f"  Security:     {_count(security, 'implemented')}/{len(security)} features")

    return 0


if __name__ == "__main__":
    sys.exit(main())
