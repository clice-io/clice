#!/usr/bin/env bash
# ThinLTO link-cache measurements on the Bazel spike, run by
# .github/workflows/bazel-lto.yml from the repository root with xclang on
# PATH. Every link of --define=llvm=bitcode is one of the variants; results go
# to lto-out/results.txt, with each build's log and profile next to it.
#
#   lto-bench.sh build   compile and link everything once, into the disk cache
#   lto-bench.sh probes  where the linker can write its cache, on a tiny program
#   lto-bench.sh full    every variant, interleaved, on this machine
#   lto-bench.sh ci      a CI run on the disk cache and ThinLTO cache a full
#                        run saved: file times after the restore, then edits
set -uo pipefail

case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    *) os=windows ;;
esac
exe=
[ "$os" = windows ] && exe=.exe

CACHE=${LTO_CACHE:?}
OUT=lto-out
mkdir -p "$OUT"
read -r -a bazel <<< "bazel ${STARTUP:-}"
py=$(command -v python3 || command -v python)
common=(--define=llvm=bitcode)
if [ "$os" = windows ]; then
    # Git Bash would turn //lto_probe:main into a path.
    export MSYS2_ARG_CONV_EXCL='*'
    # bazel/workspace_status.sh hung for four hours on a Windows runner
    # (run 37027122711); the version falls back to 0.1.0 without it.
    common+=(--workspace_status_command=)
fi
probe_src=src/driver/query.cc

cache_flag() {
    if [ "$os" = macos ]; then
        echo "--linkopt=-Wl,-cache_path_lto,$1"
    else
        echo "--linkopt=-Wl,--thinlto-cache-dir=$1"
    fi
}
lto=("$(cache_flag "$CACHE")")
writable=()
[ "$os" != windows ] && writable=("--sandbox_writable_path=$CACHE")

log() { printf '%s\n' "$*" | tee -a "$OUT/results.txt"; }
now() { "$py" -c 'import time; print(time.time())'; }
since() { "$py" -c "print('%.1fs' % ($(now) - $1))"; }
sha() { "$py" -c 'import hashlib, sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest()[:16])' "$1"; }
procs() { grep -o '[0-9]* processes:.*' "$1" | tail -1; }

entries() {
    local n kb
    n=$(find "$1" -maxdepth 1 -type f -name 'llvmcache-*' ! -name llvmcache.timestamp 2>/dev/null | wc -l | tr -d ' ')
    kb=$(du -sk "$1" 2>/dev/null | cut -f1)
    echo "entries=$n size=$((${kb:-0} / 1024))MiB"
}

wipe() {
    rm -rf "$CACHE"
    mkdir -p "$CACHE"
}

# link <label> <target> [bazel flags]: relinks //:<target> and logs the link
# action's time from Bazel's profile, the build's wall time, the strategy and
# the output's hash.
link() {
    local label=$1 target=$2
    shift 2
    rm -f "bazel-bin/$target$exe"
    local t0 rc wall action h=-
    t0=$(now)
    "${bazel[@]}" build "${common[@]}" --profile="$OUT/$label.profile.json" "$@" "//:$target" > "$OUT/$label.log" 2>&1
    rc=$?
    wall=$(since "$t0")
    action=$("$py" scripts/lto-profile.py "$OUT/$label.profile.json" "$target")
    [ $rc = 0 ] && h=$(sha "bazel-bin/$target$exe")
    log "$label	$target	rc=$rc	wall=$wall	$action	sha=$h	$(procs "$OUT/$label.log")	$(entries "$CACHE")"
    [ $rc = 0 ] || tail -20 "$OUT/$label.log"
}

# A change to the code of one translation unit of clice's own.
edit_plain() {
    printf '\nextern "C" int clice_lto_probe_%s() { return %s; }\n' "$1" "$1" >> "$probe_src"
}

# One that references an LLVM function no other code of clice does.
edit_llvm() {
    cat >> "$probe_src" << 'EOF'

#include "llvm/TargetParser/Host.h"

extern "C" const char* clice_lto_probe_cpu() {
    static std::string cpu = llvm::sys::getHostCPUName().str();
    return cpu.c_str();
}
EOF
}

revert() { git checkout -- "$probe_src"; }

# atime and mtime of the cache's entries, against now.
times() {
    "$py" - "$CACHE" "$1" << 'EOF' | tee -a "$OUT/results.txt"
import os, sys, time
root, label = sys.argv[1], sys.argv[2]
now = time.time()
files = [os.path.join(root, f) for f in os.listdir(root) if f.startswith("llvmcache-") and f != "llvmcache.timestamp"]
st = [os.stat(f) for f in files]
if not st:
    print(label, "no entries")
    sys.exit()
age = lambda t: "%.0fs" % (now - t)
used = sum(1 for s in st if s.st_atime > 1e9)
print(label, "entries=%d atime-age min/max=%s/%s mtime-age min/max=%s/%s atime>2001=%d" % (
    len(st), age(max(s.st_atime for s in st)), age(min(s.st_atime for s in st)),
    age(max(s.st_mtime for s in st)), age(min(s.st_mtime for s in st)), used))
ts = os.path.join(root, "llvmcache.timestamp")
if os.path.exists(ts):
    print(label, "timestamp mtime-age=%s" % age(os.stat(ts).st_mtime))
EOF
}

