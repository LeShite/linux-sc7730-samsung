#!/usr/bin/env bash
# Install a locally built Box86 package into Batocera's writable userdata partition.
# Usage on the tablet: install-box86-userdata.sh /userdata/system/box86-armv7.tar.gz
set -euo pipefail

if command -v box86 >/dev/null 2>&1 && [[ -x /userdata/system/box86/run_portal.sh ]]; then
	echo "Box86 is already available at $(command -v box86)"
	exit 0
fi

PACKAGE=${1:-/userdata/system/box86-armv7.tar.gz}
PREFIX=${BOX86_PREFIX:-/userdata/system/box86}
[[ -r "$PACKAGE" ]] || { echo "Box86 package not found: $PACKAGE" >&2; exit 1; }
LISTING=$(tar -tzf "$PACKAGE")
grep -Fxq 'box86/bin/box86' <<<"$LISTING" || {
	echo "package does not contain box86/bin/box86" >&2
	exit 1
}
grep -Fxq 'box86/run_portal.sh' <<<"$LISTING" || {
	echo "package does not contain box86/run_portal.sh" >&2
	exit 1
}
grep -Fxq 'box86/portal-rotation-hook.sh' <<<"$LISTING" || {
	echo "package does not contain box86/portal-rotation-hook.sh" >&2
	exit 1
}
[[ "$PREFIX" == */box86 ]] || { echo "BOX86_PREFIX must end in /box86" >&2; exit 1; }
PARENT=${PREFIX%/box86}
mkdir -p "$PARENT"
tar -xzf "$PACKAGE" -C "$PARENT" --no-same-owner --no-same-permissions
chmod 0755 "$PREFIX/bin/box86"
chmod 0755 "$PREFIX/run_portal.sh"
chmod 0755 "$PREFIX/portal-rotation-hook.sh"
echo "Installed persistent Box86 at $PREFIX/bin/box86"
echo "No binfmt registration is needed when run_portal.sh invokes Box86 directly."
