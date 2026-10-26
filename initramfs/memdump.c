/*
 * memdump ADDR SIZE: copy physical memory from /dev/mem to stdout.
 *
 * TWRP's busybox dd uses a 32-bit off_t, which overflows for RAM above
 * 2 GiB (all of it on this SoC, which starts at 0x80000000). This uses
 * _llseek instead. Freestanding, built like init.c.
 */

#define SYS_exit	1
#define SYS_read	3
#define SYS_write	4
#define SYS_open	5
#define SYS__llseek	140

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

static unsigned long hex(const char *s)
{
	unsigned long v = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	for (; *s; s++) {
		int d = *s >= 'a' ? *s - 'a' + 10 : *s >= 'A' ? *s - 'A' + 10 : *s - '0';
		v = v * 16 + d;
	}
	return v;
}

int main(int argc, char **argv)
{
	static char buf[65536];
	unsigned long addr, size, chunk;
	long long result;
	long fd, n;

	if (argc != 3)
		return 2;
	addr = hex(argv[1]);
	size = hex(argv[2]);

	fd = sys(SYS_open, (long)"/dev/mem", 0, 0, 0, 0);
	if (fd < 0)
		return 3;
	/* _llseek(fd, offset_high, offset_low, &result, SEEK_SET) */
	if (sys(SYS__llseek, fd, 0, (long)addr, (long)&result, 0) < 0)
		return 4;

	while (size) {
		chunk = size < sizeof(buf) ? size : sizeof(buf);
		n = sys(SYS_read, fd, (long)buf, chunk, 0, 0);
		if (n <= 0)
			return 5;
		sys(SYS_write, 1, (long)buf, n, 0, 0);
		size -= n;
	}
	return 0;
}

void __attribute__((naked, noreturn)) _start(void)
{
	/* the kernel leaves argc at [sp] and argv right above it */
	asm volatile("ldr r0, [sp]\n"
		     "add r1, sp, #4\n"
		     "bl main\n"
		     "mov r7, #1\n"
		     "svc #0\n");
}
