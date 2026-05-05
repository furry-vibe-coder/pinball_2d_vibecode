#!/usr/bin/env sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
UCRT64_BIN="/c/GameDev/SDL/msys2/ucrt64/bin"
if [ -d "$UCRT64_BIN" ]; then
  export PATH="$UCRT64_BIN:$PATH"
fi
cd "$ROOT"
./pinball.exe
