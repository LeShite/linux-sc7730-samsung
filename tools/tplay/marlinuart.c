/*
 * marlinuart: the first step of the Marlin download, under TWRP. The chip's boot ROM listens on the
 * UART (Android's /system/bin/download uses /dev/ttyS0); the host sends 0x7E until the ROM answers
 * with its version frame. Powers the chip up and resets it through /dev/power_ctl first.
 *   marlinuart DEV BAUD     e.g. marlinuart /dev/ttyS0 115200
 * Prints every byte received in hex, for 3 s. No C library, static, ARM EABI.
 */

typedef unsigned int u32;

static long sys3(long nr, long a, long b, long c)
{
	register long r0 asm("r0") = a, r1 asm("r1") = b, r2 asm("r2") = c, r7 asm("r7") = nr;

	asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
	return r0;
}
#define SYS_read	3
#define SYS_write	4
#define SYS_open	5
#define SYS_close	6
#define SYS_ioctl	54
#define SYS_nanosleep	162
#define O_RDWR		2
#define O_NOCTTY	0400
#define O_NONBLOCK	04000

#define IO(t, n)	(((t) << 8) | (n))
#define POWER_ON	IO('z', 1)
#define POWER_RST	IO('z', 3)

/* struct termios of the ARM kernel ABI */
struct termios {
	u32 c_iflag, c_oflag, c_cflag, c_lflag;
	unsigned char c_line, c_cc[19];
};
#define TCGETS		0x5401
#define TCSETS		0x5402
#define TCFLSH		0x540b
#define CBAUD		0010017
#define CBAUDEX		0010000
#define CS8		0000060
#define CREAD		0000200
#define CLOCAL		0004000
#define VTIME		5
#define VMIN		6

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys3(SYS_write, 1, (long)s, slen(s)); }
static void puthex(unsigned int v)
{
	char b[4] = { "0123456789abcdef"[(v >> 4) & 15], "0123456789abcdef"[v & 15], ' ', 0 };

	puts1(b);
}
static void sleep_ms(long ms)
{
	long ts[2] = { ms / 1000, (ms % 1000) * 1000000 };

	sys3(SYS_nanosleep, (long)ts, 0, 0);
}
static long atol1(const char *s) { long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0'; return v; }

static u32 baud_code(long baud)
{
	switch (baud) {
	case 9600:	return 0000015;
	case 57600:	return 0010001;
	case 115200:	return 0010002;
	case 230400:	return 0010003;
	case 460800:	return 0010004;
	case 921600:	return 0010007;
	case 3000000:	return 0010015;
	}
	return 0010002;
}

int cmain(long *sp)
{
	int argc = sp[0];
	char **argv = (char **)(sp + 1);
	struct termios t;
	unsigned char c = 0x7e, buf[64];
	int fd, ctl, i, n, total = 0;

	if (argc < 3)
		return puts1("marlinuart DEV BAUD\n"), 1;

	fd = sys3(SYS_open, (long)argv[1], O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
	if (fd < 0)
		return puts1("open uart failed\n"), 1;
	sys3(SYS_ioctl, fd, TCGETS, (long)&t);
	t.c_iflag = 0;
	t.c_oflag = 0;
	t.c_lflag = 0;
	t.c_cflag = baud_code(atol1(argv[2])) | CS8 | CREAD | CLOCAL;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	if (sys3(SYS_ioctl, fd, TCSETS, (long)&t) < 0)
		return puts1("TCSETS failed\n"), 1;
	sys3(SYS_ioctl, fd, TCFLSH, 2);

	ctl = sys3(SYS_open, (long)"/dev/power_ctl", O_RDWR, 0);
	if (ctl < 0)
		return puts1("open /dev/power_ctl failed\n"), 1;
	sys3(SYS_ioctl, ctl, POWER_ON, 0);
	sleep_ms(50);
	sys3(SYS_ioctl, ctl, POWER_RST, 0);
	sys3(SYS_close, ctl, 0, 0);
	puts1("powered and reset, sending 0x7e\n");

	for (i = 0; i < 300; i++) {	/* 3 s */
		sys3(SYS_write, fd, (long)&c, 1);
		sleep_ms(10);
		while ((n = sys3(SYS_read, fd, (long)buf, sizeof(buf))) > 0) {
			int j;

			for (j = 0; j < n; j++)
				puthex(buf[j]);
			total += n;
		}
		if (total >= 64)
			break;
	}
	puts1("\nreceived ");
	{
		char b[12]; int k = 11; b[k] = 0;
		do { b[--k] = '0' + total % 10; total /= 10; } while (total);
		puts1(b + k);
	}
	puts1(" bytes\n");
	sys3(SYS_close, fd, 0, 0);
	return 0;
}

__attribute__((naked, noreturn)) void _start(void)
{
	asm volatile("mov r0, sp\n"
		     "bl cmain\n"
		     "mov r7, #1\n"
		     "svc 0\n");
}
