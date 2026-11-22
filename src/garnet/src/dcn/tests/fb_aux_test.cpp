#define main protocol_regression_main
#include "aux_transport_test.cpp"
#undef main
#include "dp_binding.h"
#include <mutex>
#include <thread>
#include <map>
#include <string>
using IOReturn = int;
constexpr int kIOReturnSuccess=0, kIOReturnBadArgument=-1;
struct OSSymbol { const char *value=RDNA4_DPCD_FUNCTION; bool isEqualTo(const char *s) const { return !std::strcmp(s,value); } };
struct IOLock { std::mutex mutex; };
static void IOLockLock(IOLock *l) { l->mutex.lock(); }
static void IOLockUnlock(IOLock *l) { l->mutex.unlock(); }
static bool IOLockTryLock(IOLock *l) { return l->mutex.try_lock(); }
static Engine *active;
static uint64_t mach_absolute_time() { return active->time*1000; }
static void absolutetime_to_nanoseconds(uint64_t t, uint64_t *n) { *n=t; }
static void IODelay(uint32_t us) { active->time+=us; }
static void IOSleep(uint32_t ms) { active->time+=ms*1000; }
template<class... Args> static void fbLog(const char *,Args...) {}
#define FBLOG(...) fbLog(__VA_ARGS__)
struct AtomBios {
    enum { MaxDisplayPaths=8, ConnectorDP=1, ConnectorUSBC=2, DisplayObjectInfo=22 };
    struct DisplayPath { uint16_t connectorObjId=0x3113, encoderObjId=0x211e; };
    struct PathRecords { bool hasI2c=true; uint8_t i2cId=0x90; bool i2cHwCapable=true; uint8_t ddcLine=0; bool hasHpd=true; uint8_t hpdPin=3, hpdPlugState=0; };
    struct GpioPin { uint32_t regIndex; uint8_t shift, maskShift, gpioId; };
    unsigned count=1;
    PathRecords record;
    bool badHpd=false, badDdc=false;
    unsigned type=ConnectorDP;
    size_t getDisplayPaths(DisplayPath *p,size_t) { for (unsigned i=0;i<count;++i) p[i]={}; return count; }
    static unsigned connectorType(uint16_t obj) { return (obj & 255)==0x13 ? ConnectorDP : ConnectorUSBC; }
    size_t dataTable(unsigned) { return 0; }
    bool getPathRecords(const DisplayPath &,PathRecords &r) { r=record; return true; }
    bool findGpioPin(uint8_t id,GpioPin &p) {
        if (id==record.hpdPin) p={0x5db5u+(badHpd?1u:0u),16,0,id};
        else if(id==0x90+record.ddcLine) p={0x5d91u+4u*record.ddcLine+(badDdc?1u:0u),8,0,id};
        else return false;
        return true;
    }
};
struct IpDiscovery {
    enum { HwDmu=271 };
    bool isValid() { return true; }
    bool regByteOffset(unsigned,unsigned,unsigned,uint32_t off,uint32_t &out) { out=4*(0x34c0+off); return true; }
};
struct Pci { uint64_t getRegistryEntryID() { return 42; } };
struct Super { static IOReturn callPlatformFunction(const OSSymbol *,bool,void *,void *,void *,void *) { return -2; } };
struct RDNA4FB {
    using super=Super;
    Engine engine;
    IOLock mutex;
    IOLock *auxLock=&mutex;
    bool auxStopping=false;
    uint64_t auxGeneration=1;
    rdna4_aux_session auxSession {};
    AtomBios atomBios;
    IpDiscovery ipDiscovery;
    Pci pci;
    Pci *pciDevice=&pci;
    bool rmmio=true;
    bool displaySleepEnabled=true, displayPowerOn=true, sinkAuxValid=true, testPower=false;
    uint8_t sinkAuxInst=0;
    uint32_t stream=1;
    std::map<std::string,rdna4_dpcd_snapshot> published;
    std::array<uint8_t,24> bios {{24,0,1,5,0,0,1,0,0x13,0x31,0,0,0x1e,0x21}};
    uint8_t *vbiosData=bios.data();
    size_t vbiosSize=bios.size();
    std::map<uint32_t,uint32_t> source;
    unsigned mutateAfter=0;
    uint32_t mutateOffset=0, mutateBits=1;
    bool boundPublished=false;
    rdna4_bound_snapshot boundProperty {};
    RDNA4FB() {
        active=&engine;
        for(unsigned i=0;i<4;++i) {
            source[0x1b43+i*0x80]=0; source[0x179a+3*i]=0; source[0x179b+3*i]=0;
            for(auto off:rdna4_binding_timing_offsets) source[off+i*0x80]=0;
            for(auto off:rdna4_binding_stream_offsets) source[off+i*0x124]=0;
            for(auto off:rdna4_binding_link_offsets) source[off+i*0x124]=0;
        }
        source[0x1b43+2*0x80]=1;
        source[0x2122+3*0x124]=0x10001;
        source[0x2093+3*0x124]=2;
        source[0x2094+3*0x124]=0x10;
        source[0x2095+3*0x124]=1;
        source[0x20bc]=0x20000800;
        source[0x20bb]=0x10; source[0x20bd]=1;
    }
    uint32_t regReadDmu(unsigned,uint32_t off) {
        if(source.count(off)) {
            uint32_t v=off==0x2122 ? stream : source[off];
            return mutateAfter && engine.requests>=mutateAfter && off==mutateOffset ? v^mutateBits : v;
        }
        return Engine::read(&engine,off);
    }
    bool regWriteDmu(unsigned,uint32_t off,uint32_t v) {
        if(testPower) {
            std::thread competitor([&] { check(query().status==RDNA4_AUX_BUSY,"actual power sequence retains software owner at every write"); });
            competitor.join();
        }
        if(off==0x2122) { stream=v; return true; }
        return Engine::write(&engine,off,v);
    }
    void removeProperty(const char *key) { published.erase(key); if(!std::strcmp(key,"RDNA4,DPCD-Console")) boundPublished=false; }
    void setProperty(const char *key,const void *v,size_t n) {
        if(!std::strcmp(key,"RDNA4,DPCD-Console")) { check(n==sizeof(rdna4_bound_snapshot),"bound plain C size"); boundProperty=*static_cast<const rdna4_bound_snapshot *>(v); boundPublished=true; }
        else { check(n==sizeof(rdna4_dpcd_snapshot),"export exact plain C size"); published[key]=*static_cast<const rdna4_dpcd_snapshot *>(v); }
    }
    static uint32_t auxRead(void *,uint32_t);
    static int auxWrite(void *,uint32_t,uint32_t);
    static uint64_t auxNow(void *);
    static void auxDelay(void *,uint32_t);
    bool auxRoute(uint32_t,rdna4_dpcd_snapshot &);
    bool beginAux(uint8_t,uint64_t);
    int auxTransaction(uint8_t,uint8_t,uint32_t,const uint8_t *,uint8_t,uint8_t *,uint8_t,uint8_t *);
    void invalidateDpcd();
    void boundDpcd(const rdna4_bound_request &,rdna4_bound_snapshot &);
    IOReturn callPlatformFunction(const OSSymbol *,bool,void *,void *,void *,void *);
    bool readEDID(uint8_t,uint8_t *,size_t,uint16_t);
    void setDisplayPower(bool);
    rdna4_bound_snapshot boundQuery(unsigned dp=3,unsigned dig=0,unsigned otg=2) {
        OSSymbol symbol {RDNA4_DPCD_BOUND_FUNCTION};
        rdna4_bound_request request {1,sizeof(request),dp,dig,otg,0,42,engine.time+50000};
        rdna4_bound_snapshot out {};
        check(!callPlatformFunction(&symbol,false,&request,&out,nullptr,nullptr),"actual bound service callable");
        return out;
    }
    rdna4_dpcd_snapshot query() {
        OSSymbol symbol;
        rdna4_dpcd_request req {1,sizeof(req),0,0,42,engine.time+50000};
        rdna4_dpcd_snapshot out {};
        check(callPlatformFunction(&symbol,false,&req,&out,nullptr,nullptr)==0,"production export callable");
        return out;
    }
};
#include "fb_aux_actual.inc"
int main() {
    { RDNA4FB f; auto out=f.query();
      check(out.status==RDNA4_AUX_OK && out.aux==0 && out.hpd==2 && out.pciRegistryId==42 && out.generation==1,"actual export pins VBIOS AUX0 to HPD2 same PCI");
      check(f.published.size()==1 && f.engine.requests==4,"actual export publishes actual fixed probe");
      f.invalidateDpcd(); check(f.published.empty() && f.auxGeneration==2,"power/stop invalidation drops properties and advances generation"); }
    { RDNA4FB f; f.engine.inst=1; f.atomBios.record.ddcLine=1; f.atomBios.record.i2cId=0x91; check(f.query().status==RDNA4_AUX_OK,"VBIOS AUX1 independent of HPD2"); }
    { RDNA4FB f; f.atomBios.record.i2cId=0x80; check(f.query().status==RDNA4_AUX_UNAVAILABLE && !f.engine.grants,"raw I2C engine ID must match actual LUT entry"); }
    { RDNA4FB f; f.atomBios.badHpd=true; check(f.query().status==RDNA4_AUX_UNAVAILABLE && !f.engine.grants,"HPD LUT mismatch refuses MMIO"); }
    { RDNA4FB f; f.atomBios.badDdc=true; check(f.query().status==RDNA4_AUX_UNAVAILABLE && !f.engine.grants,"DDC LUT mismatch refuses MMIO"); }
    { RDNA4FB f; f.atomBios.record.hasHpd=false; check(f.query().status==RDNA4_AUX_UNAVAILABLE && !f.engine.grants,"missing record refuses MMIO"); }
    { RDNA4FB f; f.atomBios.count=2; check(f.query().status==RDNA4_AUX_UNAVAILABLE && !f.engine.grants,"duplicate AUX/HPD mapping refuses MMIO"); }
    { RDNA4FB f; f.auxStopping=true; check(f.query().status==RDNA4_AUX_UNAVAILABLE && !f.engine.grants,"stop blocks retained clients"); }
    { RDNA4FB f; f.engine.hpd=1; check(f.query().status!=RDNA4_AUX_OK && !f.engine.grants,"hardware mux differs from VBIOS mapping"); }
    { RDNA4FB f; OSSymbol name; rdna4_dpcd_request req {1,sizeof(req),0,0,43,50100}; rdna4_dpcd_snapshot out {};
      check(!f.callPlatformFunction(&name,false,&req,&out,nullptr,nullptr) && out.status==RDNA4_AUX_INVALID && !f.engine.reads,"actual service rejects other PCI before MMIO"); }
    { RDNA4FB f;
      // Actual mutex owner spans all EDID MOT calls; another thread cannot query.
      FbAuxLock owner(f.auxLock);
      std::thread competitor([&] { check(f.query().status==RDNA4_AUX_BUSY,"DPCD cannot interrupt EDID/power owner"); }); competitor.join();
      uint8_t edid[128]; check(f.readEDID(0,edid,128,0),"actual EDID uses shared protocol grant");
      check(f.engine.requests==9 && f.engine.grants==1 && f.engine.releases==1,"actual EDID whole MOT grant"); }
    { RDNA4FB f; check(f.query().status==RDNA4_AUX_OK,"snapshot before power");
      f.testPower=true; f.setDisplayPower(false);
      check(!f.displayPowerOn && !f.stream && f.published.empty() && f.auxGeneration==2,"actual power invalidates snapshots and retains legacy behavior");
      check(f.engine.grants==2 && f.engine.releases==2,"actual power uses a complete HW session"); }
    { RDNA4FB f; f.engine.inst=1; f.atomBios.record.ddcLine=1; f.atomBios.record.i2cId=0x91;
      const auto out=f.boundQuery();
      check(out.binding==RDNA4_BIND_OK && out.link.status==RDNA4_AUX_OK && out.link.aux==1 && out.link.hpd==2 && out.dp==3 && out.dig==0 && out.otg==2,"actual FE3 BE0 OTG2 AUX1 HPD2 proven without equal indices");
      check(f.engine.requests==4 && f.engine.grants==1 && f.engine.releases==1 && f.boundPublished,"bound fixed probe owns one full grant and publishes proof");
      f.invalidateDpcd(); check(!f.boundPublished,"bound property invalidates on power/stop"); }
    { RDNA4FB f; f.source[0x20bc]=0x10000800; const auto out=f.boundQuery();
      check(out.binding==RDNA4_BIND_HPD && !f.engine.grants,"BE HPD mismatch refuses before GO"); }
    { RDNA4FB f; f.source[0x20bc]|=4; const auto out=f.boundQuery();
      check(out.binding==RDNA4_BIND_MUX && !f.engine.grants,"unproved RB switch refuses before GO"); }
    { RDNA4FB f; f.bios[14]=1; const auto out=f.boundQuery();
      check(out.binding==RDNA4_BIND_VBIOS && !f.engine.grants,"reserved/external path refuses before GO"); }
    { RDNA4FB f; f.atomBios.count=2; const auto out=f.boundQuery();
      check(out.binding!=RDNA4_BIND_OK && !f.engine.grants,"ambiguous VBIOS routes refuse"); }
    { RDNA4FB f; f.source[0x1b43]=1; const auto out=f.boundQuery();
      check(out.binding==RDNA4_BIND_AMBIGUOUS && !f.engine.grants,"multiple lit OTGs refuse"); }
    { RDNA4FB f; f.source[0x2094+3*0x124]=0x15; const auto out=f.boundQuery();
      check(out.binding==RDNA4_BIND_SOURCE && !f.engine.grants,"MST source refuses"); }
    { RDNA4FB f; f.mutateAfter=1; f.mutateOffset=0x20bc; f.mutateBits=0x10000000;
      const auto out=f.boundQuery(); check(out.binding==RDNA4_BIND_CHANGED && out.link.status!=RDNA4_AUX_OK && !out.link.config[0],"HPD source mux change inside grant discards bytes"); }
    { RDNA4FB f; f.mutateAfter=1; f.mutateOffset=0x1b2f+2*0x80;
      const auto out=f.boundQuery(); check(out.binding==RDNA4_BIND_CHANGED && out.link.status!=RDNA4_AUX_OK,"source geometry change inside grant refused"); }
    { RDNA4FB f; f.engine.sense|=0x100;
      const auto out=f.boundQuery(); check(out.link.status==RDNA4_AUX_CHANGED && !f.engine.grants,"hotplug/RX event refuses bound probe"); }
    { RDNA4FB f; f.source[0x179a]=2;
      const auto out=f.boundQuery(); check(out.binding==RDNA4_BIND_MUX && out.phyMux==2 && !f.engine.grants,"HPO PHY mux cannot impersonate stale legacy BE"); }
    { RDNA4FB f; f.source[0x179b]=1;
      const auto out=f.boundQuery(); check(out.binding==RDNA4_BIND_MUX && !f.engine.grants,"unknown port flag refuses"); }
    { RDNA4FB f; f.mutateAfter=1; f.mutateOffset=0x179a;
      const auto out=f.boundQuery(); check(out.binding==RDNA4_BIND_CHANGED && out.link.status!=RDNA4_AUX_OK,"PHY mux switch during grant invalidates proof"); }
    { RDNA4FB f; f.source[0x179a]=0xffffffffu;
      const auto out=f.boundQuery(); check(out.binding==RDNA4_BIND_UNREADABLE && !f.engine.grants,"unreadable PHY mux refuses"); }
    { RDNA4FB f; f.engine.hotplugAt=1;
      const auto out=f.boundQuery(); check(out.link.status==RDNA4_AUX_CHANGED && !out.link.config[0] && f.engine.requests==1,"hotplug during native transaction discards bound bytes"); }
    { RDNA4FB f; f.source[0x20bc]=0xffffffffu;
      const auto out=f.boundQuery(); check(out.binding==RDNA4_BIND_UNREADABLE && !f.engine.grants,"all-ones source route refuses"); }
    std::printf("fb_aux_actual: %u checks PASS\n",checks);
}
