#!/usr/bin/env bash
# Deterministic instruction counts (cachegrind Ir) of `clice index` on two
# corpus files; three runs of NEW (A/A) and one of OLD.
set -uo pipefail
CORPUS="$1"; OLD="$2"; NEW="$3"
ws=cg-ws; rm -rf $ws; mkdir -p $ws
cp "$CORPUS/sqlite3.c" "$CORPUS/reflection.cpp" $ws/
python3 - "$PWD/$ws" <<'PY'
import json,sys
d=sys.argv[1]
json.dump([{"directory":d,"file":f"{d}/sqlite3.c","arguments":["clang","--target=x86_64-unknown-linux-gnu","-std=c11","-w","-c","sqlite3.c"]},
           {"directory":d,"file":f"{d}/reflection.cpp","arguments":["clang++","--target=x86_64-unknown-linux-gnu","-std=c++17","-w","-c","reflection.cpp"]}],
          open(f"{d}/compile_commands.json","w"))
PY
for run in NEW1 NEW2 NEW3 OLD1; do
  bin=$NEW; [ "${run#OLD}" != "$run" ] && bin=$OLD
  rm -rf $ws/.clice cg.out.*
  s=$(date +%s)
  valgrind --tool=cachegrind --cache-sim=no --trace-children=yes --cachegrind-out-file=cg.out.%p \
    "$bin" index --workers 1 --workspace "$PWD/$ws" >/dev/null 2>cg.err || { tail -20 cg.err; }
  e=$(date +%s)
  echo "$run seconds=$((e-s))"
  for f in cg.out.*; do echo "  $f $(grep -E '^(summary|cmd):' $f | tr '\n' ' ' | cut -c1-160)"; done
done
