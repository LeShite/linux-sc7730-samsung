# ESTADO DEL PROYECTO: Batocera en Galaxy Tab A 7.0 2016 (SM-T280 "gtexswifi")
Fecha: 2026-10-24 · Kernel: Linux 6.18.58 LTS · Todo vive en ~/batocera-t280/

## 0. Hardware verificado
- SoC Spreadtrum SC7730SW (familia sc8830): 4x Cortex-A7 32 bits, NEON/VFPv4, Mali-400 MP2
- RAM 1.5 GB en 0x80000000-0xDFFFFFFF · eMMC 7.3 GB · pantalla 800x1280
- Bootloader SBOOT T280XXS0ARK3, desbloqueo OEM activo, hw_revision=5
- Particiones: KERNEL=p20 (boot, 16M) · RECOVERY=p21 (TWRP 3.1.0) · CACHE=p24 (200M)
  SYSTEM=p25 (2G) · HIDDEN=p26 (40M) · userdata=p27 (4.9G)
- Framebuffer del bootloader: 0x9EA48000, 800x1280, a8r8g8b8, stride 3200
- El micro-USB de la tablet es inestable (error -71 / EMI): siempre usar scripts/autoflash.sh
- Batocera "h3" (Cortex-A7 + Mali-400 + Lima + labwc) es compatible a nivel binario con este CPU/GPU

## 1. Avance
- v1: PRIMERA LUZ (Linux 6.18 + simpledrm en pantalla)
- v2/v3: /init propio sin libc, DRM_PANIC, COMPAT_32BIT_TIME
- v4: timer con el orden del vendor (esperar BUSY antes de deshabilitar)
- v5: driver INTC; el CPU despierta de WFI. TIMERS OK: el contador avanza, NO_HZ funciona
- v6: driver eMMC (SDHCI) FUNCIONA: mmc0 high speed, mmcblk0p1..p27 correctas (16:41)

## 2. Parches (exactos en ~/batocera-t280/patches/*.patch, git en src/linux-6.18.58)
A) arch/arm/Kconfig.platforms: ARCH_SPRD (ARMv7) select ARM_GIC, SPRD_SC8830_INTC, SPRD_SC8830_TIMER
B) drivers/clocksource/timer-sprd-sc8830.c  (TIMER_OF "sprd,sc8830-timer", 32768 Hz)
   - 3 timers por bloque, separados 0x20: LOAD+0, CTL+8 (b6 PERIOD, b7 ENABLE, b8 NEW),
     INT+0xC (b0 EN, b3 CLR, b4 BUSY), CNT_RD+0x10
   - timer0 = clockevent (SPI 28); timer1 = clocksource + sched_clock (cuenta hacia abajo, se lee invertido)
   - CLAVE 1: clkevt.cpumask = cpumask_of(0); si no, gana dummy_timer (sin oneshot)
   - CLAVE 2: esperar BUSY (con límite) ANTES de deshabilitar; set_oneshot carga el periodo y habilita
   - habilita relojes: AON APB EB0 0x402E0000 bits 10-12
C) drivers/irqchip/irq-sprd-sc8830-intc.c  (IRQCHIP "sprd,sc8830-intc", jerárquico sobre el GIC, modelo irq-tegra.c)
   - INTC0-3 en 0x71400000/0x71500000/0x71600000/0x71700000; EN=+0x08, DIS=+0x0C
   - INTC = SPI/32, bit = SPI%32; relojes en 0x71300000 bits 19-22
   - MOTIVO: con todos los cores en WFI se apaga el reloj del GIC y solo el INTC despierta
D) drivers/mmc/host/sdhci-sprd-sc8830.c  ("sprd,sc8830-sdhci", sobre sdhci-pltfm)
   - caps forzadas (__sdhci_read_caps): VDD_300|SDMA|HISPD|8BIT, versión 3.00; base 384 MHz
   - SDCLK = 384MHz / (2*(div+1)), divisor de 10 bits
   - reset: escribir siempre el bit3 (RST_EMMC); tras RESET_ALL reescribir los retardos 0x80/0x84/0x88
   - set_power no hace nada; quirks BROKEN_ADMA, BROKEN_TIMEOUT_VAL, NO_HISPD_BIT,
     CAP_CLOCK_BASE_BROKEN; quirks2 NO_1_8_V, BROKEN_HS200, BROKEN_DDR50
   - imprime "boot state: caps/ver/clk/hctl/pres" antes del primer reset (diagnóstico)

