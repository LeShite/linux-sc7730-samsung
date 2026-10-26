/*
 * tplay: a tiny ALSA control and playback tool with no C library, for TWRP's 3.10 kernel on the
 * SM-T280 (Batocera's glibc needs Linux >= 6.1). Raw syscalls, static, ARM EABI.
 *   tplay CARD list                 every control: name, type, range, values
 *   tplay CARD set NAME VALUE       set every channel of a control (enum: item index)
 *   tplay CARD play DEVICE FILE.wav canonical 16-bit stereo 44.1 kHz WAV, 44-byte header
 * Build: see build.sh next to this file.
 */

typedef unsigned int u32;
typedef unsigned long long u64;

/* ---- syscalls ---------------------------------------------------------- */

static long sys3(long nr, long a, long b, long c)
{
	register long r0 asm("r0") = a, r1 asm("r1") = b, r2 asm("r2") = c, r7 asm("r7") = nr;

	asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
	return r0;
}
#define SYS_exit	1
#define SYS_read	3
#define SYS_write	4
#define SYS_open	5
#define SYS_close	6
#define SYS_ioctl	54
#define O_RDONLY	0
#define O_RDWR		2

void *memset(void *d, int c, unsigned long n)
{
	unsigned char *p = d;

	while (n--)
		*p++ = c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	unsigned char *p = d;
	const unsigned char *q = s;

	while (n--)
		*p++ = *q++;
	return d;
}

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
static long atol1(const char *s)
{
	long v = 0, neg = 0;

	if (*s == '-') { neg = 1; s++; }
	while (*s >= '0' && *s <= '9')
		v = v * 10 + *s++ - '0';
	return neg ? -v : v;
}
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

#define IOC(dir, type, nr, size) (((dir) << 30) | ((size) << 16) | ((type) << 8) | (nr))
#define IOWR(t, n, s) IOC(3u, t, n, sizeof(s))
#define IOW(t, n, s) IOC(1u, t, n, sizeof(s))
#define IO(t, n) IOC(0u, t, n, 0)

/* ---- controls ---------------------------------------------------------- */

struct ctl_id {
	u32 numid, iface, device, subdevice;
	unsigned char name[44];
	u32 index;
};
struct ctl_list {
	u32 offset, space, used, count;
	struct ctl_id *pids;
	unsigned char reserved[50];
};
struct ctl_info {
	struct ctl_id id;
	u32 type, access, count;
	int owner;
	union {
		struct { long min, max, step; } integer;
		struct { u32 items, item; char name[64]; u64 names_ptr; u32 names_length; } enumerated;
		unsigned char reserved[128];
	} value;
	unsigned char reserved[64];
};
struct ctl_value {
	struct ctl_id id;
	u32 indirect;
	union {
		long integer[128];
		u32 enumerated[128];
		long long pad64;
		unsigned char bytes[512];
	} value;
	unsigned char reserved[128];
};
_Static_assert(sizeof(struct ctl_id) == 64, "id");
_Static_assert(sizeof(struct ctl_list) == 72, "list");
_Static_assert(sizeof(struct ctl_info) == 272, "info");
_Static_assert(sizeof(struct ctl_value) == 712, "value");

#define CTL_ELEM_LIST	IOWR('U', 0x10, struct ctl_list)
#define CTL_ELEM_INFO	IOWR('U', 0x11, struct ctl_info)
#define CTL_ELEM_READ	IOWR('U', 0x12, struct ctl_value)
#define CTL_ELEM_WRITE	IOWR('U', 0x13, struct ctl_value)
#define TYPE_BOOLEAN	1
#define TYPE_INTEGER	2
#define TYPE_ENUMERATED	3
#define IFACE_MIXER	2

static struct ctl_id ids[512];

static void ctl_dev(char *p, const char *card)
{
	memcpy(p, "/dev/snd/controlC", 17);
	memcpy(p + 17, card, slen(card) + 1);
}

