#!/usr/bin/env python3
"""Attempt a lowest-common-i386 core4m build on the current host."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
PROFILE_DIR = ROOT / "build" / "profiles"
LOG_PATH = PROFILE_DIR / "i386-build-attempt.log"
JSON_PATH = PROFILE_DIR / "i386-build-attempt.json"
ENV_HELPER = ROOT / "tools" / "with-netsurf-env.sh"
TARGET_ABI = "i386-pc-linux-gnu"
TOOLCHAIN_DIR = PROFILE_DIR / "i386-toolchain"
I386_FLAGS = [
    "-m32",
    "-march=i386",
    "-mtune=i386",
    "-mno-80387",
    "-msoft-float",
    "-mno-mmx",
    "-mno-sse",
    "-mno-sse2",
]
I386_LINK_FLAGS = [
    "-rtlib=compiler-rt",
]


def read_env() -> dict[str, str]:
    proc = subprocess.run(
        [
            str(ENV_HELPER),
            "--shell",
            f'unset HOST; export TARGET_ABI="{TARGET_ABI}"; source "{ROOT / "docs" / "env.sh"}" >/dev/null; '
            'printf "HOST=%s\\n" "${HOST:-}"; '
            'printf "PREFIX=%s\\n" "${PREFIX:-}"; '
            'printf "BUILD_PREFIX=%s\\n" "${BUILD_PREFIX:-}"',
        ],
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


def summarize_failure(output: str) -> dict[str, object]:
    interesting: list[str] = []
    workspace_incompatible: list[str] = []
    system_incompatible: list[str] = []
    for line in output.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        lower = stripped.lower()
        if "skipping incompatible" in lower and "/projects/inst-" in stripped:
            workspace_incompatible.append(stripped)
        elif "skipping incompatible" in lower:
            system_incompatible.append(stripped)
        if (
            "error:" in lower
            or "undefined reference" in lower
            or "skipping incompatible" in lower
            or "cannot find" in lower
            or "ld returned" in lower
        ):
            interesting.append(stripped)
    blockers = []
    if workspace_incompatible:
        blockers.append("workspace prefix libraries are x86_64, not ELF32/i386")
    if system_incompatible:
        blockers.append("pkg-config is still pulling host-sized helper libraries into the framebuffer link")
    summary = {
        "blockers": blockers,
        "workspace_incompatible_examples": workspace_incompatible[:10],
        "system_incompatible_examples": system_incompatible[:10],
    }
    if interesting:
        summary["tail"] = interesting[-20:]
        return summary
    tail = [line.strip() for line in output.splitlines() if line.strip()]
    summary["tail"] = tail[-20:]
    return summary


def read_libnsfb_pc(prefix: Path) -> dict[str, object]:
    pc_path = prefix / "lib" / "pkgconfig" / "libnsfb.pc"
    if not pc_path.exists():
        return {"path": str(pc_path), "exists": False}
    requires: list[str] = []
    libs = ""
    text = pc_path.read_text(encoding="utf-8")
    for line in text.splitlines():
        if line.startswith("Requires:"):
            value = line.split(":", 1)[1].strip()
            if value:
                requires = [item.strip() for item in value.split(",")]
        elif line.startswith("Libs:"):
            libs = line.split(":", 1)[1].strip()
    return {
        "path": str(pc_path),
        "exists": True,
        "requires": requires,
        "libs": libs,
    }


def read_libnsfb_requires(prefix: Path) -> list[str]:
    info = read_libnsfb_pc(prefix)
    if not info.get("exists"):
        return []
    return list(info.get("requires", []))


def ensure_compiler_wrapper(name: str, compiler: str) -> Path:
    TOOLCHAIN_DIR.mkdir(parents=True, exist_ok=True)
    path = TOOLCHAIN_DIR / name
    script = """#!/bin/sh
link_args=""
for arg in "$@"; do
    if [ "$arg" = "-c" ]; then
        exec {compiler} {compile_flags} "$@"
    fi
done
exec {compiler} {compile_flags} {link_flags} "$@"
""".format(
        compiler=compiler,
        compile_flags=" ".join(I386_FLAGS),
        link_flags=" ".join(I386_LINK_FLAGS),
    )
    path.write_text(script, encoding="utf-8")
    path.chmod(0o755)
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default=str(JSON_PATH))
    parser.add_argument("--log", default=str(LOG_PATH))
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Return non-zero unless the i386 core4m build succeeds.",
    )
    args = parser.parse_args()

    PROFILE_DIR.mkdir(parents=True, exist_ok=True)
    env = read_env()
    cc_wrapper = ensure_compiler_wrapper("i386-clang", "clang")
    cxx_wrapper = ensure_compiler_wrapper("i386-clang++", "clang++")
    cmd = [
        "make",
        "build-profile",
        "PROFILE=core4m",
        "BUILD_FLAVOUR=i386-denominator",
        f"CC={cc_wrapper}",
        f"CXX={cxx_wrapper}",
        f"HOST_CC={cc_wrapper}",
        f"HOST_CXX={cxx_wrapper}",
    ]
    proc = subprocess.run(
        cmd,
        cwd=ROOT,
        env={**os.environ, "TARGET_ABI": TARGET_ABI},
        capture_output=True,
        text=True,
        check=False,
    )
    combined = proc.stdout + proc.stderr
    Path(args.log).write_text(combined, encoding="utf-8")

    payload = {
        "schema": 1,
        "scenario": "lowest-common-i386-build-attempt",
        "target_abi": TARGET_ABI,
        "profile": "core4m",
        "command": cmd,
        "flags": I386_FLAGS + I386_LINK_FLAGS,
        "cc_wrapper": str(cc_wrapper),
        "cxx_wrapper": str(cxx_wrapper),
        "returncode": proc.returncode,
        "succeeded": proc.returncode == 0,
        "log": str(Path(args.log)),
        "prefix": env["PREFIX"],
        "libnsfb_requires": read_libnsfb_requires(Path(env["PREFIX"])),
        "libnsfb_pc": read_libnsfb_pc(Path(env["PREFIX"])),
        "failure_summary": summarize_failure(combined),
    }
    Path(args.output).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(payload, indent=2))

    if args.strict and proc.returncode != 0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
