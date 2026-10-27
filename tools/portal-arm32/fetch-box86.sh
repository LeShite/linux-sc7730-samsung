#!/usr/bin/env bash
# Fetch the official Box86 source into this directory.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
DEST="$HERE/box86"

if [[ -d "$DEST/.git" ]]; then
	git -C "$DEST" pull --ff-only
else
	if [[ -d "$DEST" ]]; then
		rmdir "$DEST" 2>/dev/null || { echo "refusing to replace non-empty $DEST" >&2; exit 1; }
	fi
	git clone --depth 1 https://github.com/ptitSeb/box86.git "$DEST"
fi
