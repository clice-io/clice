#!/usr/bin/env bash
# Deterministic instruction counts (cachegrind Ir) of in-process `clice inspect
# tu_index`: three runs of NEW (A/A) and one of OLD per file.
set -uo pipefail
CORPUS="$1"; OLD="$2"; NEW="$3"; REF="$4"
cd "$CORPUS"
for spec in reflection.cpp:c++17:clang++ sqlite3.c:c11:clang; do
  IFS=: read -r f std drv <<< "$spec"
  flags="[\"$REF/bin/$drv\",\"--target=x86_64-unknown-linux-gnu\",\"-std=$std\",\"-w\"]"
  for run in NEW NEW NEW OLD; do
    bin=$NEW; [ $run = OLD ] && bin=$OLD
    s=$(date +%s)
    ir=$(valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=/dev/null \
      "$bin" inspect tu_index "$f" --flags "$flags" 2>&1 >/dev/null | grep "I *refs" | awk '{print $NF}')
    echo "CG $f $run Ir=$ir secs=$(( $(date +%s)-s ))"
  done
done
