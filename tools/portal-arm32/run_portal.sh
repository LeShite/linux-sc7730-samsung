#!/usr/bin/env bash
# First Box86/GL4ES launch attempt; stdout/stderr and the process status go to a log.
set -u

# Box86 is a 32-bit ARM binary and needs a 32-bit ARM userspace. The T280's
# Batocera v38 image is armhf; fail early if this runner is copied elsewhere.
MACHINE=$(uname -m 2>/dev/null || true)
BITS=$(getconf LONG_BIT 2>/dev/null || true)
CPUINFO=$(cat /proc/cpuinfo 2>/dev/null || true)
if [[ "$BITS" != 32 || "$MACHINE" != armv* ]]; then
	echo "Box86 needs a 32-bit ARM userspace (got uname=$MACHINE, LONG_BIT=$BITS)."
	echo "Use an armhf rootfs/chroot on a 64-bit ARM OS."
	exit 126
fi
if ! grep -Eiq '(^|[[:space:]])neon([[:space:]]|$)' <<<"$CPUINFO" ||
   ! grep -Eiq '(^|[[:space:]])vfpv3(d16)?([[:space:]]|$)|(^|[[:space:]])vfpv4([[:space:]]|$)' <<<"$CPUINFO"; then
	echo "Box86 ARM dynarec requires NEON and VFPv3; CPU flags were not detected."
	exit 126
fi

GAME_ROOT=${PORTAL_GAME_ROOT:-/userdata/roms/ports/.data/Portal_Linux/game}
LOG_DIR=${PORTAL_LOG_DIR:-/userdata/system/logs}
GL4ES_LIB=${GL4ES_LIB:-/usr/lib/gl4es/libGL.so.1}

if [[ ! -r "$GL4ES_LIB" && -r /userdata/system/gl4es/libGL.so.1 ]]; then
	GL4ES_LIB=/userdata/system/gl4es/libGL.so.1
fi
if [[ -n "${BOX86_BIN:-}" ]]; then
	BOX86=$BOX86_BIN
elif command -v box86 >/dev/null 2>&1; then
	BOX86=$(command -v box86)
else
	BOX86=/userdata/system/box86/bin/box86
fi

if [[ ! -x "$BOX86" ]]; then
	echo "Box86 missing: $BOX86"
	echo "Build and install it with build-box86-armv7.sh and install-box86-userdata.sh."
	exit 127
fi
if [[ ! -x "$GAME_ROOT/hl2_linux" ]]; then
	echo "Portal executable missing: $GAME_ROOT/hl2_linux"
	echo "Set PORTAL_GAME_ROOT to the extracted Portal_Linux/game directory."
	exit 2
fi
if [[ ! -r "$GL4ES_LIB" ]]; then
	echo "GL4ES libGL not found: $GL4ES_LIB"
	echo "Batocera v38 includes /usr/lib/gl4es/libGL.so.1; set GL4ES_LIB if your image differs."
	exit 3
fi

GL4ES_DIR=$(dirname -- "$GL4ES_LIB")
STEAM_RUNTIME=$(cd -- "$GAME_ROOT/../steam-runtime" 2>/dev/null && pwd || true)
X86_PATH="$GAME_ROOT/bin:$GAME_ROOT/portal/bin"
for candidate in \
	"$STEAM_RUNTIME/i386/usr/lib/i386-linux-gnu" \
	"$STEAM_RUNTIME/i386/lib/i386-linux-gnu"; do
	[[ -d "$candidate" ]] && X86_PATH="$X86_PATH:$candidate"
done

export BOX86_DYNAREC=1
export BOX86_LOG=${BOX86_LOG:-1}
export BOX86_LIBGL="$GL4ES_LIB"
export BOX86_LD_LIBRARY_PATH="$X86_PATH${BOX86_LD_LIBRARY_PATH:+:$BOX86_LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH="$GL4ES_DIR:/usr/lib:/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBGL_ES=2
export LIBGL_GL=${LIBGL_GL:-21}
export LIBGL_EGL=${LIBGL_EGL:-/usr/lib/libEGL.so.1}
export LIBGL_GLES=${LIBGL_GLES:-/usr/lib/libGLESv2.so.2}
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-kmsdrm}

ROTATION_HOOK=${PORTAL_ROTATION_HOOK:-/userdata/system/box86/portal-rotation-hook.sh}
if [[ -x "$ROTATION_HOOK" ]]; then
	"$ROTATION_HOOK" start || echo "Portal rotation hook failed; continuing." >&2
	trap '"$ROTATION_HOOK" stop || true' EXIT
fi

mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/portal-box86-$(date +%Y%m%d-%H%M%S).log"
cd "$GAME_ROOT" || exit 2
echo "Logging to $LOG"
echo "Game: $GAME_ROOT/hl2_linux"
echo "Box86: $BOX86"
echo "GL4ES: $GL4ES_LIB"
echo "SDL video driver: $SDL_VIDEODRIVER"
echo "CPU: $MACHINE, ${BITS}-bit ARM (NEON + VFPv3 confirmed)"
echo "Args: -fullscreen -width 800 -height 600 -dxlevel 80 (retry 81 on shader error)"

STATUS=0
ATTEMPT=0
DXLEVEL=80
while :; do
	ATTEMPT=$((ATTEMPT + 1))
	echo "Launch attempt $ATTEMPT (-dxlevel $DXLEVEL)"
	set +e
	{
		"$BOX86" ./hl2_linux -fullscreen -width 800 -height 600 "-dxlevel" "$DXLEVEL"
	} 2>&1 | tee -a "$LOG"
	STATUS=${PIPESTATUS[0]}
	set -e
	if [[ "$STATUS" -eq 42 && "$ATTEMPT" -lt 3 ]]; then
		continue
	fi
	if [[ "$DXLEVEL" == 80 ]] && [[ "$STATUS" -ne 0 ]] &&
	   grep -Eiq 'shader|GLSL|GL[[:space:]_-]*error|failed to compile' "$LOG"; then
		echo "Shader/GL error detected; retrying once with -dxlevel 81."
		DXLEVEL=81
		ATTEMPT=0
		continue
	fi
	break
done

echo "Portal/Box86 exit status: $STATUS"
case "$STATUS" in
	139) echo "Process ended with SIGSEGV (segmentation fault)." ;;
	132) echo "Process ended with SIGILL (illegal instruction)." ;;
	0) echo "Process exited normally." ;;
	42) echo "Source requested another launch; stopped after 3 attempts." ;;
	*) echo "See the log for the last successful initialization stage." ;;
esac
echo "Log saved: $LOG"
echo "Review Box86/GL4ES, shader, libtcmalloc and dynarec messages in this log."
exit "$STATUS"
