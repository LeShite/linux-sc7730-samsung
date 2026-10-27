# Portal on the SM-T280: Box86 + GL4ES first-launch path

## What is already in Batocera v38

The local Batocera v38 image inventory includes ARM libraries at:

- `/usr/lib/gl4es/libGL.so.1`
- `/usr/lib/libEGL.so.1`
- `/usr/lib/libGLESv2.so.2`
- `/usr/lib/libSDL2-2.0.so.0`

This image is a 32-bit ARMHF userspace: its rootfs contains
`/lib/ld-linux-armhf.so.3` and ARMHF Python extensions. The tablet's documented SoC is
four Cortex-A7 cores with NEON and VFPv4 (which includes the VFPv3 feature level Box86
dynarec requires). The runner checks `uname`, `getconf LONG_BIT`, and `/proc/cpuinfo`
before starting Box86, so a 64-bit-only or feature-incompatible rootfs fails with a
useful message. Box86 requires a 32-bit ARM host/userspace, or an armhf chroot on a
64-bit ARM kernel; its official build guide documents both the 32-bit subsystem and
dynarec requirements.

The same inventory has no Box86 executable. So reuse Batocera's GL4ES first; the
GL4ES cross-build script is an optional fallback for a different image or a library
change. This avoids installing a second GL stack before confirming the existing one.

## Cross-build prerequisites

- A Box86 source checkout: <https://github.com/ptitSeb/box86>
- Optionally a GL4ES source checkout: <https://github.com/ptitSeb/gl4es>
- The same Debian bookworm ARMHF sysroot used by `tools/n64/build-parallel-n64.sh`.
  Its glibc 2.36 is older than the tablet's 2.37, so the resulting ARM binaries should
  remain compatible with the target. The scripts need host `clang`, `clang++`, `lld`,
  and CMake.

Fetch the official Box86 source into `tools/portal-arm32/box86`:

```sh
tools/portal-arm32/fetch-box86.sh
```

