# Batocera en Samsung Galaxy Tab A 7.0 (2016), SM-T280 "gtexswifi"
## Bitácora completa del porte · 24 de octubre de 2026

> **Resultado:** Batocera arranca en la tablet y muestra **EmulationStation con aceleración 3D**
> (GPU Mali-400 con el driver libre Lima). Corre sobre un **Linux 6.18.58 portado desde cero** al chip
> Spreadtrum SC7730, que nunca tuvo soporte en Linux moderno. Para jugar todavía faltan los controles,
> los 4 núcleos y el audio (ver la sección 9).

Todo vive en `~/batocera-t280/`. Los cambios al kernel están en git, en `src/linux-6.18.58`
(tag `v11-emulationstation`), y exportados en `patches/`. La versión anterior de esta bitácora quedó en
`ESTADO-anterior.md`.

---

## 1. Hardware (verificado en la tablet)

| | |
|---|---|
| SoC | Spreadtrum SC7730SW (familia sc8830 "shark"), 4x Cortex-A7 32 bits, NEON/VFPv4, 1.3 GHz |
| GPU | Mali-400 MP2 en 0x60000000, todas sus interrupciones en SPI 39 |
| RAM | 1.5 GB, 0x80000000-0xDFFFFFFF |
| eMMC | 7.28 GB, chip "300071", 27 particiones GPT |
| Pantalla | panel S6D7AA0X11 MIPI-DSI 800x1280; el bootloader deja el framebuffer en 0x9EA48000 (a8r8g8b8, stride 3200) |
| Táctil | Melfas MIP4, i2c1 (0x70600000) dirección 0x48, IRQ GPIO 238 |
| Otros | MUIC SM5504, cargador SM5701, PMIC SC2723, WiFi/BT Marlin (sin driver libre) |
| Bootloader | SBOOT T280XXS0ARK3, desbloqueo OEM activo, hw_revision=5, secureboot activo (con hash) |
| Consola UART | ttyS1 = 0x70100000 (sin acceso físico) |

**Particiones relevantes:** p1/p2 SBOOT/SBOOT2 (bootloader, NO TOCAR) · p17 efs · p20 KERNEL (16 MB, aquí va
nuestro kernel) · p21 RECOVERY (TWRP 3.1.0) · p24 CACHE · p25 SYSTEM → **BATOCERA** · p26 HIDDEN (cabecera
borrada) · p27 userdata → **SHARE**.

**Ojo:** el micro-USB de la tablet es inestable (errores -71, "EMI"); la conexión dura ~1 minuto. Todo lo que
transfiere datos está hecho para resistir cortes (`autoflash.sh`, `push-partition.sh`, `backup.sh`).

---

## 2. Archivos del proyecto

```
~/batocera-t280/
  ESTADO.md                 esta bitácora
  backup/                   24 particiones originales + PIT, verificadas (SHA256SUMS)
  info/                     cmdline del bootloader, dmesg, iomem y framebuffer de Android/TWRP
  twrp/                     TWRP 3.1.0 (_mone) original y desempacado
  src/linux-6.18.58/        kernel (git, 8 commits sobre el tarball de kernel.org)
  src/kernel-stock-3.10/    kernel de Samsung (referencia de registros)
  patches/                  0001..0008 *.patch (git format-patch)
  initramfs/                init.c (PID 1 propio, sin libc), memdump.c, initramfs.list
  scripts/
    build-kernel.sh         compila el kernel y empaqueta out/boot-gtex.img
    mkgteximg.py            empaquetador DHTB/Android/dt.img (byte a byte igual a TWRP)
    autoflash.sh            espera una conexión USB estable y flashea con heimdall
    push-partition.sh       escribe una imagen en una partición por TWRP, en bloques verificados
    backup.sh               respaldo reanudable de particiones por adb
    read-ramoops.py         (no sirve con TWRP: su kernel borra esa RAM)
  batocera-v38/             Batocera H3 v38 (boot.tar.xz verificado) e imagen batocera-p25.ext4
  build/                    salida de compilación (kernel, initrd de Batocera, init)
  out/                      boot-gtex-v2..v11.img (cada versión flasheada)
```

---

## 3. Cronología: qué pasó en cada versión

| Ver. | Cambio | Resultado / lección |
|---|---|---|
| — | Detección por adb/heimdall, respaldo de 24 particiones, TWRP en RECOVERY | El hardware es SC7730, no Exynos (eso era el SM-T580) |
| — | Ingeniería inversa del formato de arranque | `mkgteximg.py` reproduce TWRP **byte a byte** |
| v1 | Kernel mínimo + timer propio + simple-framebuffer | **PRIMERA LUZ**: log de Linux en pantalla |
| v1 | — | El clockevent con cpumask de todos los CPUs perdía contra `dummy_timer` (sin oneshot ni hrtimers) |
| v2 | `cpumask_of(0)`, DRM_PANIC, /init propio | El texto se "congelaba": con simpledrm la consola usa una copia en RAM que no se vuelca tras un panic |
| v3 | COMPAT_32BIT_TIME | El init no podía dormir: allnoconfig quitó las syscalls de tiempo de 32 bits |
| v4 | Timer: esperar BUSY antes de deshabilitar | Seguía congelándose en 0.15 s (15 interrupciones) |
| v5 | **Driver INTC** | **Causa real:** con todos los CPUs en WFI se apaga el reloj del GIC y solo el INTC despierta al CPU. Timers OK y NO_HZ funcionando |
| v6 | **Driver eMMC (SDHCI)** | Funcionó a la primera: mmcblk0p1..p27 |
| — | Instalación de Batocera en el eMMC (sección 6) | 44 bloques verificados, sin cortes |
| v7 | initrd de Batocera + configuración (ext4, loop, squashfs, overlay…) | **Batocera arranca hasta la consola "READY TO RETRO"** (tardaba ~5 min) |
| v8 | rng-seed en el DT | La espera de 5 min era getrandom() bloqueado: con un reloj de 32 kHz la entropía por jitter no funciona |
| v9 | Dominio de energía GPU + Lima + simpledrm renombrado a "sun4i-drm" | Se colgaba en el logo de Samsung: leía la GPU APB estando apagada |
| v10 | Encendido tardío y paso a paso, con mensajes en pantalla | Llegaba a "GPU encendida" y se colgaba al leer los registros Mali |
| v11 | **Compuerta de la fuente de 384 MHz (PMU 0x402b011c bit 5)** | **EMULATIONSTATION CON GPU** 🎉 |

---

## 4. Cambios al kernel (`patches/`)

**A) Plataforma** — `arch/arm/Kconfig.platforms`: `ARCH_SPRD` (ARMv7) select ARM_GIC, SPRD_SC8830_INTC,
SPRD_SC8830_TIMER.

**B) Timer** — `drivers/clocksource/timer-sprd-sc8830.c` ("sprd,sc8830-timer", 32768 Hz):
- bloque de 3 timers separados 0x20: LOAD+0, CTL+8 (b6 PERIOD, b7 ENABLE, b8 NEW), INT+0xC (b0 EN, b3 CLR,
  b4 BUSY), CNT_RD+0x10
- timer0 = clockevent (SPI 28); timer1 = clocksource + sched_clock (cuenta hacia abajo, se lee invertido)
- `cpumask_of(0)`; esperar BUSY (con límite) **antes** de deshabilitar; set_oneshot carga el periodo y habilita
- relojes: AON APB EB0 0x402E0000 bits 10-12

**C) INTC** — `drivers/irqchip/irq-sprd-sc8830-intc.c` ("sprd,sc8830-intc"), jerárquico sobre el GIC
(modelo: irq-tegra.c):
- INTC0-3 en 0x71400000/0x71500000/0x71600000/0x71700000; EN=+0x08, DIS=+0x0C; INTC = SPI/32, bit = SPI%32
- relojes en 0x71300000 bits 19-22. Sin esto el CPU nunca vuelve de WFI.

**D) eMMC** — `drivers/mmc/host/sdhci-sprd-sc8830.c` ("sprd,sc8830-sdhci", sobre sdhci-pltfm):
- caps forzadas: VDD_300|SDMA|HISPD|8BIT, versión 3.00 (el registro real dice 07e50080/00000007, ver 0002)
- SDCLK = 384 MHz / (2*(div+1)); reset siempre con el bit3 (RST_EMMC); retardos 0x80/0x84/0x88 tras RESET_ALL
- set_power no hace nada; solo SDMA (quirks BROKEN_ADMA, BROKEN_TIMEOUT_VAL, NO_HISPD_BIT, CAP_CLOCK_BASE_BROKEN;
  quirks2 NO_1_8_V, BROKEN_HS200, BROKEN_DDR50). El bootloader lo deja en 8 bits a 48 MHz.

**E) GPU** — `drivers/pmdomain/sprd/sc8830-gpu-pd.c` (genpd always-on, late_initcall, anuncia cada paso):
1. PMU 0x402b0020 (PD_GPU_TOP_CFG): limpiar b25 FORCE_SHUTDOWN y b24 AUTO_SHUTDOWN_EN
2. AON 0x402e0000 (APB_EB0): GPU_EB b27
3. **PMU 0x402b011c: compuerta de la fuente** (b0 300M, b1 312M, b2 208M, b3 256M, b4 460.8M, **b5 384M**,
   b6 153.6M, b7 512M). Sin ella los registros Mali cuelgan el bus.
4. GPU APB 0x60100004 (CLK_CTRL): sel=5 (384 MHz), div=0
- **Nunca** leer la GPU APB (0x60100000) antes de los pasos 1 y 2: cuelga el bus.

**F) simpledrm** — `CONFIG_DRM_SIMPLEDRM_NAME` (nuevo): nombre que ve userspace. Lo fijamos en
**"sun4i-drm"** para que el Mesa de Batocera (solo trae lima + kmsro, sin swrast) use kmsro: Lima dibuja y
simpledrm muestra (copia por CPU en cada cuadro).

**G) Config** — `arch/arm/configs/gtexswifi.config` (sobre allnoconfig). Puntos clave:
MMU=y (¡allnoconfig lo apaga!), MISC_FILESYSTEMS (para pstore), COMPAT_32BIT_TIME, HIGHMEM,
ARM_APPENDED_DTB + ATAG_DTB_COMPAT, CMDLINE_FORCE="label=BATOCERA console=tty0 loglevel=7 panic=0
fbcon=nodefer", DRM_SIMPLEDRM + DRM_PANIC(kmsg), PSTORE_RAM, MMC, EXT4, VFAT, EXFAT, SQUASHFS (XZ/ZSTD/LZ4/LZO),
OVERLAY_FS, TMPFS_XATTR, BLK_DEV_LOOP, FUSE, INOTIFY_USER, NET/UNIX/INET/PACKET, INPUT_EVDEV/JOYDEV/UINPUT,
PM, SPRD_SC8830_GPU_PD, DRM_LIMA, INITRAMFS_SOURCE (initrd de Batocera + nuestro init, uid 1000 → root).

---

## 5. Device tree (`arch/arm/boot/dts/unisoc/`)

`sc7730.dtsi`: raíz interrupt-parent=&intc · cpus f00..f03 · clk32k (32768) y gpu_clk (384 MHz) fijos ·
gic@12001000 (arm,cortex-a7-gic, interrupt-parent=&gic) · intc@71400000 · timer@40050000 (SPI 28) ·
mmc@20300000 (SD, SPI 57, disabled) · mmc@20600000 (eMMC, SPI 60, delays a/2/2) ·
power-controller@402b0020 (gpu_pd: regs pmu/aon/gate/gpu) · gpu@60000000 (mali-400, 7x SPI 39, power-domains).

`sc7730-samsung-gtexswifi.dts`: `sprd,sc-id = <8830 5 0x20000>` (SBOOT elige el DTB por esto) ·
chosen/framebuffer@9ea48000 simple-framebuffer · memory <0x80000000 0x60000000> · reserved-memory no-map:
0x86a00000 2M (sec_log), 0x87800000 4M (smem), 0x88000000 0x1d00000 (módem), 0x9ea48000 0xbb8000 (fb),
ramoops 0x9f600000 1M · aliases mmc0/mmc1 · &emmc 8 bits, high speed, 52 MHz.

`build-kernel.sh` además inyecta en cada compilación `/chosen/rng-seed` (32 bytes aleatorios, con fdtput).

---

## 6. Instalación de Batocera (Android BORRADO por decisión del usuario)

- Batocera **H3 v38** (Orange Pi PC, el último H3 publicado), `boot.tar.xz` del mirror oficial, sha256
  7ad3643a34bf61130aacb7e1cb1891bbb23927452378916fc3b7c592738720ed (verificado).
  H3 = Cortex-A7 + Mali-400: el userspace es binariamente compatible.
- **p25 SYSTEM → ext4 `BATOCERA`** (imagen `batocera-v38/batocera-p25.ext4`, 1400 MiB, features compatibles
  con el kernel 3.10 de TWRP): `/batocera-boot.conf`, `/boot/batocera` (squashfs zstd 1.2 GB), `/boot/batocera.board`.
  Escrita con `push-partition.sh` (44 bloques de 32 MB, SHA-256 verificado, reanudable).
- **p26 HIDDEN →** primeros 2 MiB a cero.
- **p27 userdata → ext4 `SHARE`**, UUID 27eceb4e-56ef-42b6-acc2-576df969b1a1 (mke2fs de TWRP).
- `batocera-boot.conf`: `sharedevice=DEVICES`, `sharedevice_part1=SHARE@<uuid>`, **`autoresize=false`**.

⚠️ **NUNCA activar `autoresize`:** Batocera ejecutaría `parted resizepart 2 100%`, y aquí **p2 es SBOOT2**
(bootloader). Además `batocera-part` calcula la partición de datos como "arranque + 1" usando un solo dígito
(p25 → p26), por eso se monta SHARE por UUID y se borró la cabecera de p26 (doble protección).

Flujo de arranque: SBOOT → nuestro kernel → `/init` (diagnóstico: eMMC, GPU, caja negra) → desmonta → exec
`/init.batocera` (busybox estático de Batocera) → monta LABEL=BATOCERA, loop+squashfs, overlay tmpfs →
switch_root → sysvinit de Batocera → servicios → EmulationStation (SDL2 KMSDRM + Mesa kmsro → Lima).

---

## 7. Procedimientos

```bash
# Compilar y empaquetar (deja out/boot-gtex.img)
~/batocera-t280/scripts/build-kernel.sh

# Flashear (la tablet en modo Download; espera a que el USB esté estable 3 s)
~/batocera-t280/scripts/autoflash.sh KERNEL ~/batocera-t280/out/boot-gtex-v11.img

# Escribir una imagen en una partición desde TWRP (reanudable)
~/batocera-t280/scripts/push-partition.sh IMAGEN NOMBRE_PARTICION
```

- **Modo Download:** Vol- + Encendido hasta que se apague → Vol- + Home + Encendido → Vol+.
- **TWRP:** Vol- + Encendido hasta que se apague → Vol+ + Home + Encendido.
- Desde TWRP: `adb reboot download`.
- Restaurar el kernel de Android: `heimdall flash --KERNEL ~/batocera-t280/backup/KERNEL.img`. Para volver a
  Android completo hace falta el firmware oficial de Samsung (SYSTEM y userdata fueron reemplazados).
- Fotos de la pantalla: en `~/Descargas`; convertir con `magick -auto-orient -resize 1500x1500`.

---

## 8. Detalles técnicos útiles

- Formato de arranque: cabecera DHTB de 512 B ("DHTB", u32 1, sha256 del payload, tamaño en +48) + Android
  boot v0 (página 2048, kernel en 0x8000, ramdisk en 0x1000000, tags en 0x100, dt_size en la palabra 9) +
  dt.img "SPRD" (versión 1; entradas 8830/rev/0x20000 para revs 0,2,3,4,5) + "SEANDROIDENFORCE" + ffffffff.
  El SBOOT usa libfdt, necesita un nodo /chosen y escribe ahí los bootargs (los ignoramos: DTB adjunto + CMDLINE_FORCE).
- El kernel de TWRP borra la RAM ION (0x9f600000) al arrancar, así que la caja negra solo la lee el siguiente
  arranque de NUESTRO kernel (el init muestra `/sys/fs/pstore`).
- El busybox `dd` de TWRP tiene off_t de 32 bits: falla por encima de 2 GiB (toda la RAM está arriba de 0x80000000).
  Por eso existe `initramfs/memdump.c`.
- allnoconfig quita en silencio todo lo que no tiene sus dependencias: `build-kernel.sh` verifica la lista de
  opciones obligatorias.

---

## 9. Pendiente (en orden de prioridad)

1. **Controles** — hoy no hay ningún `/dev/input`. Opciones:
   - **USB OTG en modo host** (dwc2 en 0x20200000) para mando/teclado/pendrive (recomendado). La alimentación
     del mando necesita el elevador del SM5701 o un hub con fuente propia.
   - Botones físicos (Vol- en EIC, Vol+/Home en el keypad, Power en el PMIC).
   - Táctil Melfas MIP4 (necesita I2C y GPIO de Spreadtrum y el regulador del PMIC).
2. **SMP**: arrancar los 4 Cortex-A7 (temporizadores por CPU: APTIMER 0x40220000/0x40330000/0x40340000, IRQs 29/119/121).
3. **Pantalla horizontal** (rotación en Batocera, `display.rotate`).
4. **Audio** (codec del SC2723 + VBC: sin driver libre; alternativa: audio USB).
5. **Pasar juegos**: por TWRP (montar SHARE) o por USB cuando funcione.
6. Mejoras: un clocksource más rápido (para entropía y timing), driver de pantalla nativo (DISPC/DSI), WiFi.

---

## 10. Avances después de EmulationStation (24 de octubre de 2026)

| Ver. | Cambio | Resultado |
|---|---|---|
| v12 | SMP: `arch/arm/mach-sprd/platsmp.c` (pen AP AHB 0x20D00014, salto 0x20D00018+4n, reset 0x20D00008, energía PMU 0x402B0004+4n) | Los núcleos despiertan, pero se colgaba: los CPUs 1-3 solo tenían dummy_timer y el broadcast con hrtimer no los despierta |
| v13 | Un temporizador por CPU (GPTIMER0 SPI28 / APTIMER0 SPI29 / APTIMER1 SPI119 / APTIMER2 SPI121; EB1 0x402E0004 b9-10) y nuevo estado CPUHP_AP_SPRD_SC8830_TIMER_STARTING | **4 NÚCLEOS**: el video de Batocera ya no se traba |
| v14 | **Táctil**: i2c-sprd-sc8830.c (I2C viejo, por sondeo, gate AP APB 0x71300000 b9) + sc2723-regulator.c (ADI 0x40030000, ventana del PMIC 0x40038000; vddsim2 pd 0x814 b3, trim 0x82c[15:8] = 1.2 V + n·10 mV, 3.0 V) + gpio-sprd de Linux ("sprd,sc9860-gpio" 0x40280000, SPI 35, registros idénticos) + melfas_mip4 con vdd-supply | **TÁCTIL FUNCIONANDO** (40 toques detectados en la prueba del init) |

Notas:
- En platsmp **no** se activa AUTO_SHUTDOWN_EN (b24): apagaría el núcleo en cada WFI del bucle de inactividad.
- ADI versión 1 (SC8830): RD_CMD +0x24, RD_DATA +0x28 (b31 ocupado, la dirección vuelve en b30:16), FIFO_STS +0x2c (b10 vacío), GSSI_CFG0 +0x1c (ancho de la dirección devuelta).
- I2C viejo: CTL+0 (b0 INT, b3 IE, b4 EN, b10 PULLUP, b11 DUTY), CMD+4 (dato en b15:8; b0 INT_ACK, b1 TX_ACK, b2 WRITE, b3 READ, b4 STOP, b5 START, b6 ACK = NACK recibido, b7 BUSY), CLKD0 = (div<<16)|div con div = 26 MHz / (4·f) − 1.
- El init muestra un RESUMEN y una prueba del táctil de 10 s antes de pasar a Batocera.

Siguiente: USB OTG (dwc2 en 0x20200000, SPI 55; MUIC SM5504 y cargador SM5701 por i2c-gpio) y después audio nativo.

## 11. Pantalla horizontal y mando táctil (EN CURSO al 24 de octubre de 2026, ~18:30)

- **Horizontal:** `display.rotate=1` en `/userdata/system/batocera.conf` (YA APLICADO). Batocera 38 en DRM
  lanza EmulationStation con `--screenrotate` y le pasa a RetroArch `video_rotation` (1→3, 3→1).
  Si se ve de cabeza, usar `display.rotate=3`.
- **Mando táctil** (`~/batocera-t280/userdata/system/touchpad/touchpad.py`, Python + evdev de Batocera):
  toma el MIP4 en exclusiva (grab) y crea por uinput un "Microsoft X-Box 360 pad" (045e:028e, versión 0x0114,
  que está en el es_input.cfg de Batocera). Zonas en 1280x800: cruceta (190,545) r175 con diagonales;
  A(1110,650) B(1210,545) X(1010,545) Y(1110,440); Select(540,745) Start(740,745) Hotkey(640,55);
  L1/R1 en las esquinas superiores; L2/R2 debajo. La rotación sale de display.rotate. Lógica probada en el PC.
