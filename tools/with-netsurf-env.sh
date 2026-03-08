#!/usr/bin/env bash
set -eo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

unset HOST

if [ -z "${TARGET_WORKSPACE:-}" ]; then
    TARGET_WORKSPACE="${ROOT_DIR}/projects"
fi
export TARGET_WORKSPACE

cd "${ROOT_DIR}"

source "${ROOT_DIR}/docs/env.sh" >/dev/null

if [ "${1:-}" = "--print-env" ]; then
    printf 'ROOT_DIR=%s\n' "${ROOT_DIR}"
    printf 'BUILD=%s\n' "${BUILD:-}"
    printf 'HOST=%s\n' "${HOST:-}"
    printf 'TARGET_WORKSPACE=%s\n' "${TARGET_WORKSPACE:-}"
    printf 'USE_CPUS=%s\n' "${USE_CPUS:-}"
    printf 'PREFIX=%s\n' "${PREFIX:-}"
    printf 'BUILD_PREFIX=%s\n' "${BUILD_PREFIX:-}"
    printf 'PKG_CONFIG_PATH=%s\n' "${PKG_CONFIG_PATH:-}"
    printf 'NS_BUILDSYSTEM=%s\n' "${NS_BUILDSYSTEM:-}"
    printf 'NS_INTERNAL_LIBS=%s\n' "${NS_INTERNAL_LIBS:-}"
    printf 'NS_FRONTEND_LIBS=%s\n' "${NS_FRONTEND_LIBS:-}"
    printf 'NS_TOOLS=%s\n' "${NS_TOOLS:-}"
    printf 'BUILD_TARGET=%s\n' "${BUILD_TARGET:-}"
    exit 0
fi

if [ "${1:-}" = "--shell" ]; then
    shift
    if [ "$#" -eq 0 ]; then
        echo "error: --shell requires a command string" >&2
        exit 1
    fi
    eval "$1"
    exit 0
fi

if [ "${1:-}" = "--" ]; then
    shift
fi

if [ "$#" -eq 0 ]; then
    exec "${SHELL:-/bin/bash}" -i
fi

exec "$@"
