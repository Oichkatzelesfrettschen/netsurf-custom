#!/bin/sh
# fetch-spec-idl.sh -- download current WebIDL extracts from w3c/webref
#
# WHY: Provides authoritative interface definitions for spec-coverage analysis.
# WHAT: Downloads .idl files to tools/spec-idl/ and writes manifest.json.
# HOW: curl from w3c/webref curated branch on GitHub; run monthly to update.
#
# Usage: sh tools/fetch-spec-idl.sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/spec-idl"
BASE_URL="https://raw.githubusercontent.com/w3c/webref/curated/ed/idl"

SPECS="dom html url uievents cssom console xhr fetch hr-time storage geometry"

mkdir -p "${OUT_DIR}"

ok=0
fail=0
for spec in ${SPECS}; do
    url="${BASE_URL}/${spec}.idl"
    dest="${OUT_DIR}/${spec}.idl"
    printf "Fetching %s.idl ... " "${spec}"
    if curl -sfL -o "${dest}" "${url}"; then
        printf "ok (%s bytes)\n" "$(wc -c < "${dest}" | tr -d ' ')"
        ok=$((ok + 1))
    else
        printf "FAILED\n" >&2
        fail=$((fail + 1))
    fi
done

# Write manifest
timestamp="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
cat > "${OUT_DIR}/manifest.json" <<MANIFEST
{
  "source": "${BASE_URL}/",
  "branch": "curated",
  "fetched": "${timestamp}",
  "specs": [$(printf '"%s", ' ${SPECS} | sed 's/, $//')],
  "ok": ${ok},
  "failed": ${fail}
}
MANIFEST

printf "\nDone: %d ok, %d failed. Manifest written to %s/manifest.json\n" \
    "${ok}" "${fail}" "${OUT_DIR}"

if [ "${fail}" -gt 0 ]; then
    exit 1
fi
