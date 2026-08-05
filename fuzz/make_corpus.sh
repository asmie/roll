#!/bin/sh
# Generate a fuzz seed corpus using the built binary.
#
# The seeds are produced rather than committed on purpose: a checked-in delta is
# pinned to the format version that created it, so after a version bump it would
# only ever exercise the version-rejection path and would quietly stop covering
# the parser. Generating them means the corpus is always current, at the cost of
# needing a working binary first.
#
# Usage: make_corpus.sh <path-to-rolling_hash> <output-dir>

set -eu

ROLLING_HASH="${1:?usage: make_corpus.sh <rolling_hash> <outdir>}"
OUT="${2:?usage: make_corpus.sh <rolling_hash> <outdir>}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

# Each pair targets a distinct shape the readers must handle. Content is
# generated with printf/dd rather than /dev/urandom so the corpus is
# reproducible across runs.
gen() {
	name="$1"; old="$2"; new="$3"
	printf '%s' "$old" > "$WORK/$name.old"
	printf '%s' "$new" > "$WORK/$name.new"
	"$ROLLING_HASH" create "$WORK/$name.old" "$WORK/$name.new" "$OUT/$name.delta" >/dev/null
}

# Small, fully-specified shapes.
gen tiny            'x'                'y'
gen empty_old       ''                 'appeared from nothing'
gen empty_new       'about to vanish'  ''
gen both_empty      ''                 ''
gen identical       'unchanged bytes'  'unchanged bytes'
gen in_chunk_edit   'the quick brown fox' 'the quick brawn fox'
gen prefix_grow     'shared'           'shared and then some'
gen suffix_shrink   'shared and then some' 'shared'

# Larger shapes that span several chunks and exercise boundary handling,
# duplicate reuse, and the literal-versus-diff decision.
yes 'ABCDEFGHIJKLMNOP' 2>/dev/null | head -c 40000 > "$WORK/rep.old" || true
yes 'ABCDEFGHIJKLMNOP' 2>/dev/null | head -c 80000 > "$WORK/rep.new" || true
"$ROLLING_HASH" create "$WORK/rep.old" "$WORK/rep.new" "$OUT/repetitive.delta" >/dev/null

head -c 40000 /dev/zero > "$WORK/zero.old"
head -c 60000 /dev/zero > "$WORK/zero.new"
"$ROLLING_HASH" create "$WORK/zero.old" "$WORK/zero.new" "$OUT/zeros.delta" >/dev/null

# Reordered blocks, so ORIGINAL entries reference content that moved.
head -c 20000 "$WORK/rep.old" > "$WORK/a.part"
tail -c 20000 "$WORK/rep.old" > "$WORK/b.part"
cat "$WORK/a.part" "$WORK/b.part" > "$WORK/reorder.old"
cat "$WORK/b.part" "$WORK/a.part" > "$WORK/reorder.new"
"$ROLLING_HASH" create "$WORK/reorder.old" "$WORK/reorder.new" "$OUT/reordered.delta" >/dev/null

printf 'generated %s seed(s) in %s\n' "$(ls -1 "$OUT" | wc -l)" "$OUT"
