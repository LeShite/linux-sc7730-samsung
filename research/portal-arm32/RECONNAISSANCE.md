# Portal Linux ARM32 reconnaissance

Initial read-only analysis of `~/Descargas/Portal_Linux.tar.gz`, started 2026-10-26.
The archive and proprietary binaries remain outside this repository. `inspect_portal.py`
reads the tar stream and prints a package, ELF, BSP, and VPK inventory without extracting files.

## Findings

- The gzip tar stream reaches its end cleanly: 7,088 members, 4,480,128,477 uncompressed
  member bytes. It contains 26 `.bsp`, 39 `.vpk`, and 314 `.so` files (including Steam Runtime).
- The Linux game executable `game/hl2_linux`, engine module `game/bin/engine.so`, and
  Portal modules `game/portal/bin/{client,server}.so` are all ELF32, little-endian,
  Intel 80386 (i386). The package also contains x86_64 Steam Runtime components; those
  do not make the game binaries ARM-compatible.
- `engine.so` dynamically depends on `libtier0.so`, `libvstdlib.so`, `libsteam_api.so`,
  SDL2, and 32-bit glibc/system libraries.
- `client.so` strings expose interface identifiers such as `VEngineClient014` and
  Portal class/type names such as `CPortalGameMovement`, `CPortalSimulatorEventCallbacks`,
  and `CWeaponPortalBase`. These are clues for analysis, not source code. The module's
  exported game entry point is `CreateInterface`; it has no useful debug information.
- `gameinfo.txt` mounts the Portal VPKs and shared HL2 resources through Source's virtual
  filesystem. The Portal VPK directory index has 3,509 entries: 859 VTF textures, 748 VMT
  materials, 597 VTX model meshes, 566 WAVs, 200 MDLs, 199 VVDs, and smaller groups.
- The sample `testchmb_a_00.bsp` is `VBSP` version 20 with 64 lump descriptors. Its
  entity lump has 323 entities, including eight `prop_portal`, four
  `func_portal_detector`, two `trigger_portal_cleanser`, and two `func_noportal_volume`
  entities. This exposes the map-side contracts the game code has to implement.
- `export_bsp_obj.py` successfully converted that map's face/edge/surfedge/vertex lumps
  into an untextured OBJ: 5,307 vertices, 2,694 polygons, no skipped faces, 298 KB.

## What this means for a native ARM build

The tablet's ARMv7 Linux and Lima/GLES2 stack are a useful target platform, but the
download is an i386 executable package. Neither changing ELF headers nor recompiling
the public Source SDK game code can turn these proprietary x86 engine and game modules
into ARM code. A native project needs an ARM-capable engine with compatible content
loading, rendering, physics, audio, and Portal gameplay. The proprietary binaries can
serve as a local behavioral reference; don't copy them into the project or treat symbol
names as a replacement for source.

## Suggested next reverse-engineering slice

1. Add texture/material lookup to the BSP mesh export using VMT/VTF resources in VPK.
2. Decode BSP collision/visibility data and decide what the first GLES2 renderer needs.
3. Use the client/server strings and interface boundaries to map Portal-specific behavior,
   starting with portal placement, portal traversal, and player movement.

The first two are data-format work and can produce a clean, testable ARM32 foundation.
The third informs a clean compatible implementation; matching the complete Source engine
remains the large part of the effort.

## References

- [Valve's Source SDK 2013 `bspfile.h`](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/public/bspfile.h)
  documents the BSP lump indices and structures used as a format reference.
- [Valve's Source SDK 2013 repository](https://github.com/ValveSoftware/source-sdk-2013)
  describes its public game code scope; it is not the downloaded Portal engine binary's source.
