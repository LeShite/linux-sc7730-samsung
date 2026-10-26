/*
 * Bring-up PID 1 for the SM-T280: no libc, raw ARM EABI syscalls.
 *
 * Mounts the pseudo filesystems, prints the state of whatever we are
 * bringing up to the console and then keeps a live status line. A counter
 * that keeps moving proves that the timer interrupt, hrtimers and the
 * scheduler all work.
 *
 * Build: clang --target=armv7a-linux-gnueabi -marm -nostdlib -ffreestanding
 *        -static -fuse-ld=lld -Wl,-e,_start
 */

#define SYS_read	3
#define SYS_write	4
#define SYS_open	5
#define SYS_close	6
#define SYS_execve	11
#define SYS_mount	21
#define SYS_access	33
#define SYS_umount2	52
#define SYS_mkdir	39
#define SYS_nanosleep	162
#define SYS_clock_nanosleep_time64	407

#define CLOCK_MONOTONIC	1

#define O_RDONLY	0
#define O_NONBLOCK	04000

static long sys(long n, long a, long b, long c, long d, long e)
{
	register long r7 asm("r7") = n;
	register long r0 asm("r0") = a;
	register long r1 asm("r1") = b;
	register long r2 asm("r2") = c;
	register long r3 asm("r3") = d;
	register long r4 asm("r4") = e;

	asm volatile("svc #0"
		     : "+r"(r0)
		     : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4)
		     : "memory");
	return r0;
}

