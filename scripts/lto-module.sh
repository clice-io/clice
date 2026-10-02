#!/usr/bin/env bash
# The xclang module's thinlto_cache feature (lto_probe/module, a tool linking
# libclang's bitcode), run by .github/workflows/bazel-lto.yml from the
# repository root: the cache directory made by the module, warm links, the
# same output without the feature, the directory made again once deleted,
# and the link's disk-cache hit from another checkout path.
set -uo pipefail

case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    *) os=windows ;;
esac
exe=
[ "$os" = windows ] && exe=.exe

CACHE=${LTO_CACHE:?}-module
root=$PWD
OUT=$root/lto-out/module
mkdir -p "$OUT"
read -r -a bazel <<< "bazel ${STARTUP:-}"
py=$(command -v python3 || command -v python)
flags=("--repo_env=XCLANG_THINLTO_CACHE=$CACHE")
[ "$os" != windows ] && flags+=("--sandbox_writable_path=$CACHE")
disk=("--disk_cache=$root/../lto-module-disk")

log() { printf '%s\n' "$*" | tee -a "$OUT/results.txt"; }
now() { "$py" -c 'import time; print(time.time())'; }
since() { "$py" -c "print('%.1fs' % ($(now) - $1))"; }
sha() { "$py" -c 'import hashlib, sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest()[:16])' "$1"; }
procs() { grep -o '[0-9]* processes:.*' "$1" | tail -1; }
entries() {
    local n
    n=$(find "$1" -maxdepth 1 -type f -name 'llvmcache-*' ! -name llvmcache.timestamp 2>/dev/null | wc -l | tr -d ' ')
    echo "entries=$n exists=$([ -d "$1" ] && echo yes || echo no)"
}

# step <label> [bazel flags]: relinks //:lex in the current directory.
step() {
    local label=$1
    shift
    rm -f "bazel-bin/lex$exe"
    local t0 rc h=-
    t0=$(now)
    "${bazel[@]}" build --profile="$OUT/$label.profile.json" "$@" //:lex > "$OUT/$label.log" 2>&1
    rc=$?
    [ $rc = 0 ] && h=$(sha "bazel-bin/lex$exe")
    log "$label	rc=$rc	wall=$(since "$t0")	$("$py" "$root/scripts/lto-profile.py" "$OUT/$label.profile.json" lex)	sha=$h	$(procs "$OUT/$label.log")	$(entries "$CACHE")"
    [ $rc = 0 ] || tail -30 "$OUT/$label.log"
}

rm -rf "$CACHE" "$root/../lto-module-disk" "$root/../lto-module-copy"
cd lto_probe/module || exit 1
log "== $os, cache $CACHE"
step m-cold "${flags[@]}"
step m-warm "${flags[@]}"
"./bazel-bin/lex$exe" > "$OUT/run.txt" 2>&1
log "run	rc=$?	$(tail -1 "$OUT/run.txt")"
step m-off "${flags[@]}" --features=-thinlto_cache
step m-nocache
rm -rf "$CACHE"
step m-deleted "${flags[@]}"
step m-disk "${flags[@]}" "${disk[@]}"
if [ "$os" = windows ]; then
    step m-stamp-1 "${flags[@]}" --linkopt=-Wl,--no-insert-timestamp
    step m-stamp-2 "${flags[@]}" --linkopt=-Wl,--no-insert-timestamp
    step m-stamp-n --linkopt=-Wl,--no-insert-timestamp
fi
if [ "$os" = macos ]; then
    step m-repro-1 "${flags[@]}" --features=macos_reproducible
    step m-repro-2 "${flags[@]}" --features=macos_reproducible
    step m-repro-n --features=macos_reproducible
fi

# The same build from another checkout path, on the same disk cache.
mkdir -p "$root/../lto-module-copy"
cp MODULE.bazel BUILD.bazel .bazelrc main.cpp "$root/../lto-module-copy/"
cd "$root/../lto-module-copy" || exit 1
step m-copy "${flags[@]}" "${disk[@]}"
