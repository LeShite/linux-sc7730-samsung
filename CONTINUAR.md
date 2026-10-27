# CONTINUAR: contexto completo para retomar el port (SM-T280)

Estado al **27 oct 2026**. Este archivo es el punto de entrada para una sesión nueva de Claude en otro PC.
Habla español con el usuario. Notas maestras detalladas: `ESTADO.md` (1187 líneas), `LEEME-CLAUDE.md` y `LEEME-CODEX.md` (Portal).

## Cómo arrancar en otro PC

```bash
git clone git@github.com:LeShite/linux-sc7730-samsung.git batocera-t280   # rama sm-t280-project (proyecto)
cd batocera-t280 && git checkout sm-t280-project
git clone git@github.com:LeShite/linux-sc7730-samsung.git src/linux-6.18.58  # rama master (kernel, tags v67..v70)
claude    # y dile: "lee CONTINUAR.md y ESTADO.md y continuemos"
```

**Lo que NO está en GitHub** (está en `.gitignore`, solo vive en el PC de casa): `android-fw/`, `backup/`, `twrp/`
(firmware y particiones de Samsung: IMEI/MAC/calibración), `batocera-v38/` (imagen base), `out/` (imágenes de boot),
`build-*.log`, `userdata/`, `snapshots/`, `tools/src/`, `tools/portal-arm32/{box86,gl4es}/` (clones de terceros).
Sin esas carpetas **no se puede compilar ni flashear desde otro PC**; solo editar código, parches y documentación.
Flashear exige la tablet en modo Download por USB al PC de casa.

## Qué se ha hecho

- Kernel mainline **6.18.58** propio para SC7730 (Cortex-A7 32 bits, Mali-400 con driver lima). Arranca **Batocera v38** (userspace armhf).
- Funcionan: eMMC, pantalla vertical 800x1280, táctil, botones, brillo, reposo, USB gadget (SSH), audio por bocina
  (perfil VBC del vendor, lock ADI único), batería/FGU, CPU hasta 1482 MHz (driver `sc8830-cpuclk`, freno térmico 80/68 C),
  menú Home con deslizador de velocidad.
- **Wi-Fi SC2331 (Marlin)** completo y automático: carga FDL1 por UART0, transporte SDIO, driver cfg80211 propio,
  script `/boot/marlin/marlin-up.sh`. Arranque completo ~41 s, DHCP, Internet OK.
- **N64** por RetroArch con `parallel_n64` compilado a mano (plugin rice, ~30 fps en Mario Kart 64, controles alt-map,
  overlay táctil con etiquetas N64).
- **Portal 1 (experimental)**: Box86 v0.3.9 cruzado y funcionando, GL4ES parcheado, entrada en Ports del menú;
  el motor Source arranca y llega a "Console initialized" sin crash.

## Fase actual

Port base terminado y usable (kernel + audio + táctil + Wi-Fi + N64). Ahora: **extras**. Portal está casi en el límite
del hardware; Bluetooth en pausa.

## Archivos y comandos clave

- Hardware/DTS: `src/linux-6.18.58/arch/arm/boot/dts/unisoc/sc7730-samsung-gtexswifi.dts` (+ `sc7730.dtsi`).
- Drivers propios: `drivers/misc/{sprd-sc2331-marlin.c, sprd-marlin-sdio.c, sprd-sc2331-wlan.c, sc8830-adi-lock.c}`,
  `drivers/thermal/sc8830-cpuclk.c`, `include/linux/sc8830-adi.h`. Parches exportados en `patches/` (0001..0046).
- Compilar: `./scripts/build-kernel.sh > build-NN.log`; imagen resultante `out/boot-gtexv.img`
  (defconfig exacto: mirar `scripts/build-kernel.sh`).
- Flashear: tablet en modo Download (Vol- + Encendido, lo hace el usuario) y `./scripts/autoflash.sh KERNEL out/boot-gtexv.img`.
- Particiones: `BATOCERA` en **p25** (`/boot`, se actualiza con `scripts/push-bootpart.sh` desde TWRP);
  userdata/SHARE en **p27** (ext4, ~400 MB libres).
- Acceso: `ssh root@10.42.7.2` (USB gadget); Wi-Fi por DHCP (`192.168.1.67`, antes `.74`). El PC de casa tiene IP fija
  `192.168.1.64` y sshd activo.
