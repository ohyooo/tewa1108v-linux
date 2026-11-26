/* Freestanding ARMEL bounded IPv4/ICMP test, no MMIO */
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
static __attribute__((unused)) void text(const char *s){(void)C(4,1,s,len(s),0,0);}
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


/* Standard read-only SIOCETHTOOL, no MMIO, link changes or module loading. */
static unsigned char request[32] __attribute__((aligned(8)));
static unsigned char info[32] __attribute__((aligned(8)));
static unsigned char names[12+1024*32] __attribute__((aligned(8)));
static unsigned char stats[8+1024*8] __attribute__((aligned(8)));
static void word(unsigned char *p,unsigned n){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(n>>(8*i));}
static unsigned getword(const unsigned char *p){return p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24);}
static long query(long fd,unsigned char *data){word(request+16,(unsigned)data);return C(54,fd,0x8946,request,0,0);}
__attribute__((noreturn)) void _start(void){
 long fd=C(281,2,2,0,0,0);if(fd<0)finish(1);
 request[0]='l';request[1]='a';request[2]='n';request[3]='1';
 word(info,0x37);word(info+8,2);
 long rc=query(fd,info);if(rc<0){number("ETHTOOL_SSET_RC=",rc);finish(1);}
 unsigned count=getword(info+16);number("ETHTOOL_STAT_COUNT=",count);if(!count||count>1024)finish(1);
 word(names,0x1b);word(names+4,1);word(names+8,count);
 rc=query(fd,names);if(rc<0){number("ETHTOOL_NAMES_RC=",rc);finish(1);}
 word(stats,0x1d);word(stats+4,count);
 rc=query(fd,stats);if(rc<0){number("ETHTOOL_STATS_RC=",rc);finish(1);}
 for(unsigned i=0;i<count;i++){
  char out[64];unsigned n=0;while(n<32&&names[12+i*32+n]){out[n]=(char)names[12+i*32+n];n++;}
  out[n++]='=';out[n++]='0';out[n++]='x';
  for(unsigned j=0;j<16;j++){unsigned v=stats[8+i*8+7-j/2];v=(j&1)?v&15:v>>4;out[n++]="0123456789abcdef"[v];}
  out[n++]='\n';(void)C(4,1,out,n,0,0);
 }
 (void)C(6,fd,0,0,0,0);finish(0);
}