# Every entry's atime to 2000-01-01, its mtime kept: an entry read after this
# has a newer atime (relatime updates an atime that is not newer than mtime).
reset_atimes() {
    "$py" - "$CACHE" << 'EOF'
import os, sys
root = sys.argv[1]
for f in os.listdir(root):
    if f.startswith("llvmcache-") and f != "llvmcache.timestamp":
        p = os.path.join(root, f)
        os.utime(p, (946684800, os.stat(p).st_mtime))
EOF
}

# Where a program holds the path of a sandbox's execution root.
sandbox_paths() {
    "$py" - "$1" << 'EOF' | tee -a "$OUT/results.txt"
import re, sys
data = open(sys.argv[1], "rb").read()
hits = [m.start() for m in re.finditer(rb"/sandbox/[a-z-]+-sandbox/[0-9]+/", data)]
example = data[max(0, hits[0] - 60):hits[0] + 80] if hits else b""
print("sandbox paths in %s: %d %r" % (sys.argv[1], len(hits), example))
EOF
}

env_info() {
    log "== $os $(uname -srm) cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo "$NUMBER_OF_PROCESSORS")"
    log "clang: $(clang --version | head -1)"
    [ "$os" = macos ] && log "ld: $(ld -v 2>&1 | head -1)"
    [ "$os" = linux ] && log "fs of $CACHE: $(findmnt -no SOURCE,FSTYPE,OPTIONS -T "$(dirname "$CACHE")")"
    [ "$os" = macos ] && log "fs of $CACHE: $(mount | grep "^$(df /private/var/tmp | tail -1 | cut -d' ' -f1) ")"
}

case ${1:?} in
build)
    env_info
    t0=$(now)
    "${bazel[@]}" build "${common[@]}" --disk_cache="$DISK_CACHE" --repository_cache="$REPO_CACHE" \
        --profile="$OUT/build.profile.json" //:clice //:unit_tests > "$OUT/build.log" 2>&1
    rc=$?
    log "build	rc=$rc	wall=$(since "$t0")	$(procs "$OUT/build.log")"
    [ $rc = 0 ] || { tail -40 "$OUT/build.log"; exit 1; }
    ;;

