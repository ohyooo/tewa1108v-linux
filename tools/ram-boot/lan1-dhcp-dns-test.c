/* SPDX-License-Identifier: GPL-2.0-only */
/* Isolated DHCP DORA and DNS transport test, freestanding ARM EABI */
#if !defined(__arm__) || defined(__aarch64__)
#error ARM EABI required
#endif
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
_Static_assert(sizeof(long) == 4 && sizeof(u32) == 4, "ARM EABI widths");

static long call6(long nr, long a, long b, long c, long d, long e, long f)
{
    register long r0 __asm__("r0")=a, r1 __asm__("r1")=b;
    register long r2 __asm__("r2")=c, r3 __asm__("r3")=d;
    register long r4 __asm__("r4")=e, r5 __asm__("r5")=f, r7 __asm__("r7")=nr;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1),"r"(r2),"r"(r3),
                     "r"(r4),"r"(r5),"r"(r7) : "memory","cc");
    return r0;
}
#define CALL(n,a,b,c,d,e,f) call6(n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f))
#define PRINT(s) ((void)CALL(4,1,s,sizeof(s)-1,0,0,0))
#define STAGE(s) do { stage=s; stage_len=sizeof(s)-1; } while (0)
static u8 nonce[6], mac[6], frame[342], reply[1024], ifreq[32];
static const u8 server[4]={192,168,0,10}, lease[4]={192,168,0,1};
static const u8 mask[4]={255,255,255,0}, cookie[4]={99,130,83,99};
static const u8 question[]={7,'e','x','a','m','p','l','e',3,'c','o','m',0,0,1,0,1};
static u8 query[12+sizeof(question)];
struct inet_addr { u16 family,port; u8 ip[4],zero[8]; };
struct link_addr { u16 family,protocol; int index; u16 type; u8 pkttype,halen,addr[8]; };
_Static_assert(sizeof(struct link_addr)==20, "sockaddr_ll layout");
static void copy(u8 *d,const u8 *s,unsigned n) { for(unsigned i=0;i<n;i++)d[i]=s[i]; }
static int equal(const u8 *a,const u8 *b,unsigned n)
{ for(unsigned i=0;i<n;i++)if(a[i]!=b[i])return 0; return 1; }
static void put16(u8 *p,unsigned v) { p[0]=(u8)(v>>8);p[1]=(u8)v; }
static unsigned get16(const u8 *p) { return (unsigned)p[0]*256+p[1]; }
static unsigned checksum(const u8 *p,unsigned n)
{
    u32 sum=0;
    for(unsigned i=0;i<n;i+=2)sum+=(unsigned)p[i]*256+p[i+1];
    while(sum>>16)sum=(sum&65535)+(sum>>16);
    return (~sum)&65535;
}
static void make_frame(unsigned type)
{
    for(unsigned i=0;i<sizeof(frame);i++)frame[i]=0;
    for(unsigned i=0;i<6;i++)frame[i]=255;
    copy(frame+6,mac,6);put16(frame+12,0x0800);
    u8 *ip=frame+14,*udp=ip+20,*bootp=udp+8;
    ip[0]=0x45;put16(ip+2,328);copy(ip+4,nonce+2,2);ip[8]=64;ip[9]=17;
    for(unsigned i=16;i<20;i++)ip[i]=255;
    put16(ip+10,checksum(ip,20));
    put16(udp,68);put16(udp+2,67);put16(udp+4,308);
    bootp[0]=1;bootp[1]=1;bootp[2]=6;copy(bootp+4,nonce,4);
    bootp[10]=0x80;copy(bootp+28,mac,6);copy(bootp+236,cookie,4);
    unsigned at=240;
    bootp[at++]=53;bootp[at++]=1;bootp[at++]=(u8)type;
    bootp[at++]=61;bootp[at++]=7;bootp[at++]=1;copy(bootp+at,mac,6);at+=6;
    bootp[at++]=55;bootp[at++]=3;bootp[at++]=1;bootp[at++]=6;bootp[at++]=51;
    if(type==3) {
        bootp[at++]=50;bootp[at++]=4;copy(bootp+at,lease,4);at+=4;
        bootp[at++]=54;bootp[at++]=4;copy(bootp+at,server,4);at+=4;
    }
    bootp[at]=255;
}
static int dhcp_valid(unsigned n,unsigned message)
{
    if(n<240 || reply[0]!=2 || reply[1]!=1 || reply[2]!=6 ||
       !equal(reply+4,nonce,4) || !equal(reply+28,mac,6) ||
       !equal(reply+16,lease,4) || !equal(reply+236,cookie,4))return 0;
    unsigned at=240,seen=0;
    while(at<n) {
        unsigned code=reply[at++];
        if(code==255)return seen==31;
        if(!code)continue;
        if(at==n)return 0;
        unsigned size=reply[at++],bit=0;
        if(size>n-at)return 0;
        const u8 *p=reply+at;
        if(code==53) { bit=1;if(size!=1 || p[0]!=message)return 0; }
        if(code==54) { bit=2;if(size!=4 || !equal(p,server,4))return 0; }
        if(code==1) { bit=4;if(size!=4 || !equal(p,mask,4))return 0; }
        if(code==6) { bit=8;if(size<4 || size%4 || !equal(p,server,4))return 0; }
        if(code==51) { bit=16;if(size!=4 || !(p[0]|p[1]|p[2]|p[3]))return 0; }
        if(seen&bit)return 0;
        seen|=bit;at+=size;
    }
    return 0;
}
static unsigned skip_name(unsigned at,unsigned n)
{
    unsigned labels=0;
    while(at<n && labels++<128) {
        unsigned part=reply[at++];
        if(!part)return at;
        if((part&192)==192) {
            if(at==n)return 0;
            unsigned target=((part&63)<<8)|reply[at];
            return target>=12 && target<at-1 ? at+1 : 0;
        }
        if(part>63 || part>n-at)return 0;
        at+=part;
    }
    return 0;
}
static int dns_valid(unsigned n)
{
    if(n<sizeof(query) || !equal(reply,nonce+4,2) ||
       !(reply[2]&128) || (reply[2]&120) || (reply[2]&2) || (reply[3]&15) ||
       get16(reply+4)!=1 || !equal(reply+12,question,sizeof(question)))return 0;
    unsigned count=get16(reply+6),at=sizeof(query),found=0;
    for(unsigned i=0;i<count;i++) {
        at=skip_name(at,n);
        if(!at || n-at<10)return 0;
        unsigned type=get16(reply+at),klass=get16(reply+at+2),size=get16(reply+at+8);
        at+=10;
        if(size>n-at)return 0;
        if(type==1 && klass==1) { if(size!=4)return 0;found++; }
        at+=size;
    }
    return found!=0;
}
__attribute__((noreturn)) void _start(void)
{
    long udp=-1,raw=-1,dns=-1,rc=-71;
    const char *stage="NONCE";unsigned stage_len=5;
    unsigned used=0;
    while(used<sizeof(nonce)) {
        rc=CALL(3,0,nonce+used,sizeof(nonce)-used,0,0,0);
        if(rc<=0){if(!rc)rc=-71;goto done;}
        used+=(unsigned)rc;
    }
    STAGE("DHCP_SOCKET");udp=CALL(281,2,2,0,0,0,0);if(udp<0){rc=udp;goto done;}
    struct { long sec,usec; } timeout={4,0};
    int yes=1;
    rc=CALL(294,udp,1,20,&timeout,sizeof(timeout),0);if(rc<0)goto done;
    rc=CALL(294,udp,1,6,&yes,sizeof(yes),0);if(rc<0)goto done;
    rc=CALL(294,udp,1,25,"lan1",5,0);if(rc<0)goto done;
    struct inet_addr local={2,0x4400,{0},{0}};
    rc=CALL(282,udp,&local,sizeof(local),0,0,0);if(rc<0)goto done;
    copy(ifreq,(const u8 *)"lan1",5);
    STAGE("INTERFACE");rc=CALL(54,udp,0x8933,ifreq,0,0,0);if(rc<0)goto done;
    int index=(int)((u32)ifreq[16]|(u32)ifreq[17]<<8|(u32)ifreq[18]<<16|(u32)ifreq[19]<<24);
    rc=CALL(54,udp,0x8927,ifreq,0,0,0);if(rc<0)goto done;
    copy(mac,ifreq+18,6);
    STAGE("RAW_DHCP_SOCKET");
    raw=CALL(281,17,3,0x0008,0,0,0);if(raw<0){rc=raw;goto done;}
    struct link_addr link={17,0x0008,index,0,0,6,{255,255,255,255,255,255,0,0}};
    for(unsigned type=1;type<=3;type+=2) {
        if(type==1) { STAGE("DHCP_DISCOVER"); } else { STAGE("DHCP_REQUEST"); }
        make_frame(type);
        rc=CALL(290,raw,frame,sizeof(frame),0,&link,sizeof(link));if(rc!=(long)sizeof(frame))goto done;
        rc=CALL(3,udp,reply,sizeof(reply),0,0,0);if(rc<0)goto done;
        if(!dhcp_valid((unsigned)rc,type==1?2:5)){rc=-71;goto done;}
    }
    PRINT("DHCP_DORA_PASS\n");
    STAGE("DHCP_APPLY");
    for(unsigned i=16;i<sizeof(ifreq);i++)ifreq[i]=0;
    ifreq[16]=2;copy(ifreq+20,lease,4);
    rc=CALL(54,udp,0x8916,ifreq,0,0,0);if(rc<0)goto done;
    copy(ifreq+20,mask,4);rc=CALL(54,udp,0x891c,ifreq,0,0,0);if(rc<0)goto done;
    STAGE("DNS_SOCKET");dns=CALL(281,2,2,0,0,0,0);if(dns<0){rc=dns;goto done;}
    rc=CALL(294,dns,1,20,&timeout,sizeof(timeout),0);if(rc<0)goto done;
    struct inet_addr resolver={2,0x3500,{192,168,0,10},{0}};
    rc=CALL(283,dns,&resolver,sizeof(resolver),0,0,0);if(rc<0)goto done;
    copy(query,nonce+4,2);query[2]=1;query[5]=1;copy(query+12,question,sizeof(question));
    STAGE("DNS_QUERY");rc=CALL(4,dns,query,sizeof(query),0,0,0);if(rc!=(long)sizeof(query))goto done;
    rc=CALL(3,dns,reply,sizeof(reply),0,0,0);if(rc<0)goto done;
    if(!dns_valid((unsigned)rc)){rc=-71;goto done;}
    PRINT("DNS_A_RESPONSE_PASS\n");rc=0;
done:
    if(rc) {
        PRINT("PROTOCOL_TEST_FAILED_STAGE=");
        (void)CALL(4,1,stage,stage_len,0,0,0);PRINT("\n");
    }
    if(udp>=0)(void)CALL(6,udp,0,0,0,0,0);
    if(raw>=0)(void)CALL(6,raw,0,0,0,0,0);
    if(dns>=0)(void)CALL(6,dns,0,0,0,0,0);
    (void)CALL(1,rc?1:0,0,0,0,0,0);
    for(;;){}
}
