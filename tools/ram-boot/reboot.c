/* Minimal ARM EABI reboot utility for the RAM-only Debian diagnostic.
 * Linux syscall, not an OEM bootloader command. Requires CAP_SYS_BOOT.
 * No daemon, init signalling, watchdog MMIO, NAND or persistent writes.
 */
#if !defined(__arm__) || defined(__aarch64__)
#error "ARM EABI (armel) required"
#endif

static inline long call(long nr, long a0, long a1, long a2, long a3) {
    register long r0 __asm__("r0") = a0;
    register long r1 __asm__("r1") = a1;
    register long r2 __asm__("r2") = a2;
    register long r3 __asm__("r3") = a3;
    register long r7 __asm__("r7") = nr;
    __asm__ volatile ("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r7) : "memory", "cc");
    return r0;
}
static void emit(const char *s, unsigned n) {(void)call(4, 1, (long)s, n, 0);}
#define SAY(s) emit((s), sizeof(s)-1)
__attribute__((noreturn)) void _start(void) {
    /* Sync RAM/VFS state before asking the kernel to restart the board. */
    (void)call(36,0,0,0,0);
    SAY("TEWA161_REBOOT_SYSCALL_REQUESTED\n");
    long ret = call(88, 0xfee1dead, 672274793, 0x01234567, 0);
    if (ret == -1) SAY("reboot: EPERM (CAP_SYS_BOOT required)\n");
    else if (ret == -22) SAY("reboot: EINVAL (kernel rejected command)\n");
    else SAY("reboot: kernel returned without restarting\n");
    (void)call(1, 1,0,0,0);
    for (;;) __asm__ volatile("wfe");
}