static int ctl_list(int fd)
{
	struct ctl_list l;
	struct ctl_info info;
	struct ctl_value v;
	u32 i, j;

	memset(&l, 0, sizeof(l));
	l.space = 512;
	l.pids = ids;
	if (sys3(SYS_ioctl, fd, CTL_ELEM_LIST, (long)&l) < 0)
		return puts1("ELEM_LIST failed\n"), 1;
	for (i = 0; i < l.used; i++) {
		memset(&info, 0, sizeof(info));
		info.id = ids[i];
		if (sys3(SYS_ioctl, fd, CTL_ELEM_INFO, (long)&info) < 0)
			continue;
		memset(&v, 0, sizeof(v));
		v.id = ids[i];
		puts1((char *)ids[i].name);
		puts1(" | type ");
		putn(info.type);
		puts1(" count ");
		putn(info.count);
		if (info.type == TYPE_INTEGER) {
			puts1(" range ");
			putn(info.value.integer.min);
			puts1("..");
			putn(info.value.integer.max);
		} else if (info.type == TYPE_ENUMERATED) {
			puts1(" items ");
			putn(info.value.enumerated.items);
		}
		puts1(" =");
		if (info.type <= TYPE_ENUMERATED &&
		    sys3(SYS_ioctl, fd, CTL_ELEM_READ, (long)&v) >= 0)
			for (j = 0; j < info.count && j < 8; j++) {
				puts1(" ");
				putn(info.type == TYPE_ENUMERATED ? (long)v.value.enumerated[j] : v.value.integer[j]);
			}
		puts1("\n");
	}
	return 0;
}

static int ctl_set(int fd, const char *name, long val)
{
	struct ctl_info info;
	struct ctl_value v;
	u32 j;
	long r;

	memset(&info, 0, sizeof(info));
	info.id.iface = IFACE_MIXER;
	memcpy(info.id.name, name, slen(name) < 43 ? slen(name) : 43);
	if (sys3(SYS_ioctl, fd, CTL_ELEM_INFO, (long)&info) < 0) {
		puts1("no such control: ");
		puts1(name);
		puts1("\n");
		return 1;
	}
	memset(&v, 0, sizeof(v));
	v.id = info.id;
	for (j = 0; j < info.count && j < 128; j++) {
		if (info.type == TYPE_ENUMERATED)
			v.value.enumerated[j] = val;
		else
			v.value.integer[j] = val;
	}
	r = sys3(SYS_ioctl, fd, CTL_ELEM_WRITE, (long)&v);
	puts1(name);
	puts1(r < 0 ? " <- FAILED " : " <- ");
	putn(val);
	puts1("\n");
	return r < 0;
}

/* ---- playback ---------------------------------------------------------- */

struct mask { u32 bits[8]; };
struct interval { u32 min, max; u32 flags; };	/* openmin:1 openmax:1 integer:1 empty:1 */
struct hw_params {
	u32 flags;
	struct mask masks[3];
	struct mask mres[5];
	struct interval intervals[12];
	struct interval ires[9];
	u32 rmask, cmask, info, msbits, rate_num, rate_den;
	unsigned long fifo_size;
	unsigned char reserved[64];
};
struct xferi { long result; void *buf; unsigned long frames; };
_Static_assert(sizeof(struct hw_params) == 604, "hw_params");

#define PCM_HW_PARAMS	IOWR('A', 0x11, struct hw_params)
#define PCM_PREPARE	IO('A', 0x40)
#define PCM_DRAIN	IO('A', 0x44)
#define PCM_WRITEI	IOW('A', 0x50, struct xferi)
#define P_CHANNELS	10
#define P_RATE		11
#define P_PERIOD_SIZE	13
#define P_PERIODS	15
#define P_BUFFER_SIZE	17

static void fix(struct hw_params *p, int param, u32 v)
{
	struct interval *i = &p->intervals[param - 8];

	i->min = i->max = v;
	i->flags = 1u << 2;	/* integer */
}

static short buf[4096 * 2];