The host is Arch Linux and currently has no ARMHF sysroot. Create a Debian Bookworm
ARMHF sysroot (glibc 2.36, compatible with the tablet's 2.37) with:

```sh
sudo pacman -S --needed debootstrap qemu-user-static qemu-user-static-binfmt
sudo systemctl enable --now systemd-binfmt
test -e /proc/sys/fs/binfmt_misc/qemu-arm

SYSROOT="$HOME/batocera-t280/build/sysroot-bookworm-armhf"
sudo debootstrap --arch=armhf --foreign --variant=minbase bookworm \
  "$SYSROOT" https://deb.debian.org/debian
sudo install -D -m 0755 /usr/bin/qemu-arm-static "$SYSROOT/usr/bin/qemu-arm-static"
sudo chroot "$SYSROOT" /debootstrap/debootstrap --second-stage
sudo chroot "$SYSROOT" apt-get update
sudo chroot "$SYSROOT" apt-get install -y libc6-dev linux-libc-dev g++
```

Build Box86 and make a small install archive:

```sh
tools/portal-arm32/build-box86-armv7.sh \
  tools/portal-arm32/box86 "$HOME/batocera-t280/build/sysroot-bookworm-armhf"
```

Output: `~/batocera-t280/out/portal-arm32/box86/box86-armv7.tar.gz`. The build also stages
the archive and target-side installer in `~/batocera-t280/userdata/system/` for the existing
`scripts/push-userdata.sh` workflow.
The build passes `-DARM_DYNAREC=1` and targets Cortex-A7/NEON/VFPv4 hard-float. The
generated toolchain file explicitly includes `-marm` in C, C++ and assembly flags;
Box86's ARM dynarec does not support a Thumb build. It packages Box86, the runner and
the Portal rotation hook together.

Batocera is an immutable image with writable user data, so the installer extracts the
binary and runner to `/userdata/system/box86`; it does not use `apt` or write `/usr`.
With the tablet in TWRP, run the existing host-side transfer script:

```sh
scripts/push-userdata.sh
```

After it reboots into Batocera, run over SSH:

```sh
/userdata/system/install-box86-userdata.sh /userdata/system/box86-armv7.tar.gz
```

If your Batocera build lacks GL4ES or its bundled library does not work, cross-build a
KMSDRM-compatible shared library:

```sh
tools/portal-arm32/build-gl4es-armv7.sh /path/to/gl4es /path/to/armhf-sysroot
```

The output is `~/batocera-t280/out/portal-arm32/gl4es/gl4es-armv7.tar.gz`; put its
`gl4es/libGL.so.1` at `/userdata/system/gl4es/libGL.so.1` or set `GL4ES_LIB`.
The build keeps EGL enabled, uses the GLES2 backend, and disables X11/GBM because SDL
creates the KMSDRM/EGL context on this Batocera target.

## Launch and logs

Place the extracted `Portal_Linux` folder in `/userdata/roms/ports/.data/`. The executable
will be `/userdata/roms/ports/.data/Portal_Linux/game/hl2_linux`, with `steam-runtime` as
its sibling. The launcher uses that path by default; override it with `PORTAL_GAME_ROOT`.
It configures Box86's app-library path separately from the ARM `LD_LIBRARY_PATH`, then
points Box86's GL wrapper at native GL4ES:

- `BOX86_LD_LIBRARY_PATH`: Portal's i386 game modules and i386 Steam Runtime libraries.
- `LD_LIBRARY_PATH`: ARM GL4ES/EGL/GLES search paths.
- `BOX86_LIBGL=/usr/lib/gl4es/libGL.so.1`: tells Box86 which native `libGL` to wrap.
- `BOX86_DYNAREC=1`, `LIBGL_ES=2`, `LIBGL_GL=21`.

Launch with:

```sh
PORTAL_GAME_ROOT=/userdata/roms/ports/.data/Portal_Linux/game \
	tools/portal-arm32/run_portal.sh
```

The script attempts `-fullscreen -width 800 -height 600 -dxlevel 80`. If the log shows
a GL/shader compilation error, it retries once with `-dxlevel 81`. It records stdout,
stderr, detected ARM features and the child status at
`/userdata/system/logs/portal-box86-*.log`. Check it with:

```sh
grep -Ei 'GL4ES|shader|GLSL|tcmalloc|dynarec|error|SIGSEGV|SIGILL' \
  /userdata/system/logs/portal-box86-*.log
```

The rotation hook runs immediately before Box86 and restores the prior system rotation
on exit when Batocera reports hardware/system rotation support. I inspected this
tablet's Batocera v38 `batocera-resolution`: `supportSystemRotation` and `setRotation`
both return failure (`no possible rotation`). Its existing `display.rotate=1` is
software rotation used by EmulationStation/RetroArch, so that setting cannot rotate
Portal's direct SDL KMSDRM output. The hook logs this and still allows the experiment;
Portal may therefore appear sideways. This image cannot promise a correctly oriented
native KMSDRM frame without adding a real software-rotation path or changing the video
stack. No boot service or `custom.sh` change is needed for a Ports launcher.

## EmulationStation port entry

The ready-made [Portal.sh](Portal.sh) is the Ports menu entry.
Batocera accepts executable `.sh` files in `/userdata/roms/ports`; launching one suspends
the frontend for the game, and exiting the script returns to the Ports menu. Do not stop
or restart EmulationStation yourself when launched from the menu.

1. Copy the game data to `/userdata/roms/ports/.data/Portal_Linux/`.
2. Install Box86 with the command above; it also installs the runner script.
3. Copy `tools/portal-arm32/Portal.sh` to `userdata/roms/ports/Portal.sh` on the host.
   The build folder already contains this staged copy. `scripts/push-userdata.sh` transfers
   it; if needed, set the executable bit on Batocera with
   `chmod +x /userdata/roms/ports/Portal.sh`.
4. Refresh the game list in EmulationStation; `Portal` appears under Ports.

GL4ES maps desktop OpenGL 2.1 calls to GLES2, but its shader conversion has limitations.
`-dxlevel 80` and the fallback `-dxlevel 81` are experiments; neither guarantees that
Portal's renderer will initialize. If the process fails, inspect the log for GL4ES and
shader conversion messages, missing `libtcmalloc`, and Box86 dynarec/illegal-instruction
errors. A segfault or absent logo remains a useful first result, but not proof that
rotation or the full game works.