## 3. Device tree (arch/arm/boot/dts/unisoc/)
sc7730.dtsi:
  / interrupt-parent=&intc · cpus f00..f03 cortex-a7 · clk32k fixed-clock 32768
  gic@12001000: arm,cortex-a7-gic, reg <0x12001000 0x1000>,<0x12002000 0x2000>, interrupt-parent=&gic
  intc@71400000: 4 regs + "eb" <0x71300000 0x4>, interrupt-parent=&gic
  timer@40050000: reg + <0x402e0000 0x8>, SPI 28, clocks=&clk32k
  mmc@20300000 (SD): SPI 57, sprd,delays=<3 3 3>, disabled
  mmc@20600000 (eMMC): SPI 60, sprd,delays=<0xa 2 2>
  gpu@60000000: arm,mali-400, 6x SPI 39, disabled
sc7730-samsung-gtexswifi.dts:
  sprd,sc-id=<8830 5 0x20000> (SBOOT elige el DTB por esto)
  chosen/framebuffer@9ea48000: simple-framebuffer 800x1280 a8r8g8b8
  memory <0x80000000 0x60000000>
  reserved no-map: 0x86a00000 2M (sec_log) · 0x87800000 4M (smem) · 0x88000000 0x1d00000 (módem)
                   0x9ea48000 0xbb8000 (fb) · ramoops 0x9f600000 1M
  aliases mmc0=&emmc, mmc1=&sdc
  &emmc: bus-width=8, non-removable, no-sd, no-sdio, cap-mmc-highspeed, max-frequency=52000000
unisoc/Makefile: dtb-$(CONFIG_ARCH_SPRD) += sc7730-samsung-gtexswifi.dtb

## 4. Compilar, empaquetar y flashear
~/batocera-t280/scripts/build-kernel.sh, que hace:
  1) clang (armv7, sin libc) initramfs/init.c -> build/initramfs/init
  2) make -C src/linux-6.18.58 O=build/kernel ARCH=arm LLVM=1 allnoconfig \
        KCONFIG_ALLCONFIG=arch/arm/configs/gtexswifi.config
  3) verifica que no falte ninguna opción clave, luego make zImage dtbs
  4) cat zImage + sc7730-samsung-gtexswifi.dtb > out/zImage-dtb   (ARM_APPENDED_DTB)
  5) scripts/mkgteximg.py --kernel out/zImage-dtb --dtb <dtb> -o out/boot-gtex.img
     Formato: DHTB 512 B ("DHTB", 1, sha256(payload), tamaño en +48) + Android boot v0
     (página 2048) + dt.img "SPRD" (revs 0,2,3,4,5) + "SEANDROIDENFORCE"+ffffffff.
     Verificado byte a byte contra TWRP.
Config clave (gtexswifi.config): MMU=y (allnoconfig lo apaga), MISC_FILESYSTEMS, PSTORE_RAM,
  COMPAT_32BIT_TIME, DRM_SIMPLEDRM, DRM_PANIC (kmsg), INITRAMFS_SOURCE, MMC_SDHCI_SPRD_SC8830,
  EFI_PARTITION, CMDLINE_FORCE="console=tty0 loglevel=8 ignore_loglevel panic=0 fbcon=nodefer"
Flashear:          scripts/autoflash.sh KERNEL out/boot-gtex-v6.img   (tablet en modo Download)
Restaurar Android: heimdall flash --KERNEL ~/batocera-t280/backup/KERNEL.img
Modo Download:     Vol- + Encendido hasta que se apague, luego Vol- + Home + Encendido, luego Vol+
TWRP:              Vol- + Encendido, luego Vol+ + Home + Encendido
Fotos de pantalla: en ~/Descargas (convertir con magick -auto-orient -resize 1500x1500)

