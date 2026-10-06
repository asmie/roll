#!/bin/sh
# Generate separate parser and round-trip corpora with the same old-file
# fixture used by fuzz_apply. Build RH_BUILD_FUZZERS=ON first.
set -eu
ROLLING_HASH="${1:?usage: make_corpus.sh <rolling_hash> <outdir>}"
OUT="${2:?usage: make_corpus.sh <rolling_hash> <outdir>}"
GENERATOR="$(dirname "$ROLLING_HASH")/fuzz_make_corpus"
"$GENERATOR" "$OUT"
