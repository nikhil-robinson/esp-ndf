#!/bin/bash
# Flash each application folder in turn on the DEVKIT and save its console (until a line with "done", or TMO seconds)
# to bench/remeasure/data/<name>.txt.   usage: tools/capture_apps.sh [-t seconds] app-folder...
cd "$(dirname "$0")/.."
D=$(grep DEVKIT make/boards.mk | sed 's/.*= *//')
TMO=40; [ "$1" = "-t" ] && { TMO=$2; shift 2; }
mkdir -p bench/remeasure/data
for a in "$@"; do
  [ -e "$D" ] || { echo "== $a: board port missing, stopping"; exit 1; }
  n=$(basename $a)
  make -C $a -j8 flash PORT=$D >/dev/null 2>&1 || { echo "== $a: BUILD/FLASH FAILED"; continue; }
  timeout $((TMO + 20)) python3 tools/run_app.py $D "done" $TMO > bench/remeasure/data/$n.txt 2>&1
  echo "== $n: $(grep -c . bench/remeasure/data/$n.txt) lines; $(grep -m1 -E 'FAIL|PANIC' bench/remeasure/data/$n.txt | cut -c1-80)"
done