- Overlays táctiles: `scripts/make-overlay.py` (genera `gamepad.png`/`gamepad-n64.png` y cfg de RetroArch),
  `scripts/push-overlay.sh` o scp a `/userdata/system/touchpad/overlay/`. Touchpad v6: ver `ESTADO.md`
  (`touchpad.py` en `/boot/touchpad`; `boot-custom.sh` remonta `/boot` rw y escribe `/boot/touchpad.log`).
- Portal: `tools/portal-arm32/` (README, `run_portal.sh`, `Portal.sh`, `gamelist-ports.xml`, `gl4es-t280.patch`,
  `box86-t280.patch`, `patch-engine-sdlver.sh`). En la tablet: `/userdata/system/box86/`, `/userdata/system/gl4es/`,
  `/userdata/roms/ports/{Portal.sh,gamelist.xml,.data/Portal_Linux}`.
  Box86 renombra el proceso a `hl2_linux` (usar `pgrep -x hl2_linux`).
  `BOX86_NODYNAREC=0x61700000-0x619c7000` evita un SIGILL del dinarec en `filesystem_stdio.so`.

## Trampas conocidas

- `pkill -f` por ssh mata la propia sesión: usar `[x]` en el patrón (p. ej. `pkill -f '[h]l2_linux'`).
- `sleep` largo está bloqueado en el harness: usar `until` con `sleep` corto.
- `/tmp` de la tablet se pierde al reiniciar.
- Reiniciar EmulationStation por ssh deja el DRM ocupado (RetroArch falla con "Error when switching mode"):
  **reiniciar la tablet entera**.
- **NO subir la GPU a 512 MHz por `/dev/mem`**: colgó la tablet 5 veces.

## Problemas abiertos

- **La tablet no respondía** ni por USB (`enp0s20f0u2` desaparece) ni por Wi-Fi (sin ping a 192.168.1.67). Posibles causas:
  cable, cuelgue o batería (el 0% ya la apagó una vez; antes marcaba 68%). Hay que mirarla físicamente.
- **Portal**: pantalla oscura; 61 shaders de materiales (54 `lightmappedgeneric_ps20b`, 7 `vertexlit_and_unlit_generic_ps20b`)
  no compilan en GLES2/lima; rendimiento esperado de pocos fps. El panel es vertical y el juego dibuja a 800x600 (sin rotación).
- **Bluetooth**: desactivado por defecto (el chip no contesta por UART0 a ninguna velocidad; con el reposo activo el CP
  llegó a colgarse una vez).
- **N64**: falta que el usuario confirme velocidad y sonido jugando (el sonido se perdía por fps bajos);
  quitar/ajustar mupen64plus independiente.
- Menores: IP fija del Wi-Fi de la tablet, consumo sin cable, iperf largo, menú de red de Batocera.

## Siguientes pasos

1. Recuperar la tablet (revisar pantalla/cable; si está apagada, botón de encendido 10 s; cargarla) y comprobar
   `ssh root@10.42.7.2`, EmulationStation y Wi-Fi.
2. Portal: ejecutar con `LIBGL_LOGSHADERERROR=1`, leer el error de compilación y adaptar el convertidor de shaders de
   gl4es (`src/gl/shaderconv.c`) a los límites de la Mali-400; si no es viable, dejar Portal documentado y cerrar.
3. Pedir al usuario que confirme N64 (sonido/fps) y fijar la IP del Wi-Fi (connman manual en `/userdata`, no en `/var`, que es tmpfs).
4. Bluetooth: observar el arranque real de Android en TWRP para sacar el comando/pin de enable.

## Seguridad

- La **clave Wi-Fi del usuario vive SOLO en `/userdata/system/batocera.conf` de la tablet**. Nunca ponerla en repo,
  notas ni mensajes.
- No subir a GitHub nada nuevo sin que el usuario lo pida.
- Nunca versionar firmware/particiones de Samsung (IMEI/MAC/calibración): ya están en `.gitignore`.

## Repos

- Proyecto: `git@github.com:LeShite/linux-sc7730-samsung.git`, rama `sm-t280-project` (también `main` local → `portal/main`).
- Kernel: mismo remoto, rama `master`, tags `v67-wlan-power` … `v70-uart-raw`, directorio `src/linux-6.18.58`.
- Portal: `git@github.com:LeShite/portal-1-ARM.git`, rama `main`.