- **Arranque:** `/userdata/system/custom.sh` (Batocera lo ejecuta con "start") lanza touchpad.py; log en
  `/userdata/system/logs/touchpad.log`.
- **Instalación:** `scripts/push-userdata.sh [clave=valor ...]` (con la tablet en TWRP): copia
  `~/batocera-t280/userdata/*` a /data (SHARE), verifica con sha256, edita batocera.conf y reinicia.
  ESTADO: TODO COPIADO Y VERIFICADO en la tablet (24 de octubre, ~18:35): custom.sh, touchpad.py, display.rotate=1.
  tenía un bug, `adb shell` leía stdin, ya corregido). Para terminar: entrar a TWRP y ejecutar
  `~/batocera-t280/scripts/push-userdata.sh display.rotate=1`.
- Por probar: ¿ES horizontal y derecho? ¿La cruceta y A mueven el menú? ¿ES pide configurar el mando?

## 12. Próximos pasos
1. Terminar el mando táctil (arriba) y ajustar las zonas si hace falta.
2. Imagen de guía del mando para RetroArch (overlay solo visual) y RetroArch en horizontal.
3. USB OTG (requiere adaptador OTG; el usuario tiene un mando USB): dwc2 0x20200000 SPI55, MUIC SM5504
   (extcon-sm5502), VBUS con el elevador del SM5701 por i2c-gpio, vddusb del PMIC.
4. Audio nativo (VBC + DMA + codec SC2723 por ADI): proyecto largo. Bluetooth descartado (Marlin sin driver).
5. Kernel: quitar la prueba táctil de 10 s del init cuando ya no haga falta.
- 18:45: La primera prueba del mando no hizo nada. touchpad.log vacío (el programa sí corre); EmulationStation
  ignora el mando virtual porque no lo encuentra en su es_input.cfg (el GUID de SDL no coincide, probablemente
  por el CRC del nombre). Solución aplicada: touchpad.py crea además un teclado virtual con el mapeo de
  teclado de Batocera (flechas, Enter = confirmar "b", Esc = volver "a", Espacio = start, Retroceso = select,
  "=" x, "-" y). Instalado en la tablet; falta probarlo. Pendiente: calcular el GUID con CRC y agregar la
  entrada a es_input.cfg para que el mando sirva en los juegos.
- 18:55: El teclado v2 tampoco respondía. Causa probable: udev solo marca ID_INPUT_KEYBOARD (y SDL solo escucha
  teclados con esa marca) si el dispositivo tiene un juego completo de teclas; el teclado virtual declaraba
  10. v3: declara las teclas 1-255 y además escribe un log (dispositivo, rangos, rotación y los primeros 30
  toques con coordenadas y teclas) en /userdata/system/logs/touchpad.log. Se instala con push-userdata.sh
  (esperando la entrada a TWRP).
- 19:xx: El log de la v3 salió VACÍO, aunque imprime al encontrar el táctil: el proceso moría antes de escribir.
  Causa probable: custom.sh lo lanzaba con "&" dentro de la sesión de los scripts de arranque, y al terminar
  estos el proceso recibía SIGHUP y moría en silencio. v4: custom.sh usa "nohup setsid python3 -u ... &" y
  anota cada arranque en el log; touchpad.py ignora SIGHUP y escribe "python arrancado" en su primera línea.
  Instalado; falta probar. Si falla otra vez: leer /data/system/logs/touchpad.log desde TWRP.
- El log de la v4 tenía 0 bytes aunque custom.sh escribe una fecha antes de lanzar Python: **custom.sh nunca se
  ejecuta**. S99userservices nunca llega porque algún servicio anterior se cuelga (S6x/S9x: NFS, CEC, Samba,
  NTP…; por investigar). El gancho /userdata/system/custom-es-config tampoco sirve: solo corre si existe
  xrandr (X11).
- **v5:** el mando arranca desde **/boot/boot-custom.sh** (S00bootcustom, el primer paso del arranque; /boot es
  la partición BATOCERA p25): `nohup setsid python3 -u /boot/touchpad/touchpad.py >/tmp/touchpad.log &`.
  El script espera a /userdata/system/batocera.conf para leer display.rotate (por defecto 1) y copia su log a
  /userdata/system/logs/touchpad.log cada 5 s. Se instala con `scripts/push-bootpart.sh` (monta p25 desde TWRP),
  que además borra /data/system/custom.sh. Fuentes en ~/batocera-t280/bootpart/. Instalado; falta probar.
- v5 también dejó el log vacío (el reflejo a /userdata cada 5 s no sirve si Python muere antes). El programa
  completo funciona en una simulación en el PC (un toque en A → BTN_SOUTH + Enter), así que la falla está en
  cómo se ejecuta en la tablet. **v6:** boot-custom.sh remonta /boot en rw y escribe todo en
  **/boot/touchpad.log** (partición BATOCERA, se lee desde TWRP montando p25 en /tmp/bato). Instalado.
- **23:20 (sesión nueva):** la tablet no estaba conectada, así que la v6 sigue SIN LEER (logs/touch-v6 vacío).
  Revisado offline en la imagen de Batocera: python3.11, evdev (con _input/_uinput para ARM), setsid, nohup,
  dmesg y batocera-settings-get-master existen, así que el código no tiene un fallo evidente. Orden de arranque:
  S00bootcustom (antes de udev) → S05udev → S31emulationstation → … → S99userservices.
  **v7** de boot-custom.sh añade una traza: /boot/trace.log (procesos y /dev/input cada 3 s durante 4 min,
  para ver qué servicio se cuelga antes de S99) y /boot/dmesg.txt (log del kernel cada 30 s).
  `scripts/push-bootpart.sh NOMBRE` ahora primero BAJA los logs (touchpad.log, trace.log, dmesg.txt y
  /data/system/logs) a logs/NOMBRE y luego instala. Dejado esperando la tablet en TWRP (nombre touch-v6).
- **23:15 v6 leída:** /boot/touchpad.log = "boot-custom.sh start, python3=/usr/bin/python3", "lanzado, pid 116",
  "touchpad: python arrancado" y NADA más (ni siquiera "dispositivos ..."). O sea: boot-custom.sh SÍ corre y
  Python arranca; muere o se cuelga entre esa línea y la siguiente (imports, sobre todo `import evdev`), o la
  sesión se cortó antes. En /data/system/logs no hay es_log.txt (ES no llegó a escribirlo en esa sesión) y el
  splash de mpv sí terminó (12 s), así que el arranque llegó al menos a S28. v7 instalada en p25 (trace.log
  + dmesg.txt). `scripts/pull-bootlogs.sh NOMBRE` baja los logs desde TWRP sin instalar nada.
- **23:20 v7 leída — CAUSA RAÍZ del mando táctil (y de wireplumber/lircd):** dmesg dice
  `python3: CONFIG_KUSER_HELPERS disabled at 0xffff0ffc` (igual para wireplumber y lircd). `allnoconfig` había
  apagado CONFIG_KUSER_HELPERS; cualquier proceso que lea la versión de los ayudantes de la página de vectores
  (0xffff0ffc) recibe SIGSEGV. Por eso Python moría en silencio justo después de "python arrancado" (trace.log:
  pid 115 en estado D a los 13 s y desaparecido a los 16 s) y los logs del mando siempre salían vacíos.
  Además el arranque SÍ llega hasta el final: rcS termina a ~40 s (ES arranca a ~38 s con --screenrotate 1), así
  que "S99 nunca llega" era falso; el único servicio lento es S27audioconfig (~20 s, no hay audio).
- **v15** = v14 + `CONFIG_KUSER_HELPERS=y` (arch/arm/configs/gtexswifi.config; ahora también en la lista de
  opciones obligatorias de build-kernel.sh). out/boot-gtex-v15.img, build-20.log. FLASHEADA a las 23:24.
  Falta probar el mando táctil. Cambios del kernel sin commit en git todavía.
- **23:30 v15 PROBADA — MANDO TÁCTIL FUNCIONANDO.** touchpad.log: encuentra "MELFAS MIP4 Touchscreen"
  (x 0-800, y 0-1280, rotate=1), crea "Microsoft X-Box 360 pad" (event1, y aparece js0) y "touchpad keyboard"
  (event2); toques con teclas (28 = Enter por el botón A, 1 = Esc, 12 = "-"). trace.log: python3 sigue vivo
  (pid 115, Ssl) con EmulationStation. El usuario navegó EmulationStation tocando y LANZÓ UN JUEGO: dmesg
  `[61.7] process '/usr/bin/retroarch' started` y existen es_launch_stdout/stderr.log. Batocera trae dos juegos
  libres de PC Engine en /roms/pcengine (Reflectron y Santatlantean, de aetherbyte). Falta confirmar: ¿los
  botones sirven DENTRO del juego (RetroArch usa el mando virtual, no el teclado)? ¿se ve horizontal y derecho?
  Nota: el log /boot/touchpad.log acumula varios arranques (v6, v7, v15).
- **23:40 OVERLAY VISUAL del mando (RetroArch).** El usuario confirma: se ve horizontal y los botones funcionan en
  el juego, pero no los ve. Batocera no sirve para esto: su sistema de bezels descarta overlays si la proporción
  del modo de video es < 1.6 (aquí 800x1280 = 0.625). Solución: RetroArch lee `<retroarchRoot>/<sistema>.cfg`
  (/userdata/system/configs/retroarch/) con --appendconfig, que gana sobre lo generado. `scripts/make-overlay.py`
  dibuja las zonas LEYÉNDOLAS de touchpad.py (no pueden desfasarse) y genera: gamepad.png (800x1280, girado 90°
  horario porque RetroArch dibuja los overlays SIN aplicar video_rotation: orientación física del panel),
  gamepad-landscape.png (1280x800, plan B si sale girado), gamepad.cfg (overlay0_full_screen) y un .cfg por
  sistema (117) con input_overlay_enable/opacity 0.8/hide_in_menu. Todo en overlay.tar; `scripts/push-overlay.sh`
  lo instala desde TWRP (un solo archivo con sha256, guarda configs previas en retroarch.bak-overlay) y reinicia.
  Comprobación en el PC: el pixel del botón B del panel pasa por make_mapper(rotation=1) y cae en su zona.
  PENDIENTE de ver en la tablet: ¿el overlay sale derecho o girado? Si sale girado, cambiar overlay0_overlay a
  gamepad-landscape.png (o rotar al revés). Opacidad: `make-overlay.py 0.5` y volver a instalar.
  Limpieza: userdata/system/custom.sh y touchpad/ viejos movidos a old/ (custom.sh habría lanzado un 2º mando).
- **23:55 El overlay SE VE en juego (no en ES: es de RetroArch).** Problemas vistos por el usuario en juego:
  (1) "se cuelga y no cierra con Hotkey+Start", (2) "se ve en un lateral qué tecla se presiona".
  Causa probable: el teclado virtual (hecho para ES) seguía activo DENTRO del juego y RetroArch lo lee: Esc
  cierra al instante (quit_press_twice=false), Espacio = avance rápido, Enter = Start; y al salir ES la consola
  Linux recupera el teclado y hace eco de las teclas (el texto del lateral). Hotkey=btn 8 y Salir=btn 7 sí están
  bien en retroarchcustom.cfg, pero exigen dos dedos a la vez.
  **touchpad v8:** (a) un hilo mira cada 0.25 s si corre `emulatorlauncher` (el mismo criterio que
  `batocera-es-swissknife --emupid`) y mientras tanto el teclado virtual calla; (b) mantener Hotkey+Start 1.5 s
  llama a `batocera-es-swissknife --emukill` (cierra el emulador a la manera de Batocera); (c) X/Y estaban
  cruzadas: un Xbox real manda BTN_X=BTN_NORTH (izquierda) y BTN_Y=BTN_WEST (arriba); ahora igual.
  Overlay: letras Xbox actualizadas, leyenda "Mantén HOTKEY + START 1.5 s para salir", oculto en el menú de RA.
  Se deja `log_to_file` de RetroArch (retroarch.log en /userdata/system/logs) para ver si de verdad se colgó.
  Instalación: `NOREBOOT=1 push-overlay.sh overlay-v2 && push-bootpart.sh touch-v8` (una sola sesión de TWRP).
- **00:05 Log de RetroArch de la prueba "colgada" (logs/overlay-v2/logs/retroarch.log, RA 1.15.0, truncado a
  8 KB por el apagado forzado):** carga retroarchcustom.cfg y "Appending config pcengine.cfg" (el overlay por
  sistema SÍ se aplica); modo de video 800x1280 (el overlay va en orientación física, como se dibujó); lee
  "touchpad keyboard" (event2) como teclado (confirma que RA veía mi teclado virtual); audio ALSA falla
  ("No such file", sigue sin audio). El "texto del lateral" eran avisos del núcleo pce_fast: "6-button mode
  selected for pad 1" / "2-button mode selected ..." repetidos; no era un cuelgue real, el juego seguía vivo.
  Posible causa de que Hotkey+Start no cerrara: RA exige Hotkey (btn 8) PRIMERO y luego Start (btn 7); con dos
  dedos casi a la vez cae en el orden equivocado. El cierre por pulsación larga (v8) no depende del orden.
  v8 instalada (overlay v2 + touchpad v8). make-overlay.py ya no deja __pycache__ en bootpart/.
- **00:20 Commodore 64 no abre** (reporte del usuario; sin log: el log de RA solo guarda el último juego y los
  apagados forzados lo truncan). Se usa vice_x64_libretro (no pide BIOS: solo JiffyDOS opcional); el juego de
  ejemplo es /roms/c64/fix_it_felix_64.d64. Auditoría de CONFIG del kernel: epoll/eventfd/timerfd/signalfd/
  futex/shmem/sysvipc/posix_timers etc. están; faltan CROSS_MEMORY_ATTACH, SECCOMP, CGROUPS, VDSO (no críticos).
  **touchpad v9:** mientras corre un emulador, copia cada 5 s (y al cerrar) retroarch.log, es_launch_stdout/
  stderr.log y `dmesg` a /boot/lastgame/ con fsync, para sobrevivir a un apagado forzado. push-bootpart.sh y
  pull-bootlogs.sh traen /boot/lastgame a logs/NOMBRE/lastgame/. Pendiente: probar el C64 y leer el log.
  IDEA de fondo: el cuello de botella es el ciclo TWRP. Un USB gadget (dwc2 en modo periférico + ethernet por
  USB) daría ssh desde el PC y copia de ROMs sin TWRP; requiere driver del PHY USB del SC8830.
- **00:35 Start del NES "no funciona" + Commodore sin sonido (reportes del usuario).**
  C64: el cuadro dice "VICE error: sound initialization failed for device alsa" (el usuario lo leyó bien: no
  hay tarjeta de sonido). RA lo confirma en todos los juegos ("[ALSA] Failed to open PLAYBACK ... default").
  **Kernel v16** = v15 + tarjeta de sonido virtual: CONFIG_SOUND, SND, SND_PROC_FS, SND_DRIVERS, SND_PCM_TIMER,
  SND_DUMMY (arch/arm/configs/gtexswifi.config; en la lista obligatoria de build-kernel.sh). out/boot-gtex-v16.img,
  build-21.log. No hay audio real todavía, pero VICE/RA/Batocera ya ven una tarjeta ALSA.
  NES/Start: la última prueba fue "2048 (tsone).nes" (fceumm). En ES, Start solo se detectó 18 veces y siempre
  con raw x=0 (pegado al borde inferior; el panel sí llega a x=0..798: no hay zona muerta). Dentro del juego NO
  hay datos: el log solo guardaba 30 toques por arranque. **touchpad v10:** registra cada botón pulsado/soltado
  ("pad [juego] +START dedos=[(x,y)]") y refila el presupuesto de toques al abrir/cerrar un juego; Select/Start
  se subieron de y=745 a 725 y su zona pasó de r=70 a r=85 (el overlay se regenera solo desde touchpad.py).
  `scripts/install-all.sh NOMBRE [KERNEL_IMG]` = una sola sesión de TWRP: baja logs, instala overlay+/boot y
  flashea el kernel por Download mode. Armado con v10 + kernel v16 (logs a logs/v10/).
- **00:00 (25 oct) v10 + kernel v16 INSTALADOS** (install-all.sh corrió completo al instante porque el usuario ya
  estaba en TWRP; yo había dicho "falta que entres a TWRP" sin comprobarlo: error mío). Flasheo OK 23:59:25 y la
  tablet reinició en Batocera. logs/v10/ = logs del arranque anterior (v9); su lastgame/ es el NES "2048" sin
  datos útiles del Start (v9 aún no registraba botones). Falta probar: Start en el NES (ahora con log
  "pad [juego] +START dedos=..."), C64 con la tarjeta de sonido virtual, y que el cierre Hotkey+Start 1.5 s funcione.
  Leer con `scripts/pull-bootlogs.sh NOMBRE` (solo lee; deja la tablet en TWRP; luego `adb reboot`).
- **00:15 CAUSA del Start del NES y de "los botones no sirven bien": el overlay salía GIRADO 180° respecto a las
  zonas** (el usuario: "horizontal pero de cabeza"). Lo había girado 90° en sentido horario; correcto es
  antihorario (np.rot90(L, 1)). Así, al tocar el "Start" dibujado se tocaba otra zona. Mi comprobación anterior
  daba OK porque suponía que la coordenada cruda del táctil == píxel físico del panel; era falso (el táctil va
  girado 180° respecto a la pantalla), y lo que vale es lo que ve el usuario. Ahora: check_mapping deshace el giro
  y compara con el dibujo horizontal. overlay.tar regenerado. Se instala con `install-all.sh v10-test` (sin kernel).
  Por confirmar: que ahora salga derecho (y no ESPEJADO: si el texto se ve invertido como en un espejo, el cambio
  correcto sería un volteo vertical y no un giro).
- **00:30 (25 oct) Overlay derecho INSTALADO (install-all.sh v10-test; tablet reiniciada en Batocera).** Del log de la
  prueba anterior (overlay de cabeza, logs/v10-test/touchpad.log): confirma la causa. Los toques sobre el D-pad
  y A "dibujados" (arriba, por estar girado) caían en R2/L2/L1 (decenas de "+L2", "+R2" con dedos en (80-250,
  180-290) y (1060-1200, 230-290)), y Start/Select sí se detectan al tocar su zona real (muchos "+START" con
  dedos y≈770-800). Intento de salida: "+HOTKEY" y "+START" pero nunca juntos 1.5 s, no hubo "cierro el emulador".
  **Sonido virtual OK:** dmesg "ALSA device list: #0: Dummy 1"; RetroArch ya dice 'Initialized PLAYBACK device
  "default"' (antes fallaba); S27audioconfig ya no aparece en trace.log (ya no espera ~20 s). Falta probar
  C64 (VICE) con la tarjeta.
- **00:55 (25 oct) CORRECCIÓN del diagnóstico anterior.** El usuario aclaró: con el primer overlay (giro
  horario) el DIBUJO coincidía con la pantalla/emulador y la que estaba girada 180° era la HITBOX; al girar yo el
  dibujo (antihorario) dibujo e hitbox coincidieron entre sí pero ambos quedaron 180° al revés respecto a la
  pantalla. Mi entrada de las 00:15 ("el overlay salía girado") era una lectura equivocada: "dibujo al revés"
  significaba "dibujo y zona no coinciden". Causa real: **el panel táctil MIP4 está montado girado 180° respecto
  a la pantalla.** Arreglo: touchpad.py `make_mapper.norm` cuenta desde la esquina opuesta (1 - x/pw, 1 - y/ph) y
  make-overlay.py vuelve al giro HORARIO (np.rot90(L, -1)). Comprobación en el PC: para cada botón, un dedo
  sobre su dibujo (píxel físico) -> toque crudo (799-x, 1279-y) -> make_mapper -> zona correcta (7/7 OK).
  Nota para el usuario: las zonas en EmulationStation también cambian de sitio (ahora sí donde dice el diseño:
  A abajo a la derecha de la vista horizontal). Instalación: install-all.sh v10-flip2 (sin kernel).
- **01:10 (25 oct) MANDO TÁCTIL FUNCIONANDO** (confirmado: el usuario jugó "2048 (tsone)" del NES; dibujo y zonas
  alineados con la pantalla). C64 con la tarjeta virtual: sin confirmar todavía.

