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

/* Fixed direct test link: board 192.168.0.1/24, host 192.168.0.10.
 * Requires an already-UP and running LAN1; never changes link ownership.
 */
static inline long call6(long n,long a,long b,long c,long d,long e,long f) {
    register long r0 __asm__("r0")=a,r1 __asm__("r1")=b,r2 __asm__("r2")=c;
    register long r3 __asm__("r3")=d,r4 __asm__("r4")=e,r5 __asm__("r5")=f,r7 __asm__("r7")=n;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1),"r"(r2),"r"(r3),"r"(r4),"r"(r5),"r"(r7) : "memory","cc");
    return r0;
}
static union {unsigned long align;unsigned char bytes[32];} req;
static unsigned char packet[64],received[16384];
static struct {unsigned short family,port;unsigned char address[4],zero[8];}
    destination={2,0,{192,168,0,10},{0}};
struct poll_fd {int fd;short events,revents;};
static unsigned checksum(const unsigned char *data,unsigned length) {
    unsigned sum=0,i=0;
    while(i+1<length){sum+=((unsigned)data[i]<<8)|data[i+1];i+=2;}
    if(i<length)sum+=(unsigned)data[i]<<8;
    while(sum>>16)sum=(sum&65535)+(sum>>16);
    return (~sum)&65535;
}
__attribute__((noreturn)) void _start(void) {
    long fd=C(281,2,2,0,0,0);
    if(fd<0){number("NET_SOCKET_RC=",fd);finish(1);}
    req.bytes[0]='l';req.bytes[1]='a';req.bytes[2]='n';req.bytes[3]='1';
    long rc=C(54,fd,0x8913,req.bytes,0,0);
    unsigned flags=req.bytes[16]|((unsigned)req.bytes[17]<<8);
    number("NET_FLAGS=",flags);
    if(rc<0 || (flags&0x41)!=0x41){text("NET_BLOCKED_LINK_NOT_RUNNING\n");finish(1);}
    for(unsigned i=16;i<32;i++)req.bytes[i]=0;
    req.bytes[16]=2;req.bytes[20]=192;req.bytes[21]=168;req.bytes[22]=0;req.bytes[23]=1;
    rc=C(54,fd,0x8916,req.bytes,0,0);number("NET_SET_IPV4_RC=",rc);
    if(rc<0)finish(1);
    req.bytes[20]=req.bytes[21]=req.bytes[22]=255;req.bytes[23]=0;
    rc=C(54,fd,0x891c,req.bytes,0,0);number("NET_SET_MASK_RC=",rc);
    (void)C(6,fd,0,0,0,0);if(rc<0)finish(1);
    fd=C(281,2,3,1,0,0); /* AF_INET/SOCK_RAW/IPPROTO_ICMP */
    if(fd<0){number("ICMP_SOCKET_RC=",fd);finish(1);}
    unsigned replies=0;
    for(unsigned attempt=1;attempt<=3;attempt++) {
        packet[0]=8;packet[1]=0;packet[2]=packet[3]=0;
        packet[4]=0x54;packet[5]=0xea;packet[6]=0;packet[7]=(unsigned char)attempt;
        for(unsigned i=8;i<sizeof(packet);i++)packet[i]=(unsigned char)(0xa5^i);
        unsigned sum=checksum(packet,sizeof(packet));packet[2]=(unsigned char)(sum>>8);packet[3]=(unsigned char)sum;
        rc=call6(290,fd,(long)packet,sizeof(packet),0,(long)&destination,sizeof(destination));
        number("ICMP_SEND_RC=",rc);if(rc!=(long)sizeof(packet))break;
        for(unsigned wait=0;wait<10;wait++) {
            struct poll_fd event={(int)fd,1,0};
            rc=C(168,&event,1,200,0,0);if(rc<=0 || !(event.revents&1))continue;
            rc=C(3,fd,received,sizeof(received),0,0);if(rc<28)continue;
            unsigned header=(received[0]&15u)*4u;
            if(header<20 || (unsigned long)rc<header+64 || received[9]!=1)continue;
            unsigned char *icmp=received+header;
            if(received[12]==192 && received[13]==168 && received[14]==0 && received[15]==10 &&
               received[16]==192 && received[17]==168 && received[18]==0 && received[19]==1 &&
               icmp[0]==0 && icmp[1]==0 && icmp[4]==0x54 && icmp[5]==0xea &&
               icmp[6]==0 && icmp[7]==attempt && checksum(icmp,(unsigned)rc-header)==0) {
                unsigned matches=1;
                for(unsigned i=8;i<64;i++)if(icmp[i]!=(unsigned char)(0xa5^i))matches=0;
                if(matches){replies++;number("ICMP_REPLY_BYTES=",rc);break;}
            }
        }
    }
    number("ICMP_VALID_ECHO_REPLIES=",replies);(void)C(6,fd,0,0,0,0);finish(replies?0:1);
}
