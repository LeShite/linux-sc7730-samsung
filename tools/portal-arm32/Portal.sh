#!/usr/bin/env bash
# Copy this executable entry to /userdata/roms/ports/Portal.sh.
set -euo pipefail

export PORTAL_GAME_ROOT=${PORTAL_GAME_ROOT:-/userdata/roms/ports/.data/Portal_Linux/game}
RUNNER=/userdata/system/box86/run_portal.sh
if [[ ! -x "$RUNNER" ]]; then
	echo "Portal launcher is missing: $RUNNER"
	echo "Run /userdata/system/install-box86-userdata.sh first."
	exit 127
fi
exec "$RUNNER"