## 13. Audio nativo (EN CURSO desde el 25 de octubre de 2026)
Kernel de Samsung (src/kernel-stock-3.10): CONFIG_SND_SOC_SPRD_VBC_R2P0_SPRD_CODEC_V4. Piezas: DMA AP
(dmac@20100000, SPI 50), VBC r2p0 (0x40020000, 12 KB; DA0 = 0x40020000), codec digital "DP" (0x40000000, 8 KB),
AUDIO_IF (0x40010000) y codec analógico dentro del PMIC SC2723 (por ADI). El VBC lo comparten AP/CP0/CP1:
hay que dárselo al AP (REG_AON_APB_VBC_CTRL, AP_WPROT_EN b6).
- **Fase 1 (kernel v17): tono de prueba sin ALSA ni DMA.** drivers/misc/sc8830-audio-test.c (CONFIG_SPRD_SC8830_AUDIO_TEST,
  nodo audio-test@40020000 en sc7730.dtsi). 3 s después del probe: dueño del VBC/codec = AP (AON VBC_CTRL
  b19:1 = 0, AP_WPROT_EN b6 = 0), relojes AON EB0 VBC b19/AUD b18 (AUDIF auto b20 de VBC_CTRL), SINDRV b0,
  resets AON RST0 b20:18; PMU SLEEP_CTRL b8, MEM_PD_CFG0 b5:2 = 0. PMIC por ADI (ventana 0x40038000): GLB
  0x800 ARM_MODULE_EN b4, 0x804 CLK b0-1, 0x8fc b1, 0x808 b4, 0x8e8 b8, 0x8a4 = 0, reset 0x80c b11:9; LDO
  VB(PMU0 b9) BG(PMU1 b15) BG_IBIAS(b14) VCOM(b13, VCM_V=3) VCOM_BUF(b12) VBO(PMU0 b8) +20 ms; codec
  analógico en 0x600 (PMU0-6, CDC0-18, DIG_CFG0 0x68c). Ruta DAPM: DA clk, DRV clk, AUD_TOP_CTL DAC_EN,
  DIG_CFG0 DACL/R, CDC2 AOL_EN, PA_SW_EN, CDC6 DACL/DACR_AOL, CDC2 DACL/R_EN, ganancias, PA clase AB
  desde VBAT (PMU3/5, OVP_LDO, PA_EN). VBC: búferes ping-pong de 160 muestras llenados con un seno de
  300 Hz a 48 kHz (RAMSW_EN), canales DA0/DA1, VBENABLE sin DMA. Vuelca registros al log (dmesg) y dice
  si el contador VBDACNT se mueve. 6 s de tono. out/boot-gtex-v17.img (build-22.log).
- **00:50 ¡EL TONO SUENA!** (confirmado por el usuario con el kernel v17). Funciona toda la cadena: dueño
  AP del VBC, relojes, LDO del codec, DAC, mezclador hacia AOL y PA del altavoz. Guardado en git local
  (src/linux-6.18.58): commits v15/v16 (config) y v17 (tono), tag v17-audio-tone, patches 0012-0013.
  Siguiente: fase 2 = driver DMA del SoC + driver ALSA (VBC como DAI, codec) para que suene Batocera.
- **Fase 2 (kernel v18): driver ALSA real**, sound/arm/sc8830-audio.c (CONFIG_SND_ARM + CONFIG_SND_SC8830; nodo
  audio@40020000 con regs vbc/codec/aon/pmu/adi/pmic/dma(0x20100000)/ahb(0x20d00000) e IRQ SPI 50). Sale la
  tarjeta virtual y el tono. DMA AP r4p0 programado a mano como el kernel de Samsung (sprd-pcm.c +
  sprd_dma.c): canales COMPLETOS 24 (izq, UID 29 -> VBDA0) y 25 (der, UID 30 -> VBDA1) (los 0-23 son
  "estándar", sin lista enlazada; por eso no sirve tal cual el sprd-dma de mainline), lista enlazada cíclica
  con un nodo por periodo (nodo = imagen de los 16 registros del canal; LLIST_PTR = siguiente nodo + 0x10),
  FRG_LEN 16 bits, petición por fragmento de 320 bytes, paso de origen 4 (lee una muestra de cada dos del
  búfer intercalado), BLK_DONE del canal izquierdo = fin de periodo; REQ_CID(uid) = canal + 1 en +0x2000.
  Reloj del DMA: AP AHB EB b5. Periodos múltiplos de 640 bytes. 44.1/48 kHz S16 estéreo. Controles
  "Speaker Playback Volume" (AOL_G 0-15, 10 por defecto) y "Speaker Playback Switch". Codec encendido al
  abrir, apagado 5 s después de cerrar. out/boot-gtex-v18.img (build-23.log). Sin probar.
- **v18 probada: sin sonido.** dmesg: "card registered", "ALSA device list: #0: Spreadtrum SC7730 ..." y el codec se
  enciende/apaga al abrir/cerrar; pero RetroArch: 'Failed to open PLAYBACK stream on device "default": Invalid
  argument'. Causa: exigía periodos múltiplos de 160 frames (FIFO del vendor) y dmix/pipewire piden 1024.
  **v19:** FIFO del VBC de 64 muestras (el campo admite hasta 256): periodos múltiplos de 64 frames. FLASHEADA 01:00.
- **v19 probada: SUENA, pero "distorsionado infernal".** Causa: ancho de dato del DMA. Códigos del hardware 0 = byte,
  1 = 16 bits, 2 = 32 bits (memcpy del vendor: datawidth 2 con paso 4; dma_r4p0.c traduce SHORT_WIDTH -> 1).
  Yo escribía 2 (el valor del enum SHORT_WIDTH), así que mandaba palabras de 32 bits (izq+der pegadas).
  **v20:** ancho 1 en FRG_LEN. out/boot-gtex-v20.img (build-25.log).
- **v20 probada: "se escucha muchísimo mejor pero satura / falla mucho".** AOL_G es -24 dB + 3 dB/paso: el 10 por
  defecto era +6 dB, y además se suman izq+der en el altavoz mono. **v21:** ganancia por defecto 6 (-6 dB) y
  TLV en "Speaker Playback Volume" (para que pipewire/alsamixer usen dB). Pendiente ver si los cortes ("falla")
  siguen: podrían ser xruns (mirar logs).
- **v21 probada: "algo bajo y saturado aún"** -> bajar 12 dB la ganancia del PA no quitó la distorsión: no es el PA.
  **v22:** como el driver del vendor al arrancar un stream: ganancia digital del VBC apagada (DADGCTL b7/b15 = 0),
  ALC (limitador) activo con sus parámetros por defecto (DAALCCTL0-10 de vbc_da_eq_profile_default), side tone
  off (STCTL0/1 = 0x183), DACSRCCTL = MIXERCTL = 0, DAHPCTL = ALC_EN (EQ apagado: plano). Ganancia del PA 8 (0 dB).
  Vuelca registros VBC/codec al log en cada prepare ("Hz, period ..." y "AUD TOP=..."). Hipótesis abiertas si sigue:
  desplazamiento de un bit en la interfaz VBC->codec (valores x2: el tono a 1/4 de escala sonaba limpio), suma
  izq+der en el mezclador analógico del altavoz, o modo del PA (clase AB vs D).
- **v22 + logs (logs/v22): CAUSA de la saturación.** Volcado en juego: CDC11=f000 -> AOL_G = 15 = +21 dB. Batocera/
  pipewire lleva el control "Speaker Playback Volume" al 100 % (desde la v21 tiene TLV); mi valor por defecto no
  importaba. Resto correcto: juego a 44.1 kHz (DAC=3f2), DG=1818 (ganancia digital apagada), HP=0800 (ALC),
  I2S=0780 (por defecto), CDC2=d050, CDC6=3000, PMU0=ab00. **v23:** el control llega como máximo a 8 (0 dB).
- **v23 probada: "suena peor que antes".** Con el PA a 0 dB sigue distorsionado -> no es el amplificador: el problema está
  antes (datos/DMA/DAC/mezclador). Batocera pasa por PipeWire (S06audio), sensible a la exactitud del puntero.
  Hipótesis sin descartar: NES es mono y el mezclador analógico del altavoz SUMA L+R (+6 dB antes de AOL_G),
  inicio de la lista enlazada (el vendor carga el nodo 0 dos veces), velocidad real del DMA, salto del puntero.
  **v24:** el driver se mide mientras suena (cada 250 ms, log cada 2 s): "stats: N frames/s (X Hz), irqs/s, back,
  skew (max) B, L= R= of tamaño". Sale en logs/NOMBRE/lastgame/dmesg.txt (touchpad.py lo guarda cada 5 s).
- **v23 leída (logs/v23):** CDC11=8000 (AOL_G=8, 0 dB): el tope funciona, y el usuario oye "peor que antes" -> la
  distorsión NO está en el PA. v24 (con estadísticas) FLASHEADA 05:56. Falta: jugar ~20 s y leer "stats:" en
  lastgame/dmesg.txt (frames/s reales, irqs/s, retrocesos del puntero, desfase L/R).
- **v24 leída (logs/v24):** irqs/s = 42-43 con periodos de 1024 a 44.1 kHz (esperado 43.07): el DMA/VBC consumen a la
  velocidad EXACTA; skew L/R = 0 B siempre. (Mis "frames/s" y "back" no valen: el búfer de 4096 frames da varias
  vueltas entre muestras de 250 ms; aliasing. "pointer outside the buffer: L=16384" = final del búfer, inofensivo.)
  -> sincronía y velocidad bien; queda el CONTENIDO (nivel, suma L+R, datos). **Prueba de niveles** en el arranque:
  /boot/audiotest/ (scripts/make-audiotest.py + audio-test.sh, una sola vez por instalación): n pitidos + seno de
  440 Hz a -30, -18, -9, -3 dBFS y un barrido, por pipewire (`aplay -D default`).
- **Prueba de niveles: nunca sonó** (el usuario no oyó pitidos ni notas) pero la música del menú de ES SÍ suena, así
  que la ruta PipeWire->ALSA funciona. Su descripción de la música: "áspero, frío, sucio, con muchos tonos por los 1k".
  Hipótesis: artefacto periódico por el tamaño del fragmento del VBC (64 muestras -> 689/750 Hz y armónicos); el
  tono de prueba de 300 Hz con periodo = 160 muestras (tamaño del búfer) ocultaría cualquier repetición.
  Pendiente: leer /boot/audiotest/audiotest.log para ver por qué no sonó (¿aplay/pactl sin servidor?).
- **Prueba de niveles: por qué no sonó (logs/v24e/audiotest.log):** a los 6 s de arrancar PipeWire solo existía el sink
  "auto_null" (destino falso); aplay -D default fue ahí. El volumen del mixer estaba en 1 (alsactl init) antes de que
  llegara un stream. Idea del usuario: usar un fragmento de 16 en vez de 64 (él lo llama "bits": es el tamaño del
  FIFO del VBC en MUESTRAS). Razonamiento: un fragmento pequeño da MENOS margen al DMA (16 muestras = 0.36 ms a
  44.1 kHz) y el vendor usa 160; no hay certeza, así que **kernel v25**: tamaño ajustable, control ALSA
  "FIFO Frames" (16..256 de 16 en 16; -EBUSY con el stream abierto; por defecto 128).
  **Prueba de fragmentos** (audio-test.sh nuevo): espera el sink alsa, `pactl suspend-sink` para soltar la tarjeta,
  y `aplay -D plughw:0,0` toca music.wav (8 s, onda cuadrada tipo NES) con FIFO 16, 32, 64, 128, 256 anunciados por
  1..5 pitidos. v25 flasheada a las 06:18 y /boot/audiotest instalado (una sola vez por instalación).
- **v25 leída (logs/v25b):** el sink real ya aparece ("alsa_output.platform-40020000.audio.stereo-fallback" a los 7 s)
  pero amixer/aplay: "Device or resource busy": EmulationStation arranca (~38 s) y su música reabre la tarjeta
  (dmesg: nuevo "codec on" 44.1 kHz) en plena prueba; pactl suspend-sink no basta. audio-test.sh ahora PARA
  ES (S31emulationstation stop) antes, espera a que /proc/asound/card0/pcm0p/sub0/status diga "closed", prueba,
  y arranca ES al final. Pantalla negra mientras suena es normal.
- **v25d leído:** en ese arranque PipeWire NUNCA creó el sink alsa en 150 s (solo auto_null); en el anterior sí a los 7 s.
  Carrera entre wireplumber (sondea la tarjeta a los 19 s) y ES (abre la tarjeta directo a 44.1 kHz a los 26 s):
  depender de PipeWire/ES para la prueba es frágil. **audio-test.sh v3:** sin PipeWire; arranca al principio del boot
  (la tarjeta existe desde 0.65 s; /dev/snd por devtmpfs), toca con `aplay -D plughw:0,0` y reintenta si está
  ocupada; repone "Speaker Playback Volume" = 8 antes de cada tramo (alsactl init lo baja a 1).
- **Prueba de fragmentos (por el usuario, reinició antes de oír 64-256):** "16 y 32 suenan bien" (con 64 sonaba
  áspero, tonos cerca de 1 kHz). Confirma la sospecha: el FIFO del VBC solo aguanta fragmentos pequeños sin
  errores de empalme; la intuición del usuario (usar 16) era buena. **v26:** fragmento 32 por defecto (el más
  grande de los dos limpios, más margen al DMA con carga); control "FIFO Frames" sigue (16..256); se quita el
  medidor "stats". out/boot-gtex-v26.img (build-31.log). Por probar con un juego (carga de CPU/GPU).
- **v26 (fragmento 32) flasheada 06:31; el usuario: "probemos con 16"** (interpretado: con 32 en uso real no queda bien).
  **v27:** VBC_FIFO_FRAMES = 16. out/boot-gtex-v27.img (build-32.log).
- **v27 (fragmento 16) en uso real: "áspero, tonos agudos".** La prueba de fragmentos sonaba limpia porque usaba música a -12 dBFS;
  la real es mucho más fuerte -> sospecha: RECORTE por la suma L+R en el mezclador analógico del altavoz (CDC6
  DACL_AOL|DACR_AOL), ALC o ganancias. **v28:** controles "Speaker Mix" (L+R/L/R), "DAC Gain" (0-7), "ALC Switch",
  "VBC Digital Gain" (0 = apagado). Prueba nueva (music.wav a -3 dBFS, 6 variantes con 1..6 pitidos: base, solo
  izquierdo, DAC -3 dB, ALC off, VBC DG 12, fragmento 128). out/boot-gtex-v28.img (build-33.log).
- **Pregunta del usuario: ¿a cuántos kHz va? ¿16 bits y 44.1 kHz (calidad CD)?** Hardware: SOLO S16_LE (el driver no
  acepta otro); 44.1 kHz en 65 aperturas y 48 kHz en 48 (PipeWire a 48 kHz por defecto; apps abren a 44.1).
  Se fija PipeWire a 44.1 kHz con un drop-in 10-rate.conf (default.clock.rate = 44100, allowed-rates = [44100]) en
  /userdata/system/configs/pipewire/pipewire.conf.d/ y /userdata/system/.config/pipewire/pipewire.conf.d/ (va en
  overlay.tar, instalado por install-all.sh v28). Independiente de la prueba de variantes (que va directa a ALSA).
- **v28 oída (prueba de variantes, música a -3 dBFS):** el usuario: "el 4 y el 3 fueron los mejores" = ALC apagado y
  DAC -3 dB. -> problema de nivel/limitador, no de fragmento ni de frecuencia. **v29:** por defecto ALC off,
  DAC gain 1, fragmento 32. Nueva ronda de 6 variantes alrededor de esa base (solo izquierdo, fragmento 16, 128,
  ALC on, DAC 7). out/boot-gtex-v29.img (build-34.log).
- **v29 oída (usuario): "suenan bien 2 y más 4 que 3"** = solo izquierdo suena bien (la suma L+R satura el amplificador mono)
  y fragmento 128 mejor que 16. **v30:** (1) mezcla digital "Downmix" en .copy (L=R=(L+R)/2; ruta analógica solo DAC
  izquierdo) -> sin mmap (info = INTERLEAVED|BLOCK_TRANSFER), (2) fragmento 128, (3) control "Speaker Boost" 0-7
  (pasos de 3 dB que se suman al volumen, tope 15) para buscar volumen, (4) defaults: Downmix, ALC off, DAC 1.
  Prueba nueva: 6 tramos (DAC 1/7 y boost 0/2/4) + comprobación ESTÉREO (stereo.wav: lead izquierda, bajo derecha).
  out/boot-gtex-v30.img (build-35.log).
- **v30 leída (logs/v30):** "aplay gave up" para blip y música: codec on a los 15.7 s (open OK) pero NUNCA llega a
  prepare (sin volcado "Hz, period" de aplay) -> falla entre open y prepare, solo con el cambio de la v30
  (sin MMAP + .copy). El script silenciaba el error de aplay. **v31:** el modo nuevo es OPCIONAL: control "Write Mode"
  (apagado por defecto = mmap como la v29), .copy siempre presente; default mix = Left, FIFO 128. Con Write Mode
  el driver imprime open/hw_params/prepare/copy (para ver dónde se detiene). audio-test.sh v5: 1-4 volumen (izquierdo,
  DAC 1/7, boost 0/2/4), 5 estéreo con downmix (registra el error de aplay), 6 estéreo solo izquierdo.
- **v31 leída (logs/v31):** el usuario: "1-3 bien, 4 distorsiona (+12 dB), 5 no se oyó o tardó". Log: el modo Write Mode FUNCIONA
  con audio estéreo (open, hw_params, prepare y copy sin errores; "copy: channel 0" siempre, el interleaved va por canal 0).
  Lo que falla es aplay con archivos MONO en write mode ("Unable to install hw params", plug mono->estéreo sin mmap): 5 pitidos
  x 7 s de reintentos = ~35 s de retraso ("tardó mucho"). Y en la v30 TODOS mis archivos de prueba eran mono: por eso "nunca sonó".
  Valores limpios: DAC 7 (0 dB) + boost 2 (+6 dB); boost 4 distorsiona. **v32:** defaults = Downmix + Write Mode + DAC 7 +
  boost 2 + ALC off + FIFO 128; sin trazas. Sin prueba de audio (se borró /boot/audiotest/run). Falta oírlo con ES/juegos.
- **v32 en Batocera real: "no suena Batocera".** Hipótesis (sin log aún): PipeWire/wireplumber no abre la tarjeta en modo write
  (sin mmap) o negocia algo que el driver rechaza; aplay estéreo sí funcionó. **v33:** vuelve el camino mmap por defecto
  (write_mode off, mix Left) con DAC 7 + boost 2 (los volúmenes limpios); Write Mode/Downmix quedan como controles.
  install-all.sh v33 baja primero los logs del arranque anterior (v32) -> logs/v33/: mirar dmesg/wireplumber por qué no suena.
- **v32 leída (logs/v33):** "no suena Batocera". dmesg: tras el arranque, las aperturas a 26.5 s y 45.8 s ("codec on") NUNCA llegan a
  prepare (sin volcado "Hz, period"): PipeWire/ES no consiguen configurar la tarjeta en Write Mode (aplay estéreo sí
  pudo). También: PipeWire sigue abriendo a 48000 Hz a los 19 s -> mi drop-in de 44.1 kHz NO se aplicó: PipeWire 0.3.57
  lo lanzan los scripts init (HOME distinto de /userdata/system, XDG_CONFIG_HOME sin definir). **Arreglo:**
  boot-custom.sh copia /boot/pipewire/10-rate.conf a /etc/pipewire/pipewire.conf.d/ en cada arranque (S00, antes de S06audio).
  Pregunta del usuario: "buffer 128 y DAC a 16 bits 44.1 kHz?" -> buffer 128 ya; 16 bit es lo único que acepta el driver; falta 44.1.
- **v33 en uso real: "se escucha mal, muy mal".** Hipótesis: el audio real va por PipeWire a 48 kHz (el drop-in de 44.1 no estaba
  aplicado) y TODAS las pruebas limpias directas fueron a 44.1 kHz; nunca se probó música a 48 kHz. Prueba nueva en el
  siguiente TWRP (push-bootpart v33b ya armado, con /etc/pipewire 44.1 kHz): 4 tramos directos con los defaults (izquierdo,
  DAC 7, boost 2/0, FIFO 128): 1 = 44.1 kHz, 2 = 48 kHz, 3 = 44.1 con boost 0, 4 = 48 con boost 0. music48.wav = misma música.
- **Prueba 44.1 vs 48 kHz oída (usuario): "parece ser 3"** = 44.1 kHz con el amplificador a 0 dB (boost 0), 6 dB menos que el default
  (boost 2). El +6 dB estaba bien con la música sintética pero la música real distorsiona. **v34:** boost 0 por defecto (volumen 100%
  = AOL_G 8 = 0 dB). PipeWire ya fijado a 44.1 kHz (/etc/pipewire/pipewire.conf.d, v33b). out/boot-gtex-v34.img (build-39.log).
  Pendiente: oír ES y juegos con v34 + PipeWire 44.1; el volumen puede quedar algo bajo (un solo canal, sin refuerzo).
- **El primer pitido de la prueba suena con un "golpe" (pop)** (confirmado por el usuario). Los registros mostraron que ganancias DAC/AOL son
  las del script (no hay cambio de volumen entre pitidos): es el transitorio del primer encendido del codec. Samsung lo evita así
  (sprd-codec.c pga_event): ganancias al MÍNIMO mientras enciende la cadena y se suben al final; al apagar, se bajan primero.
  **v35 = v34 (boost 0) + arranque suave:** codec_power_on deja DAC/AOL en 0, enciende el PA, espera 30 ms, abre el DAC y sube AOL_G
  un paso (3 dB) cada 4 ms; codec_power_off baja los dos a 0 antes de apagar el PA. out/boot-gtex-v35.img (build-40.log).
