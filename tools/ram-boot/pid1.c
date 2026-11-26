/* RAM-only PID 1: mount virtual filesystems, run diagnostics once, then keep a
 * real Debian Bash on ttyAMA0. No persistent storage or reboot operations. */
#if !defined(__arm__) || defined(__aarch64__)
#error ARM EABI required
#endif
static inline long call(long n,long x0,long x1,long x2,long x3,long x4) {
    register long r0 __asm__("r0")=x0, r1 __asm__("r1")=x1, r2 __asm__("r2")=x2;
    register long r3 __asm__("r3")=x3, r4 __asm__("r4")=x4, r7 __asm__("r7")=n;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1),"r"(r2),"r"(r3),"r"(r4),"r"(r7) : "memory","cc");
    return r0;
}
#define C(n,a,b,c,d,e) call((n),(long)(a),(long)(b),(long)(c),(long)(d),(long)(e))
#define OUT(s) ((void)C(4,1,(s),sizeof(s)-1,0,0))
static const long delay[2]={1,0};
static void pause_one(void){(void)C(162,delay,0,0,0,0);}
static void console(const char *path,int ctty){
    long fd=C(5,path,2,0,0,0);
    if(fd<0)return;
    if(ctty)(void)C(54,fd,0x540e,0,0,0);
    (void)C(63,fd,0,0,0,0);(void)C(63,fd,1,0,0,0);(void)C(63,fd,2,0,0,0);
    if(fd>2)(void)C(6,fd,0,0,0,0);
}
static char *const env[]={"PATH=/usr/sbin:/usr/bin:/sbin:/bin","HOME=/root","TERM=vt100","LANG=C","PS1=TEWA160# ",0};
static char *const first[]={"sh","-c","/bin/sh /tewa/diagnose.sh; cd /root || cd /; echo TEWA160_SHELL_START; exec /bin/bash --noprofile --norc -i",0};
static char *const later[]={"bash","--noprofile","--norc","-i",0};
__attribute__((noreturn)) void _start(void){
    int first_child=1;
    (void)C(21,"devtmpfs","/dev","devtmpfs",0,0);
    (void)C(21,"proc","/proc","proc",0,0);
    (void)C(21,"sysfs","/sys","sysfs",0,0);
    (void)C(21,"tmpfs","/run","tmpfs",0,0);
    (void)C(21,"tmpfs","/tmp","tmpfs",0,0);
    console("/dev/console",0);
    OUT("TEWA160_DEBIAN_PID1_ENTERED\n");
    for(;;){
        long pid=C(2,0,0,0,0,0);
        if(pid==0){
            (void)C(66,0,0,0,0,0);
            console("/dev/ttyAMA0",1);
            if(first_child)(void)C(11,"/bin/sh",first,env,0,0);
            (void)C(12,"/root",0,0,0,0);
            (void)C(11,"/bin/bash",later,env,0,0);
            OUT("TEWA160_BASH_EXEC_FAILED\n");
            (void)C(1,127,0,0,0,0);for(;;)pause_one();
        }
        if(pid>0){
            long status=0;first_child=0;
            while(C(114,pid,&status,0,0,0)==-4){}
            /* Reap exited orphan children without blocking the next shell. */
            while(C(114,-1,&status,1,0,0)>0){}
            OUT("TEWA160_SHELL_RESTARTING\n");
        }else OUT("TEWA160_FORK_FAILED\n");
        pause_one();
    }
}
