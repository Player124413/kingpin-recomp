#!/bin/bash
# Starts the recompiled Kingpin (macOS). Extra arguments go to the game, e.g.
#   ./run.sh +map sr1
#   ./run.sh +set developer 1      (enables the console, ` key)
cd "$(dirname "$0")"
exec ./build/kingpin "$@"