- **El "golpe" solo salía en la prueba** (el usuario: "no está, solo era en tu prueba"): con la v35 flasheada (07:22) ya no se oye en uso normal.
  **Guardado en git local** (src/linux-6.18.58): commit "ALSA: sc8830-audio ... (v18-v35)", tag v35-audio, patch 0014.
  Estado del audio: suena por PipeWire a 44.1 kHz fijo (/etc/pipewire, desde boot-custom.sh), SOLO el canal izquierdo, DAC 0 dB, amp 0 dB,
  ALC apagado, FIFO 128, arranque suave. Pendientes: mezcla estéreo (Write Mode falla con PipeWire), audífonos (CDC3 clase G, jack),
  volumen algo bajo, quitar de boot-custom.sh la traza de arranque y de init la prueba táctil de 10 s.
- **Foto del usuario (consola de la tablet tras una prueba, 07:27):** seis "ALSA lib pcm.c:8684:(snd_pcm_recover) underrun occurred" con ES abierto
  (46-150 s) y, con RetroArch en marcha (153.7 s), un nuevo "prepare" del flujo (44100 Hz, period 1024 x 4) cada ~10 s (154.7, 160.8,
  171.1, 181.9): un corte de audio periódico. NO es un log viejo (marcas de tiempo de este arranque). Sospechosos propios: el bucle de traza
  de boot-custom.sh (ps/grep/ls cada 3 s, 4 min) y touchpad.save_game_logs (3 logs + dmesg con fsync a la eMMC cada 5 s con un juego).
  **v36:** se quitan (traza de 1 min cada 5 s; copia de logs solo al abrir/cerrar un juego; /proc cada 1 s); PipeWire quantum 2048 /
  min-quantum 1024 (drop-in 10-rate.conf); el driver registra "stop while running (xrun?): hw= appl= buffer= period=" para saber si el
  corte es un salto del puntero (hw salta ~un búfer) o falta de datos (hw apenas pasa de appl). out/boot-gtex-v36.img (build-41.log).
- **Pregunta del usuario: "¿cuánto le da la CPU de datos al DAC?"** Respuesta: casi nada, lo hace el DMA. 44.1 kHz x 16 bit x 2 ca = 176 400 B/s (1.4 Mbit/s);
  88 200 B/s por canal DMA, bloques de 256 B (FIFO 128 muestras), ~344 peticiones/s por canal; la CPU solo atiende ~43 IRQ/s (periodo 1024) o ~22/s
  (periodo 2048). Lo que sí consume CPU son las apps (PipeWire convirtiendo 47.9 -> 44.1 kHz, el emulador). No estaba medido: **touchpad.py
  v10b** muestrea cada 2 s el % de CPU total y de pipewire, wireplumber, retroarch, emulationstation, python3 y emulatorlauncher (en memoria)
  y lo guarda en /boot/lastgame/cpu.log al abrir y cerrar un juego. Va en el bootpart del install-all v36 ya armado.
- **v36 ya instalada (07:31)**; el medidor de CPU se añadió DESPUÉS (07:34) y NO está aún en la tablet: armado push-bootpart.sh v36b (baja los logs de
  v36 = líneas "stop while running (xrun?)" del driver, y sube touchpad.py con el medidor; reinicia en Batocera).
- **v36 probada: el usuario NO nota cortes.** logs/v36b: 6 "stop while running (xrun?)" con appl=0 (hw 131072-919552 frames = 3-21 s): son flujos
  ociosos (PipeWire/ES) que nunca recibieron datos y se cerraron; no son cortes. Todo abre a 44100 Hz (el drop-in de /etc/pipewire SÍ se aplica);
  el periodo de PipeWire sigue en 1024 x 4 (quantum 2048 no aplicado, sin importancia). Los "underrun" de la foto eran de la versión anterior
  con las ayudas de depuración activas. touchpad.py con medidor de CPU instalado (v36b, 07:4x): /boot/lastgame/cpu.log al abrir/cerrar un juego.
  Siguiente: estéreo (Write Mode no funciona con PipeWire), volumen, audífonos, limpieza (prueba táctil de 10 s del init).
- **Foto del usuario 07:46 (v36 instalada, justo tras abrir el 2048): SIGUE habiendo "ALSA lib ... underrun occurred" (8 + 8)** -> mi afirmación de que con
  la v36 ya no había evidencia de cortes era PREMATURA (el usuario solo dice que no los oye). Línea de tiempo: ES abre 44.1 kHz a 45.9 s y salen 8
  underrun; a 346.2 s cierra ese flujo tras 300 s (hw=13241344) con appl=0; RetroArch arranca a 371.1 s; flujos de 13.7 s (372.0-385.7) y 14.3 s
  (388.0-402.3), 8 underrun en medio, todos con appl=0 -> inexplicado: o mi lectura de appl_ptr es errónea, o esos flujos no llevan el audio del juego.
  Los "underrun occurred" los imprime una aplicación (SDL de ES probablemente), no el kernel. **v37:** registra en CADA stop (estado, periodos, hw, appl) y
  los primeros 6 periodos de cada flujo + uno cada ~11 s (hw/appl) para ver si appl avanza. install-all v37 baja antes los logs de esta sesión (cpu.log).
- **HALLAZGO GRANDE (cpu.log de la v36 + /etc/asound.conf de la imagen):** los juegos y ES NO pasan por PipeWire (pipewire=0 % y wireplumber=0 % durante el 2048;
  retroarch ~37 % de un núcleo). /etc/asound.conf: `pcm.!default {type plug; slave.pcm "dmixer"}`, dmixer = dmix sobre hw:0,0, rate 44100, period_size 1024,
  buffer_size 4096 -> el "44100 Hz, period 1024 x 4" de todos los volcados. Consecuencias: (1) el drop-in de PipeWire (44.1 kHz, quantum) es irrelevante para
  los juegos; (2) `appl=0` en los "stop while running" es lo normal de dmix: mantiene el esclavo corriendo libre (stop_threshold = boundary, silencio automático) y
  nunca avanza appl_ptr; hw=13241344 = 300 s girando en vacío (a confirmar con los logs de v37); (3) los "ALSA lib ... underrun occurred" los imprime un
  CLIENTE de ALSA (SDL de ES: snd_pcm_recover con silent=0), no el kernel; RetroArch usa snd_pcm_recover en silencio; (4) EL ESTÉREO SE ARREGLA EN USUARIO:
  ~/.asoundrc con un plugin `route` delante de dmix (ttable.0.0 0.5, ttable.1.0 0.5) -> /userdata/system/.asoundrc (en overlay.tar). Bug del medidor: comm va
  truncado a 15 caracteres ("emulationstatio"): ES salía a 0 %.
- **v37 leída (logs/v37c): el lado hardware está limpio.** Interrupción de periodo cada 23.2 ms EXACTOS (1024/44100 = 23.22) en todos los flujos, sin jitter; en cada
  stop periods x 1024 = hw (448 x 1024 = 458752, 2981 x 1024 = 3052544...): ni una IRQ perdida; appl=0 constante incluso en el flujo del 2048 (59.3-128.5 s,
  2981 periodos): confirma que dmix mantiene el hardware corriendo libre. state=3 (RUNNING) en todos los stops. Por tanto los "underrun occurred" son de un
  cliente ALSA (SDL de ES), no del driver ni del DMA. Falta oír la prueba de estéreo (.asoundrc): variante 1 (default con downmix) = melodía + bajo.
- **Estéreo por ~/.asoundrc: FUNCIONA** (el usuario oyó melodía y bajo a la vez en la variante 1; "es una bocina pequeña, casi sin graves, le falta cuerpo").
  Foto 08:15 (v37): ritmo de periodos idéntico (500 periodos = 11.6 s) durante >300 s; underruns de cliente aislados y espaciados; "14 callbacks suppressed" =
  limitador de mi propio log. Guardado en git: v37-audio (parche 0016). Pendiente: graves/cuerpo (ecualización), volumen, audífonos, limpieza.
- **Pregunta del usuario: "¿no recuerdas nada? lo de Android está en la carpeta".** Comprobado: el respaldo (backup/, idéntico en ~/batocera-t280 y en
  ~/Escritorio/batocera-t280 [copia antigua]) tiene 24 particiones SIN SYSTEM ni userdata; el perfil de EQ del VBC de Samsung es el archivo "vbc_eq"
  (request_firmware, magic "VBEQ") que Android carga de /system: NO existe en disco (solo el texto "VBEQ" dentro de KERNEL.img, constante del driver). No hay
  imagen de Android en ~ (las .zip grandes son de ~/Escritorio/mago, otro tema). Si se quisiera, habría que bajar el firmware oficial SM-T280 de Samsung y extraer
  /system/etc/.../vbc_eq. **Alternativa sin Android:** Batocera trae filtros de audio de RetroArch (/usr/share/audio_filters: iir.so, panning.so, EQ.dsp, BassBoost.dsp...).
  Prueba: /userdata/system/audiofilter/body.dsp = iir BBOOST +6 dB a 220 Hz + panning 0.7 (-3 dB), activado por `audio_dsp_plugin` en los .cfg por sistema (overlay.tar;
  solo juegos, no el menú). Instalación: push-overlay.sh v37d.
- **El usuario: "aún se escucha mal, muy saturado"** (antes de instalar el filtro de graves, que seguía sin instalar). RetroArch aplica 0 dB (el generador fuerza
  audio_volume = 0; el "2.0" de libretroRetroarchCustom.py se sobreescribe) -> no es ganancia de software de RA. Se RETIRA del overlay la línea audio_dsp_plugin (el filtro
  de graves queda disponible en /userdata/system/audiofilter pero sin activar) hasta entender la saturación. **v38:** el driver mide el nivel de lo que lee el DMA:
  cada 250 ms escanea el anillo (s16 L/R), cada 2 s registra "level: peak L=.. (..% of full scale) R=.. left samples >= 32000: N" si hubo sonido. Distingue
  recorte DIGITAL (software/dmix/plug) de distorsión ANALÓGICA (DAC/amplificador). out/boot-gtex-v38.img (build-43.log).
- **LA BATERÍA ESTABA A 0 % (3.1 V) — causa del "se apaga al iniciar Batocera" con la v38** (TWRP: battery capacity=0, voltage_now=3099000 µV, "Discharging" aunque
  sec-charger dice "Charging 500 mA": el USB del PC da 500 mA y Batocera gasta más). NO es un crash del kernel (dmesg.txt acaba a los 13.4 s y p25 pidió "recovery required":
  corte brusco). **HIPÓTESIS NUEVA PARA LA SATURACIÓN (sin comprobar):** el PA del altavoz se alimenta DIRECTO de VBAT (PA_LDO_EN apagado, PA_SW_EN) y el vendor ajusta
  los LDO del codec según la batería (sprd_codec_auto_ldo_volt con ldo_v_map: VCM_V/PA_LDO_V dependen de sprd_get_vbat_voltage()); aquí VCM_V=3 está fijo "para batería >= 3.7 V".
  Con ~3.1-3.5 V el audio podía recortar/distorsionar. TODAS las pruebas largas se hicieron con la tablet enchufada al PC (500 mA) mientras corría Batocera: probablemente
  descargándose. **Pendiente:** cargar con cargador de pared (>= 50-80 %) y repetir la prueba de audio; si mejora, implementar la selección de LDO según VBAT (ADC/FGU).
- **Intento de bajar el firmware Android del SM-T280 (el usuario lo autorizó):** instalado samloader 0.4.1 (nlscc, revisado: 329 líneas, solo habla con los servidores de
  Samsung) en tools/venv (código en tools/src/samloader). Hallazgos: (1) la consulta de versión de Samsung exige el User-Agent "Kies2.0_FUS" (python-requests da 403);
  (2) versiones para SM-T280: XAR y XAC = **T280XXS0ARK3** (la MISMA que el bootloader de la tablet), XSP = ARL1 (la más nueva), XEF/DBT/XEO/BTU/PHE = ARJ2;
  (3) la descarga FALLA: Samsung cambió la clave que protege el nonce de FUS (ni la KEY_1 "hqzdurufm2c8mf6bsjezu1qgveouv7c7" de nlscc ni la "vicopx7dqu06emacgpnpy8j8zwhduwlh" de martinetd
  descifran el NONCE real; salen 32 bytes de basura). Dos búsquedas web no dieron una clave nueva. No se va a deducir. **Camino alternativo:** que el usuario baje el AP_T280XXS0ARK3
  (región XAR) con Frija/SamFW/SamMobile y lo deje en ~/batocera-t280/android-fw/; entonces: tar -xf AP_*.tar.md5, lz4 -d system.img.lz4, simg2img, debugfs rdump -> buscar vbc_eq, audio_para.
- **FIRMWARE ANDROID OBTENIDO** (el usuario lo bajó de SamFW: ~/Descargas/SAMFW.COM_SM-T280_BNN_T280XXS0ARK3_fac.zip, 1.1 GB, copiado a android-fw/zip). Contiene AP_T280XXS0ARK3
  (boot, recovery, system.img [sparse 2 GB], SPRDCP, SPRDDSP, nvitem.bin 200 KB), BL y CSC_BNN. La suma md5 del AP NO coincide con la anotada en su cola (160858b3.. vs 8aa79cbc..): no se pudo
  verificar integridad; la imagen es un ext4 válido; solo se LEE (simg2img + debugfs rdump -> android-fw/sys/tree, 1.8 GB; nada se ejecuta). Copias útiles en info/android/ (audio_para, codec_pga.xml,
  audio_hw.xml, audio_policy.conf, audio_effects.conf, build.prop). android-fw/ y tools/ (samloader, roto: Samsung cambió la clave del nonce) se pueden borrar (~4 GB).
  **Qué hacía funcionar el audio en Android:** (1) /vendor/firmware/vbc_eq es un enlace a /data/local/media/vbc_eq: el HAL lo GENERA al arrancar desde /system/etc/audio_para (4576 líneas
  clave=valor: audio_arm\audio_arm\<Modo>\AudioStructure... y %audio\EQ_<Modo>\eq_mode_N\eq_band_M\{fo,q,gaindB,boostdB}); modos Headset/Headfree/Handset/Handsfree (audio_hw.xml). (2) Volúmenes
  analógicos (arm_volume, empaquetados según codec_pga.xml: altavoz = bits 0-3, auriculares 4-7, DAC 8, DG 16, interruptor DG bit 31): modo Handsfree [1]=0x801807ca y niveles [2..9]=0x0a0a: el
  altavoz va FIJO en AOL_G=10 (+6 dB) con DAC=7 (0 dB) y VBC DG 0x18 ENCENDIDO; el volumen real lo da el software (AudioFlinger). Es EXACTAMENTE lo que se encontró a oído (DAC 7 + boost 2).
  (3) EQ del altavoz (EQ_Handsfree, preset 1): UNA sola banda activa: fo=1500 Hz, q=1024 (=1.0), boostdB=65476 (16 bits con signo = -60 -> -6.0 dB): un corte de 6 dB en 1.5 kHz (la bocina resuena ahí);
  presets 2-6 son variantes (graves/agudos). EQ_Headfree preset 1: banda 100 Hz, -18 dB (paso alto), etc. (4) La nvitem.bin trae la calibración del módem de Spreadtrum.
  **Plan:** overlay con /userdata/system/audiofilter/speaker.dsp (iir PEQ 1500 Hz Q 1 -6 dB) activado por audio_dsp_plugin: SOLO juegos (RetroArch). Instalar con NOREBOOT (batería 0 %).

## v38e — `~/.asoundrc` faltaba en la tablet (25 oct)
- Hallazgo: `/userdata/system/.asoundrc` NO existía en la tablet (ni en otro sitio de /data) aunque está en overlay.tar; por eso el medidor
  mostraba L≠R con R alto en RetroArch (sin downmix). Algo lo borra tras instalar/arrancar, o el tar no lo extrajo. Aún sin causa confirmada.
- RetroArch usa el dispositivo ALSA "default" (FLOAT_LE, periodo 1024 x3); el log del Mega Drive no muestra línea de carga del DSP.
- Acción: copiado a mano a la tablet; `bootpart/asoundrc` + `boot-custom.sh` registra en touchpad.log si `.asoundrc` está presente cada 5 s
  durante 60 s y lo restaura desde /boot si falta. Empujado con push-bootpart.sh (logs previos en logs/v38e). Tablet reiniciada en Batocera.
- Siguiente: que el usuario juegue Mega Drive; bajar logs (pull-bootlogs.sh v38f) y mirar touchpad.log (presente/FALTA) y el medidor (R≈0 esperado).

## v38g — causa de que faltara `.asoundrc` (25 oct)
- **Causa raíz:** `/etc/init.d/S12populateshare` de Batocera ejecuta en CADA arranque `rm -f /userdata/system/.asoundrc` y borra `/boot/asoundrc`
  ("cleaning old alsa files - 08/07/2021"). Por eso el downmix nunca estuvo activo en el sistema real (solo en la prueba con aplay -c).
  (Obtenido leyendo el squashfs de la tablet, copia en el scratchpad; 4.3 MB/s por adb.)
- Arreglo: la config ALSA va en `/etc/asound.conf` (nadie más lo toca): `bootpart/asound.conf` = el original de Batocera + `pcm.mono_left`
  (route ttable 0.5/0.5 hacia canal 0 -> dmixer) como default; `boot-custom.sh` start lo copia a /etc al principio (S00). `.asoundrc` quitado del overlay.
- Pendiente: que el usuario pruebe Mega Drive; con el medidor esperamos R≈0 y menos tramos con peak=32767.

## v39 — salida por el aux (audífonos), 25 oct
- Usuario: tras v38g (asound.conf con downmix) "sigue igual" (áspero). Pide usar el aux por ahora.
- Estudiado el kernel de Samsung (src/kernel-stock-3.10/sound/soc/sprd/codec/sprd/v4/sprd-codec.c, drivers/input/misc/headset_sprd_sc2723.c):
  los audífonos usan un amplificador CLASE G propio (ANA_CDC3 = 0x628), HPL_G/HPR_G en ANA_CDC12 (0x64c), ganancias de clase G en CDC13/14 (G_1=0x10, G_2=7),
  rieles LDOCG (PMU0 bit10, PMU1 bits 1-3), HDT1 bit14, rampa anti-pop en CDC16 (CTL bits 7:6, STEP 5:3; flag en STS0 bits 8:7 == 3), DAC L->HPL (CDC7 bit7), DAC R->HPR (CDC7 bit2),
  HPL_EN/HPR_EN/DIFF_EN en CDC2 bits 8/7/6. Volúmenes Headset de Samsung: niveles 4..11 (0x404..0xb0b), DAC 7, VBC DG 0x18.
- Detección de enchufe: bandera HEAD_INSERT = ANA_STS0 (0x674) bit 10 (circuito HDT: GLB MODULE_EN bit5, CLK_EN bits 4/3, HDT0 sin power-down). Polaridad SIN CONFIRMAR.
- Driver (patch pendiente de exportar): controles nuevos "Output Select" (Speaker/Headphone/Both/Auto, defecto **Both**), "Headphone Playback Volume" (0-15, defecto 6),
  "Headphone CG Link" (HPL_CGL/HPR_CGR, apagado), "Jack Invert", "Headphone Jack" (solo lectura). level_work sondea la bandera cada 250 ms y la anota ("jack: ANA_STS0=.. insert flag a -> b").
  Log de arranque del amplificador: "HP pop up: flag N after X ms", "headphone amplifier on, gain 6", "codec on, output 2, jack flag N".
- bootpart/asound.conf vuelto al ORIGINAL de Batocera (estéreo, sin route) para que los audífonos reciban L y R; la bocina sigue con solo el DAC izquierdo (Speaker Mix=Left).
- Kernel out/boot-gtex-v39.img. install-all.sh v39pre armado esperando TWRP (logs previos -> logs/v39pre).
- Prueba: arrancar con audífonos puestos y sin ellos; con "Both" suena por bocina y aux a la vez. Mirar en dmesg las líneas "jack:" para fijar la polaridad.
- Pendiente de este tema: confirmar que suena; polaridad de la bandera -> Auto; calidad/volumen; la causa del sonido áspero de la bocina sigue sin resolver.

## CIERRE DE SESIÓN (25 oct) — dónde quedamos
- Kernel **v39** flasheado (09:27) y guardado en git local (tag v39-audio, parche patches/0017). bootpart con asound.conf ORIGINAL (estéreo) instalado por install-all.sh.
- Resultado de la prueba del usuario con v39: **"no suena nada"** (no aclaró si también calló la bocina ni si había audífonos puestos). Sin logs aún.
- PRIMER PASO de la próxima sesión: `adb devices`; con la tablet en TWRP correr `scripts/pull-bootlogs.sh v39` y leer logs/v39/dmesg.txt:
  líneas "HP pop up: flag N after X ms" (¿llega a 3?), "headphone amplifier on", "codec on, output 2, jack flag N", "jack: ANA_STS0=..", y "level:" (¿hay datos?).
  Si ni la bocina suena: regresión de v39 (revisar apply_outputs/apply_speaker_gain: set_gains(dac si spk||hp_up, aol si spk) y que want_spk() sea cierto con out=Both; el arranque suave usa want_spk()).