stamp)
    # Two commits in a row on one disk cache: what the stamped version.h
    # (bazel/workspace_status.sh) does to the keys of clice's compiles.
    libs=(//:core //:config //:project //:worker //:sched //:server)
    "${bazel[@]}" build "${common[@]}" --disk_cache="$DISK_CACHE" "${libs[@]}" > "$OUT/stamp-a.log" 2>&1
    log "stamp-a	rc=$?	$(procs "$OUT/stamp-a.log")	$(grep -o 'CLICE_VERSION_STRING.*' bazel-bin/generated/version.h)"
    "${bazel[@]}" clean > /dev/null 2>&1
    "${bazel[@]}" build "${common[@]}" --disk_cache="$DISK_CACHE" "${libs[@]}" > "$OUT/stamp-same.log" 2>&1
    log "stamp-same	rc=$?	$(procs "$OUT/stamp-same.log")"
    git -c user.name=bench -c user.email=bench@localhost commit -q --allow-empty -m stamp
    "${bazel[@]}" clean > /dev/null 2>&1
    "${bazel[@]}" build "${common[@]}" --disk_cache="$DISK_CACHE" "${libs[@]}" > "$OUT/stamp-b.log" 2>&1
    log "stamp-b	rc=$?	$(procs "$OUT/stamp-b.log")	$(grep -o 'CLICE_VERSION_STRING.*' bazel-bin/generated/version.h)"
    ;;

probes)
    # The tiny program, each time with a cache directory of its own: is the
    # link refused, or does it succeed with the entries written (or lost)?
    probe() {
        local label=$1 dir=$2
        shift 2
        rm -f "bazel-bin/lto_probe/main$exe"
        "${bazel[@]}" build "${common[@]}" "$(cache_flag "$dir")" "$@" //lto_probe:main > "$OUT/$label.log" 2>&1
        local rc=$?
        log "$label	$dir	rc=$rc	$(procs "$OUT/$label.log")	$(entries "$dir")	$(grep -m1 -o 'LLVM ERROR.*\|I/O exception.*\|ld: .*error.*' "$OUT/$label.log")"
    }
    base=${CACHE%/*}/lto-probe
    rm -rf "$base" "/tmp/lto-probe"
    probe probe-absent "$base/absent"
    mkdir -p "$base/exists" "$base/writable" "$base/local" "/tmp/lto-probe"
    probe probe-exists "$base/exists"
    if [ "$os" != windows ]; then
        probe probe-writable "$base/writable" --sandbox_writable_path="$base/writable"
        probe probe-writable-absent "$base/wabsent" --sandbox_writable_path="$base/wabsent"
        probe probe-local "$base/local" --strategy=CppLink=local
        probe probe-tmp-writable /tmp/lto-probe --sandbox_writable_path=/tmp/lto-probe
    fi
    if [ "$os" = macos ]; then
        mkdir -p "/private$base/real"
        probe probe-private-path "/private$base/real" --sandbox_writable_path="/private$base/real"
        mkdir -p "$base/link-only-real"
        probe probe-symlinked-writable "$base/link-only-real" --sandbox_writable_path="/private$base/link-only-real"
    fi
    ;;

full)
    wipe
    link n1 clice
    wipe
    link c1 clice "${lto[@]}" "${writable[@]}"
    link w1 clice "${lto[@]}" "${writable[@]}"
    link n2 clice
    wipe
    link c2 clice "${lto[@]}" "${writable[@]}"
    link w2 clice "${lto[@]}" "${writable[@]}"
    if [ "$os" != windows ]; then
        link wl1 clice "${lto[@]}" --strategy=CppLink=local
    fi
    link w3 clice "${lto[@]}" "${writable[@]}"
    if [ "$os" != windows ]; then
        link wl2 clice "${lto[@]}" --strategy=CppLink=local
    fi

    link un1 unit_tests
    wipe
    link c3 clice "${lto[@]}" "${writable[@]}"
    link uc1 unit_tests "${lto[@]}" "${writable[@]}"
    link uw1 unit_tests "${lto[@]}" "${writable[@]}"

    edit_plain 1
    link e1w1 clice "${lto[@]}" "${writable[@]}"
    edit_plain 2
    link e1w2 clice "${lto[@]}" "${writable[@]}"
    edit_llvm
    link e2w1 clice "${lto[@]}" "${writable[@]}"
    link e2n1 clice
    link e2w2 clice "${lto[@]}" "${writable[@]}"
    revert
    times full-end
    ;;

win)
    # Windows' links take minutes more than the others': full's variants
    # that answer something, once.
    wipe
    link n1 clice
    wipe
    link c1 clice "${lto[@]}"
    link w1 clice "${lto[@]}"
    link w2 clice "${lto[@]}"
    stamp=(--linkopt=-Wl,--no-insert-timestamp)
    link t-w1 clice "${lto[@]}" "${stamp[@]}"
    link t-w2 clice "${lto[@]}" "${stamp[@]}"
    link t-n1 clice "${stamp[@]}"
    link uc1 unit_tests "${lto[@]}"
    link uw1 unit_tests "${lto[@]}"
    edit_plain 1
    link e1w1 clice "${lto[@]}"
    edit_llvm
    link e2w1 clice "${lto[@]}" "${stamp[@]}"
    link e2n1 clice "${stamp[@]}"
    revert
    times win-end
    ;;

ci)
    times restored
    reset_atimes
    edit_plain 1
    link ci-e1 clice "${lto[@]}" "${writable[@]}"
    times after-e1
    link ci-u1 unit_tests "${lto[@]}" "${writable[@]}"
    times after-u1
    edit_llvm
    link ci-e2 clice "${lto[@]}" "${writable[@]}"
    times after-e2
    sandbox_paths bazel-bin/clice$exe

    # The same output with and without the cache, from links whose execution
    # root is the same path (not a sandbox's).
    link ci-e2-local clice "${lto[@]}" --strategy=CppLink=local
    link ci-e2-local-n clice --strategy=CppLink=local
    link ci-e2-local-2 clice "${lto[@]}" --strategy=CppLink=local

    # macOS: ld records each object's absolute path (the sandbox's) unless
    # -oso_prefix makes it relative (rules_cc's macos_reproducible feature).
    if [ "$os" = macos ]; then
        oso=(--linkopt=-Wl,-oso_prefix,.)
        link ci-oso-1 clice "${lto[@]}" "${writable[@]}" "${oso[@]}"
        sandbox_paths "bazel-bin/clice$exe"
        link ci-oso-2 clice "${lto[@]}" "${writable[@]}" "${oso[@]}"
        link ci-oso-n clice "${oso[@]}"
    fi

    # Windows: lld puts the link's time in the PE header unless told not to.
    if [ "$os" = windows ]; then
        stamp=(--linkopt=-Wl,--no-insert-timestamp)
        link ci-stamp-1 clice "${lto[@]}" "${stamp[@]}"
        link ci-stamp-2 clice "${lto[@]}" "${stamp[@]}"
        link ci-stamp-n clice "${stamp[@]}"
    fi
    revert
    ;;
esac
