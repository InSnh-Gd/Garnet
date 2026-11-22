#include "dp_binding.h"
#include "dp_dpcd.h"
#include "dp_link.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static unsigned checks, logs;
static void check(bool ok,const char *text) { ++checks; if(!ok) { std::fprintf(stderr,"FAIL %s\n",text); std::abort(); } }
static uint64_t clockUs=100;
static uint32_t sourceRegs[0x10000], mutateSourceOffset=0;
static rdna4lr_ro gLr;
static uint32_t sourceRead(void *,uint32_t off) { return off<0x10000 ? sourceRegs[off] : 0xffffffffu; }
static uint64_t now_ns() { return clockUs*1000; }
template<class... Args> static void logLine(const char *, Args...) { ++logs; }
#define RDNA4LOG(...) logLine(__VA_ARGS__)
using IOReturn=int;
constexpr int kIOReturnSuccess=0, gIOServicePlane=0;
struct OSSymbol {
    const char *name;
    static const OSSymbol *withCString(const char *s) { check(!std::strcmp(s,RDNA4_DPCD_FUNCTION) || !std::strcmp(s,RDNA4_DPCD_BOUND_FUNCTION),"actual bridge requests versioned function"); static OSSymbol symbol; symbol.name=s; return &symbol; }
    void release() const {}
};
struct Object { virtual ~Object()=default; };
struct OSIterator : Object {
    std::vector<Object *> children;
    size_t i=0;
    Object *getNextObject() { return i<children.size()?children[i++]:nullptr; }
    void release() { delete this; }
};
struct IOService : Object {
    IOService *provider=nullptr;
    unsigned retained=0, calls=0;
    bool isFb=false, malformed=false, wrongPci=false, stale=false, late=false;
    uint8_t rateCode=0x14, laneCount=4;
    unsigned boundFail=RDNA4_AUX_OK;
    IOService *getProvider() { return provider; }
    bool metaCast(const char *name) { return isFb && !std::strcmp(name,"RDNA4FB"); }
    void retain() { ++retained; }
    void release() { check(retained>0,"retained provider reference released"); --retained; }
    IOReturn callPlatformFunction(const OSSymbol *symbol,bool wait,void *p1,void *p2,void *p3,void *p4) {
        check(retained==1 && !wait && !p3 && !p4,"actual synchronous call owns provider reference"); ++calls;
        if(!std::strcmp(symbol->name,RDNA4_DPCD_BOUND_FUNCTION)) {
            const auto &req=*static_cast<rdna4_bound_request *>(p1);
            check(req.pciRegistryId==42 && req.dp==3 && req.dig==0 && req.otg==2,"actual bound bridge sends observed route, not AUX index");
            auto &out=*static_cast<rdna4_bound_snapshot *>(p2); out={};
            out.version=1; out.size=sizeof(out); out.binding=RDNA4_BIND_OK;
            out.dp=req.dp; out.dig=req.dig; out.otg=req.otg;
            out.beControl=0x20000800; out.feControl=2;
            auto &link=out.link; link.version=1; link.size=sizeof(link); link.pciRegistryId=wrongPci?43:42;
            link.connector=1; link.connectorObj=0x3113; link.encoderObj=0x211e; link.directDp=1; link.aux=malformed?4:1; link.hpd=2;
            link.generation=1; link.sampledUs=stale?99:100;
            link.config[0]=rateCode; link.config[1]=0x80|laneCount; link.config[8]=1;
            link.lanes[0]=link.lanes[1]=0x77; link.lanes[2]=1;
            if(boundFail) { link.status=boundFail; out.binding=RDNA4_BIND_TRANSPORT; }
            if(mutateSourceOffset) sourceRegs[mutateSourceOffset]^=1;
            if(late) clockUs=50100;
            return 0;
        }
        const auto &req=*static_cast<rdna4_dpcd_request *>(p1);
        check(req.pciRegistryId==42 && req.deadlineUs==50100,"actual bridge sends same PCI and absolute deadline");
        auto &out=*static_cast<rdna4_dpcd_snapshot *>(p2);
        out={}; out.version=1; out.size=sizeof(out); out.connector=req.connector; out.status=RDNA4_AUX_OK;
        out.pciRegistryId=wrongPci?43:42; out.generation=1; out.sampledUs=stale?99:100;
        out.aux=malformed?4:1; out.hpd=2;
        out.config[0]=0x14; out.config[1]=0x84; out.config[8]=1;
        out.lanes[0]=out.lanes[1]=0x77; out.lanes[2]=1;
        if(late) clockUs=50100;
        return 0;
    }
};
struct IOPCIDevice : IOService {
    std::vector<Object *> children;
    uint64_t getRegistryEntryID() { return 42; }
    OSIterator *getChildIterator(int) { auto *it=new OSIterator; it->children=children; return it; }
};
#define OSDynamicCast(type,obj) dynamic_cast<type *>(obj)
struct { IOService *owner=nullptr; } gDcn;
#include "dpcd_bridge_actual.inc"
#include "dpcd_snapshot_actual.inc"
static void setupSource() {
    std::memset(sourceRegs,0,sizeof(sourceRegs));
    const uint32_t bases[]={0x12,0xc0,0x34c0,0x9000,0x2403c00};
    check(rdna4lr_ro_build(&gLr,nullptr,sourceRead,bases,0x10000),"production read-only source builds");
    const unsigned f=0x34c0+3*0x124, b=0x34c0;
    sourceRegs[0x34c0+0x1b43+2*0x80]=1;
    sourceRegs[f+0x2122]=0x10001; sourceRegs[f+0x211f]=0x200; sourceRegs[f+0x2120]=0x40000000;
    sourceRegs[f+0x2093]=2; sourceRegs[f+0x2094]=0x10; sourceRegs[f+0x2095]=1;
    sourceRegs[b+0x20bc]=0x20000800; sourceRegs[b+0x20bb]=0x10; sourceRegs[b+0x20bd]=1;
    sourceRegs[b+0x211e]=0x110; sourceRegs[b+0x2121]=3;
}
int main() {
    IOPCIDevice pci, other;
    IOService owner,fb;
    owner.provider=&pci; fb.provider=&pci; fb.isFb=true;
    pci.children={&fb}; gDcn.owner=&owner;
    rdna4_dpcd_snapshot out {};
    check(dpConnectorSnapshot(0,&out,50100)==RDNA4_AUX_OK && fb.calls==1 && !fb.retained,"production bridge calls sibling FB and releases");
    check(dpGatherUnbound() && logs==8 && fb.calls==9,"production gather queries and logs every connector");
    check(dpConnectorSnapshot(8,&out,50100)==RDNA4_AUX_INVALID && fb.calls==9,"invalid connector refused before provider");
    fb.provider=&other; check(dpConnectorSnapshot(0,&out,50100)==RDNA4_AUX_UNAVAILABLE && fb.calls==9,"other PCI FB never called"); fb.provider=&pci;
    IOService fb2; fb2.isFb=true; fb2.provider=&pci; pci.children.push_back(&fb2);
    check(dpConnectorSnapshot(0,&out,50100)==RDNA4_AUX_UNAVAILABLE && !fb.retained && !fb2.calls,"ambiguous provider refuses"); pci.children.pop_back();
    fb.wrongPci=true; check(dpConnectorSnapshot(0,&out,50100)==RDNA4_AUX_INVALID,"provider PCI identity rechecked"); fb.wrongPci=false;
    fb.malformed=true; check(dpConnectorSnapshot(0,&out,50100)==RDNA4_AUX_INVALID,"provider engine bounds rechecked"); fb.malformed=false;
    fb.stale=true; check(dpConnectorSnapshot(0,&out,50100)==RDNA4_AUX_INVALID,"stale provider timestamp refused"); fb.stale=false;
    fb.late=true; check(dpConnectorSnapshot(0,&out,50100)==RDNA4_AUX_INVALID,"provider late return refused");
    clockUs=100; fb.late=false;
    rdna4_bound_snapshot bound {};
    check(dpBoundSnapshot(3,0,2,&bound,50100)==RDNA4_AUX_OK && !fb.retained,"actual bound bridge retains same PCI provider");
    auto link=dpObserveBound(nullptr,{3,0,2});
    check(link.status==rdna4dp::SinkStatus::observed && link.configuredRateKbps==5400000 && link.clockRecovery==2 && link.channelEq==2,"actual bound observer decodes physical sink configured and training status");
    fb.wrongPci=true; check(dpBoundSnapshot(3,0,2,&bound,50100)==RDNA4_AUX_INVALID,"bound output PCI rechecked"); fb.wrongPci=false;
    fb.malformed=true; check(dpBoundSnapshot(3,0,2,&bound,50100)==RDNA4_AUX_INVALID,"bound output mux/engine rechecked"); fb.malformed=false;
    fb.stale=true; check(dpBoundSnapshot(3,0,2,&bound,50100)==RDNA4_AUX_INVALID,"bound stale observation refused"); fb.stale=false;
    fb.late=true; check(dpBoundSnapshot(3,0,2,&bound,50100)==RDNA4_AUX_INVALID,"bound late observation refused");
    clockUs=100; fb.late=false;
    const rdna4dp::Timing timing {2560,1440,2720,1525,497750000};
    for(uint8_t code : {uint8_t(6),uint8_t(10),uint8_t(20),uint8_t(30)}) {
        setupSource(); fb.rateCode=code;
        rdna4dp::Snapshot snapshot; rdna4dp::Bandwidth bandwidth;
        check(dpPreflight(2,&timing,&snapshot,&bandwidth,nullptr)==1 && snapshot.dp==3 && snapshot.dig==0 && snapshot.hpd==2 && snapshot.rateFromSinkStatus==1 && snapshot.coding==8 && snapshot.sink.connector==1 && snapshot.sink.aux==1 && snapshot.sink.hpd==2 && snapshot.sink.encoderObj==0x211e,"actual default production snapshot uses bound backend on independent indices");
        check(snapshot.rateKbps==uint32_t(code)*270000 && bandwidth.requiredBps==14932500000ull && bandwidth.payloadBps==uint64_t(snapshot.rateKbps)*3200,"actual exact target bandwidth uses bound RBR/HBR/HBR2/HBR3");
        check(bandwidth.decision==(code>=20?rdna4dp::Decision::fitsPayload:rdna4dp::Decision::exceedsPayload),"actual bandwidth decision on proven sink rate");
        std::printf("production default DP%u BE%u OTG%u connector=%u AUX%u HPD%u rate-kbps=%u coding=%u demand=%llu capacity=%llu decision=%s\n",
            snapshot.dp,snapshot.dig,snapshot.otg,snapshot.sink.connector,snapshot.sink.aux,snapshot.sink.hpd,snapshot.rateKbps,snapshot.coding,
            (unsigned long long)bandwidth.requiredBps,(unsigned long long)bandwidth.payloadBps,rdna4dp::decisionName(bandwidth.decision));
    }
    setupSource(); fb.rateCode=20; fb.laneCount=2;
    rdna4dp::Snapshot snapshot; rdna4dp::Bandwidth bandwidth;
    check(dpPreflight(2,&timing,&snapshot,&bandwidth,nullptr)==1 && !snapshot.rateFromSinkStatus && bandwidth.decision==rdna4dp::Decision::unknown,"source/sink lane mismatch cannot promote rate"); fb.laneCount=4;
    for(uint32_t off : {0x34c0u+0x20bcu,0x34c0u+0x1b2fu+2*0x80u,0x34c0u+0x211fu+3*0x124u}) {
        setupSource(); mutateSourceOffset=off;
        check(dpSnapshot(2,&snapshot,nullptr)==0 && snapshot.status==rdna4dp::Status::changed && !snapshot.rateFromSinkStatus && !snapshot.rateKbps,"source mux/geometry/format change after bound service rejects snapshot");
    }
    mutateSourceOffset=0; setupSource(); fb.boundFail=RDNA4_AUX_CHANGED;
    check(dpPreflight(2,&timing,&snapshot,&bandwidth,nullptr)==1 && snapshot.sink.status==rdna4dp::SinkStatus::changed && !snapshot.rateFromSinkStatus && bandwidth.decision==rdna4dp::Decision::unknown,"hotplug transport failure cannot supply console rate");
    std::printf("dpcd_bridge_actual: %u checks PASS\n",checks);
}
