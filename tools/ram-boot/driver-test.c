/* Explicitly requested diagnostic, using only normal kernel module loading and
 * the existing ndo_open gate. No forced loading, raw MMIO or auth bypass.
 * The shell checks exact SHA-256 pins and live DT properties before invoking it.
 * This does not impersonate the unfinished production trusted-receiver protocol. */
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
static void finish(long n) __attribute__((noreturn));
static void finish(long n){(void)C(1,n,0,0,0,0);for(;;){}}
static long len(const char *s){long n=0;while(s[n])n++;return n;}
static void text(const char *s){(void)C(4,1,s,len(s),0,0);}
/* Decimal without ARMv5 divide runtime; one write for each final value. */
static void number(const char *prefix,long n){
    char out[100];unsigned i=0;unsigned v=(unsigned)n;
    static const unsigned power[]={1000000000u,100000000u,10000000u,1000000u,100000u,10000u,1000u,100u,10u,1u};
    while(prefix[i]&&i<70u){out[i]=prefix[i];i++;}
    if(n<0){out[i++]='-';v=0u-v;}
    unsigned started=0;
    for(unsigned j=0;j<10;j++){unsigned k=0;while(v>=power[j]){v-=power[j];k++;}if(k||started||j==9){out[i++]=(char)('0'+k);started=1;}}
    out[i++]='\n';(void)C(4,1,out,i,0,0);
}
#ifndef GATE_ONLY
struct module {const char *name;const char *path;unsigned size;const char *parameters;};
static const struct module mods[]={
#include "module-pins.inc"
};
static unsigned char buf[131072];
static long load(const struct module *m){
    long fd=C(5,m->path,0,0,0,0), used=0,rc=0;
    if(fd<0)return fd;
    if(m->size>sizeof(buf)){rc=-27;goto out;}
    while(used<(long)m->size){long k=C(3,fd,buf+used,m->size-(unsigned)used,0,0);if(k==-4)continue;if(k<=0){rc=k?k:-5;goto out;}used+=k;}
    {char extra;long k;do{k=C(3,fd,&extra,1,0,0);}while(k==-4);if(k!=0){rc=k<0?k:-27;goto out;}}
    rc=C(128,buf,m->size,m->parameters,0,0); /* normal init_module, no force/flags */
out:
    (void)C(6,fd,0,0,0,0);return rc;
}
__attribute__((noreturn)) void _start(void){
    unsigned count=0;
    for(unsigned i=0;i<sizeof(mods)/sizeof(mods[0]);i++){
        long rc=load(&mods[i]);
        text("TEWA160_MODULE ");text(mods[i].name);number(" rc=",rc);
        if(rc<0)break;
        count++;
    }
    number("TEWA160_LOAD_COUNT=",count);finish(count==sizeof(mods)/sizeof(mods[0])?0:1);
}
#else
/* Called exactly once by the diagnostic script only if all modules are Live,
 * current LAN1 owner/PHY match, and experimental-dma-enable is absent. The
 * current source rejects before queue allocation at this missing opt-in. */
static union {unsigned long align;unsigned char bytes[32];} req;
__attribute__((noreturn)) void _start(void){
    long fd=C(281,2,2,0,0,0); /* socket(AF_INET,SOCK_DGRAM,0) */
    if(fd<0){number("TEWA160_GATE_SOCKET_RC=",fd);finish(1);}
    req.bytes[0]='l';req.bytes[1]='a';req.bytes[2]='n';req.bytes[3]='1';
    long rc=C(54,fd,0x8913,req.bytes,0,0);
    if(rc<0){number("TEWA160_GET_FLAGS_RC=",rc);(void)C(6,fd,0,0,0,0);finish(1);}
    unsigned before=(unsigned)req.bytes[16]|((unsigned)req.bytes[17]<<8);
    number("TEWA160_FLAGS_BEFORE=",before);
    if(before&1u){OUT("TEWA160_GATE_ALREADY_UP_NO_IOCTL\n");(void)C(6,fd,0,0,0,0);finish(1);}
    req.bytes[16]|=1u;
    OUT("TEWA160_NDO_OPEN_ATTEMPT_ONCE\n");
    rc=C(54,fd,0x8914,req.bytes,0,0);
    number("TEWA160_NDO_OPEN_RC=",rc);
    long after=C(54,fd,0x8913,req.bytes,0,0);
    if(after==0)number("TEWA160_FLAGS_AFTER=",(unsigned)req.bytes[16]|((unsigned)req.bytes[17]<<8));
    else number("TEWA160_GET_FLAGS_AFTER_RC=",after);
    (void)C(6,fd,0,0,0,0);
    finish(rc==-13?0:2);
}
#endif
