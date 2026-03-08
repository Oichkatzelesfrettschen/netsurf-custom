#!/usr/bin/env python3
"""Rebuild the low-spec workspace libraries as ELF32/i386."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
ENV_HELPER = ROOT / "tools" / "with-netsurf-env.sh"
PROFILE_DIR = ROOT / "build" / "profiles"
LOG_PATH = PROFILE_DIR / "i386-workspace-rebuild.log"
JSON_PATH = PROFILE_DIR / "i386-workspace-rebuild.json"
TARGET_ABI = "i386-pc-linux-gnu"
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
TOOLCHAIN_DIR = PROFILE_DIR / "i386-toolchain"
LIBNSFB_MAKEFILE = ROOT / "projects" / "libnsfb" / "Makefile"
LIB_COMPONENTS = [
    "buildsystem",
    "libwapcaplet",
    "libparserutils",
    "libhubbub",
    "libdom",
    "libcss",
    "libnsgif",
    "libnsbmp",
    "libnsutils",
    "libsvgtiny",
]
LIBNSFB_LOW_SPEC_OLD = """NSFB_XCB_PKG_NAMES := xcb xcb-icccm xcb-image xcb-keysyms xcb-atom

# determine which surface handlers can be compiled based upon avalable library
$(eval $(call pkg_config_package_available,NSFB_VNC_AVAILABLE,libvncserver))
$(eval $(call pkg_config_package_available,NSFB_SDL_AVAILABLE,sdl))
$(eval $(call pkg_config_package_available,NSFB_XCB_AVAILABLE,$(NSFB_XCB_PKG_NAMES)))
$(eval $(call pkg_config_package_available,NSFB_WLD_AVAILABLE,wayland-client))
"""
LIBNSFB_LOW_SPEC_NEW = """NSFB_XCB_PKG_NAMES := xcb xcb-icccm xcb-image xcb-keysyms xcb-atom
NSFB_LOW_SPEC ?= no

