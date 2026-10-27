#!/usr/bin/env bash
# Best-effort per-Portal KMS rotation hook for Batocera v38.
# On this T280 image batocera-resolution reports no system rotation, so this
# logs the limitation and leaves the ES software rotation setting untouched.
set -u

ACTION=${1:-}
STATE=/tmp/portal-kms-rotation.state
LOG_DIR=${PORTAL_LOG_DIR:-/userdata/system/logs}
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/portal-rotation.log"

case "$ACTION" in
	start)
		if ! command -v batocera-resolution >/dev/null 2>&1 ||
		   ! batocera-resolution supportSystemRotation >/dev/null 2>&1; then
			echo "$(date -Is) KMS output does not support system rotation; Portal may display sideways." >> "$LOG"
			exit 0
		fi
		OUTPUT=$(batocera-resolution currentOutput 2>/dev/null || true)
		if [[ -z "$OUTPUT" ]]; then
			echo "$(date -Is) Could not identify active DRM output; leaving rotation unchanged." >> "$LOG"
			exit 0
		fi
		PREVIOUS=$(/usr/bin/batocera-settings-get-master "display.rotate.$OUTPUT" 2>/dev/null || true)
		[[ -n "$PREVIOUS" ]] || PREVIOUS=$(/usr/bin/batocera-settings-get-master display.rotate 2>/dev/null || true)
		[[ "$PREVIOUS" =~ ^[0-3]$ ]] || PREVIOUS=0
		printf '%s\n%s\n' "$OUTPUT" "$PREVIOUS" > "$STATE"
		if batocera-resolution setRotation "${PORTAL_KMS_ROTATION:-1}"; then
			echo "$(date -Is) Set $OUTPUT system rotation to ${PORTAL_KMS_ROTATION:-1}; will restore $PREVIOUS on exit." >> "$LOG"
		else
			rm -f "$STATE"
			echo "$(date -Is) DRM rotation change failed; continuing with existing orientation." >> "$LOG"
		fi
		;;
	stop)
		if [[ -r "$STATE" ]]; then
			IFS= read -r OUTPUT < "$STATE"
			PREVIOUS=$(sed -n '2p' "$STATE")
			if [[ "$OUTPUT" && "$PREVIOUS" =~ ^[0-3]$ ]]; then
				batocera-resolution setRotation "$PREVIOUS" &&
					echo "$(date -Is) Restored $OUTPUT system rotation to $PREVIOUS." >> "$LOG"
			fi
			rm -f "$STATE"
		fi
		;;
	*)
		echo "usage: $0 start|stop" >&2
		exit 2
		;;
esac