- Hipótesis si la bocina suena pero el aux no: voltaje del riel clase G (PMU3 bits 7:0, CG_LDO_V, el vendor no lo fija), switches HPL_CGL/HPR_CGR (control "Headphone CG Link"),
  DANGL/DANGR (solo chip AA), ganancia HPL_G, DIFF_EN, que HP_POP_FLG nunca llegue a 3. Sin shell en la tablet: probar variantes con un script en /boot/audiotest (corre al arrancar si existe /boot/audiotest/run).
- Si la bocina NO suena con v39: volver al kernel v38 (out/boot-gtex-v38.img) con scripts/autoflash.sh KERNEL out/boot-gtex-v38.img.
- Hallazgo importante de hoy: Batocera S12populateshare borra ~/.asoundrc y /boot/asoundrc en cada arranque; el downmix mono de v38g (en /etc/asound.conf) tampoco quitó la aspereza => la causa NO era la mezcla estéreo.
  Pendientes anteriores siguen: reducir ganancia digital de RetroArch (audio_volume -6 dB) + Speaker Boost 2; EQ; limpieza (prueba táctil de 10 s, trazas de debug).

## v40 — PipeWire manipulaba los controles del aux (25 oct)
- Logs v39: el amplificador del aux SÍ enciende (HP pop flag 3 en 60-90 ms), pero tras la 1.ª apertura quedó "output 1" (solo Headphone) y "gain 0".
  Causa: alsa-card-profile de PipeWire (usr/share/alsa-card-profile/mixer/paths) reconoce por nombre `Output Select` (opción "Headphone"),
  `Headphone` (en su ruta de bocina: volume=off -> mínimo) y `Headphone Jack`. Con AOL_G 0 la bocina seguía sonando (no es silencio).
- v40: controles renombrados a "Aux Output", "Aux Gain", "Aux CG Link", "Aux Jack Invert", "Aux Jack Flag"; apply_route corta DACx_AOL si la salida excluye la bocina,
  y "Speaker Playback Switch" también corta la ruta. Flasheado 11:04 (tag v40-audio, patches/0018).
- Detector: en v39 la bandera HEAD_INSERT (STS0 bit10) estuvo SIEMPRE en 0 (STS0=1200: bits 12 y 9). Falta saber si había audífonos puestos; bit 9 = HEAD_INSERT_2 del vendor.
- Bocina: sigue "distorsionada" (usuario). CDC11 (AOL_G) en las aperturas: 1 y luego 8 (igual en v38).

## v41 — tierra del jack, DANGL/DANGR, detector por EIC, gamepad en el menú (25 oct)
- Prueba de v40 (usuario): "no se escucha absolutamente nada" por el aux; Doom se juega bien pero la bocina suena saturada; el jack sigue sin detectarse.
  logs/v40: amplificador del aux encendido (HP pop flag 3, "output 2" = Both, gain 6); STS0 bit10 siempre 0. PMU0=af00 => **GND_PLGPD_EN (bit 4) APAGADO**.
- Hallazgos en el kernel de Samsung:
  (1) con CONFIG_SPRD_INTERNAL_HEADSET_SUPPORT=y, sprd_codec_analog_open() y el probe SIEMPRE ponen GND_PLGPD_EN (PMU0 bit4 = interruptor de tierra del jack)
      y HEAD_L_INT_PU_PD; el driver de headset (USE_HEADSET_WATERPROOF definido) además limpia GND_DET_PU_PD (HDT1 bit15). Aquí no se hacía => sospechoso nº 1 del silencio.
  (2) sprd_inter_headphone_pa(): DANGL/DANGR (0x680/0x684) = 0xffff si el chip es AA (sci_get_ana_chip_ver()==0; SC2723 "S" = 0x000 = AA). No se hacía.
  (3) El jack se detecta por el **EIC analógico canal 8** (gpio_detect=312 = base a_eic 304 + 8; irq_trigger_level_detect=1: alto = enchufado), no por STS0.
      EIC: ADI 0x100 DATA, 0x104 DMSK, 0x140+4n CTRL (DBNC_EN bit14, cuenta 25); requiere GLB MODULE_EN bit3 (EIC) y RTC_CLK_EN bit3.
  (4) Android: tiny_hw.xml "Inter PA Config"=304 (0x130): clase AB, sin LDO, DEMI on (PMU6=0x400 + CDC4 bit12), DTRI_F=3. "Inter HP PA Config"=7489 (0x1d41)
      = osc 1, mode 0, lpw 0, cgcal 1 (igual que lo nuestro salvo retardos).
- Driver v41 (tag v41-audio, patches/0019, out/boot-gtex-v41.img, build-46.log sin avisos): GND_PLGPD_EN + HDT1 en codec_power_on; DANGL/R con el
  amplificador si chip AA (log "PMIC chip id ..."); jack por EIC 8 con jack_work cada 300 ms (también sin audio), log "jack: EIC data .. STS0 insert bits .."
  y "jack: plugged in/out"; PA de la bocina como Android; volcado de registros al encender el aux; **sysfs `adi`** (/sys/bus/platform/devices/*.audio/adi):
  leer = volcado 0x600-0x6a4 + EIC + GLB; escribir "reg mask val" (hex) = adi_upd (queda en dmesg como "adi: ...").
- bootpart/audiotest/audio-test.sh (corre una vez, desde up=60 s; `run` armado): 4 pasos acumulativos, N pitidos por la BOCINA y luego stereo.wav SOLO por el aux
  (melodía izq., bajo der.): 1 = v41 tal cual, 2 = + Aux CG Link, 3 = + DIFF_EN off, 4 = + Aux Gain 12. Log en /boot/audiotest/audiotest.log.
- **Gamepad en el menú de ES:** tema `carbon-touch` en /userdata/themes (overlay.tar): theme.xml de carbon (copia del squashfs en overlay/es-theme/carbon-theme.xml)
  + include de touchpad.xml (vista "screen", imagen extra a pantalla completa = gamepad-landscape.png, color FFFFFFCC); el resto, enlaces simbólicos a
  /usr/share/emulationstation/themes/es-theme-carbon. push-overlay.sh pone ThemeSet=carbon-touch en es_settings.cfg (copia en es_settings.cfg.bak-overlay).
  Para volver: ThemeSet=es-theme-carbon (desde el menú de ES o restaurando la copia).
- **Resultados v41 (logs/v41a, v41b):** chip PMIC = 2723 **a0c1 => versión 0x0C1 = AUDIO_2723_VER_M** (no AA: DANGL/R no aplica; SÍ aplica el
  is_low_power_support() del vendor: biases de CDC16, POP_CHGR_PD/SOFTCHG_EN, CG_LPW=2, DAC -2.5 dB — pendiente de probar).
  **Detector del jack FUNCIONA:** EIC bit 8 alto = enchufado (y STS0 bit 9 a la vez); en v41b arrancó con "jack: plugged in". El bit 0 del EIC parpadea (otra línea).
  **El aux SUENA** (usuario: "suenan los primeros 3, más fuerte el 3"; el paso 3 = DIFF_EN off). **El oído derecho = "saturación, ruido"** con audífonos Harman planos.
  Escala HPL_G del vendor (hp_tlv): -36 dB + 3 dB/paso => 6 = -18 dB, 9 = -9, 12 = 0 dB.
- Prueba 2 (bootpart/audiotest, dual.wav = misma música en ambos canales, DIFF_EN off): 1 normal; 2 ambos oídos desde el DAC izq. (CDC7 DACL_N_HPR);
  3 ambos desde el DAC der. (CDC7 DACR_P_HPL); 4 normal con ganancia 12. Empujada con push-bootpart.sh v41c (13:15).
- **Prueba 2 (logs/v41d): NO SONÓ NADA** con dual.wav (mismo audio en L y R), aunque con stereo.wav (L y R distintos) sí sonaba => cada auricular recibe
  L − R: la TIERRA del jack (manga) flota y los dos auriculares quedan en serie entre HPL y HPR. Explica también v40 (Doom casi mono = silencio) y el
  "ruido" del lado derecho. GND_PLGPD_EN (PMU0 bit4) ya estaba puesto (PMU0=af10) y no basta. (Bug menor del script 2: hp_off no limpia DACL_N_HPR/DACR_P_HPL.)
- Prueba 3 (push-bootpart v41e, 13:2x): dual.wav en 5 pasos sobre ANA_HDT1 (=0x4300 de base): 1 control; 2 GND_DET_PU_PD=1; 3 HEAD_APP_G=2/APP_P=1;
  4 APP_G=0/APP_P=0; 5 APP_G=3/APP_P=3. El paso que SUENE es el que conecta la tierra. Otra causa posible: audífonos OMTP (tierra y micro cruzados).
- Prueba 3 se cortó en el paso 3 (el usuario forzó el reinicio para cambiar de audífonos). Audífonos: Harman con micrófono (4 contactos) y **KZ Castor
  (3 contactos, sin micro): tampoco suenan** => no es cruce CTIA/OMTP; la tierra suelta es del lado de la tablet.
- Prueba 4 (push-bootpart v41g): 8 pasos con dual.wav, base v41 antes de cada uno: 1 control; 2 PMU0 bit4 OFF; 3 HDT1 bit15; 4 APP_G=2/APP_P=1;
  5 APP 0/0; 6 CDC3 CG_HP_DIFF_EN (bit11); 7 CDC3 CG_CHP_MODE (bit4); 8 PMU0 HEADMICBIAS_EN (bit7).
- Prueba 4 (logs/v41h): ningún paso sonó (log hasta el paso 7). Los bits de tierra NO son: en TWRP el kernel de Samsung deja PMU0 bit4 y HDT1=0x4300 igual que v41.
- **HERRAMIENTA NUEVA: tools/tplay** (tplay.c + build.sh): C sin libc, estático ARM, syscalls directos; funciona en TWRP (kernel Samsung 3.10; el glibc de
  Batocera pide Linux >= 6.1). `tplay 2 list` / `set NOMBRE VALOR` / `play 0 archivo.wav`. Tarjeta del vendor en TWRP = card 2 "sprdphone", pcm 0 (periodo 160, buffer 2560).
  Volcado de registros del vendor: /proc/asound/sprdphone/sprd-codec (y vbc, aud-glb). Copias en logs/twrp-vendor/.
- **Registros del vendor reproduciendo por audífonos** (ruta de tiny_hw.xml: HPL/HPR vol 8, DAC 7, CG vol1 63, vol2 7, mezcladores DACLHPL/DACRHPR, HeadPhone Function):
  PMU0=0710 PMU1=fc8f CDC0=0900 CDC1=44e8 CDC2=d1c0 (DIFF_EN on) CDC3=f0c4 (LPW=2) CDC4=2000 CDC7=**0c84 (HPL_CGL/HPR_CGR ON)** CDC10=4800 (DAC 2)
  CDC12=0880 CDC13/14=**ff80 (CG G1=63)** CDC16=**ee1e** HDT0=033e HDT1=4300; digital TOP=0005 I2S=0780 DAC=03f2 SDM 0100, 0x34=0018, 9999/0001.
  Controles del vendor por defecto: HPLCGL/HPRCGR Switch=1. Las pruebas 2-4 con dual.wav tenían el enlace CG APAGADO.
- **Usuario: con el driver de Samsung en TWRP los audífonos suenan con dual.wav ("medianamente bien") y la BOCINA "suena super bien"** => la saturación
  de la bocina es de NUESTRA configuración, no del hardware. Registros del vendor con la bocina sonando (logs/twrp-vendor/play-spk.txt):
  PMU0=ab10 PMU1=fc81 PMU3=473c PMU5=e000 PMU6=0400 CDC2=d010 CDC3=00c4 CDC4=7000 CDC6=**3000 (DACL_AOL + DACR_AOL)** CDC10=**2400 (DAC -3 dB)**
  CDC11=**a000 (AOL_G 10, +6 dB)** CDC16=**ee1e** HDT0=033e 0x9c=4000; digital igual que en audífonos. PA idéntico al nuestro (PMU3/5/6, CDC4).
  Diferencias con v41: biases de CDC16 (bajo consumo, chip M) — ya en v42 —, DAC/AOL gains, mezcla de ambos DACs.
- **v42** (tag v42-audio, patches/0020, out/boot-gtex-v42.img, build-47.log): CG link ON por defecto, CG G1 63, CG_LPW 2 y CDC16 bajo consumo (chip E/T/M),
  salida Auto, ganancia aux 8. Flasheada 14:04 (install-all v41i). Pendiente: oír v42 (aux y bocina); si la bocina sigue mal, copiar DAC -3/AOL +6/ambos DACs
  y comparar el VBC con el del vendor (volcar /proc/asound/sprdphone/vbc reproduciendo).
- **Volcado completo del vendor reproduciendo (logs/vendor-dump/{speaker,headphone}.txt, tools/tplay/vdump.sh):** VBC idéntico en bocina y audífonos:
  VBBUFFSIZE 0x9f00 (FIFO 160), VBIISSEL 0x4000, DADGCTL 1818, **DAHPCTL 0x0d7f (EQ6_EN + ALC_EN + bits 0-8)**, DAALCCTL0-10 = perfil por defecto,
  STCTL 0183, STFIFOLVL 0x44, **HPCOEF 0x100-0x25c = 7 bandas planas (0x1000,0,0x4000,0,0x4000,0...)**. El nuestro: DAHPCTL 0, coeficientes 0, ALC off.
  Bocina vendor: CDC6 3000 (ambos DACs), CDC10 2400 (DAC -3 dB), CDC11 a000 (AOL 10). Audífonos: CDC10 4800, CDC12 0880.
- **v43** (tag v43-audio, patches/0021, out/boot-gtex-v43.img, build-50.log): tabla HPCOEF + DAHPCTL del vendor (control "VBC Vendor Profile"), ALC on,
  bocina con ambos DACs, DAC 1, boost 2; el sysfs adi vuelca también la parte digital del códec y el VBC. audiotest: dual.wav por hw:0,0 y por default.
- **v43: LA BOCINA SUENA "INCREÍBLE" (usuario, 25 oct).** Causa raíz de la saturación/ruido de TODAS las versiones: el VBC sin el perfil del vendor
  (DAHPCTL 0x0d7f con EQ6 en bandas planas HPCOEF + ALC). Pendiente de confirmar: audífonos con v43, gamepad en el menú de ES (tema carbon-touch),
  y quitar trazas de depuración (medidor level, audiotest, sysfs adi puede quedarse).
- **Método que lo resolvió:** en TWRP corre el kernel de Samsung con su tarjeta "sprdphone" (card 2); tools/tplay (sin libc) reproduce y
  /proc/asound/sprdphone/{sprd-codec,vbc,aud-glb} da los registros reales mientras suena => copiar al driver. Usar esto antes de adivinar bits.
- **v43, problema nuevo:** "dejó de sonar cuando entré a 2048". Gamepad dibujado en el menú de ES: **SÍ aparece** (tema carbon-touch funciona).
  logs/v43a/lastgame: el DMA siguió llevando el sonido del juego (level 25-32 %, 2144 periodos sin cortes) => el silencio es después del DMA
  (controles/PipeWire, o el ALC recién activado silenciando sonidos bajos). Primera apertura del arranque con CDC11=0x3000 (volumen 1 + boost 2).
- Monitor (push-bootpart v43b): audiotest anota cada 5 s, 15 min, los cambios de amixer y de registros (PMU0, CDC6, CDC10/11, VBC DAHPCTL), marcando [menu]/[game].

## v44 — botones (25 oct)
- Petición: que funcionen los botones de volumen y el de encendido (mantener presionado para apagar, no un toque).
- Hardware (kernel Samsung: sc_keypad.c + DTS): encendido = EIC analógico 2 (bajo = pulsado), vol− = EIC 10, vol+ = keypad 0x40250000 fila 0 col 0-1,
  home = fila 0 col 2 (keypad: AON EB0 bit8, RTC_EB bit1, RST0 bit8; KPD_CTRL rows 0x30000 cols 0x700, polaridad 0xffff, debounce 5).
- Kernel v44 (tag v44-keys, patches/0022, out/boot-gtex-v44.img, build-51.log): drivers/input/keyboard/sc8830-keys.c (sondeo 20 ms, antirrebote de 2 muestras,
  niveles de reposo medidos al arrancar), nodo keys@40250000 en sc7730.dtsi, CONFIG_KEYBOARD_SC8830 (y comprobado en build-kernel.sh). Incluye el audio de v43.
- Batocera: triggerhappy (S50) lee /userdata/system/configs/multimedia_keys.conf (overlay.tar): vol± -> bash /boot/volkey.sh (ganancia hardware: Speaker
  Playback Volume 0-8 o Aux Gain 0-15 si hay audífonos); power -> bash /boot/powerkey.sh 1/0 (mantener 2 s = shutdown -h now; toque = nada).
  OJO: sin esa config, el batocera-shutdown de fábrica apaga AL INSTANTE al pulsar.
- Orden al instalar: primero pull-bootlogs (monitor de 2048 en audiotest.log), luego install-all con v44 (monitor re-armado).

## v45/v46 — apagado real, brillo y reposo (25 oct)
- v44 probado: volumen ± funciona. Mantener encendido 2 s lanzaba el apagado ("power held 2 s" en touchpad.log) pero "no mata bien": `shutdown -h`
  solo detiene y el kernel no tenía manejador de apagado. EIC: la línea de encendido reposa en BAJO (EIC 0201: bit2=0), vol− = bit 10.
- **v45** (tag v45-poweroff, patches/0023): sc2723-regulator registra power-off (secuencia del vendor sc8830_power_off: LDO_PD_CTRL 0x1fff, PWR_WR_PROT
  0x950 = 0x6e7f hasta bit15, LDO_PD_CTRL 0xfff, LDO_DCDC_PD 0x7fff). powerkey.sh usa poweroff (+ poweroff -f a los 25 s si algo se cuelga). Flasheada 15:08.
- **v46** (tag v46-backlight, patches/0024, out/boot-gtex-v46.img, build-53.log): drivers/video/backlight/sc8830-dsi-bl.c: DCS 0x51 por la interfaz
  genérica del host DSI DesignWare 1.21a (0x21800000: GEN_HDR 0x6c, CMD_PKT_STATUS 0x74), 53 2C al arrancar, curva del vendor (0,10,20,143,255 ->
  0,3,11,95,202); /sys/class/backlight/panel. Reposo (toque corto): powerkey.sh pone brillo 0, silencia, SIGSTOP al emulador, touchpad.py ignora toques
  (/var/run/tablet-sleep); otro toque despierta. No es suspensión a RAM.
- **v46 CONFIRMADO por el usuario (25 oct):** brillo desde el menú de Batocera funciona; toque corto apaga la pantalla (reposo); mantener 2 s apaga limpio
  (poweroff + corte del PMIC, sin forzar). Volumen ± funciona (v44). Bocina limpia (v43). Gamepad dibujado en el menú de ES (tema carbon-touch).

## ESTADO ACTUAL (25 oct, fin de la jornada) — qué funciona y qué falta
- Kernel en la tablet: **v46** (out/boot-gtex-v46.img; tags v43-audio, v44-keys, v45-poweroff, v46-backlight; parches 0021-0024).
- Funciona: bocina limpia (perfil VBC del vendor), aux con detección de jack y cambio automático (v42-v43, aux con v43 sin confirmar del todo),
  botones de volumen, encendido (toque = reposo, 2 s = apagado limpio), brillo, gamepad táctil en juegos y en el menú.
- Copia de lo que NO está en git (bootpart, scripts, tools/tplay, overlay, initramfs, ESTADO): snapshots/2026-10-25-v46-userspace.tar.gz.
- Pendiente: confirmar audífonos con v43+ (limpios en ambos oídos); el silencio de 2048 no se reprodujo con el monitor; botón Home sin acción;
  reposo real (suspensión a RAM) no existe; limpieza de trazas (medidor "level", logs de periodos, audiotest, dmesg cada 30 s en boot-custom.sh).

## WiFi/BT — Marlin SC2331 (empezado 25 oct; el usuario autorizó usar el firmware del Android de SamFW)
- CORRECCIÓN: sí hay código fuente GPL en el kernel de Samsung: drivers/misc/sdiodev (transporte SDIO, 3.2k líneas), drivers/net/wireless/sprdwl
  (cfg80211, 8.9k), sc2331, mdbg; sprd_dwnload (/dev/power_ctl y /dev/download). Firmware en android-fw/sys/tree/etc/firmware: sc2331_fdl.bin,
  sc2331_fw.bin; connectivity_configure.ini, connectivity_calibration.ini, pskey_bt.txt; módulo lib/modules/sprdwl.ko; programa bin/download
  (protocolo de carga del firmware por /dev/download = canales SDIO 3 escribir / 12 leer; hay que reimplementarlo leyendo el binario).
- Hardware (DTS del vendor, nodo sprd-marlin): SDIO1 0x20400000 SPI 58 ("sdio_wifi", ocr 0x00360080, base 96 MHz); vddcon (DCDC pd 0x814 b13, trim 0xa18
  0x3ff, 0.6 V + n·3.125 mV; el vendor pide 1.6 V); vddwifipa (LDO pd 0x814 b11, trim 0x818 0xff); CLK_AUX0 32 kHz (sel 0x402e3020 b2:0 = 0, EB 0x402e0004 b2);
  reloj SDIO1 (sel 0x7120004c = 2 → 96 MHz, EB 0x20d00000 b9); reset GPIO 230 (pulso 1 ms); gpios marlin 148/45/46 (irq/wake). BT: UART + sprd_lpm + pskey.
- Plan: 1) chip responde en SDIO ("new SDIO card"); 2) sdiodev + carga de firmware; 3) sprdwl a cfg80211 6.18 -> wlan0; 4) wpa_supplicant + SSH; 5) BT.
- **v47 (fase 1)** (tag v47-wifi1, patches/0025, out/boot-gtex-v47.img, build-55.log): nodo sdio1 + mmc-pwrseq-simple (GPIO 230) + vddcon/vddwifipa
  always-on SIN cambiar voltaje (se registra el que dejó el bootloader) + sprd,init-regs para los relojes. Buscar en dmesg: "init reg", "vddcon",
  "vddwifipa", "mmc2"/"new SDIO card" o errores -110.
- **Resultado v47 (logs/v47a):** vddcon on 1.6 V y vddwifipa on 3.3 V (ya los dejaba así el bootloader); relojes aplicados; pines SD1/CLK_AUX0/TRACECLK(=GPIO230,
  AF3) correctos (leídos en TWRP con devmem: SD1 0x402a016c.. = 0x82001/0x43088, CLK_AUX0 0x402a0374 = 0x42001, TRACECLK 0x402a0020 = 0x42031).
  Pero "mmc2: Failed to initialize a non-removable card": el chip no contesta CMD5.
- **Con el kernel de Samsung en TWRP pasa lo mismo** (tools/tplay/marlinctl: ioctls 'z' 1/3 de /dev/power_ctl + abrir /dev/download -> rescan: nada).
  Motivo (cadenas de android-fw/sys/tree/bin/download): **la primera etapa va por UART /dev/ttyS0**: protocolo BootROM de Spreadtrum (HandShake 0x7E,
  versión, CMD_CONNECT, DATA_START/DATA/DATA_END, EXEC) carga sc2331_fdl.bin; después el FDL activa el SDIO y por /dev/download (canales SDIO) se carga
  sc2331_fw.bin + calibración (connectivity_calibration.ini, RF_PARA). Chips "marlin AA"/"BA" (imagen combinada, marlin.hardware.version).
- **EL CHIP RESPONDE (tools/tplay/marlinuart, TWRP, /dev/ttyS0 115200 8N1 crudo, tras power+reset, enviando 0x7E cada 10 ms):**
  `... 55 55 7e 00 81 00 06 53 50 52 44 33 00 57 0a 7e` = trama BSL_REP_VER (0x0081), largo 6, "SPRD3\0". Trama: 0x7E | tipo(2,BE) | largo(2,BE) |
  datos | CRC(2,BE) | 0x7E; **CRC-16/XMODEM** (poly 0x1021, init 0, sin reflejar) sobre tipo+largo+datos (verificado). FASE 1 CUMPLIDA (eléctricamente).
- Fase 2 (plan): UART0 en nuestro kernel (sprd_serial de Linux) + driver serdev "marlin" que encienda/resetee, haga el BSL por UART con request_firmware
  (sc2331_fdl.bin), EXEC, y luego dispare el rescan del SDIO; después portar sdiodev para cargar sc2331_fw.bin. El reset hoy lo posee mmc-pwrseq.

## v48 — batería (FGU del SC2723) (25 oct)
- Vendor: sprd_2713_fgu.c + sprd27x3_fuelgauge4samsung.c (FGU en ADI 0x500) y cargador SM5701 por I2C (no portado aún). Datos de TWRP (kernel Samsung):
  4048 mV, +429 mA (cargando por USB), soc 55 % interno / 46 % UI, cnom 3950 mAh. Crudos leídos por devmem vía ADI (escribir 0x40030024 = dirección,
  leer 0x40030028): VOLT_VAL 0x520 = 0x0AFA (2810), CURT_VAL 0x52c = 0x24A2, OCV 0x524, POCV 0x528 = 0xAA8, CLBCNT 0x568/0x56c,
  USER_AREA_STATUS 0x5a8 = 0x502E (tipo 5, UI soc 46), STATUS1 0x5c8 = 0xA03B (raw 59).
- Calibración: efuse A-die (TWRP /sys/class/misc/sprd_adie_efuse_otp/dump, copia logs/twrp-vendor/adie-efuse.txt): blk0 bit7=0 (hay calibración),
  9 bits desde el bloque 12 = 0x130 -> 4.2 V = 0x130 + 6963 - 4096 - 256 = 2915 cuentas -> 694 cuentas/V; corriente: (raw - 8192) * 1000 / (694*4*199/200 = 2762).
  Verificado: 2810 -> 4049 mV (vendor 4048), 0x24A2 -> 427 mA (vendor 429). Lectura del efuse: GLB MODULE_EN b7, BLOCK_INDEX 0x20c, MODE_CTRL 0x210 = RD_START b1,
  STATUS 0x214 b1 busy, DATA_RD 0x204 (invertido).
- **v48** (tag v48-battery, patches/0026, out/boot-gtex-v48.img, build-56.log): drivers/power/supply/sc2723-fgu.c -> /sys/class/power_supply/battery
  (status, voltage_now, current_now, capacity, charge_full_design). Capacidad = inicio (soc guardado en USER_AREA o OCV de encendido) + contador de coulombs;
  guarda el % cada minuto en USER_AREA (compartido con el kernel Samsung). simple-battery con la tabla OCV del vendor. Estado: cargando si I > 20 mA.
- bootpart/batwatch.sh (desde boot-custom.sh): registra en /boot/battery.log cada 10 min; poweroff limpio si Discharging, <= 3 % Y < 3600 mV.
- Pendiente: cargador SM5701 (límite de corriente, "online" del USB), temperatura de la batería (ADC), suspensión real.

## WiFi v49 — FDL1 por UART0 (en desarrollo, 25 oct)
- Continúa sobre v48 (sin tocar su parche ni su imagen): activa UART0/serdev en el SC7730, reproduce por DT el mux y gate de reloj del UART, y añade un driver `sprd,sc2331-marlin`.
- El driver hace handshake BSL, valida tramas HDLC con CRC-16/XMODEM, carga `sc2331_fdl.bin` con CONNECT/START_DATA/MIDST_DATA/END_DATA/EXEC_DATA, y luego habilita SDIO1 y pide un rescan al host SDHCI. La secuencia y los códigos BSL se contrastaron con una implementación pública del protocolo; el firmware corresponde al Android de SamFW ya autorizado.
- `sprd,fdl-load-address = <0x80000000>` se infiere de los punteros absolutos de la tabla vectorial de `sc2331_fdl.bin`; falta confirmar el arranque real del FDL en la tablet.
- **Compilado:** `out/boot-gtex-v49.img` (3,142,164 bytes), registro `build-57.log`. Está listo para cargar desde Download mode. No se flasheó ni se probó en la tablet: este entorno no tiene acceso al USB (`/dev/bus/usb`). La etapa aún no carga `sc2331_fw.bin` ni crea `wlan0`.
- **Siguiente:** compilar/arrancar y revisar UART0, la respuesta del FDL y la enumeración SDIO. Después portar el transporte SDIO del vendor para descargar `sc2331_fw.bin` y avanzar con `sprdwl`/cfg80211.

## v49 — red por USB para SSH (25 oct)
- v48 confirmado: el icono de batería se ve en ES; arrancó en 46 % (el guardado por Samsung), batwatch registra en /boot/battery.log.
- Vendor (drivers/usb/gadget/dwc_otg/usb_hw.c, usb_startup SCX35): vddusb 3.3 V (pd 0x814 b8, trim 0x83c), AON PWR_CTRL 0x402e0024 b0 = 0 (PHY on),
  AP AHB EB 0x20d00000 b4 (USB_EB), AP AHB RST 0x20d00004 b5-7 pulso 5 ms, tune 0x4407ae33 en AP APB USB_PHY_TUNE 0x71303000. Controlador DWC OTG 2.93a.
- **v49** (tag v49-usb, patches/0027, out/boot-gtex-v49.img, build-57.log): drivers/phy/phy-sc8830-usb.c (PHY genérico) + dwc2 de Linux en modo peripheral
  + g_ether (CDC ECM, sin RNDIS), MAC fijas en el cmdline (tablet 02:28:0a:2a:07:02, PC 02:28:0a:2a:07:01).
- bootpart/usbnet.sh (desde boot-custom.sh): usb0 = 10.42.7.2/24, reasegura cada 10 s, log /boot/usbnet.log. En la PC: 10.42.7.1/24 en la interfaz cdc_ether;
  `ssh root@10.42.7.2` (dropbear de Batocera, contraseña por defecto "linux").

## Separación del trabajo de Codex (25 oct, 19:30)
- Codex hizo un commit en ESTE repositorio (29ba5bcfc "misc: add SC2331 FDL1 loader over UART0", 18:50) entre v48 y v49, y además cambió
  scripts/build-kernel.sh. Por eso **v49 y v50 se compilaron con su driver incluido y activado** (sin saberlo); la v49 llegó a flashearse en la tablet.
- Arreglo (autorizado por el usuario, sin borrar nada):
  - rama **codex/wifi-fase2** = la historia anterior completa (v48 + su commit + mis v49/v50), tal cual; su worktree claude/v50-continue
    (~/Escritorio/batocera-t280-claude) no se tocó; las etiquetas v49-usb y v50-usb-fifo siguen apuntando a esa historia.
  - **master** = v48-battery + mis dos commits de USB reaplicados (26a38aeb1 PHY/dwc2/g_ether, 6c584aa93 FIFOs) -> etiqueta **v51-usb**. Sin su código.
  - scripts/build-kernel.sh: su versión guardada en scripts/build-kernel.sh.codex; la mía restaurada (vuelve PWRSEQ_SIMPLE, fuera SERIAL_*/FW_LOADER/SPRD_SC2331_MARLIN).