# determine which surface handlers can be compiled based upon avalable library
ifeq ($(NSFB_LOW_SPEC),yes)
NSFB_VNC_AVAILABLE := no
NSFB_SDL_AVAILABLE := no
NSFB_XCB_AVAILABLE := no
NSFB_WLD_AVAILABLE := no
else
$(eval $(call pkg_config_package_available,NSFB_VNC_AVAILABLE,libvncserver))
$(eval $(call pkg_config_package_available,NSFB_SDL_AVAILABLE,sdl))
$(eval $(call pkg_config_package_available,NSFB_XCB_AVAILABLE,$(NSFB_XCB_PKG_NAMES)))
$(eval $(call pkg_config_package_available,NSFB_WLD_AVAILABLE,wayland-client))
endif
"""


def shell_env_dump() -> str:
    env_lines = [
        'printf "ROOT_DIR=%s\\n" "${ROOT_DIR:-}"',
        'printf "BUILD=%s\\n" "${BUILD:-}"',
        'printf "HOST=%s\\n" "${HOST:-}"',
        'printf "TARGET_WORKSPACE=%s\\n" "${TARGET_WORKSPACE:-}"',
        'printf "USE_CPUS=%s\\n" "${USE_CPUS:-}"',
        'printf "PREFIX=%s\\n" "${PREFIX:-}"',
        'printf "BUILD_PREFIX=%s\\n" "${BUILD_PREFIX:-}"',
        'printf "PKG_CONFIG_PATH=%s\\n" "${PKG_CONFIG_PATH:-}"',
    ]
    return "; ".join(env_lines)


def read_env() -> dict[str, str]:
    proc = subprocess.run(
        [
            str(ENV_HELPER),
            "--shell",
            f'unset HOST; export TARGET_ABI="{TARGET_ABI}"; source "{ROOT / "docs" / "env.sh"}" >/dev/null; {shell_env_dump()}',
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


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def ensure_compiler_wrapper(name: str, compiler: str) -> Path:
    TOOLCHAIN_DIR.mkdir(parents=True, exist_ok=True)
    path = TOOLCHAIN_DIR / name
    script = "#!/bin/sh\nexec {} {} \"$@\"\n".format(compiler, " ".join(I386_FLAGS))
    path.write_text(script, encoding="utf-8")
    path.chmod(0o755)
    return path


def cleanup_prefix(prefix: Path) -> None:
    targets = [
        prefix / "lib",
        prefix / "include",
        prefix / "share" / "netsurf-buildsystem",
    ]
    for path in targets:
        if path.is_dir():
            shutil.rmtree(path)


def ensure_low_spec_libnsfb(log: list[str]) -> str | None:
    if not LIBNSFB_MAKEFILE.exists():
        raise FileNotFoundError(f"missing {LIBNSFB_MAKEFILE}")

    content = LIBNSFB_MAKEFILE.read_text(encoding="utf-8")
    if "NSFB_LOW_SPEC ?= no" in content:
        log.append(f"libnsfb low-spec support already present in {LIBNSFB_MAKEFILE}")
        return None
    if LIBNSFB_LOW_SPEC_OLD not in content:
        raise RuntimeError(f"unable to patch {LIBNSFB_MAKEFILE} for low-spec mode")

    LIBNSFB_MAKEFILE.write_text(
        content.replace(LIBNSFB_LOW_SPEC_OLD, LIBNSFB_LOW_SPEC_NEW, 1),
        encoding="utf-8",
    )
    log.append(f"patched {LIBNSFB_MAKEFILE} for NSFB_LOW_SPEC=yes support")
    return content


def restore_libnsfb_makefile(original_content: str | None, log: list[str]) -> None:
    if original_content is None:
        return
    LIBNSFB_MAKEFILE.write_text(original_content, encoding="utf-8")
    log.append(f"restored {LIBNSFB_MAKEFILE} after low-spec rebuild")


def run_make(
    repo: str,
    env: dict[str, str],
    log: list[str],
    cc_wrapper: Path,
    cxx_wrapper: Path,
    extra_args: list[str] | None = None,
) -> dict[str, object]:
    extra_args = extra_args or []
    run_env = dict(os.environ)
    run_env.update(env)
    run_env["CC"] = str(cc_wrapper)
    run_env["CXX"] = str(cxx_wrapper)
    run_env["AR"] = shutil.which("ar") or "ar"
    run_env["RANLIB"] = shutil.which("ranlib") or "ranlib"
    run_env["STRIP"] = shutil.which("strip") or "strip"
    run_env["PKG_CONFIG_PATH"] = env["PKG_CONFIG_PATH"]
    common = [
        "make",
        "-C",
        str(ROOT / "projects" / repo),
        f"HOST={env['HOST']}",
        f"PREFIX={env['PREFIX']}",
        *extra_args,
    ]
    proc_clean = subprocess.run(
        [*common, "clean"],
        cwd=ROOT,
        env=run_env,
        capture_output=True,
        text=True,
        check=False,
    )
    log.append(f"$ {' '.join([*common, 'clean'])}")
    if proc_clean.stdout:
        log.append(proc_clean.stdout.rstrip())
    if proc_clean.stderr:
        log.append(proc_clean.stderr.rstrip())
    if proc_clean.returncode != 0:
        return {
            "repo": repo,
            "command": [*common, "clean"],
            "returncode": proc_clean.returncode,
            "succeeded": False,
        }
    proc = subprocess.run(
        [*common, "install"],
        cwd=ROOT,
        env=run_env,
        capture_output=True,
        text=True,
        check=False,
    )
    log.append(f"$ {' '.join([*common, 'install'])}")
    if proc.stdout:
        log.append(proc.stdout.rstrip())
    if proc.stderr:
        log.append(proc.stderr.rstrip())
    return {
        "repo": repo,
        "command": [*common, "install"],
        "returncode": proc.returncode,
        "succeeded": proc.returncode == 0,
    }


def read_libnsfb_pc(prefix: Path) -> dict[str, object]:
    pc_path = prefix / "lib" / "pkgconfig" / "libnsfb.pc"
    info: dict[str, object] = {
        "path": str(pc_path),
        "exists": pc_path.exists(),
    }
    if not pc_path.exists():
        return info
    requires = ""
    libs = ""
    for line in pc_path.read_text(encoding="utf-8").splitlines():
        if line.startswith("Requires:"):
            requires = line.split(":", 1)[1].strip()
        if line.startswith("Libs:"):
            libs = line.split(":", 1)[1].strip()
    info["requires"] = requires
    info["libs"] = libs
    info["slim_linux_only"] = requires == ""
    return info


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default=str(JSON_PATH))
    parser.add_argument("--log", default=str(LOG_PATH))
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Return non-zero unless the i386 workspace rebuild succeeds.",
    )
    args = parser.parse_args()

    PROFILE_DIR.mkdir(parents=True, exist_ok=True)
    env = read_env()
    prefix = Path(env["PREFIX"])
    cleanup_prefix(prefix)
    cc_wrapper = ensure_compiler_wrapper("i386-gcc", "gcc")
    cxx_wrapper = ensure_compiler_wrapper("i386-g++", "g++")

    log: list[str] = []
    libnsfb_original = ensure_low_spec_libnsfb(log)
    results: list[dict[str, object]] = []
    try:
        for repo in LIB_COMPONENTS:
            result = run_make(repo, env, log, cc_wrapper, cxx_wrapper)
            results.append(result)
            if result["returncode"] != 0:
                break

        if all(result["succeeded"] for result in results):
            nsfb_extra = [
                "NSFB_LOW_SPEC=yes",
            ]
            results.append(run_make("libnsfb", env, log, cc_wrapper, cxx_wrapper, nsfb_extra))
    finally:
        restore_libnsfb_makefile(libnsfb_original, log)

    Path(args.log).write_text("\n\n".join(log) + ("\n" if log else ""), encoding="utf-8")
    payload = {
        "schema": 1,
        "scenario": "lowest-common-i386-workspace-rebuild",
        "target_abi": TARGET_ABI,
        "prefix": env["PREFIX"],
        "build_prefix": env["BUILD_PREFIX"],
        "flags": I386_FLAGS,
        "cc_wrapper": str(cc_wrapper),
        "cxx_wrapper": str(cxx_wrapper),
        "components": results,
        "succeeded": bool(results) and all(result["succeeded"] for result in results),
        "patched_libnsfb_makefile": libnsfb_original is not None,
        "libnsfb_pc": read_libnsfb_pc(prefix),
        "log": str(Path(args.log)),
    }
    write_json(Path(args.output), payload)
    print(json.dumps(payload, indent=2))

    if args.strict and not payload["succeeded"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
