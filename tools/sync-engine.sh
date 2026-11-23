#!/bin/sh
# Re-vendor the portable OWIS engine headers from a sibling checkout of github.com/brstic/owis.
# Usage: tools/sync-engine.sh [path-to-owis-repo]   (default: ../porsche-tool)
set -e
SRC="${1:-../porsche-tool}"
DST="$(dirname "$0")/../app/src/main/cpp/engine"
for h in engine pack bus isotp tp20 sim trace seedkey safety json diag_core; do
  cp "$SRC/src/$h.h" "$DST/$h.h"
done
echo "engine headers synced from $SRC -> $DST"