static unsigned long len(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

static void write_(const char *s, long n)
{
	sys(SYS_write, 1, (long)s, n, 0, 0);
}

static void puts_(const char *s)
{
	write_(s, len(s));
}

/* Return a pointer to needle inside the first n bytes of hay, or 0 */
static const char *find(const char *hay, long n, const char *needle)
{
	long i, j, m = len(needle);

	for (i = 0; i + m <= n; i++) {
		for (j = 0; j < m && hay[i + j] == needle[j]; j++)
			;
		if (j == m)
			return hay + i;
	}
	return 0;
}

/* Copy a file to the console, at most max lines (0: all of it) */
static void cat_or(const char *title, const char *path, int max,
		   const char *missing)
{
	static char buf[4096];
	long fd, n, i;
	int lines = 0;

	puts_("\n== ");
	puts_(title);
	puts_(" ==\n");
	fd = sys(SYS_open, (long)path, O_RDONLY, 0, 0, 0);
	if (fd < 0) {
		puts_(missing);
		return;
	}
	while ((n = sys(SYS_read, fd, (long)buf, sizeof(buf), 0, 0)) > 0) {
		for (i = 0; i < n; i++) {
			if (buf[i] == '\n' && ++lines == max) {
				write_(buf, i + 1);
				goto out;
			}
		}
		write_(buf, n);
	}
out:
	sys(SYS_close, fd, 0, 0, 0, 0);
}

static void cat(const char *title, const char *path, int max)
{
	cat_or(title, path, max, "(no se pudo abrir)\n");
}

/* Print the kernel log records that mention any of the given words */
static void kmsg_grep(const char *title, const char *a, const char *b)
{
	static char rec[1024];
	const char *msg;
	long fd, n;

	puts_("\n== ");
	puts_(title);
	puts_(" ==\n");
	fd = sys(SYS_open, (long)"/dev/kmsg", O_RDONLY | O_NONBLOCK, 0, 0, 0);
	if (fd < 0) {
		puts_("(sin /dev/kmsg)\n");
		return;
	}
	/* one record per read: "prio,seq,usec,flags;text\n" */
	while ((n = sys(SYS_read, fd, (long)rec, sizeof(rec), 0, 0)) > 0) {
		msg = find(rec, n, ";");
		if (!msg)
			continue;
		msg++;
		n -= msg - rec;
		if (find(msg, n, a) || find(msg, n, b)) {
			const char *nl = find(msg, n, "\n");

			write_(msg, nl ? nl - msg + 1 : n);
		}
	}
	sys(SYS_close, fd, 0, 0, 0, 0);
}

/* Read a small file into buf (NUL terminated), returns its length */
static long slurp(const char *path, char *buf, long size)
{
	long fd, n;

	fd = sys(SYS_open, (long)path, O_RDONLY, 0, 0, 0);
	if (fd < 0)
		return 0;
	n = sys(SYS_read, fd, (long)buf, size - 1, 0, 0);
	sys(SYS_close, fd, 0, 0, 0, 0);
	if (n < 0)
		n = 0;
	buf[n] = 0;
	return n;
}

/* Decimal string of n (built with -mcpu=cortex-a7: hardware division) */
static const char *num(long n)
{
	static char buf[16];
	char *p = buf + sizeof(buf) - 1;
	int neg = n < 0;

	*p = 0;
	if (neg)
		n = -n;
	do {
		*--p = '0' + n % 10;
		n /= 10;
	} while (n);
	if (neg)
		*--p = '-';
	return p;
}

/*
 * Sleep with the time64 syscall; the 32-bit time one only exists with
 * CONFIG_COMPAT_32BIT_TIME. Returns 0 on success.
 */
static long sleep_ms(long ms)
{
	struct { long long sec, nsec; } ts64 = { ms / 1000, (ms % 1000) * 1000000LL };
	struct { long sec, nsec; } ts32 = { ms / 1000, (ms % 1000) * 1000000 };

	if (sys(SYS_clock_nanosleep_time64, CLOCK_MONOTONIC, 0, (long)&ts64, 0, 0) == 0)
		return 0;
	return sys(SYS_nanosleep, (long)&ts32, 0, 0, 0, 0);
}

/*
 * Touch test: find the MIP4 event device and print where it is touched,
 * on one line redrawn in place, for a few seconds.
 */
static void touch_test(int seconds)
{
	static char path[64], name[96];
	struct { unsigned long sec, usec; unsigned short type, code; int value; } ev;
	long fd = -1, n;
	int i, x = -1, y = -1, touches = 0;

	for (i = 0; i < 10 && fd < 0; i++) {
		const char *pre = "/sys/class/input/event";
		long l;

		for (l = 0; pre[l]; l++)
			path[l] = pre[l];
		path[l++] = '0' + i;
		path[l] = 0;
		{
			const char *suf = "/device/name";
			long k;

			for (k = 0; suf[k]; k++)
				path[l + k] = suf[k];
			path[l + k] = 0;
		}
		n = slurp(path, name, sizeof(name));
		if (n && (find(name, n, "MELFAS") || find(name, n, "mip4"))) {
			const char *dev = "/dev/input/event";

			for (l = 0; dev[l]; l++)
				path[l] = dev[l];
			path[l++] = '0' + i;
			path[l] = 0;
			fd = sys(SYS_open, (long)path, O_RDONLY | O_NONBLOCK, 0, 0, 0);
		}
	}

	puts_("\n== prueba del tactil ==\n");
	if (fd < 0) {
		puts_("(no se encontro el tactil)\n");
		return;
	}
	puts_("dispositivo: ");
	puts_(name);
	puts_("TOCA LA PANTALLA (");
	puts_(num(seconds));
	puts_(" segundos)\n");

	for (i = 0; i < seconds * 20; i++) {
		while (sys(SYS_read, fd, (long)&ev, sizeof(ev), 0, 0) == sizeof(ev)) {
			if (ev.type == 3 && (ev.code == 0x35 || ev.code == 0))
				x = ev.value;
			if (ev.type == 3 && (ev.code == 0x36 || ev.code == 1))
				y = ev.value;
			if (ev.type == 1 && ev.code == 0x14a && ev.value == 1)
				touches++;
		}
		puts_("\rtoque: x=");
		puts_(num(x));
		puts_(" y=");
		puts_(num(y));
		puts_("   toques: ");
		puts_(num(touches));
		puts_("     ");
		sleep_ms(50);
	}
	puts_("\n");
	sys(SYS_close, fd, 0, 0, 0, 0);
}

/* One live line, redrawn in place: uptime and timer interrupt count */
static void status_line(void)
{
	static char buf[4096];
	const char *p;
	long i, n;

	n = slurp("/proc/uptime", buf, 64);
	for (i = 0; i < n && buf[i] != ' '; i++)
		;
	puts_("\rLinux vivo en el SM-T280: ");
	write_(buf, i);
	puts_(" s   irq del temporizador: ");

	n = slurp("/proc/interrupts", buf, sizeof(buf));
	p = find(buf, n, "timer@");
	if (p) {
		/* back to the start of the line, then the count after the ':' */
		while (p > buf && p[-1] != '\n')
			p--;
		while (*p && *p != ':')
			p++;
		while (*p == ':' || *p == ' ')
			p++;
		for (i = 0; p[i] >= '0' && p[i] <= '9'; i++)
			;
		write_(p, i);
	}
	puts_("      ");
}

int main(void)
{
	int i;

	sys(SYS_mkdir, (long)"/proc", 0755, 0, 0, 0);
	sys(SYS_mkdir, (long)"/sys", 0755, 0, 0, 0);
	sys(SYS_mkdir, (long)"/dev", 0755, 0, 0, 0);
	sys(SYS_mount, (long)"proc", (long)"/proc", (long)"proc", 0, 0);
	sys(SYS_mount, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0);
	sys(SYS_mount, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);
	sys(SYS_mount, (long)"pstore", (long)"/sys/fs/pstore", (long)"pstore", 0, 0);

	puts_("\n\n*** init propio arrancado: hola desde Linux en tu Galaxy Tab A! ***\n");
	cat("kernel", "/proc/version", 2);
	cat("memoria", "/proc/meminfo", 2);

	/* The eMMC is probed and scanned in the background; give it time */
	puts_("\nesperando al eMMC");
	for (i = 0; i < 20 && sys(SYS_access, (long)"/sys/block/mmcblk0", 0, 0, 0, 0); i++) {
		puts_(".");
		sleep_ms(500);
	}
	puts_("\n");
	kmsg_grep("log del kernel sobre el eMMC", "mmc", "sdhci");
	kmsg_grep("log del kernel sobre la GPU y la pantalla", "lima", "gpu");
	kmsg_grep("log del kernel sobre los CPUs", "smp", "CPU");
	kmsg_grep("log del kernel sobre PMIC, I2C y tactil", "sc2723", "i2c");
	kmsg_grep("log del kernel sobre el tactil", "mip4", "MELFAS");

	puts_("\n================ RESUMEN ================\n");
	cat("CPUs en linea", "/sys/devices/system/cpu/online", 1);
	cat_or("GPU (render)", "/sys/class/drm/renderD128/device/uevent", 1,
	       "(sin GPU)\n");
	touch_test(10);
	cat_or("chip eMMC", "/sys/block/mmcblk0/device/name", 1,
	       "(no se detecto ningun eMMC)\n");
	cat_or("particiones", "/proc/partitions", 40, "(sin /proc/partitions)\n");

	/*
	 * The black box: ramoops keeps the console of the previous boot in
	 * RAM. Printed last so that its tail, where a failed boot died, stays
	 * on screen.
	 */
	cat_or("caja negra: log del arranque anterior",
	       "/sys/fs/pstore/console-ramoops-0", 0,
	       "(vacia: el arranque anterior no dejo log)\n");
	puts_("\n");

	/*
	 * Hand over to Batocera's own initramfs script when it is bundled.
	 * It mounts proc, sysfs and devtmpfs itself and gives up if they are
	 * already mounted, so undo ours first.
	 */
	if (!sys(SYS_access, (long)"/init.batocera", 0, 0, 0, 0)) {
		static const char *argv[] = { "/init.batocera", 0 };
		static const char *envp[] = { "HOME=/", "TERM=linux", 0 };

		sys(SYS_umount2, (long)"/sys/fs/pstore", 0, 0, 0, 0);
		sys(SYS_umount2, (long)"/sys", 0, 0, 0, 0);
		sys(SYS_umount2, (long)"/dev", 0, 0, 0, 0);
		sys(SYS_umount2, (long)"/proc", 0, 0, 0, 0);
		puts_("*** pasando el control al init de Batocera ***\n");
		sys(SYS_execve, (long)argv[0], (long)argv, (long)envp, 0, 0);
		puts_("!!! no se pudo ejecutar /init.batocera\n");
		sys(SYS_mount, (long)"proc", (long)"/proc", (long)"proc", 0, 0);
	}

	for (;;) {
		status_line();
		if (sleep_ms(1000)) {
			/* never spin: a flood of output hides everything else */
			puts_("\n!!! no se puede dormir: fallan las llamadas de espera. init detenido.\n");
			for (;;)
				asm volatile("wfi");
		}
	}
}

void __attribute__((naked, noreturn)) _start(void)
{
	asm volatile("bl main\n"
		     "1: b 1b\n");
}
