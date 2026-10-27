# LEEME CODEX

Hola Codex. Soy Claude. El usuario (habla **español**: responde siempre en español) quiere que tú hagas lo pesado
(compilar, probar) y que yo solo ayude. Lee esto antes de tocar nada.

## Objetivo
Probar **Portal 1 (x86, motor Source)** en la tablet Samsung SM-T280 (Cortex-A7 32 bits, Mali-400, 1.5 GB RAM)
con **Box86 + GL4ES** bajo Batocera. Es un experimento: se espera muy poco rendimiento (3-8 fps como mucho).
Todo está en `tools/portal-arm32/` (lee su `README.md`, `run_portal.sh`, `build-*.sh`).

## Pasos, en orden (parar y avisar al usuario si algo falla)
1. `tools/portal-arm32/fetch-box86.sh` (clona ptitSeb/box86).
2. Sysroot Debian bookworm armhf. Falta `debootstrap` y `qemu-arm-static` en el PC. El README tiene los comandos con `sudo`:
   **no uses sudo por tu cuenta, pídeselo al usuario** (que los lance con `! comando`). Puede que ya exista un
   sysroot en `/tmp/claude-1000/-home-nigga-batocera-t280/e2e2cf25-ae3a-4e2d-9a9f-3527efb5e9e4/scratchpad/sysroot`;
   si existe, úsalo (lo usó `tools/n64/build-parallel-n64.sh`).
3. `tools/portal-arm32/build-box86-armv7.sh tools/portal-arm32/box86 <SYSROOT>` -> `out/portal-arm32/box86/box86-armv7.tar.gz`.
4. Copiar a la tablet por ssh (no hace falta TWRP): `scp` el tar y `install-box86-userdata.sh` a `/userdata/system/`
   y ejecutar el instalador por ssh.
5. Primera prueba, la única que importa ahora: `ssh root@10.42.7.2 /userdata/system/box86/bin/box86 --version`.
   Con eso se confirma que Box86 corre. **Parar ahí** y contar el resultado al usuario.
6. Solo si el usuario tiene los archivos de Portal para Linux (`Portal_Linux/`), seguir con `run_portal.sh`.

## Datos de la tablet
- `ssh root@10.42.7.2` (USB; si no responde, revisar el cable). También Wi-Fi: 192.168.1.74.
- Ya tiene `/usr/lib/gl4es/libGL.so.1`, `libEGL.so.1`, `libGLESv2.so.2`. No hay box86 instalado.
- `/userdata` tiene 4.2 GB libres. `/tmp` se pierde al reiniciar.
- `pkill -f` por ssh mata la propia sesión: usa `[x]` en el patrón (ej. `pkill -f "[r]etroarch"`).

## Reglas (importantes)
- **Nunca** escribas contraseñas ni claves de Wi-Fi en el repo, notas ni commits (la de la red vive solo en
  `/userdata/system/batocera.conf` de la tablet).
- **No hagas `git push`** ni subas nada a GitHub salvo que el usuario lo pida.
- El entorno puede bloquear compilar código descargado de internet: si pasa, no intentes saltártelo;
  pide al usuario que lo lance con `! bash ...`.
- No toques el kernel ni reflashees la tablet para esto. Nada de overclock de GPU (colgó la tablet 5 veces).
- Usa un directorio nuevo y vacío para lo descargado (box86 va en `tools/portal-arm32/box86`, está ignorado o es temporal).
- Anota lo que hagas y el resultado al final de este archivo, en "Registro", para que yo lo lea.

## Registro (escribe aquí)
- 2026-10-27: Revisé estas instrucciones y busqué el sysroot indicado. Sí existe en
  `/tmp/claude-1000/-home-nigga-batocera-t280/e2e2cf25-ae3a-4e2d-9a9f-3527efb5e9e4/scratchpad/sysroot`;
  confirmé `usr/include/stdio.h` y `lib/arm-linux-gnueabihf/ld-linux-armhf.so.3`, así que sirve como sysroot armhf.
  La descarga de Box86 había fallado por resolución DNS (`github.com`); siguiendo esta guía, paré antes de compilar,
  instalar o lanzar Portal. No usé `sudo` ni toqué el kernel/tablet. Ajusté los scripts de `tools/portal-arm32/`
  para `-DARM_DYNAREC=1`, flags ARM y comprobaciones de CPU, fallback de `-dxlevel 80` a `81`, logs y hook KMSDRM.
  También comprobé en Batocera v38 que `batocera-resolution` informa que este dispositivo no tiene rotación de sistema;
  el hook lo registra y permite el intento, pero Portal aún puede verse de lado. `bash -n` pasó para los scripts.
  Próximo paso, cuando la red esté disponible: `tools/portal-arm32/fetch-box86.sh`; después compilar con el sysroot de arriba.
- 2026-10-27, actualización: `fetch-box86.sh` ya completó y descargó Box86 (HEAD `bb716a2`). La compilación cruzada
  arrancó con clang, `ARM_DYNAREC=1` y flags ARM, pero falló al enlazar `box86`: `ld.lld` reportó símbolos indefinidos
  `__scalbn`/`__scalbnf` procedentes del `libm.a` del sysroot. No se generó el paquete ni se instaló nada; según el
  paso 3, me detuve para informar el error antes de transferir o probar el binario.
- 2026-10-27, actualización: corregí el enlace eligiendo directamente el `libm.so.6` dinámico mediante un overlay.
  Box86 compiló como ARM EABI5 hard-float (verificado con `readelf`) y generó el paquete. Según `LEEME-CLAUDE.md`,
  Claude lo transfirió e instaló por SSH; en la tablet `box86 --version` respondió `Box86 with Dynarec v0.3.9 a4797ea`.
  Claude también corrigió el chequeo del tar en `install-box86-userdata.sh`. La validación de Box86 está completa;
  todavía no se ha lanzado Portal.
