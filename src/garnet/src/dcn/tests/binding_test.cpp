#include "dp_binding.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static unsigned checks;
static void expect(bool ok,const char *s) { ++checks; if(!ok) { std::fprintf(stderr,"FAIL %s\n",s); std::abort(); } }
static rdna4_binding_raw route(unsigned dp,unsigned dig,unsigned otg,unsigned hpd) {
    rdna4_binding_raw r {};
    r.otg[otg]=1;
    r.stream[dp][0]=0x10001;
    r.stream[dp][5]=otg; r.stream[dp][6]=0x10; r.stream[dp][7]=1;
    r.link[dig][0]=(1u<<(dp+8)) | (hpd<<28);
    r.link[dig][1]=0x10; r.link[dig][2]=1;
    return r;
}
int main() {
    const unsigned encoders[]={0x211e,0x221e,0x2120,0x2220};
    for(unsigned dp=0;dp<4;++dp) for(unsigned dig=0;dig<4;++dig)
    for(unsigned otg=0;otg<4;++otg) for(unsigned aux=0;aux<4;++aux) for(unsigned hpd=0;hpd<4;++hpd) {
        auto r=route(dp,dig,otg,hpd);
        rdna4_dpcd_snapshot p {}; p.connectorObj=0x3113; p.encoderObj=encoders[dig]; p.aux=aux; p.hpd=hpd; p.directDp=1;
        expect(rdna4_binding_check(&r,dp,dig,otg,&p)==RDNA4_BIND_OK,"independent FE BE OTG AUX HPD proven");
        p.encoderObj=encoders[(dig+1)%4]; expect(rdna4_binding_check(&r,dp,dig,otg,&p)==RDNA4_BIND_ENCODER,"encoder must prove physical BE"); p.encoderObj=encoders[dig];
        p.hpd=(hpd+1)%4; expect(rdna4_binding_check(&r,dp,dig,otg,&p)==RDNA4_BIND_HPD,"source HPD must prove connector"); p.hpd=hpd;
        p.directDp=0; expect(rdna4_binding_check(&r,dp,dig,otg,&p)==RDNA4_BIND_VBIOS,"unproved direct path refuses"); p.directDp=1;
        for(unsigned bit=1;bit<=4;bit<<=1) { auto q=r; q.link[dig][0]|=bit; expect(rdna4_binding_check(&q,dp,dig,otg,&p)==RDNA4_BIND_MUX,"unknown dual swap RB-switch refuse"); expect(!rdna4_binding_same(&r,&q),"mux change is source witness change"); }
        for(unsigned mux=1;mux<=3;++mux) { auto q=r; q.mux[dig][0]=mux; expect(rdna4_binding_check(&q,dp,dig,otg,&p)==RDNA4_BIND_MUX,"non-legacy PHY mux refuses"); expect(!rdna4_binding_same(&r,&q),"PHY mux change is source witness change"); }
        auto q=r; q.link[(dig+1)%4][2]=1; q.link[(dig+1)%4][0]=r.link[dig][0];
        expect(rdna4_binding_check(&q,dp,dig,otg,&p)==RDNA4_BIND_AMBIGUOUS,"multiple BE owners refuse");
        q=r; q.otg[(otg+1)%4]=1; expect(rdna4_binding_check(&q,dp,dig,otg,&p)==RDNA4_BIND_AMBIGUOUS,"ambiguous console OTG refuses");
        q=r; q.link[dig][0]^=0x10000000; expect(!rdna4_binding_same(&r,&q),"HPD epoch witness changes");
        q=r; q.timing[otg][1]^=1; expect(!rdna4_binding_same(&r,&q),"geometry epoch witness changes");
    }
    for(unsigned id : {0u,0x211fu,0x231eu,0x211eu+0x10000u,0x2121u,0x3120u}) expect(rdna4_binding_encoder(id)==0xffffffffu,"unknown encoder has no inferred BE");
    std::vector<uint8_t> rom(24); rom[0]=24; rom[2]=1; rom[3]=5; rom[6]=1;
    rom[8]=0x13; rom[9]=0x31; rom[12]=0x1e; rom[13]=0x21;
    expect(rdna4_binding_direct_path(rom.data(),rom.size(),0,0x3113,0x211e),"exact direct ATOM v1.5 path accepted");
    for(unsigned b : {14u,15u,16u,17u,18u,19u,22u,23u}) { rom[b]=1; expect(!rdna4_binding_direct_path(rom.data(),rom.size(),0,0x3113,0x211e),"external/reserved path refuses"); rom[b]=0; }
    rom[3]=4; expect(!rdna4_binding_direct_path(rom.data(),rom.size(),0,0x3113,0x211e),"legacy layout not guessed"); rom[3]=5;
    for(size_t bytes=0;bytes<24;++bytes) expect(!rdna4_binding_direct_path(rom.data(),bytes,0,0x3113,0x211e),"all truncated table lengths refuse");
    expect(!rdna4_binding_direct_path(rom.data(),rom.size(),SIZE_MAX,0x3113,0x211e),"offset overflow refuses");
    std::printf("binding: %u checks PASS\n",checks);
}