- **v51** (out/boot-gtex-v51.img, build-60.log): v48 + USB (PHY + dwc2 + g_ether + FIFOs del vendor). Pendiente de flashear y probar ssh root@10.42.7.2.
- **v51 FUNCIONA (25 oct 19:40):** la PC registra cdc_ether (interfaz enp0s20f0u2, MAC 02:28:0a:2a:07:01); perfil NetworkManager "tablet-usb" en la PC
  (10.42.7.1/24, never-default, ligado a esa MAC); ping 0.5 ms; **SSH sin contraseña**: el usuario copió ~/.ssh/id_ed25519.pub con ssh-copy-id
  (contraseña de Batocera "linux"). `ssh root@10.42.7.2` (dropbear 2022.83). Ya no hace falta TWRP para leer logs ni copiar archivos (scp/rsync).
- Pendiente del USB: tras desconectar/conectar el cable, la PC no re-enumera (-71, full-speed): falta detección de VBUS o reinicio del controlador
  (Samsung usa el EIC analógico 0 = VCHG y llama udc_enable en cada conexión). Probar de nuevo con v51 (la falla anterior podía venir del estado roto por los FIFO).
- **v52** (tag v52-usb-replug, patches/0029, out/boot-gtex-v52.img, build-61.log): dwc2 params para "sprd,sc8830-usb": power_down NONE, sin clock gating, sin LPM (la reconexión del cable fallaba con -71). boot-custom.sh vuelca dmesg a /boot cada 30 s durante 30 min.
- usbnet.sh: si la UDC no está "configured" 8 s -> unbind/bind de dwc2 (ciclo completo de PHY y core); tras 3 intentos, uno por minuto. Prueba de la reconexión del cable sin kernel nuevo.
- **Reconexión del cable RESUELTA (25 oct ~20:00):** causa (logs/v52a): tras quitar y poner el cable, dwc2 ve al host pero
  "dwc2_hsotg_enqueue_setup: failed queue (-11)": la petición de EP0 de la sesión anterior queda pegada porque sin sensado de VBUS el core nunca
  procesa la desconexión. usbnet.sh lo cura: UDC "not attached" -> a los 8 s unbind/bind de dwc2 -> "configured" ~5 s después (cable quitado a 75.9 s,
  reconectado y funcionando a 87.4 s). Pendiente opcional en el kernel: sensado de VBUS por el EIC analógico 0 para no depender del script.

## Sesión Codex — merge v52 y flasheo pendiente (25 oct, 20:xx)
- El usuario pidió trabajar con el repositorio principal `/home/nigga/batocera-t280`, no con la copia del Escritorio.
- En el `master` anidado `src/linux-6.18.58`, HEAD es `57a345d1c` (v52); están los commits USB `26a38aeb1`, `6c584aa93` y `57a345d1c`.
- La rama `codex/wifi-fase2` vive en el worktree `/home/nigga/Escritorio/batocera-t280-claude/src/linux-6.18.58`. Se integró `master` con el merge `bebab8177`; los tres commits originales son ancestros y el trabajo FDL1 `29ba5bcfc` se conservó. Conflicto en `arch/arm/boot/dts/unisoc/sc7730.dtsi` resuelto usando `compatible = "sprd,sc8830-usb", "snps,dwc2"`.
- La rama `codex/wifi-fase2` compiló correctamente con `scripts/build-kernel.sh` de ese worktree. Imagen: `Escritorio/batocera-t280-claude/out/boot-gtex-v52.img`, 3,199,508 bytes, SHA-256 `840a10debb3fc0d2af52f48baa3fda21c16d15a3ddefd9826f8bbae430132d4b`.
- Este repositorio principal mantiene su propia `out/boot-gtex-v52.img` (3,176,980 bytes), compilada desde `master`; su `.config` no tiene `CONFIG_SERIAL_SPRD` ni `CONFIG_SERIAL_DEV_BUS`, por lo que esa imagen no incluye el cargador Wi-Fi FDL1.
- El flujo de `scripts/install-all.sh` acepta una imagen de kernel: desde TWRP copia overlay y `/boot`, ejecuta `adb reboot download` y llama a `scripts/autoflash.sh KERNEL IMAGE` (Heimdall espera USB estable y flashea).
- Intento de flasheo tras el aviso del usuario de que la tablet estaba en Download: no se realizó. `heimdall detect` devolvió `unable to initialize libusb: -99`; ADB tampoco pudo acceder a `/dev/bus/usb` en este entorno.
- Pendiente: hacer visible el USB de la tablet al entorno y decidir cuál imagen flashear. Si se busca Wi-Fi FDL1, primero trasladar/construir esa rama desde el repositorio principal; el `master` de Batocera actualmente solo tiene los cambios USB v52.

## Menú rápido del botón Home (25 oct, noche) — FUNCIONA (confirmado por el usuario)
- Home (KEY_HOMEPAGE de sc8830-keys, verificado: 86 eventos limpios) abre un panel táctil sobre lo que esté en pantalla (ES o un juego), pausándolo.
- **Cómo dibuja sobre el dueño de la pantalla** (bootpart/touchpad/screen.py): quien corre en pantalla (ES / retroarch) es DRM master del único CRTC de
  simpledrm (800x1280 XRGB8888, crtc 35, conector 37). Como root se le "presta" su fd con pidfd_getfd (syscalls 434/438 ARM), se le manda SIGSTOP,
  se crean 2 dumb buffers + ADDFB2, y cada imagen nueva = PAGE_FLIP al buffer libre (simpledrm copia al panel en cada commit, no escanea continuo).
  Al cerrar: PAGE_FLIP al fb que tenía el dueño, RMFB/DESTROY_DUMB, SIGCONT. Seguro: un `sh -c "sleep 90; kill -CONT pids"` aparte (se renueva con
  cada toque, máx. cada 10 s), por si el menú muere con el juego pausado. Imagen: se dibuja apaisada 1280x800 y se gira con ROTATE_270 (igual que el overlay).
- bootpart/touchpad/menu.py: batería (%, estado, cable/corriente de entrada, mV, mA, autonomía), sistema (CPU %, RAM, uptime; temperatura y frecuencia
  muestran "—" hasta que existan /sys/class/thermal y cpufreq), barras táctiles de brillo (5-100 %, escribe /sys/class/backlight/panel y guarda con
  batocera-settings-set) y volumen (Speaker Playback Volume 0-8, o Aux Gain 0-15 con audífonos), botones Reposo / Cerrar juego / Reiniciar / Apagar / Volver
  (los 3 destructivos piden segundo toque en 4 s). Se cierra solo tras 60 s sin tocar. Errores del menú -> se cierra y lo anota en touchpad.log.
- touchpad.py: lee sc8830-keys con select (junto al táctil); Home alterna el menú; con el menú abierto los toques van al menú y se sueltan los botones del pad.
  powerkey.sh gana los modos `sleep` y `off` (los usa el menú). Copia previa de touchpad.py: scratchpad touchpad.py.before-menu (también en el snapshot de v46).
- Aprendizaje: `pgrep -f` con el patrón de lo que se lanza en el mismo comando SSH mata la propia sesión.
- Pendiente (sin driver): temperatura (SC7730 thermal / sensor de batería por ADC) y frecuencia/DVFS de la CPU; `reboot` desde el menú depende del
  manejador de reinicio de v55 (aún sin probar). Brillo: batocera.conf guardaba 0 (el reposo lo dejaba en 0) y Batocera lo sube a 5 %: ahora 56.
- Instalado EN VIVO por scp en /boot/touchpad (touchpad.py, menu.py, screen.py) y /boot/powerkey.sh; quedan en bootpart/ para el próximo push-bootpart.
- **Aviso al conectar a una PC** (bootpart/touchpad: menu.py `toast()`, screen.py `grab()`, touchpad.py `usb_poll()`/`usb_toast()`): cada segundo el vigilante lee
  /sys/class/udc/20200000.usb/state; "configured" = una PC la enumeró (un cargador de pared nunca lo activa). Si pasa de no-configurada (>= 3 s) a configurada y
  han pasado >= 45 s desde que arrancó touchpad.py, dibuja 2.5 s un letrero arriba ("Conectada a una PC / SSH: root@10.42.7.2") sobre la imagen que había
  (se captura con GETFB2 + MAP_DUMB del fb del dueño; GEM_CLOSE del handle nuevo) pausando ese instante al juego/ES. No sale con el menú abierto, en reposo ni
  al arrancar. `State.screen_lock` evita que letrero y menú usen la pantalla a la vez. SIN PROBAR en la tablet todavía (hay que instalarlo y re-conectar el cable).

## Prueba de N64 (Mario Kart 64 USA, 25 oct, noche) — el usuario salió; la tablet quedó conectada y la dejé como estaba
- ROM: Mario Kart 64.zip (Descargas) -> .z64 de 12 582 912 bytes, cabecera 80371240, "MARIOKART64"/NKTE, SHA-1 579c48e2…d5dd215e; copiada a /userdata/roms/n64/ y verificada.
- Batocera 38 de esta imagen NO trae ningún núcleo libretro de N64 (solo los .info). Único emulador: **mupen64plus independiente** (configgen-defaults: n64 -> mupen64plus/glide64mk2),
  plugins: glide64mk2 y gliden64. Por eso: la imagen sale GIRADA 90° (solo RetroArch aplica display.rotate/video_rotation) y SIN overlay del gamepad (los toques sí funcionan);
  y ocupa solo parte de la pantalla (la imagen sale en ~800x693 de la zona apaisada).
- **glide64mk2 FUNCIONA y se dibuja bien** en Lima (capturas en logs/n64-test/glide/: título y carrera de demostración nítidos); el log dice "your video card doesn't support
  GL_ARB_multitexture..." y "gl_FragDepth undeclared" (un shader que no enlaza en GLES2) pero el juego se ve correcto. **Velocidad medida** (tools/menu/shot.py fps = cambios de imagen
  por segundo en el CRTC): 17 fps en la pantalla de título/transición, **27 fps en la carrera** (MK64 va a 30 fps nativos), usando ~19-24 % de toda la CPU = ~1 núcleo de 4
  (hilo del emulador al límite). Es decir, NO es tan lento como parecía; el "lento" percibido puede ser la transición o el reinicio.
- **gliden64 NO arranca**: el juego se cierra solo (no "RUNNING" a los 25 s); requiere GLES3. Dejado como estaba (n64.core vuelto a borrar de batocera.conf; copia previa en
  /userdata/system/batocera.conf.bak-n64test).
- Herramientas nuevas: bootpart/touchpad/shot.py (captura del panel y contador de fps vía GETFB2/GETCRTC, sin pausar), tools/menu/n64test.sh (prueba automática por SSH).
- Pendiente del N64: la ROTACIÓN. Opciones: (a) rotación por software en el kernel (simpledrm con blit 90°, ~30-50 % de un núcleo a 60 Hz, afectaría a todo);
  (b) compilar un núcleo libretro de N64 (parallel_n64) -> toma overlay y rotación de RetroArch; (c) dejar el N64 para el final.
- **Frecuencia real de la CPU medida (tools/menu/cpuspeed.c, cadena de sumas dependientes = 1 ciclo c/u en Cortex-A7): ~1220 MHz** en los 3 intentos, es decir, ya corre al
  máximo del SC7730 (1.2 GHz nominal); el arranque la deja ahí y mi kernel no la toca. => un cpufreq propio NO daría más velocidad, solo ahorro de energía/temperatura.
  Sin medir aún: reloj de la GPU (hoy fijo a 384 MHz por nuestro DT gpu_clk; la Mali-400 del SC7730 llega a 512 MHz, sin probar su estabilidad).

