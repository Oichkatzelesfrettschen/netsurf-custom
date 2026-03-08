#!/usr/bin/env python3
"""Measure and verify lowest-common-i386 readiness on the current host."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
ENV_HELPER = ROOT / "tools" / "with-netsurf-env.sh"
PROFILE_DIR = ROOT / "build" / "profiles"
I386_TARGET_ABI = "i386-pc-linux-gnu"
PROBE_SOURCE = """#include <stdint.h>
#include <stdio.h>

static uint32_t checksum(uint32_t seed)
{
\tuint32_t value = seed;
\tint i;

\tfor (i = 0; i < 64; i++) {
\t\tvalue = (value * 33U) ^ (uint32_t)(i + 17);
\t}

\treturn value;
}

int main(void)
{
\tuint32_t value = checksum(7U);
\tprintf("i386-probe:%u\\n", value);
\treturn value == 997891143U ? 0 : 1;
}
"""
PROBE_CFLAGS = [
    "-m32",
    "-march=i386",
    "-mtune=i386",
    "-mno-80387",
    "-msoft-float",
    "-mno-mmx",
    "-mno-sse",
    "-mno-sse2",
]


def read_env() -> dict[str, str]:
    cmd = [
        str(ENV_HELPER),
        "--shell",
        f'unset HOST; export TARGET_ABI="{I386_TARGET_ABI}"; source "{ROOT / "docs" / "env.sh"}" >/dev/null; '
        'printf "ROOT_DIR=%s\\n" "${ROOT_DIR:-}"; '
        'printf "BUILD=%s\\n" "${BUILD:-}"; '
        'printf "HOST=%s\\n" "${HOST:-}"; '
        'printf "TARGET_WORKSPACE=%s\\n" "${TARGET_WORKSPACE:-}"; '
        'printf "USE_CPUS=%s\\n" "${USE_CPUS:-}"; '
        'printf "PREFIX=%s\\n" "${PREFIX:-}"; '
        'printf "BUILD_PREFIX=%s\\n" "${BUILD_PREFIX:-}"; '
        'printf "PKG_CONFIG_PATH=%s\\n" "${PKG_CONFIG_PATH:-}"; '
        'printf "NS_BUILDSYSTEM=%s\\n" "${NS_BUILDSYSTEM:-}"; '
        'printf "NS_INTERNAL_LIBS=%s\\n" "${NS_INTERNAL_LIBS:-}"; '
        'printf "NS_FRONTEND_LIBS=%s\\n" "${NS_FRONTEND_LIBS:-}"; '
        'printf "NS_TOOLS=%s\\n" "${NS_TOOLS:-}"; '
        'printf "BUILD_TARGET=%s\\n" "${BUILD_TARGET:-}"',
    ]
    proc = subprocess.run(
        cmd,
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


def run_capture(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    input_text: str | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=cwd or ROOT,
        env=env,
        input=input_text,
        capture_output=True,
        text=True,
        check=False,
    )


def readelf_header(path: Path) -> dict[str, str]:
    proc = run_capture(["readelf", "-h", str(path)])
    info = {
        "path": str(path),
        "class": "unknown",
        "machine": "unknown",
    }
    if proc.returncode != 0:
        info["error"] = proc.stderr.strip() or proc.stdout.strip()
        return info
    for line in proc.stdout.splitlines():
        stripped = line.strip()
        if stripped.startswith("Class:"):
            info["class"] = stripped.split(":", 1)[1].strip()
        elif stripped.startswith("Machine:"):
            info["machine"] = stripped.split(":", 1)[1].strip()
    return info


def inspect_archive(path: Path) -> dict[str, str]:
    proc = run_capture(["ar", "t", str(path)])
    info = {
        "path": str(path),
        "format": "archive",
        "class": "unknown",
        "machine": "unknown",
    }
    if proc.returncode != 0:
        info["error"] = proc.stderr.strip() or proc.stdout.strip()
        return info
    members = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    if not members:
        info["error"] = "empty archive"
        return info
    member = members[0]
    member_proc = subprocess.run(
        ["ar", "p", str(path), member],
        cwd=ROOT,
        capture_output=True,
        check=False,
    )
    if member_proc.returncode != 0:
        info["error"] = member_proc.stderr.decode("utf-8", errors="replace").strip()
        return info
    with tempfile.NamedTemporaryFile(prefix="i386-archive-member-", delete=False) as tmp:
        tmp.write(member_proc.stdout)
        member_path = Path(tmp.name)
    try:
        header = readelf_header(member_path)
    finally:
        member_path.unlink(missing_ok=True)
    info["member"] = member
    info["class"] = header.get("class", "unknown")
    info["machine"] = header.get("machine", "unknown")
    if "error" in header:
        info["error"] = header["error"]
    return info


def inspect_library(path: Path) -> dict[str, str]:
    if path.suffix == ".a":
        return inspect_archive(path)
    return readelf_header(path)


def inspect_prefix_libs(prefix: Path) -> dict:
    libdir = prefix / "lib"
    entries: list[dict[str, str]] = []
    counts = {
        "elf32_i386": 0,
        "elf64_x86_64": 0,
        "other": 0,
    }
    if not libdir.exists():
        return {
            "libdir": str(libdir),
            "exists": False,
            "entries": entries,
            "counts": counts,
            "all_lowest_common_i386": False,
        }

    for path in sorted(libdir.iterdir()):
        if not path.is_file():
            continue
        if path.suffix not in {".a", ".so"} and ".so." not in path.name:
            continue
        info = inspect_library(path)
        entries.append(info)
        if info.get("class") == "ELF32" and info.get("machine") == "Intel 80386":
            counts["elf32_i386"] += 1
        elif info.get("class") == "ELF64" and info.get("machine") == "Advanced Micro Devices X86-64":
            counts["elf64_x86_64"] += 1
        else:
            counts["other"] += 1

    all_i386 = bool(entries) and (counts["elf64_x86_64"] == 0) and (counts["other"] == 0)
    return {
        "libdir": str(libdir),
        "exists": True,
        "entries": entries,
        "counts": counts,
        "all_lowest_common_i386": all_i386,
    }


def compile_probe(output_dir: Path) -> dict:
    output_dir.mkdir(parents=True, exist_ok=True)
    source_path = output_dir / "i386_probe.c"
    binary_path = output_dir / "i386_probe"
    source_path.write_text(PROBE_SOURCE, encoding="utf-8")
    compiler = shutil.which("gcc")
    if compiler is None:
        return {
            "compiler": "missing",
            "supported": False,
            "binary": str(binary_path),
            "compile_stdout": "",
            "compile_stderr": "gcc not found",
        }

    cmd = [compiler, *PROBE_CFLAGS, str(source_path), "-o", str(binary_path)]
    proc = run_capture(cmd)
    result = {
        "compiler": compiler,
        "command": cmd,
        "supported": proc.returncode == 0,
        "binary": str(binary_path),
        "compile_stdout": proc.stdout.strip(),
        "compile_stderr": proc.stderr.strip(),
    }
    if proc.returncode == 0:
        result["binary_header"] = readelf_header(binary_path)
    return result


def run_probe(binary_path: Path) -> dict:
    emulator = shutil.which("qemu-i386")
    if emulator is None:
        return {
            "emulator": "missing",
            "supported": False,
            "stdout": "",
            "stderr": "qemu-i386 not found",
        }
    proc = run_capture([emulator, str(binary_path)])
    return {
        "emulator": emulator,
        "command": [emulator, str(binary_path)],
        "supported": proc.returncode == 0,
        "returncode": proc.returncode,
        "stdout": proc.stdout.strip(),
        "stderr": proc.stderr.strip(),
    }


def build_report() -> dict:
    env = read_env()
    prefix = Path(env["PREFIX"])
    output_dir = PROFILE_DIR / "i386-denominator"
    compile_info = compile_probe(output_dir)
    run_info = (
        run_probe(Path(compile_info["binary"]))
        if compile_info.get("supported")
        else {
            "emulator": shutil.which("qemu-i386") or "missing",
            "supported": False,
            "stdout": "",
            "stderr": "probe compile failed",
        }
    )
    libs_info = inspect_prefix_libs(prefix)

    strict_ready = bool(compile_info.get("supported")) and bool(run_info.get("supported")) and bool(
        libs_info.get("all_lowest_common_i386")
    )
    blockers = []
    if not compile_info.get("supported"):
        blockers.append("32-bit lowest-common-i386 probe does not compile")
    if not run_info.get("supported"):
        blockers.append("qemu-i386 does not run the compiled probe")
    if not libs_info.get("all_lowest_common_i386"):
        blockers.append("workspace libraries are not all ELF32/Intel 80386")

    return {
        "schema": 1,
        "scenario": "lowest-common-i386-readiness",
        "target_abi": I386_TARGET_ABI,
        "host": env.get("HOST", ""),
        "workspace": env.get("TARGET_WORKSPACE", ""),
        "prefix": str(prefix),
        "probe_cflags": PROBE_CFLAGS,
        "compile_probe": compile_info,
        "run_probe": run_info,
        "workspace_libraries": libs_info,
        "strict_ready": strict_ready,
        "blockers": blockers,
    }


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        default=str(PROFILE_DIR / "i386-denominator.json"),
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Return non-zero unless the current workspace is fully ready for lowest-common-i386 builds.",
    )
    args = parser.parse_args()

    report = build_report()
    write_json(Path(args.output), report)
    print(json.dumps(report, indent=2))

    if args.strict and not report["strict_ready"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