## 5. Plan siguiente: rootfs Alpine en el eMMC (sin microSD)
 1. Flashear y probar v6 (solo lectura). Éxito = en pantalla aparecen mmcblk0 y mmcblk0p1..p27.
    Si falla: foto de la línea "boot state" y del log mmc/sdhci; ajustar el driver.
 2. Partición destino: CACHE (p24, 200 MB). Android la regenera y TWRP la formatea sin riesgo.
    (Después, para Batocera: userdata p27, 4.9 GB, que BORRA los datos de Android.)
 3. En el PC: bajar alpine-minirootfs-<versión>-armv7.tar.gz de alpinelinux.org y verificar su sha256.
 4. En TWRP (USB inestable: transferir en un solo paso; son unos 3 MB):
      adb push alpine-minirootfs.tar.gz /tmp/
      Wipe > Advanced > Cache (formato ext4), luego: mount /cache
      cd /cache && tar xzf /tmp/alpine-minirootfs.tar.gz
      En /cache/etc/inittab: getty de tty1 -> "tty1::respawn:/bin/sh" (todavía no hay teclado: es para ver la salida)
 5. Kernel v7: + EXT4_FS, UNIX, NET, INET, FHANDLE, INOTIFY_USER, FILE_LOCKING, UNIX98_PTYS.
    El /init de la initramfs monta /dev/mmcblk0p24 (ext4) y hace switch_root a /sbin/init de Alpine
    (o sin initramfs: root=/dev/mmcblk0p24 rootfstype=ext4 rootwait).
 6. Después: USB (dwc2, gadget serie/ACM) para tener shell desde el PC; SMP (4 cores);
    entrada (gpio-keys/EIC, táctil Melfas MIP4 por i2c-sprd); GPU Lima (relojes/power domain);
    y por último el userspace de Batocera H3 en userdata.

## 6. Instalación de Batocera (2026-10-24 17:15) — Android BORRADO por decisión del usuario
- Batocera H3 v38 (boot.tar.xz, sha256 7ad3643a... verificado) en batocera-v38/
- p25 SYSTEM  -> ext4 LABEL=BATOCERA (imagen batocera-v38/batocera-p25.ext4, 1400 MiB):
    /batocera-boot.conf, /boot/batocera (squashfs zstd 1.2 GB), /boot/batocera.board
    escrita con scripts/push-partition.sh (44 bloques de 32 MB, verificados, reanudable)
- p26 HIDDEN  -> cabecera borrada (2 MiB a cero)
- p27 userdata -> ext4 LABEL=SHARE UUID=27eceb4e-56ef-42b6-acc2-576df969b1a1 (mke2fs de TWRP)
- batocera-boot.conf: sharedevice=DEVICES + sharedevice_part1=SHARE@<uuid>; autoresize=false
  ¡PELIGRO! autoresize=true ejecutaría "parted resizepart 2 100%" y p2 es SBOOT2 (bootloader).
  batocera-part calcula share = boot + 1 con un solo dígito (p25 -> p26), de ahí DEVICES/UUID.
- v7 (out/boot-gtex-v7.img): initramfs = initrd de Batocera (/init -> /init.batocera) + nuestro
  /init (diagnóstico, desmonta y hace exec de /init.batocera); cmdline label=BATOCERA
- El Mesa de Batocera solo trae lima + kmsro (sun4i-drm, rockchip, ...): ni swrast ni simpledrm.
  EmulationStation necesita la GPU Lima y un driver de pantalla con un nombre que kmsro conozca.

## 7. ¡EmulationStation con GPU! (2026-10-24 17:39, kernel v11)
- v8: /chosen/rng-seed aleatoria por compilación (fdtput en build-kernel.sh); con el clocksource de
  32 kHz la entropía por jitter falla y getrandom() bloqueaba syslog-ng unos 5 minutos.
- v9-v11: drivers/pmdomain/sprd/sc8830-gpu-pd.c (genpd always-on, late_initcall, anuncia cada paso):
    1) PMU 0x402b0020: limpiar FORCE_SHUTDOWN (b25) y AUTO_SHUTDOWN_EN (b24)
    2) AON 0x402e0000: GPU_EB (b27)
    3) PMU 0x402b011c: compuerta de la fuente de 384 MHz (b5). SIN ESTO los registros Mali cuelgan el bus.
       (b0 300M, b1 312M, b2 208M, b3 256M, b4 460.8M, b5 384M, b6 153.6M, b7 512M)
    4) GPU APB 0x60100004: sel=5 (384 MHz), div=0
    NO leer la GPU APB antes del paso 1/2 (cuelga el bus).
- Lima: arm,mali-400 en 0x60000000, 7 irqs (gp,gpmmu,pp0,ppmmu0,pp1,ppmmu1,pmu) todas en SPI 39,
  relojes bus/core = fixed 384 MHz, power-domains=<&gpu_pd>.
- simpledrm se presenta como "sun4i-drm" (CONFIG_DRM_SIMPLEDRM_NAME) para que el kmsro del Mesa
  de Batocera use Lima para dibujar y simpledrm para mostrar.
- Resultado: video de intro, logo y EmulationStation (carrusel C64) dibujados por la GPU.
Pendiente: ENTRADA (no hay /dev/input), orientación horizontal, SMP (1 de 4 cores), audio, WiFi.