## v56/v57 — reinicio arreglado, termómetro, /dev/mem (25 oct, noche)
- **v56 (tag v56-restart-reason, patches/0032): "Reiniciar" YA REINICIA.** Causa: sprd_set_reboot_mode() del vendor escribe 0x70 (SPECIAL) cuando el reinicio llega sin comando
  (el `reboot` normal) y 0x40 solo para comandos que no conoce; v55 escribía 0x40 y SBOOT no arrancaba. Ahora: tabla completa del vendor + contador del watchdog = 1 tick
  (50*1000/32768). Probado por SSH (`reboot`): se apagó y volvió sola en ~50 s (23:15 -> 23:16).
- **v57 (tag v57-thermal, patches/0033, out/boot-gtex-v57.img, build-66.log): drivers/thermal/sc8830-thermal.c** — sensores de CPU y GPU (bloque 0x402f0000; relojes/reset en AON APB
  EB1 b1, RTC_EB b10-14, RST1 b1; lectura cruda en +0x60 de cada sensor, +0x100 por sensor; T = -40 °C + (raw-623)*160/407; calibración del efuse = milésimas, se ignora).
  Zonas thermal_zone0 = cpu_thermal, thermal_zone1 = gpu_thermal. **Medido: CPU 48.5 °C, GPU 46.9 °C** en reposo, cargando. El menú Home ya las lee (usa el máximo de las zonas).
  Además CONFIG_DEVMEM (no estricto): /dev/mem para tocar relojes/voltajes en vivo (python mmap), necesario para el overclock.
- Solicitud del usuario: overclock CPU 1.5 GHz y GPU ~500 MHz. Datos del vendor (cpufreq-scx35.c, sc7730_cpufreq_table_data): 1300 MHz @ 1.06 V, 1200 @ 1.03 V, 1000 @ 0.96 V,
  768 (TDPLL) @ 0.90 V; max_freq de enfriamiento 1300/1200/1000/768; disparos térmicos 56/60/90/110 °C (crítico 119). La CPU hoy: ~1220 MHz (medida), el bootloader la deja ahí.
  1.5 GHz = +15 % sobre el máximo de Samsung: requiere subir vddarm (DCDC ARM del PMIC) y reprogramar el MPLL (GR_MPLL_MN; 4 MHz * N) pasando antes por TDPLL. Riesgo real.
- **Overclock GPU, intento 1 y 2 (25 oct, noche): la tablet se CONGELA (hay que forzar con Vol- + Encendido).** CPU ya está a **1300 MHz** (MPLL: CFG1 0x402e3070 = 0x432 -> NINT 50 * 26 MHz;
  TDPLL 0x402e3078 -> 29.54*26 = 768 MHz; GPLL 0x402e3084 -> 39.38*26 = 1024 MHz => 512 MHz = GPLL/2). Samsung soporta oficialmente 153.6/256/384/460.8/**512** MHz para la GPU
  (freq-lists del DT; de fábrica 256 MHz y sube con la carga). Secuencia de Samsung (mali.ko, gpu_dfs_func, desensamblado de twrp/rd/lib/modules/mali.ko): mali_dev_pause ->
  clk_disable+unprepare(clk_gpu) -> clk_set_parent -> (divisor en 0x60100004 b5:4 si cambia) -> clk_prepare+enable -> mali_dev_resume.
  Intento 1 (abrir compuerta 512 b7 de 0x402b011c y luego cambiar selector con ambos relojes): colgado. Intento 2 (secuencia de Samsung con ES pausado por SIGSTOP): colgado en el
  PASO 2 (cerrar la compuerta b5 del reloj de 384 MHz); log /boot/gpuclk.log. Hipótesis: lima tiene la IRQ de la GPU (SPI 39) y al parar el reloj lee registros sin reloj.
  Intento 3 (preparado): detener ES (S31emulationstation stop) y desenganchar lima (/sys/bus/platform/drivers/lima/unbind 60000000.gpu) durante el cambio. Herramientas: tools/menu/{mem,gpuclk}.py.

## Overclock de CPU — hallazgos (26 oct, 00:00; recuperado de la transcripción tras un cierre de sesión; NO hay código ni kernel v58 todavía)
- Petición del usuario: "mete solo unos cuantos mV, sube a 1.4 GHz" (luego aprobó: "hazlo"). Plan: driver en kernel `sc8830-cpuclk` (v58), no `/dev/mem` a ciegas.
- Estado actual medido: CPU fija a **1300 MHz** (MPLL_CFG1 0x402e3070 = 0x432 -> NINT 50 x 26 MHz; MPLL_CFG 0x402e3000 = 0x031000fa; MPLL_REL 0x402b00c8 = 0x70007;
  CA7_CKG_CFG 0x20d03000 = 0x60206: mux de clk_mcu en bits 2:0 (=6 -> MPLL), divisor AXI bits 10:8 (=2), DBG bits 18:16). El kernel no tiene cpufreq.
- Tabla del vendor (cpufreq-scx35.c): 1300 MHz @ 1060 mV, 1200 @ 1030, 1000 @ 960, 768 (TDPLL) @ 900. max_axi_freq = 500000 kHz (el divisor AXI = ceil(f_cpu / 500 MHz)).
- Secuencia de Samsung para cambiar de frecuencia: subir voltaje -> pasar clk_mcu a TDPLL (768) -> reprogramar MPLL (N) -> volver a MPLL -> ajustar divisor AXI; al bajar, al revés.
  set_mcu_clk_freq() del vendor además toca GR_GEN1 bit 9 y REG_AHB_ARM_CLK (solo SC8830 viejo; no aplica seguro al 7730).
- Regulador vddarm (include/soc/sprd/__regulator_map.h): DCDCARM_CTRL0 (ADI 0x8ac, bits 2:0 = índice de tabla {1100,700,800,900,1000,650,1200,1300} mV) y
  DCDCARM_CTRL_CAL (ADI 0x8b8, bits 4:0 = ajuste fino, ~3.1 mV/paso = 100 mV/32). Los bits complementarios (~x en bits 6:4 / 12:8) los exige el vendor al escribir.
- LECTURA DEL PMIC por /dev/mem NO sirvió: el ADI (0x40030000) usa RD_CMD en +0x24 y RD_DATA en +0x28 (como adi_read() de drivers/power/supply/sc2723-fgu.c; fgu mapea
  "adi" 0x40030000 y "pmic" 0x40038000 y exige addr_mask); mis lecturas devolvían 0 o 0x411 (basura: el kernel ya usa el ADI). => el voltaje actual de vddarm sigue SIN LEER.
  Hacerlo dentro de un driver (mismo patrón que sc2723-fgu: readl_poll_timeout en RD_DATA, comparar la dirección devuelta).
- Para v58 (pendiente): driver tipo sc8830-thermal (Kconfig/Makefile/DT/gtexswifi.config), leer vddarm, +~50 mV, MPLL N=54 (54*26 = 1404 MHz) vía TDPLL, divisor AXI, medir con
  tools/menu/cpuspeed, vigilar thermal_zone0 (hoy ~50 °C; disparos del vendor 56/60/90/110 °C). Último commit del kernel: d0871a3cb (v57).

## v58 — driver sc8830-cpuclk escrito y compilado (26 oct, 00:30), SIN flashear
- tag v58-cpuclk, patches/0034, out/boot-gtex-v58.img, build-67.log. drivers/thermal/sc8830-cpuclk.c (+ Kconfig SC8830_CPUCLK, DT cpuclk@20d03000, gtexswifi.config, build-kernel.sh).
- NO cambia nada al arrancar: solo lee vddarm por ADI (como sc2723-fgu) y lo imprime en dmesg ("vddarm ... uV (coarse c trim t), CPU n MHz").
- sysfs en /sys/devices/platform/soc*/20d03000.cpuclk/ (buscar con find): vddarm_uv (r), boost_mv (rw, 0..100 mV sobre el voltaje de arranque; tope 1200 mV),
  freq_mhz (rw, múltiplos de 26 entre 754 y 1404; >1300 exige boost_mv >= 20). Subir: boost_mv primero, luego freq_mhz. Bajar: freq_mhz 1300 y luego boost_mv 0.
- Orden del voltaje: nunca baja del menor entre el viejo y el nuevo (sube el escalón grueso antes del fino, y al revés al bajar). Cambio de PLL: mux a TDPLL, NINT, 500 us, mux a MPLL, con IRQ locales apagadas.
- Plan de prueba: flashear v58, leer dmesg/vddarm_uv, boost_mv=25, freq_mhz=1352 (52x26), cpuspeed + thermal, luego 1404.
- (26 oct, 00:20) El usuario confirmó que "Reiniciar" (menú Home y `reboot`) ya funciona tras v56; el cuelgue anterior al reiniciar desde el menú no se reprodujo. Se intenta `reboot download` por SSH para flashear v58.

## v58 flasheada y probada: CPU a 1404 MHz ESTABLE (26 oct, 01:05)
- Flasheada por Heimdall (el usuario puso la tablet en Download a mano). Arranque: "vddarm 825000 uV (coarse 2 trim 8), CPU 1300 MHz" — el bootloader deja 825 mV (la tabla de Samsung dice 1060 mV para 1300: el registro leído quizá no es el voltaje real).
- Ruta sysfs: /sys/devices/platform/soc/40030000.cpuclk/{freq_mhz,boost_mv,vddarm_uv}.
- boost_mv=25 -> 850 mV (coarse 2 trim 16). freq_mhz=1352: cpuspeed 1264 MHz; freq_mhz=1404: **cpuspeed 1322 MHz** (antes 1220 a 1300 = +8 %). Sin cuelgues.
- Prueba 4 núcleos al 100 % por 30 s a 1404: CPU 62.6 -> 65.0 °C, GPU 61 °C, estable, sin errores en dmesg.
- NO es persistente: tras reiniciar vuelve a 1300 MHz / 825 mV. Pendiente: aplicarlo al arrancar (script de inicio o parámetro del driver) y probar con un juego (N64) durante más tiempo; disparos térmicos del vendor 56/60/90 °C (a 65 °C ya pasa el de 60).
- **1482 MHz (NINT 57) ESTABLE** (26 oct, 01:10): probado con /dev/mem (tools: scratchpad oc1500.py = misma secuencia del driver) porque el driver v58 topa en 1404. boost_mv=50 (875 mV: coarse 2 trim 24),
  divisor AXI subido a /4 (CKG bits 10:8 = 3; con /3 daría 494 MHz, justo en el límite de 500). cpuspeed 1391 MHz (vs 1220 de fábrica = +14 %). 4 núcleos 30 s: CPU 61-62.6 °C, GPU 58 °C, sin fallos.
  1500 exacto no existe con el PLL en modo entero (26 MHz x N): 57 -> 1482, 58 -> 1508. Para dejarlo en el driver: MAX_MHZ 58*26 y fijar el divisor AXI según la frecuencia (v59).

## v59 + control en el menú Home (26 oct, 01:30)
- **Kernel v59** (tag v59-cpuclk-auto, patches/0035, out/boot-gtex-v59.img, build-68.log): `freq_mhz` ahora sube/baja solo el voltaje y el divisor AXI: <=1300 sin extra; <=1404 +25 mV y AXI /3; >1404 +50 mV y AXI /4. Tope 1482 MHz.
  Orden al subir: voltaje, AXI, reloj; al bajar: reloj, AXI, voltaje.
- **Menú Home** (bootpart/touchpad/menu.py): en la tarjeta Sistema, "Velocidad CPU N MHz" con deslizador de 8 pasos (1300 ... 1482, de 26 en 26). Se aplica al soltar el dedo y se guarda en /boot/cpu_mhz.
  La hora encendida pasó a la esquina de la tarjeta. **boot-custom.sh** aplica /boot/cpu_mhz al arrancar (1482 si no existe).
- Archivos ya copiados a la tablet por scp (menu.py, boot-custom.sh, /boot/cpu_mhz=1482). Falta flashear v59 (Download); con el kernel v58 el deslizador solo funciona hasta 1404 sin boost manual.
- v59 flasheada (01:25) y verificada: al arrancar boot-custom.sh pone 1482 MHz (+50 mV) a los 14 s; el deslizador del menú funciona (el usuario lo movió a 1326 -> +25 mV, guardado en /boot/cpu_mhz). Con 1482 y carga de 4 núcleos 20 s: estable. Cierre del día.

## Gamepad: revisión de botones y cruceta (26 oct, 01:50)
- Mapeo OK: abajo BTN_SOUTH (overlay "A") = ES "b" aceptar = Enter; derecha BTN_EAST ("B") = ES "a" volver = Esc; izquierda BTN_NORTH ("X") = ES "y" = `-`; arriba BTN_WEST ("Y") = ES "x" = `=`.
  ES y RetroArch nombran estilo Nintendo (A derecha, B abajo) y cada consola reasigna (Mega Drive/picodrive: A=izquierda, B=abajo, C=derecha). Por eso un juego puede pedir "A" y ser el botón rotulado "X".
  Propuesta pendiente: overlay con letras por sistema (make-overlay.py ya genera un .cfg por sistema) o letras estilo Nintendo fijas.
- Cruceta "salta dos": hipótesis = en el menú de ES cada toque llegaba por el pad (hat) Y por el teclado virtual (flechas); es_input.cfg ya tiene el Xbox 360 pad (guid ...14010000), el espejo de teclado sobra.
  Prueba: touchpad.py ignora el teclado si existe /boot/kbd_off (creado). Si empeora/no navega: rm /boot/kbd_off y reiniciar touchpad.py. OJO: `pkill -f touchpad/touchpad.py` por ssh mata la propia sesión (la línea de comandos coincide).
  Si sigue saltando: probar histéresis en los sectores de 45° de zone_state() (un dedo en el borde alterna dirección/diagonal) o repetición de ES (toque > 500 ms).
- (26 oct, 02:00) El usuario confirmó ("perfecto") que con /boot/kbd_off la cruceta ya no salta. Queda como solución definitiva; bootpart/touchpad/touchpad.py del repo ya lo incluye.
- **Cierre del 26 oct:** kernel v59 (tag v59-cpuclk-auto, parche 0035, out/boot-gtex-v59.img) flasheado; CPU 1482 MHz por defecto, deslizador en el menú Home, /boot/cpu_mhz; teclado virtual apagado en ES. Pendiente: letras de overlay por sistema (A/B/C en Mega Drive), rotación del N64, probar un juego largo a 1482 MHz.

## Silencio intermitente de la bocina: causa encontrada (26 oct, 03:20) — SIN ARREGLAR en código
- Síntoma: tras reiniciar, DMA con señal (picos 80-100 %) pero cero sonido. dmesg de esa boot: `ADI read 0x600 failed` a los 19.9 s, y justo después `codec on ... PMU0=2000`
  (bueno = PMU0=ab10: VB, VBO, OVP, PA_SW, GND, PA_EN...). Es decir: adi_upd() de sound/arm/sc8830-audio.c, cuando la LECTURA falla (timeout o dirección distinta), usa old=0 y ESCRIBE igual
  => borra los bits de PMU0 (solo queda PA_EN, 0x2000) => amplificador sin alimentación. También salieron fallos de lectura en 0x644, 0x648, 0x624, 0x668 (496 s, 512 s).
- Causa de fondo probable: el bus ADI (RD_CMD 0x24 / RD_DATA 0x28) lo usan SIN lock común audio (adi_lock propio), sc2723-fgu (cada ~0.5 s, desde v48), sc2723-regulator, sc8830-keys (sondeo cada 20 ms)
  y sc8830-cpuclk: los comandos de lectura se pisan. Coincide con que el audio estaba bien en v46 (antes del fgu v48) y falla ahora. Cuando se atasca, todas las lecturas devuelven ab10.
- Arreglo propuesto (v60): (1) en adi_upd() del audio: si falla la lectura, reintentar y NO escribir; (2) un spinlock compartido para todos los accesos de lectura ADI (audio, fgu, regulator, keys, cpuclk).
- El usuario reportó "funcionó, escuché" después de varias reproducciones de prueba (la reproducción volvió a encender el códec con lecturas buenas): confirma que es aleatorio.

## v60 — arreglo del silencio intermitente + freno térmico (26 oct, 03:50), compilado, SIN flashear
- tag v60-adi-lock, patches/0036, out/boot-gtex-v60.img, build-69.log.
- **Lock único del bus ADI:** include/linux/sc8830-adi.h + drivers/misc/sc8830-adi-lock.c (`sc8830_adi_lock`, raw spinlock). Lo toman cada lectura y cada leer-modificar-escribir de sc8830-audio,
  sc2723-fgu, sc2723-regulator, sc8830-keys (antes SIN lock, sondeo cada 20 ms) y sc8830-cpuclk. Antes cada driver bloqueaba solo lo suyo y compartían RD_CMD/RD_DATA.
- **Audio:** adi_upd() reintenta la lectura 5 veces y, si no puede, NO escribe ("register left alone") — antes escribía old=0 y borraba PMU0 (=> bocina muda). Probable causa también de "FGU_EN se borró" del fgu.
- **cpuclk:** freno térmico: cpu_thermal >= 80 °C -> 1300 MHz (aviso en dmesg); <= 68 °C -> vuelve a lo pedido (want_mhz). freq_mhz guarda lo pedido aunque esté frenado.
- Para verificar tras flashear: dmesg sin "ADI read ... failed" y con PMU0=ab10 en "codec on"; reiniciar varias veces (el fallo era aleatorio) y probar sonido cada vez.
- (26 oct, 03:40) v60 flasheada. Primer arranque: 4 aperturas del códec con PMU0=ab10, 0 fallos "ADI read"; CPU a 1482 MHz otra vez (el /boot/cpu_mhz tenía 1300; alguien lo cambió desde el menú). Falta confirmar con varios reinicios que el sonido no vuelve a fallar.

## Recorte de la bocina (26 oct, 04:10)
- Ya existía el EQ de Samsung (audio_para, EQ_Handsfree preset 1: banda fo=1500 Hz, q=1.0, -6 dB) como /userdata/system/audiofilter/speaker.dsp (iir PEQ) vía audio_dsp_plugin de RetroArch: SOLO juegos libretro.
  El usuario pidió la muesca en 1.2 kHz (la bocina vibra): cambiado a 1200 Hz, Q 1, -6 dB (overlay/audiofilter/speaker.dsp, make-overlay.py y copiado a la tablet; se aplica al abrir el próximo juego).
- NO cubre: el menú de ES, mupen64plus y demás emuladores independientes (audio directo por ALSA dmix: /boot/asound.conf, sin plugins LADSPA en la imagen).
- Para cubrir todo el audio: EQ6 del VBC en el kernel (HPCOEF, hoy plano; el HAL de Android lo genera en /data/local/media/vbc_eq desde audio_para y ese archivo NO está en las copias; hay que sacar
  el formato de los coeficientes del HAL de Samsung, android-fw/sys/tree/lib*/audio*.so) o compilar un plugin ALSA/LADSPA para ARM.
- (26 oct, 04:20) Corrección del usuario: el valor correcto era 1.5 kHz, no 1.2 (se había dicho mal). Revertido a 1500 Hz, Q 1, -6 dB en overlay/audiofilter/speaker.dsp, make-overlay.py y la tablet. Es el EQ de Samsung tal cual; sigue siendo solo para juegos de RetroArch.
- Repo del proyecto: git local en ~/batocera-t280 (rama main, 1 commit; .gitignore excluye src/, android-fw/, backup/, twrp/, info/, imágenes y logs). SIN subir: falta destino en GitHub y acceso SSH (la llave id_ed25519 da 'Permission denied (publickey)' en github.com).
- (26 oct, 05:10) SUBIDO a github.com/LeShite/linux-sc7730-samsung: rama master del kernel + tags v11..v60, y el proyecto (scripts, bootpart, overlay, parches, ESTADO.md) en la rama sm-t280-project (desde 'main' local del repo ~/batocera-t280). Para actualizar: git push origin main:sm-t280-project (proyecto) y git push (kernel, en src/linux-6.18.58).

## v61 — Wi-Fi fase 2: cargador FDL1 por UART0 en master (26 oct, 05:40), compilado, SIN flashear
- Rama wifi-v61 del kernel = master (v60) + cherry-pick del commit de Codex 29ba5bcfc (drivers/misc/sprd-sc2331-marlin.c, UART0 + serdev en DT, sdhci con "no-sdio" hasta que el FDL arranque).
  Conflicto único en drivers/misc/Makefile (se conservan ambas líneas). tag v61-marlin-fdl, patches/0037, out/boot-gtex-v61.img, build-70.log; build-kernel.sh exige SERIAL_SPRD, SERIAL_DEV_BUS, FW_LOADER, SPRD_SC2331_MARLIN.
- sc2331_fdl.bin (7288 bytes) va EMBEBIDO en el kernel (CONFIG_EXTRA_FIRMWARE desde android-fw/sys/tree/etc/firmware): no hace falta rootfs.
- Qué buscar tras flashear (dmesg): "UART0 configured for 115200 baud", "BootROM version response ... 'SPRD3'", "uploaded and executed FDL1 (7288 bytes at 80000000)", "SDIO1 rescan requested", y luego "mmc2: new ... SDIO card".
  Si no hay respuesta BootROM: revisar reloj/mux de UART0 (init-regs 0x71200064 y 0x71300000 b13) y polaridad del reset (GPIO 230). Si el FDL arranca pero no hay tarjeta SDIO: la dirección de carga 0x80000000 está inferida, no confirmada.
