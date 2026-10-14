#!/bin/bash
# Loads every map for a while and reports traps/faults and the frame rate.
# usage: tools/smoke_maps.sh [seconds] [parallel]
cd "$(dirname "$0")/.."
secs=${1:-25}; par=${2:-4}
maps=$(python3 tools/pak.py game/main/Pak0.pak .bsp | awk '{print $2}' | sed 's#maps/##; s#\.bsp##')
run() {
  m=$1
  ./build/kingpin --seconds $secs +set developer 1 +map $m > smoke/$m.log 2>&1
  code=$?
  fps=$(grep -o '[0-9.]* fps' smoke/$m.log | tail -1)
  bad=$(grep -E 'TRAP|FAULT|ERROR|Tragic' smoke/$m.log | head -1 | cut -c1-120)
  echo "$m exit=$code ${fps:-no-frames} $bad"
}
export -f run; export secs
printf '%s\n' $maps | xargs -P $par -I{} bash -c 'run {}'
