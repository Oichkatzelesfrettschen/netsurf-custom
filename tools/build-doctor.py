#!/usr/bin/env python3
"""Report local NetSurf bootstrap and profiling readiness."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
ENV_HELPER = ROOT / "tools" / "with-netsurf-env.sh"


def read_env() -> dict[str, str]:
    proc = subprocess.run(
        [str(ENV_HELPER), "--print-env"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    env: dict[str, str] = {}
    for line in proc.stdout.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        env[key] = value
    return env


def find_missing_tools(names: list[str]) -> list[str]:
    return [name for name in names if shutil.which(name) is None]


def perf_supported() -> tuple[bool, str]:
    perf = shutil.which("perf")
    if perf is None:
        return False, "perf not installed"
    proc = subprocess.run(
        [perf, "stat", "true"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if proc.returncode == 0:
        return True, "supported"
    detail = proc.stderr.strip().splitlines()
    if detail:
        return False, detail[-1]
    return False, "perf unsupported"


def pkg_status(modules: list[str], env: dict[str, str]) -> tuple[list[str], list[str]]:
    present: list[str] = []
    missing: list[str] = []
    cmd_env = os.environ.copy()
    cmd_env.update(env)
    for module in modules:
        proc = subprocess.run(
            ["pkg-config", "--exists", module],
            cwd=ROOT,
            env=cmd_env,
        )
        if proc.returncode == 0:
            present.append(module)
        else:
            missing.append(module)
    return present, missing


def main() -> int:
    env = read_env()
    workspace = Path(env["TARGET_WORKSPACE"])
    prefix = Path(env["PREFIX"])

    required_tools = [
        "bash",
        "cc",
        "git",
        "pkg-config",
        "perl",
        "python3",
        "make",
        "flex",
        "bison",
        "gperf",
    ]
    optional_tools = [
        "ccache",
        "bear",
        "clang",
        "cppcheck",
        "gcc",
        "heaptrack",
        "heaptrack_print",
        "valgrind",
    ]
    required_pkg_modules = [
        "libcss",
        "libdom",
        "libnsutils",
        "libcurl",
        "openssl",
        "libutf8proc",
        "libpng",
        "libjpeg",
        "libwebp",
        "libjxl",
        "libnsbmp",
        "libnsgif",
        "libsvgtiny",
        "libnspsl",
        "libnslog",
    ]

    repo_tokens = [
        token
        for key in ("NS_BUILDSYSTEM", "NS_INTERNAL_LIBS", "NS_FRONTEND_LIBS", "NS_TOOLS")
        for token in env.get(key, "").split()
        if token
    ]
    repo_list = []
    seen = set()
    for repo in repo_tokens:
        if repo not in seen:
            repo_list.append(repo)
            seen.add(repo)

    missing_required_tools = find_missing_tools(required_tools)
    missing_optional_tools = find_missing_tools(optional_tools)
    present_pkg_modules, missing_pkg_modules = pkg_status(required_pkg_modules, env)
    perf_ok, perf_note = perf_supported()

    print("NetSurf Doctor")
    print("==============")
    print()
    print(f"Repo root: {ROOT}")
    print(f"Workspace: {workspace}")
    print(f"Install prefix: {prefix}")
    print(f"Build triple: {env.get('BUILD', '')}")
    print(f"Host triple: {env.get('HOST', '')}")
    print(f"Parallelism: {env.get('USE_CPUS', '')}")
    print()
    print("Workspace repositories:")
    for repo in repo_list:
        repo_path = workspace / repo
        status = "ok" if repo_path.exists() else "missing"
        print(f"  [{status}] {repo_path}")
    print()
    print("Required tools:")
    for tool in required_tools:
        status = "ok" if tool not in missing_required_tools else "missing"
        print(f"  [{status}] {tool}")
    print()
    print("Optional tools:")
    for tool in optional_tools:
        status = "ok" if tool not in missing_optional_tools else "missing"
        print(f"  [{status}] {tool}")
    print(f"  [{'ok' if perf_ok else 'skip'}] perf ({perf_note})")
    print()
    print("pkg-config modules:")
    for module in present_pkg_modules:
        print(f"  [ok] {module}")
    for module in missing_pkg_modules:
        print(f"  [missing] {module}")
    print()

    missing_workspace = [repo for repo in repo_list if not (workspace / repo).exists()]
    if missing_required_tools or missing_workspace or missing_pkg_modules:
        print("Next steps:")
        if missing_required_tools:
            print("  - Install the missing required tools listed above.")
        if missing_workspace:
            print("  - Run `make bootstrap` to clone and build the NetSurf workspace.")
        elif missing_pkg_modules:
            print("  - Run `make bootstrap-libs` to install missing workspace libraries.")
        return 1

    print("Doctor result: environment looks ready.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