- Siguiente (fase 3): portar el transporte SDIO del vendor (drivers/misc/sdiodev) para descargar sc2331_fw.bin + calibración, luego sprdwl (cfg80211) -> wlan0. Es la parte grande (~12k líneas del 3.10).

## v61 probada y v62 (26 oct, 19:50)
- **v61 en la tablet:** UART0 OK (ttyS0, 115200); el BootROM contesta "535052443300" (SPRD3) a 0.97 s; **FDL1 subido y ejecutado (7288 bytes en 0x80000000)** — la dirección inferida sirvió (al menos el EXEC fue aceptado).
- Problemas: (1) todo el proceso tardó **37 s dentro del probe** y retrasó el arranque (init arrancó a 38.6 s); sin medir qué tramo; (2) tras "SDIO1 rescan requested" NO apareció ninguna tarjeta SDIO ni error:
  el núcleo MMC escanea solo UNA vez los hosts non-removable (host->rescan_entered), mmc_detect_change no hace nada.
- **v62** (tag v62-marlin-async, patches/0038, out/boot-gtex-v62.img, build-71.log): rescan_entered = 0 antes de mmc_detect_change; la carga del FDL pasa a un work (no frena el arranque);
  trazas de tiempo (sync / connect / primer bloque). Qué mirar: "sync N ms", "connect acked", "first chunk acked", y si aparece "mmc2: new ... SDIO card" tras el rescan.
- Si el SDIO no contesta ni así: el FDL1 podría necesitar tiempo (esperar ~1-2 s antes del rescan), o activar SDIO con un comando propio del FDL; revisar bin/download de Samsung (android-fw/sys/tree/bin/download).
- **v62 PROBADA (26 oct, 19:45): ¡EL CHIP WI-FI APARECE EN SDIO!** `mmc2: new high speed SDIO card at address 8800`, SDIO_ID=0000:2331 (función 1, clase 0), tipo SDIO. El arranque ya no se retrasa (Run /init a 2.4 s).
  Tiempos: sync 540 ms, connect 559 ms, primer bloque 640 ms; el FDL1 completo termina a los ~38 s (de los 14 bloques restantes + EXEC; sin medir cuál tramo; la tarjeta SDIO sale 170 ms después del rescan).
  Pendiente de optimizar: esos ~37 s (la tarjeta tarda 38 s en estar lista tras cada arranque).
- **Siguiente (fase 3):** con la tarjeta ya enumerada, portar el transporte SDIO del vendor (drivers/misc/sdiodev: canales 3 escribir/12 leer, carga de sc2331_fw.bin y connectivity_calibration.ini por /dev/download) y luego sprdwl (cfg80211) -> wlan0, wpa_supplicant. Fuentes: src/kernel-stock-3.10/drivers/misc/sdiodev y drivers/net/wireless/sprdwl.

## Fase 3 (Wi-Fi) — análisis del código de Samsung (26 oct, 20:20). Código escrito: NINGUNO todavía
- **El driver de Wi-Fi del SC2331 NO es sprdwl** (ese es de otro chip): es `drivers/net/wireless/sc2331/` (7.4k líneas: wlan_core.c, wlan_cfg80211.c, wlan_cmd.c, wlan_msg_q/event_q/fifo, wlan_npi, wlan_wapi) + `drivers/misc/sdiodev` (transporte, 1.7k) + `drivers/misc/mdbg` (canal de depuración) + `drivers/misc/sprd_dwnload` (/dev/download, /dev/power_ctl).
- **Transporte SDIO (sdio_dev.c), lo que hay que reescribir para 6.18:** driver sdio_driver con id (vendor 0x0000, device 0x2331). Estado de canales: byte de la FUNCIÓN 0 en 0x840/0x841 (bit n = canal n activo);
  bytes pendientes del canal n: función 0, 0x800+4n .. +2 (3 bytes, 0xff = error). Lectura/escritura de datos: sdio_memcpy_fromio/toio sobre la FUNCIÓN 1 con la dirección = nº de canal (FIFO fija).
  Aviso de datos: GPIO data_irq -> workqueue lee 0x841 y llama al callback del canal (8/9 Wi-Fi, 11 ATC, 12 descarga, 13 assert, 14 log, 10 sync). GPIOs de la tarjeta: wake_ack, data_irq, wake_out, io_ready, rfctl_off (DTS del vendor, nodo sprd-marlin; notas previas: 148/45/46).
  Además hay sleep policy (set_marlin_wakeup/sleep) y una sincronización inicial marlin_sdio_sync_init (io_ready) que se puede simplificar al principio.
- **Descarga del firmware (sprd_download.c):** escribir a /dev/download = sdio_dev_write(canal 3, datos) (canal 0 durante la calibración, entre las órdenes "start_calibration"/"end_calibration"); respuestas por el canal 12.
  El programa `download` de Android (bin/download, 35 KB Thumb, desensamblado en el scratchpad) hace UART+FDL1 (ya resuelto) y luego /dev/download: lee la cabecera (IMG_HEAD_MAGIC) y elige imagen AA o BA según marlin.hardware.version; envía con las mismas rutinas send_end_message/send_exec_message.
- **sc2331_fw.bin** (661564 B): ARM crudo sin cabecera visible (tabla de vectores en 0, punteros 0x0005xxxx); calibración: connectivity_calibration.ini -> nvbuf (magic) por start_calibration. FALTA: dirección de carga, formato exacto de las tramas por SDIO (¿mismo BSL HDLC?) -> hay que terminar de leer `download` o capturarlo en TWRP (kernel Samsung, printk "sprd_download_sdio_write size") o usar el programa original allí.
- **Plan por pasos:** (A) driver `sprd-marlin-sdio` mínimo en 6.18: función SDIO, lectura de 0x840/0x841/longitudes, canal 12 por IRQ GPIO, escritura canal 3; (B) reproducir la descarga de sc2331_fw.bin y ver respuesta; (C) portar sc2331/ wlan_* a cfg80211 de 6.18 (netdev, wiphy, scan/connect, cfg80211_ops nuevas); (D) wpa_supplicant en Batocera; (E) BT (aparte).
- Riesgos: sin la descarga del firmware (B) no hay wlan; (C) es lo más grande (~7k líneas de API 3.10 a 6.18).

## v63 — transporte SDIO del Marlin (fase 3, pasos 1-2 en curso) (26 oct, 20:50), compilado, SIN flashear
- tag v63-marlin-sdio, patches/0039, out/boot-gtex-v63.img, build-72.log. drivers/misc/sprd-marlin-sdio.c (sdio_driver 0000:2331): habilita función 1, bloque 512, estado de canales (func0 0x840/0x841, longitudes 0x800+4n),
  IRQ de datos GPIO 45 (DT: data-irq-gpios en el nodo marlin; wake ack 148, wake out 46, io_ready 84 aún sin usar), /dev/marlin: write = [canal][datos]; read = [canal][datos] del primer canal 8-15 con datos (5 s); sysfs status (…/virtual/misc/marlin/status).
- Gpios del vendor (sprd-marlin en el DTS gtexswifi): 0=148 wake_ack, 1=45 data_irq, 2=46 wake_out, 3=84 io_ready, 4=230 reset.
- Experimento previsto (sin reflashear, por ssh): ver status tras el arranque; intentar leer canal 12 (¿manda algo el FDL1 solo?); probar la trama BSL (CONNECT/START_DATA/...) por el canal 3 con sc2331_fw.bin y leer la respuesta del 12. Dirección de carga a deducir (vectores en 0).

## v63 PROBADA — ¡FIRMWARE sc2331_fw.bin CARGADO POR SDIO! (26 oct, 21:40) — Wi-Fi fase 3, pasos 1 y 2
- v63 flasheada: `sprd-marlin-sdio mmc2:8800:1: Marlin SDIO function 1 enabled, /dev/marlin ready, irq 36`. /sys/devices/virtual/misc/marlin/status lista canales (canal 3 = ventana de escritura, 64 bytes al arrancar).
- **Protocolo de descarga por SDIO (sacado de bin/download desensamblado, Thumb en scratchpad/download.S; funciones 0x27e0 constructor de tramas, 0x2bf8 START, 0x3458 MIDST, 0x1ac0 bucle de imagen):**
  tramas del modo SDIO (NO HDLC): cabecera de 8 bytes = 4 halfwords little-endian h0..h3: h0 = longitud, h1 = tipo (MIDST: tipo en el byte bajo y bits 16-23 de la longitud en el alto),
  h2 = checksum del payload, h3 = checksum de los 6 primeros bytes; checksum = suma de complemento a uno de 16 bits (~sum), byte impar final sumado tal cual. Escritura por el canal 3, respuestas por el 12 (6 bytes `00 80 00 00 ff 7f` = ACK).
  * START: h0=8, h1=1, h2=csum(payload), payload = BE32 dirección + BE32 tamaño redondeado (0x100000, 0xC0000); trama de 16 bytes.
  * MIDST: cabecera h0=total&0xffff, h1=((total>>16)&0xff)<<8|2, h2=0 + 0x20000 bytes de datos (la ventana del canal 3 pasa a 131080 = 0x20008 tras el START); relleno 0xff; 6 bloques de 128 KB.
  * END: h0=4, h1=3, h2=0xffff + 4 bytes cero (12 bytes). **Tras el END el firmware arranca solo: NO hay EXEC** (el EXEC que probé no fue consumido; la ventana volvió a 64 bytes y sigue ocupada).
  * Dirección de carga del firmware 0x100000 (tabla en .data de bin/download: fdl 0x80000000/0x2000, fw 0x100000/0xC0000).
- **Resultado:** START, 6 MIDST y END recibieron ACK (probado desde un estado limpio justo tras el arranque; si antes se manda basura al canal 3 el chip deja de contestar hasta reiniciar). Scripts: tools/marlin/bsl.py (/dev/marlin) y fwload.py.
- **Siguiente:** (a) comprobar si el firmware principal está vivo: sincronización por GPIO 84 (io_ready, dos flancos), wake 46/148 (esos pines no están en modo GPIO en nuestro kernel: dmsk=0; hay que configurarlos), canales 8/9/14 y 10 (sync); (b) calibración ("start_calibration", escritura de 4096 bytes de nvbuf al canal 0 y lectura de la respuesta del 12, 2001 intentos de 1 ms);
  (c) portar drivers/net/wireless/sc2331 (wlan_core.c usa sdio_dev_write/read, canales 8/9) a 6.18 -> wlan0.
- OJO: la descarga debe hacerse una sola vez por arranque, justo después de que aparezca /dev/marlin (~30 s), y sin mandar nada antes al canal 3.

## Wi-Fi: FIRMWARE ARRANCADO Y CALIBRACIÓN OK (26 oct, 22:45) — fase 3, sincronía y calibración
- **Tras el END hay que mandar EXEC CON LA DIRECCIÓN de arranque** (lo vi en bin/download, función 0x3758): trama h0=4, h1=4, h2=csum(payload), payload = BE32(0x100000) (12 bytes). El chip da ACK y **el firmware principal arranca**.
  Mi EXEC vacío anterior no servía. (tools/marlin/fwload.py f_exec(addr), marlin_up.py ejecuta toda la secuencia.)
- Tras el EXEC las ventanas de los canales cambian (señal de que el fw principal vive): chn0/1/2 = 13312 bytes de espacio de escritura, chn3 = 256, chn5 = 64, chn6 = 128 (antes solo chn3 = 64). Sin GPIO ni sincronía especial para llegar hasta aquí.
- **Calibración:** buffer de 4096 bytes (tools/marlin/nvbuf.py: config en buf[0:0x268] + calibración en buf[0x268:] desde etc/connectivity_configure.ini y connectivity_calibration.ini con las tablas de bin/download .data 0x90a8 y 0x9468; 80 + 53 claves) escrito al **canal 0** (sin trama)
  -> la ventana del canal 0 baja a 9216 (consumió los 4096) y el fw RESPONDE por el **canal 12** con la estructura de calibración rellena (`01 01 01 01 01 01 01 01 00 00 00 00 7f 00 00 00 0a08 0208 fe07 ...` = flags, rctune_value 0x7f, códigos DCOC...). Samsung guarda esa respuesta en /productinfo/connectivity_calibration.ini y la reutiliza (is_calibrated=1).
- La calibración se hizo a los ~6 s del EXEC; el esquema de ready/GPIO (io_ready 84, wake 46/148) NO hizo falta para la descarga ni la calibración.
- **Siguiente (fase 3 restante):** (1) automatizar al arrancar (servicio en boot-custom.sh o dentro del kernel: tras /dev/marlin lanzar marlin_up.py ~30 s después del arranque) y guardar la respuesta de calibración; (2) portar drivers/net/wireless/sc2331 (wlan_core.c: canales 8/9, sdio_dev_write/read; wlan_cmd/msg_q/event_q/fifo; cfg80211) a 6.18 -> wlan0; (3) wpa_supplicant/conexión.
- **Arranque automático (26 oct, 23:00):** bootpart/marlin/{marlin-up.sh, marlin_up.py, bsl.py, fwload.py, nvbuf.bin, sc2331_fw.bin} -> /boot/marlin/; boot-custom.sh lanza marlin-up.sh, que espera /dev/marlin (~32 s) y corre la descarga+EXEC+calibración (fin a ~42 s de uptime);
  registro /boot/marlin/marlin.log, respuesta de calibración /boot/marlin/cali-response.bin (4096 bytes). Verificado tras `reboot`: rc=0, "calibracion OK: canal 12 4096 bytes". (Commit local; sin subir a GitHub todavía.)

## v64 — driver Wi-Fi cfg80211 del SC2331 (reescrito, no portado) (26 oct, 23:50), compilado, SIN flashear
- tag v64-wlan, patches/0040, out/boot-gtex-v64.img, build-73.log. drivers/misc/sprd-sc2331-wlan.c (~600 líneas) sobre sprd-marlin-sdio.c (ahora exporta marlin_sdio_chn_status/len/read/write, set_irq_cb; include/linux/sprd-marlin-sdio.h)
  + CONFIG_WIRELESS/CFG80211 (sin regulatory.db) + CONFIG_SPRD_SC2331_WLAN. Sustituye al drivers/net/wireless/sc2331 de Samsung (7.4k líneas, API de 3.10); solo modo estación.
- **Formato del protocolo (de wlan_core.c/wlan_fifo.c/wlan_cmd.c):** TX = "gran trama" de 64 bytes {u8 vif, u8 msg_num, le16 len, 14 x cabecera de mensaje, le32 tx_cnt} + mensajes {cabecera 4 bytes: u8 tipo(7 bits: 0 cmd/evento, 1 respuesta, 2 trama de datos)|vif<<7, u8 subtipo, le16 len;
  [tramas: 34 bytes extra = le32 seq + ceros]; payload} alineados a 4, todo redondeado a 1 KB (máx 13312) y escrito en un canal 0-2 que esté libre (bit 840/841 a 0). RX = canales 8/9 (y 11/13/14/15 se leen y descartan): 8 bytes + mensajes hasta tipo 0xff.
  Comandos: SET_DEV_OPEN(22){le16 modo=1, mac}, SET_SCAN(4){n, canales..., le32 len, [len,ssid]...}, WPA_VERSION/AUTH/CIPHERS/AKM = le32, SET_PSK(24){le16 len, PMK 32 B}, CHANNEL, BSSID, ESSID{le16 len, ssid}, DISCONNECT(15). Eventos: CONNECT(128), DISCONNECT(129), SCANDONE(130), REPORT_SCAN_FRAME(138){ops 0 = trama/1 = fin, canal, señal dBm, len}.
  El firmware hace él mismo el handshake de 4 vías a partir de la PMK (wiphy con NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK; wpa_supplicant le pasa la PMK en connect).
- Activación: `echo 1 > /sys/devices/virtual/misc/marlin/wlan` (lo hace /boot/marlin/marlin-up.sh tras la calibración) registra wlan0; `echo 0` la quita.
- Por probar tras flashear: `ip link set wlan0 up` (SET_DEV_OPEN, 8 s), `iw dev wlan0 scan`, `wpa_supplicant`/connmand con una red real. Sin probar nada en la tablet todavía; riesgos: formato exacto de algunos comandos (escaneo, ESSID), MAC aleatoria por arranque, el CP duerme si no se mantiene wake_out (GPIO 46).

## v64 PROBADA — wlan0 EXISTE Y ESCANEA (26 oct, 20:35)
- v64 flasheada. Arranque automático: marlin-up.sh termina a uptime 26.8 s con "wlan0 registrada" (FDL1 a 20 s, firmware+calibración ~6 s después). `wlan0` aparece (MAC aleatoria 3e:79:...), `ip link set wlan0 up` OK (SET_DEV_OPEN respondió),
  **`iw dev wlan0 scan` devuelve redes reales** con canal y señal (Share 2.4 -65 dBm, Totalplay, IZZI, INFINITUM, ARTCOUSTIX...; 28 BSS en el segundo escaneo). El formato de comandos del escaneo, la cola TX/RX y el análisis de eventos (REPORT_SCAN_FRAME ops 0/1) funcionan.
- Pendiente: conectar a una red con wpa_supplicant (PMK por NL80211_ATTR_PMK), DHCP, tráfico de datos (TX con el relleno de 34 bytes por trama), MAC estable, wake_out/sleep, BT. Escaneo con SSID en blanco muestra algunas redes ocultas.

## Wi-Fi: PRIMERA CONEXIÓN (26 oct, 21:15) — asocia, pero no pasa tráfico; v65 con trazas
- Red del usuario `INFINITUM3B1A_2.4` (OJO: el nombre exacto con mayúsculas y "_2.4"; la clave se guarda solo en /tmp/wpa-test.conf de la tablet, NO en el repo).
  Prueba: `wpa_supplicant -B -Dnl80211 -iwlan0 -c /tmp/wpa-test.conf -f /tmp/wpa.log -dd` -> **wpa_state=COMPLETED**, WPA2-PSK, CCMP/TKIP, canal 11, "Set PMK based on external data" (wpa_supplicant usa la oferta de handshake y pasa la PMK por NL80211_ATTR_PMK -> crypto.psk).
- Problemas: (1) conecta y a los pocos segundos llega EVT_DISCONNECT con reason 196 (0xc4 = AP_DEAUTH) y se repite en bucle (conecta/desconecta); (2) el ping a 192.168.1.254/.1 con IP fija no recibe respuesta: ARP "FAILED". Sí entra tráfico (RX 24 paquetes: broadcast del router) y salen (TX 39, 7 descartados mientras estaba desconectado).
  Hipótesis: handshake que falla por PMK/formato (el AP deauth tras el timeout), o trama TX mal formada; el contador seq de los 34 bytes era `tx_cnt+num` en vez de un contador corriente (corregido en v65).
- No hay cliente DHCP suelto en la imagen (solo connmand); `ping -I` no funciona (SO_BINDTODEVICE). wake_out (GPIO 46) en alto a mano no cambió nada.
- **v65** (tag v65-wlan-debug, patches/0041, out/boot-gtex-v65.img, build-74.log): seq corriente, y dev_info de cada evento (id, len, 32 bytes), de las 20 primeras tramas RX/TX. Qué mirar: motivo real del desconexión (eventos 129/131/132) y si las tramas TX salen bien (cabecera Ethernet).

## Wi-Fi: DIAGNÓSTICO con las trazas de v65 (26 oct, 21:40) -> v66
- Trazas: el firmware es `Marlin_5.1R_W17.12.2` (evento 144 al arrancar, 29 bytes de texto). Al asociar (evento 128 CONNECT, 428 bytes, status 0) llega por el canal 8 una trama EAPOL (ethertype 88 8e, mensaje 1 del handshake WPA, key info 0x008a) que se REPITE cada ~3 s hasta que el router deautentica (evento 129, motivo c4).
  => **el chip NO hace el handshake de 4 vías ni siquiera con la PMK: manda las tramas EAPOL al host y espera que wpa_supplicant las conteste y le INSTALE LAS CLAVES con KEY_ADD (13) / KEY_SET (15) / KEY_DEL (14)** (como el wlan_cfg80211_add_key del vendor). La "oferta" (NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK + SET_PSK con la PMK) era un callejón sin salida.
- **v66** (tag v66-wlan-keys, patches/0042, out/boot-gtex-v66.img, build-75.log): se quita la característica de oferta del wiphy; add_key envía KEY_ADD{mac[6], keyseq[8], pairwise, cypher_type (WEP40=1, WEP104=2, TKIP=3, CCMP=4), key_index, key_len, valor}; del_key -> KEY_DEL{idx, pairwise, mac}; set_default_key -> KEY_SET{le32 idx}. SET_PSK se manda con longitud 0 (como el vendor sin PMK).
- Prueba prevista: wpa_supplicant normal (sin PMK externa) con la misma red; mirar "add_key idx ..." en dmesg, wpa_state COMPLETED estable, ARP/ping al router.
