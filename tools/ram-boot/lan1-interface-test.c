/* SPDX-License-Identifier: GPL-2.0-only */
/* Opt-in retained-mapping interface pause/resume test, no MMIO */
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

static struct {
	char name[16];
	unsigned short flags;
	unsigned char padding[14];
} request = {.name = "lan1"};

__attribute__((noreturn)) void _start(void)
{
	long profile = CALL(5,
		"/sys/firmware/devicetree/base/pie@12080000/hsan,experimental-retain-dma-on-down",
		0, 0, 0, 0);
	long fd = -1, result = 1;
	unsigned short flags;
	struct { int sec, nsec; } pause = {1, 0}, tick = {0, 100000000};

	if (profile < 0) {
		PRINT("INTERFACE_TEST_BLOCKED_PROFILE\n");
		goto done;
	}
	(void)CALL(6, profile, 0, 0, 0, 0);
	fd = CALL(281, 2, 2, 0, 0, 0);
	if (fd < 0 || CALL(54, fd, 0x8913, &request, 0, 0) < 0 ||
	    !(request.flags & 1))
		goto done;
	flags = request.flags;
	request.flags &= ~1u;
	if (CALL(54, fd, 0x8914, &request, 0, 0) < 0) {
		PRINT("INTERFACE_DOWN_FAILED\n");
		goto done;
	}
	PRINT("INTERFACE_SOFT_DOWN_OK\n");
	(void)CALL(162, &pause, 0, 0, 0, 0);
	request.flags = flags;
	if (CALL(54, fd, 0x8914, &request, 0, 0) < 0) {
		PRINT("INTERFACE_UP_FAILED\n");
		goto done;
	}
	for (unsigned i = 0; i < 30; i++) {
		if (CALL(54, fd, 0x8913, &request, 0, 0) == 0 &&
		    (request.flags & 0x41) == 0x41) {
			PRINT("INTERFACE_SOFT_RESUME_PASS\n");
			result = 0;
			break;
		}
		(void)CALL(162, &tick, 0, 0, 0, 0);
	}
done:
	if (fd >= 0)
		(void)CALL(6, fd, 0, 0, 0, 0);
	(void)CALL(1, result, 0, 0, 0, 0);
	for (;;) {}
}
