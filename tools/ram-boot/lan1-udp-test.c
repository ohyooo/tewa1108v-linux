/* SPDX-License-Identifier: GPL-2.0-only */
/* Freestanding ARM EABI UDP round trips, including IPv4 fragmentation */
#if !defined(__arm__) || defined(__aarch64__)
#error ARM EABI required
#endif

static long syscall5(long nr, long a, long b, long c, long d, long e)
{
	register long r0 __asm__("r0") = a, r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c, r3 __asm__("r3") = d;
	register long r4 __asm__("r4") = e, r7 __asm__("r7") = nr;

	__asm__ volatile("svc #0" : "+r"(r0)
			 : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r7)
			 : "memory", "cc");
	return r0;
}

#define CALL(n, a, b, c, d, e) \
	syscall5(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e))
#define PRINT(s) ((void)CALL(4, 1, s, sizeof(s) - 1, 0, 0))

static unsigned char sent[8192], received[8193];
static const unsigned sizes[] = {32, 1472, 4096, 8192};
static const struct {
	unsigned short family, port;
	unsigned char address[4], zero[8];
} peer = {2, 0xa246, {192, 168, 0, 10}, {0}}; /* UDP 18082 */

__attribute__((noreturn)) void _start(void)
{
	long fd = CALL(281, 2, 2, 0, 0, 0);
	long result = 1;
	int pmtu = 0; /* IP_PMTUDISC_DONT: exercise IPv4 fragmentation */
	unsigned failed_seq = 0;
	unsigned rounds;
	struct { int fd; short events, revents; } hello = {(int)fd, 1, 0};

	if (fd < 0)
		goto done;
	if (CALL(294, fd, 0, 10, &pmtu, sizeof(pmtu)) < 0)
		goto close;
	if (CALL(283, fd, &peer, sizeof(peer), 0, 0) < 0)
		goto close;
	if (CALL(4, fd, "TEWAUDP?", 8, 0, 0) != 8 ||
	    CALL(168, &hello, 1, 2000, 0, 0) != 1 || !(hello.revents & 1) ||
	    CALL(3, fd, received, sizeof(received), 0, 0) != 4)
		goto close;
	rounds = ((unsigned)received[0] << 24) | ((unsigned)received[1] << 16) |
		 ((unsigned)received[2] << 8) | received[3];
	if (!rounds || rounds > 20000)
		goto close;
	for (unsigned seq = 0; seq < rounds; seq++) {
		unsigned size = sizes[seq & 3];
		struct { int fd; short events, revents; } event = {(int)fd, 1, 0};
		failed_seq = seq;

		for (unsigned i = 0; i < size; i++)
			sent[i] = (unsigned char)(i * 17 + seq * 31);
		sent[0] = (unsigned char)(seq >> 8);
		sent[1] = (unsigned char)seq;
		if (CALL(4, fd, sent, size, 0, 0) != (long)size) {
			PRINT("UDP_SEND_FAILED\n");
			goto close;
		}
		if (CALL(168, &event, 1, 2000, 0, 0) != 1 || !(event.revents & 1)) {
			PRINT("UDP_REPLY_TIMEOUT\n");
			goto close;
		}
		if (CALL(3, fd, received, sizeof(received), 0, 0) != (long)size) {
			PRINT("UDP_REPLY_LENGTH_MISMATCH\n");
			goto close;
		}
		for (unsigned i = 0; i < size; i++) {
			if (received[i] != sent[i]) {
				PRINT("UDP_REPLY_DATA_MISMATCH\n");
				goto close;
			}
		}
	}
	PRINT("UDP_ROUNDTRIPS_PASS sizes=32,1472,4096,8192\n");
	result = 0;
close:
	if (result) {
		static const char hex[] = "0123456789abcdef";
		char sequence[] = "UDP_FAILED_SEQ=0x0000\n";

		for (unsigned i = 0; i < 4; i++)
			sequence[17 + i] = hex[(failed_seq >> (12 - 4 * i)) & 15];
		(void)CALL(4, 1, sequence, sizeof(sequence) - 1, 0, 0);
	}
	(void)CALL(6, fd, 0, 0, 0, 0);
done:
	(void)CALL(1, result, 0, 0, 0, 0);
	for (;;) {}
}