static int play(const char *card, const char *dev, const char *file)
{
	char path[48];
	struct hw_params p;
	struct xferi x;
	int fd, wav, i;
	long n, total = 0;

	memcpy(path, "/dev/snd/pcmC", 13);
	i = 13 + slen(card);
	memcpy(path + 13, card, slen(card));
	path[i++] = 'D';
	memcpy(path + i, dev, slen(dev));
	i += slen(dev);
	memcpy(path + i, "p", 2);

	fd = sys3(SYS_open, (long)path, O_RDWR, 0);
	if (fd < 0)
		return puts1("open pcm failed\n"), 1;
	memset(&p, 0, sizeof(p));
	for (i = 0; i < 3; i++)
		memset(&p.masks[i], 0xff, sizeof(p.masks[i]));
	for (i = 0; i < 12; i++)
		p.intervals[i].max = ~0u;
	p.rmask = ~0u;
	p.info = ~0u;
	memset(&p.masks[0], 0, 32);
	p.masks[0].bits[0] = 1u << 3;		/* RW_INTERLEAVED */
	memset(&p.masks[1], 0, 32);
	p.masks[1].bits[0] = 1u << 2;		/* S16_LE */
	memset(&p.masks[2], 0, 32);
	p.masks[2].bits[0] = 1u;		/* standard subformat */
	fix(&p, P_CHANNELS, 2);
	fix(&p, P_RATE, 44100);
	/* period and buffer: whatever the driver chooses (the vendor's refused 1024 x 4) */
	n = sys3(SYS_ioctl, fd, PCM_HW_PARAMS, (long)&p);
	if (n < 0) {
		puts1("HW_PARAMS failed ");
		putn(n);
		puts1("\n");
		return 1;
	}
	puts1("pcm ");
	puts1(path);
	puts1(" period ");
	putn(p.intervals[P_PERIOD_SIZE - 8].min);
	puts1(" buffer ");
	putn(p.intervals[P_BUFFER_SIZE - 8].min);
	puts1("\n");
	if (sys3(SYS_ioctl, fd, PCM_PREPARE, 0) < 0)
		return puts1("PREPARE failed\n"), 1;

	wav = sys3(SYS_open, (long)file, O_RDONLY, 0);
	if (wav < 0)
		return puts1("open wav failed\n"), 1;
	sys3(SYS_read, wav, (long)buf, 44);
	while ((n = sys3(SYS_read, wav, (long)buf, sizeof(buf))) > 0) {
		x.buf = buf;
		x.frames = n / 4;
		x.result = 0;
		if (sys3(SYS_ioctl, fd, PCM_WRITEI, (long)&x) < 0) {
			sys3(SYS_ioctl, fd, PCM_PREPARE, 0);	/* underrun: start again */
			sys3(SYS_ioctl, fd, PCM_WRITEI, (long)&x);
		}
		total += n / 4;
	}
	sys3(SYS_ioctl, fd, PCM_DRAIN, 0);
	sys3(SYS_close, wav, 0, 0);
	sys3(SYS_close, fd, 0, 0);
	puts1("played ");
	putn(total);
	puts1(" frames\n");
	return 0;
}

int cmain(long *sp)
{
	int argc = sp[0];
	char **argv = (char **)(sp + 1);
	char path[32];
	int fd;

	if (argc >= 3 && seq(argv[2], "play") && argc >= 5)
		return play(argv[1], argv[3], argv[4]);
	if (argc < 3)
		return puts1("tplay CARD list | set NAME VALUE | play DEVICE FILE\n"), 1;
	ctl_dev(path, argv[1]);
	fd = sys3(SYS_open, (long)path, O_RDWR, 0);
	if (fd < 0)
		return puts1("open control failed\n"), 1;
	if (seq(argv[2], "list"))
		return ctl_list(fd);
	if (seq(argv[2], "set") && argc >= 5)
		return ctl_set(fd, argv[3], atol1(argv[4]));
	return 1;
}

__attribute__((naked, noreturn)) void _start(void)
{
	asm volatile("mov r0, sp\n"
		     "bl cmain\n"
		     "mov r7, #1\n"
		     "svc 0\n");
}
