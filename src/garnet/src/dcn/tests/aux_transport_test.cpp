#include "dp_aux_transport.h"
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned checks;
static void check(bool ok, const char *what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); std::abort(); }
}
struct Engine {
    unsigned inst = 0, hpd = 2, requests = 0, grants = 0, releases = 0, reads = 0;
    uint32_t ctl = 1, arb = 0, sw = 0, sts = 0, sense = 0x12;
    uint64_t time = 100;
    bool noGrant = false, loseOwner = false, releaseFail = false, timeout = false;
    bool shortReply = false, overflow = false, allOnes = false, bytesFF = false, changed = false;
    unsigned defers = 0, replyCode = 0, lateAt = 0, failReadAt = 0, hotplugAt = 0;
    uint32_t faultBits = 0;
    std::vector<uint8_t> fifo, response;
    size_t cursor = 0;
    std::array<uint8_t,9> config {{0x14,0x84,0,0,0,0,0,0,1}};
    std::array<uint8_t,6> lanes {{0x77,0x77,1,0,0,0}};
    static uint32_t read(void *p, uint32_t off) {
        auto &m = *static_cast<Engine *>(p);
        ++m.reads;
        if (m.allOnes || (m.failReadAt && m.reads == m.failReadAt)) return 0xffffffffu;
        if (off == 0x175a + 8*m.hpd) return m.sense;
        check(off >= 0x16b2 + m.inst*0x1c && off <= 0x16b8 + m.inst*0x1c, "only selected AUX/HPD registers read");
        switch (off - 0x16b2 - m.inst*0x1c) {
        case 0: return m.ctl | (m.hpd << 20);
        case 1: return m.sw;
        case 2: return m.loseOwner && m.requests ? 8 : m.arb;
        case 4: return m.sts;
        case 6: check(m.cursor < m.response.size(), "FIFO never overread"); return uint32_t(m.response[m.cursor++]) << 8;
        default: check(false, "unexpected read"); return 0;
        }
    }
    static int write(void *p, uint32_t off, uint32_t value) {
        auto &m = *static_cast<Engine *>(p);
        check(off >= 0x16b3 + m.inst*0x1c && off <= 0x16b8 + m.inst*0x1c, "never reset/enable/mux HPD");
        const uint32_t reg = off - 0x16b2 - m.inst*0x1c;
        if (reg == 2) {
            if (value & 0x10000) {
                ++m.grants; m.arb = m.noGrant ? 0x10000 : 0x10004;
            } else {
                ++m.releases;
                if (m.releaseFail) return 0;
                m.arb = 0;
            }
            return 1;
        }
        check((m.arb & 0xc) == 4, "FIFO/status writes require owned grant");
        if (reg == 3) { check(value == 2, "only SW done acknowledged"); m.sts = 0; return 1; }
        if (reg == 6) {
            if (value == 0x80000001u) { m.cursor = 0; return 1; }
            if (value & 0x80000000) m.fifo.clear();
            m.fifo.push_back(uint8_t(value >> 8)); return 1;
        }
        check(reg == 1, "only supported AUX writes");
        m.sw = value;
        if (!(value & 1)) return 1;
        ++m.requests;
        if(m.hotplugAt==m.requests) m.sense|=0x100;
        check(m.fifo.size() == ((value >> 16) & 31), "WR_BYTES matches actual FIFO");
        check(m.fifo.size() >= 3, "complete command header");
        const uint8_t action = m.fifo[0] & 0xf0;
        const unsigned address = (unsigned(m.fifo[0] & 15) << 16) | (unsigned(m.fifo[1]) << 8) | m.fifo[2];
        const unsigned count = m.fifo.size() >= 4 ? m.fifo[3]+1 : 0;
        unsigned reply = m.replyCode;
        if (m.defers) { --m.defers; reply = 2; }
        m.response.assign(1, uint8_t(reply << 4));
        if (!reply && (action & 0x10)) {
            if (action == 0x90) {
                check((address == 0x100 && count == 9) || (address == 0x202 && count == 6), "native reads have exact fixed address/count");
                for (unsigned i = 0; i < count; ++i) m.response.push_back(m.bytesFF ? 255 : address == 0x100 ? m.config[i] : m.lanes[i]);
                if (m.changed && m.requests == 3) m.response[1] ^= 1;
            } else m.response.resize(1+count, 0x5a);
        }
        if (m.shortReply && m.response.size() > 1) m.response.pop_back();
        m.sts = m.timeout ? 0 : 1 | (uint32_t(m.overflow ? 18 : m.response.size()) << 24) | m.faultBits;
        if (!m.timeout) m.sw &= ~1u;
        if (m.lateAt == m.requests) m.time = 50100;
        return 1;
    }
    static uint64_t now(void *p) { return static_cast<Engine *>(p)->time; }
    static void delay(void *p, uint32_t us) { static_cast<Engine *>(p)->time += us; }
    rdna4_aux_session session() { return {{this,read,write,now,delay},inst,hpd,0,0,0,0,50100}; }
    uint32_t probe(rdna4_dpcd_snapshot &out) { auto s = session(); return rdna4_aux_dpcd_probe(&s, &out); }
};
int main() {
    rdna4_dpcd_snapshot out {};
    for (unsigned inst = 0; inst < 4; ++inst) {
        Engine m; m.inst = inst;
        check(m.probe(out) == RDNA4_AUX_OK, "all engines work independently of HPD index");
        check(m.requests == 4 && m.grants == 1 && m.releases == 1, "one grant spans both stable passes");
        check(!memcmp(out.config,m.config.data(),9) && !memcmp(out.lanes,m.lanes.data(),6), "raw DPCD data preserved");
    }
    { Engine m; m.defers = 2; check(m.probe(out) == RDNA4_AUX_OK && m.requests == 6, "bounded DEFER then ACK"); }
    { Engine m; m.defers = 3; check(m.probe(out) == RDNA4_AUX_DEFER && m.requests == 3 && m.releases == 1, "DEFER exhausted releases once"); }
    { Engine m; m.replyCode = 1; check(m.probe(out) == RDNA4_AUX_NACK, "native NACK"); }
    { Engine m; m.shortReply = true; check(m.probe(out) == RDNA4_AUX_SHORT, "short ACK refused"); }
    { Engine m; m.overflow = true; check(m.probe(out) == RDNA4_AUX_SHORT, "oversized reply refused"); }
    { Engine m; m.allOnes = true; check(m.probe(out) == RDNA4_AUX_IO && !m.grants, "all ones MMIO never written"); }
    { Engine m; m.bytesFF = true; check(m.probe(out) == RDNA4_AUX_INVALID, "all ones DPCD refused"); }
    { Engine m; m.changed = true; check(m.probe(out) == RDNA4_AUX_CHANGED, "config transition refused"); }
    { Engine m; m.sense = 0; check(m.probe(out) == RDNA4_AUX_DISCONNECTED && !m.grants, "HPD low refuses acquisition"); }
    { Engine m; m.sense |= 0x100; check(m.probe(out) == RDNA4_AUX_CHANGED && !m.grants, "pending IRQ no ack"); }
    { Engine m; m.ctl |= 0x10000; check(m.probe(out) == RDNA4_AUX_INVALID && !m.grants, "ignore HPD forbidden"); }
    { Engine m; m.ctl = 0; check(m.probe(out) == RDNA4_AUX_INVALID && !m.grants, "disabled AUX never enabled"); }
    { Engine m; m.ctl |= 0x20; check(m.probe(out) == RDNA4_AUX_INVALID, "reset done state refused"); }
    { Engine m; m.arb = 8; check(m.probe(out) == RDNA4_AUX_BUSY && !m.grants, "firmware owner never stolen"); }
    { Engine m; m.sw = 1; check(m.probe(out) == RDNA4_AUX_BUSY && !m.grants, "inflight engine never reset"); }
    { Engine m; m.noGrant = true; check(m.probe(out) == RDNA4_AUX_BUSY && m.releases == 1, "ungranted request withdrawn"); }
    { Engine m; m.loseOwner = true; check(m.probe(out) == RDNA4_AUX_CHANGED && !m.releases, "new firmware owner never released"); }
    { Engine m; m.releaseFail = true; check(m.probe(out) == RDNA4_AUX_IO && !out.config[0], "release failure never publishes bytes"); }
    { Engine m; m.timeout = true; check(m.probe(out) == RDNA4_AUX_TIMEOUT && m.releases == 1, "timeout cleanup without reset"); }
    for (unsigned at = 1; at <= 4; ++at) { Engine m; m.lateAt = at; check(m.probe(out) == RDNA4_AUX_TIMEOUT, "absolute deadline rejects late ACK each pass"); }
    for (uint32_t bits : {0x100u,0x400u,0x800u,0x1000u,0x4000u,0x20000u,0x40000u,0x80000u,0x100000u,0x400000u,0x800000u}) {
        Engine m; m.faultBits = bits; check(m.probe(out) == RDNA4_AUX_IO, "DCN41 RX error fields refused");
    }
    // Real EDID-style MOT sequence retains ownership until explicit final release.
    { Engine m; auto s = m.session(); check(!rdna4_aux_begin(&s), "MOT lease begin");
      uint8_t offset = 0, bytes[16], got = 0;
      check(!rdna4_aux_transaction(&s,0x40,0x50,&offset,1,nullptr,0,&got), "MOT offset");
      for (unsigned i = 0; i < 8; ++i) check(!rdna4_aux_transaction(&s,i==7?0x10:0x50,0x50,nullptr,16,bytes,16,&got) && got==16 && m.releases==0, "MOT chunks cannot surrender grant");
      check(!rdna4_aux_end(&s) && m.grants==1 && m.releases==1, "MOT end"); }
    { Engine m; auto s=m.session(); check(!rdna4_aux_begin(&s), "timeout lease begin");
      m.timeout=true; uint8_t data[9], count=0;
      check(rdna4_aux_transaction(&s,0x90,0x100,nullptr,9,data,9,&count)<0, "inflight read timeout");
      check(rdna4_aux_transaction(&s,0,0x50,nullptr,0,nullptr,0,&count)<0 && m.requests==1, "cleanup STOP cannot replace inflight GO");
      (void)rdna4_aux_end(&s); }
    unsigned readCount=0;
    { Engine m; check(m.probe(out)==RDNA4_AUX_OK,"baseline for every read fault"); readCount=m.reads; }
    for (unsigned i=1;i<=readCount;++i) { Engine m; m.failReadAt=i;
      check(m.probe(out)!=RDNA4_AUX_OK && !out.config[0],"all-ones at each MMIO read cannot publish success"); }
    rdna4_dpcd_request req {1,sizeof(req),0,0,42,50100};
    check(rdna4_dpcd_validate(&req,42,100)==RDNA4_AUX_OK, "same PCI bounded query");
    check(rdna4_dpcd_validate(&req,43,100)==RDNA4_AUX_INVALID, "different PCI rejected");
    req.connector=8; check(rdna4_dpcd_validate(&req,42,100)==RDNA4_AUX_INVALID, "connector bounds"); req.connector=0;
    req.deadlineUs=50101; check(rdna4_dpcd_validate(&req,42,100)==RDNA4_AUX_TIMEOUT, "deadline cap");
    std::printf("aux_transport: %u checks PASS\n",checks);
    return 0;
}
