/*
 * marlinctl: power the Marlin WiFi/BT chip up with the vendor kernel's own drivers, under TWRP:
 * /dev/power_ctl ioctls (sprd_power_ctl.c) and an open of /dev/download (marlin_sdio_init(): the SDIO
 * host rescans). Then dmesg tells whether the chip answers on SDIO. No C library, static, ARM EABI.
 *   marlinctl on      DOWNLOAD_POWER_ON (vddcon 1.6 V, 32 kHz clock) + DOWNLOAD_POWER_RST
 *   marlinctl scan N  open /dev/download and keep it open N seconds (the rescan happens on open)
 *   marlinctl off     DOWNLOAD_POWER_OFF
 * Build: clang like tplay (see build.sh).
 */

static long sys3(long nr, long a, long b, long c)
{
	register long r0 asm("r0") = a, r1 asm("r1") = b, r2 asm("r2") = c, r7 asm("r7") = nr;

	asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
	return r0;
}
#define SYS_write	4
#define SYS_open	5
#define SYS_close	6
#define SYS_ioctl	54
#define SYS_nanosleep	162
#define O_RDWR		2

#define IO(t, n)	(((t) << 8) | (n))
#define POWER_ON	IO('z', 1)
#define POWER_OFF	IO('z', 2)
#define POWER_RST	IO('z', 3)

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys3(SYS_write, 1, (long)s, slen(s)); }
static void putn(long v)
{
	char b[24]; int i = 23;

	b[i] = 0;
	if (v < 0) { puts1("-"); v = -v; }
	do { b[--i] = '0' + v % 10; v /= 10; } while (v);
	puts1(b + i);
}
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void sleep_ms(long ms)
{
	long ts[2] = { ms / 1000, (ms % 1000) * 1000000 };

	sys3(SYS_nanosleep, (long)ts, 0, 0);
}

static int ctl(unsigned int cmd)
{
	int fd = sys3(SYS_open, (long)"/dev/power_ctl", O_RDWR, 0);
	long r;

	if (fd < 0)
		return puts1("open /dev/power_ctl failed\n"), 1;
	r = sys3(SYS_ioctl, fd, cmd, 0);
	sys3(SYS_close, fd, 0, 0);
	puts1("ioctl ");
	putn(cmd & 0xff);
	puts1(" -> ");
	putn(r);
	puts1("\n");
	return r < 0;
}

int cmain(long *sp)
{
	int argc = sp[0];
	char **argv = (char **)(sp + 1);
	int fd, n, i;

	if (argc < 2)
		return puts1("marlinctl on | scan N | off\n"), 1;
	if (seq(argv[1], "on")) {
		if (ctl(POWER_ON))
			return 1;
		sleep_ms(50);
		return ctl(POWER_RST);
	}
	if (seq(argv[1], "off"))
		return ctl(POWER_OFF);
	if (seq(argv[1], "scan") && argc >= 3) {
		for (n = 0, i = 0; argv[2][i] >= '0' && argv[2][i] <= '9'; i++)
			n = n * 10 + argv[2][i] - '0';
		fd = sys3(SYS_open, (long)"/dev/download", O_RDWR, 0);
		puts1("open /dev/download -> ");
		putn(fd);
		puts1("\n");
		if (fd < 0)
			return 1;
		sleep_ms(n * 1000);
		sys3(SYS_close, fd, 0, 0);
		return 0;
	}
	return 1;
}

__attribute__((naked, noreturn)) void _start(void)
{
	asm volatile("mov r0, sp\n"
		     "bl cmain\n"
		     "mov r7, #1\n"
		     "svc 0\n");
}
