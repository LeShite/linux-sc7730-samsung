/*
 * marlinboot: load the Marlin's FDL through its boot ROM over the UART, under TWRP (Samsung kernel),
 * as Android's /system/bin/download does before the SDIO stage. Spreadtrum BSL framing:
 *   0x7E | type (2, BE) | length (2, BE) | data | CRC-16/XMODEM of type+length+data (2, BE) | 0x7E
 * with 0x7E -> 7D 5E and 0x7D -> 7D 5D inside. Commands: CONNECT 0, START_DATA 1 (addr, size),
 * MIDST_DATA 2, END_DATA 3, EXEC_DATA 4 (addr); replies ACK 0x80, VER 0x81.
 *   marlinboot DEV BAUD FDL ADDR      e.g. marlinboot /dev/ttyS0 115200 /tmp/sc2331_fdl.bin 0x80000000
 * No C library, static, ARM EABI.
 */

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

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
#define O_RDONLY	0
#define O_RDWR		2
#define O_NOCTTY	0400
#define O_NONBLOCK	04000

#define IO(t, n)	(((t) << 8) | (n))
#define POWER_ON	IO('z', 1)
#define POWER_RST	IO('z', 3)

struct termios {
	u32 c_iflag, c_oflag, c_cflag, c_lflag;
	u8 c_line, c_cc[19];
};
#define TCGETS		0x5401
#define TCSETS		0x5402
#define TCFLSH		0x540b
#define CS8		0000060
#define CREAD		0000200
#define CLOCAL		0004000

#define CMD_CONNECT	0x00
#define CMD_START	0x01
#define CMD_MIDST	0x02
#define CMD_END		0x03
#define CMD_EXEC	0x04
#define REP_ACK		0x80
#define REP_VER		0x81
#define CHUNK		512

void *memset(void *d, int c, unsigned long n) { u8 *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
static int slen(const char *s) { int n = 0; while (s[