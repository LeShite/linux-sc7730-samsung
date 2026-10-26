/* cpuspeed: estimate the clock of one core. A chain of dependent adds is 1 cycle each on a Cortex-A7
 * (in order, ALU latency 1, no way to overlap them), so cycles = adds and Hz = adds / seconds.
 * No C library, static, ARM EABI: same build as tools/tplay. */
static long sys2(long nr, long a, long b)
{
	register long r0 asm("r0") = a, r1 asm("r1") = b, r7 asm("r7") = nr;

	asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r7) : "memory");
	return r0;
}
static long ms(void)
{
	long ts[2];

	sys2(263, 1, (long)ts);		/* clock_gettime(CLOCK_MONOTONIC) */
	return ts[0] * 1000 + ts[1] / 1000000;
}
static void out(const char *s, long v)
{
	char b[16]; int i = 15, n = 0;

	b[i] = 0;
	do { b[--i] = '0' + v % 10; v /= 10; } while (v);
	while (s[n]) n++;
	register long r0 asm("r0") = 1, r1 asm("r1") = (long)s, r2 asm("r2") = n, r7 asm("r7") = 4;
	asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
	n = 0;
	while (b[i + n]) n++;
	r0 = 1; r1 = (long)(b + i); r2 = n; r7 = 4;
	asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
	r0 = 1; r1 = (long)"\n"; r2 = 1; r7 = 4;
	asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
}
int cmain(void)
{
	long best = 0;

	for (int rep = 0; rep < 5; rep++) {
		long t0 = ms(), n = 20000000, x = 0;

		asm volatile(
			"1:\n"
			".rept 16\n add %0, %0, #1\n .endr\n"
			"subs %1, %1, #1\n bne 1b\n"
			: "+r"(x), "+r"(n) : : "cc");
		long dt = ms() - t0;
		long mhz = dt ? (20000000L * 16 / 1000) / dt : 0;	/* adds per ms / 1000 = MHz */
		if (mhz > best)
			best = mhz;
	}
	out("MHz (mejor de 5 medidas): ", best);
	return 0;
}
__attribute__((naked, noreturn)) void _start(void)
{
	asm volatile("bl cmain\n mov r7, #1\n mov r0, #0\n svc 0\n");
}
