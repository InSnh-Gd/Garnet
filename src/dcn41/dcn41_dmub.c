/*
 * dcn41_dmub.c - DMUB probe, GPINT, inbox1 ring and VBIOS-service command builders. See dcn41_dmub.h.
 */
#include "dcn41_io.h"
#include "dcn41_dmub.h"

#pragma GCC poison float double   /* after the includes: <stddef.h> itself names long double */

#define DCN41_DMUB_RING_MAGIC 0x42554d44u   /* "DMUB" */
#define DCN41_DMUB_MAX_RING 0x10000u        /* 64 KiB: Linux uses DMUB_RB_SIZE = 8 KiB; anything larger is not a ring */

static int rd(struct dcn41_dev *dev, uint32_t addr, uint32_t *v)
{
    return dcn41_read(dev, addr, v);
}

/* the 27 registers dcn41_dmub_probe reads, in order (a static table: no initialiser the compiler turns into bzero) */
static const struct { uint32_t off; uint32_t base_idx; } dcn41_dmub_probe_regs[] = {
    { DCN41_DMCUB_CNTL, DCN41_DMCUB_CNTL_BASE_IDX },
    { DCN41_DMCUB_CNTL2, DCN41_DMCUB_CNTL2_BASE_IDX },
    { DCN41_DMCUB_SEC_CNTL, DCN41_DMCUB_SEC_CNTL_BASE_IDX },
    { DCN41_DMCUB_SCRATCH0, DCN41_DMCUB_SCRATCH0_BASE_IDX },
    { DCN41_DMCUB_SCRATCH7, DCN41_DMCUB_SCRATCH7_BASE_IDX },
    { DCN41_DMCUB_SCRATCH14, DCN41_DMCUB_SCRATCH14_BASE_IDX },
    { DCN41_DMCUB_SCRATCH15, DCN41_DMCUB_SCRATCH15_BASE_IDX },
    { DCN41_DMCUB_GPINT_DATAIN1, DCN41_DMCUB_GPINT_DATAIN1_BASE_IDX },
    { DCN41_DMCUB_UNDEFINED_ADDRESS_FAULT_ADDR, DCN41_DMCUB_UNDEFINED_ADDRESS_FAULT_ADDR_BASE_IDX },
    { DCN41_DMCUB_INBOX1_BASE_ADDRESS, DCN41_DMCUB_INBOX1_BASE_ADDRESS_BASE_IDX },
    { DCN41_DMCUB_INBOX1_SIZE, DCN41_DMCUB_INBOX1_SIZE_BASE_IDX },
    { DCN41_DMCUB_INBOX1_WPTR, DCN41_DMCUB_INBOX1_WPTR_BASE_IDX },
    { DCN41_DMCUB_INBOX1_RPTR, DCN41_DMCUB_INBOX1_RPTR_BASE_IDX },
    { DCN41_DMCUB_OUTBOX1_BASE_ADDRESS, DCN41_DMCUB_OUTBOX1_BASE_ADDRESS_BASE_IDX },
    { DCN41_DMCUB_OUTBOX1_SIZE, DCN41_DMCUB_OUTBOX1_SIZE_BASE_IDX },
    { DCN41_DMCUB_OUTBOX1_WPTR, DCN41_DMCUB_OUTBOX1_WPTR_BASE_IDX },
    { DCN41_DMCUB_OUTBOX1_RPTR, DCN41_DMCUB_OUTBOX1_RPTR_BASE_IDX },
    { DCN41_DMCUB_REGION3_CW4_OFFSET, DCN41_DMCUB_REGION3_CW4_OFFSET_BASE_IDX },
    { DCN41_DMCUB_REGION3_CW4_OFFSET_HIGH, DCN41_DMCUB_REGION3_CW4_OFFSET_HIGH_BASE_IDX },
    { DCN41_DMCUB_REGION3_CW4_BASE_ADDRESS, DCN41_DMCUB_REGION3_CW4_BASE_ADDRESS_BASE_IDX },
    { DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS, DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS_BASE_IDX },
    { DCN41_DMCUB_REGION4_OFFSET, DCN41_DMCUB_REGION4_OFFSET_BASE_IDX },
    { DCN41_DMCUB_REGION4_OFFSET_HIGH, DCN41_DMCUB_REGION4_OFFSET_HIGH_BASE_IDX },
    { DCN41_DMCUB_REGION4_TOP_ADDRESS, DCN41_DMCUB_REGION4_TOP_ADDRESS_BASE_IDX },
    { DCN41_DCN_VM_FB_LOCATION_BASE, DCN41_DCN_VM_FB_LOCATION_BASE_BASE_IDX },
    { DCN41_DCN_VM_FB_LOCATION_TOP, DCN41_DCN_VM_FB_LOCATION_TOP_BASE_IDX },
    { DCN41_DCN_VM_FB_OFFSET, DCN41_DCN_VM_FB_OFFSET_BASE_IDX },
};
#define DCN41_DMUB_PROBE_NREGS (sizeof(dcn41_dmub_probe_regs) / sizeof(dcn41_dmub_probe_regs[0]))

bool dcn41_dmub_ring_in_vram_reservation(const struct dcn41_dmub_state *st, uint64_t vram_size,
                                        uint64_t reserve_vram_off, uint32_t reserve_size)
{
    if (!st || (st->ring_map != DCN41_DMUB_MAP_CW4 && st->ring_map != DCN41_DMUB_MAP_REGION4) ||
        !st->fb_base_mc || st->ring_addr < st->fb_base_mc || st->ring_addr >= (1ull << 48) ||
        !st->inbox1_size || st->inbox1_size > DCN41_DMUB_MAX_RING || st->inbox1_size % DCN41_DMUB_CMD_SIZE ||
        reserve_vram_off > vram_size || reserve_size > vram_size - reserve_vram_off)
        return false;
    const uint64_t inbox_vram_off = st->ring_addr - st->fb_base_mc;
    if (inbox_vram_off < reserve_vram_off)
        return false;
    const uint64_t within_reserve = inbox_vram_off - reserve_vram_off;
    return within_reserve <= reserve_size && st->inbox1_size <= reserve_size - within_reserve;
}

int dcn41_dmub_probe(struct dcn41_dev *dev, struct dcn41_dmub_state *st)
{
    uint32_t addr[DCN41_DMUB_PROBE_NREGS], v[DCN41_DMUB_PROBE_NREGS];
    uint32_t i, cw4_base, cw4_top, inbox_m, r4_top;
    bool hw_power_init_done, powered_up;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!st)
        return DCN41_E_ARG;
    for (i = 0; i < DCN41_DMUB_PROBE_NREGS; i++) {   /* every address is resolved before the first read */
        addr[i] = dcn41_abs(dev, dcn41_dmub_probe_regs[i].off, dcn41_dmub_probe_regs[i].base_idx);
        if (addr[i] == DCN41_BAD_OFFSET)
            return DCN41_E_WINDOW;
    }
    for (i = 0; i < DCN41_DMUB_PROBE_NREGS; i++) {
        rc = rd(dev, addr[i], &v[i]);
        if (rc)
            return rc;
    }
    for (i = 0; i < sizeof(*st); i++)
        ((uint8_t *)st)[i] = 0;
    st->cntl = v[0]; st->cntl2 = v[1]; st->sec_cntl = v[2]; st->scratch0 = v[3]; st->scratch7 = v[4];
    st->scratch14 = v[5]; st->scratch15 = v[6]; st->gpint_datain1 = v[7]; st->fault_addr = v[8];
    st->inbox1_base = v[9]; st->inbox1_size = v[10]; st->inbox1_wptr = v[11]; st->inbox1_rptr = v[12];
    st->outbox1_base = v[13]; st->outbox1_size = v[14]; st->outbox1_wptr = v[15]; st->outbox1_rptr = v[16];
    st->cw4_offset = v[17]; st->cw4_offset_high = v[18]; st->cw4_base = v[19]; st->cw4_top = v[20];
    st->region4_offset = v[21]; st->region4_offset_high = v[22]; st->region4_top = v[23];
    st->fb_location_base = v[24]; st->fb_location_top = v[25]; st->fb_offset = v[26];

    /* DMCUB_ENABLE, DMCUB_SOFT_RESET and the SCRATCH0 boot status (union dmub_fw_boot_status) */
    st->enabled = (uint8_t)DCN41_FG(DCN41_DMCUB_CNTL, DMCUB_ENABLE, st->cntl);
    st->soft_reset = (uint8_t)DCN41_FG(DCN41_DMCUB_CNTL2, DMCUB_SOFT_RESET, st->cntl2);
    st->dal_fw = (st->scratch0 & DCN41_DMUB_BOOT_DAL_FW) != 0;
    st->mailbox_rdy = (st->scratch0 & DCN41_DMUB_BOOT_MAILBOX_RDY) != 0;
    hw_power_init_done = (st->scratch0 & DCN41_DMUB_BOOT_HW_POWER_INIT_DONE) != 0;
    /* "alive" is Linux's dmub_dcn35_is_hw_powered_up (dmub_dcn42_is_hw_powered_up is identical; dcn401 installs no
     * is_hw_powered_up hook), past its DMCUB_ENABLE test:
     *   return (status.bits.dal_fw && status.bits.hw_power_init_done && status.bits.mailbox_rdy) ||
     *          (!status.bits.dal_fw && status.bits.mailbox_rdy);
     * dal_fw only says WHICH firmware runs (DAL vs the VBIOS-loaded one), not whether it is healthy: this card's GOP
     * leaves SCRATCH0 = 0x82 (VBIOS firmware, mailbox ready, power init done), which must classify as alive. */
    powered_up = (st->dal_fw && hw_power_init_done && st->mailbox_rdy) || (!st->dal_fw && st->mailbox_rdy);
    st->fb_base_mc = (uint64_t)DCN41_FG(DCN41_DCN_VM_FB_LOCATION_BASE, FB_BASE, st->fb_location_base) << 24;

    if (!st->enabled)
        st->verdict = DCN41_DMUB_NOT_RUNNING;
    else if (st->soft_reset)
        st->verdict = DCN41_DMUB_IN_RESET;
    else if (!powered_up)
        st->verdict = DCN41_DMUB_NOT_READY;
    else if (st->inbox1_size == 0 || (st->inbox1_size % DCN41_DMUB_CMD_SIZE) != 0 ||
             st->inbox1_size > DCN41_DMUB_MAX_RING || st->inbox1_wptr >= st->inbox1_size ||
             st->inbox1_rptr >= st->inbox1_size || (st->inbox1_wptr % DCN41_DMUB_CMD_SIZE) != 0 ||
             (st->inbox1_rptr % DCN41_DMUB_CMD_SIZE) != 0)
        st->verdict = DCN41_DMUB_RING_INSANE;
    else
        st->verdict = st->inbox1_wptr == st->inbox1_rptr ? DCN41_DMUB_ALIVE_IDLE : DCN41_DMUB_ALIVE_BUSY;

    /* where the ring lives: cached inbox in CW4 (window fields are 29 bits: the 0x64000000 base reads 0x04000000), or
     * the uncached REGION4 mapping at firmware address 0x80000000 */
    cw4_base = DCN41_FG(DCN41_DMCUB_REGION3_CW4_BASE_ADDRESS, DMCUB_REGION3_CW4_BASE_ADDRESS, st->cw4_base);
    cw4_top = DCN41_FG(DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS, DMCUB_REGION3_CW4_TOP_ADDRESS, st->cw4_top);
    inbox_m = st->inbox1_base & DCN41_DMCUB_REGION3_CW4_BASE_ADDRESS__DMCUB_REGION3_CW4_BASE_ADDRESS_MASK;
    r4_top = DCN41_FG(DCN41_DMCUB_REGION4_TOP_ADDRESS, DMCUB_REGION4_TOP_ADDRESS, st->region4_top);
    if (st->inbox1_size != 0 && DCN41_FG(DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS, DMCUB_REGION3_CW4_ENABLE, st->cw4_top) &&
        st->inbox1_base != DCN41_DMUB_REGION4_INBOX1_BASE && inbox_m >= cw4_base &&
        (uint64_t)inbox_m + st->inbox1_size - 1 <= cw4_top) {
        st->ring_map = DCN41_DMUB_MAP_CW4;
        st->ring_addr = (((uint64_t)st->cw4_offset_high << 32) | st->cw4_offset) + (inbox_m - cw4_base);
    } else if (st->inbox1_size != 0 && st->inbox1_base == DCN41_DMUB_REGION4_INBOX1_BASE &&
               DCN41_FG(DCN41_DMCUB_REGION4_TOP_ADDRESS, DMCUB_REGION4_ENABLE, st->region4_top) &&
               st->inbox1_size - 1 <= r4_top) {
        st->ring_map = DCN41_DMUB_MAP_REGION4;
        st->ring_addr = ((uint64_t)st->region4_offset_high << 32) | st->region4_offset;
    }
    return DCN41_OK;
}

int dcn41_dmub_gpint(struct dcn41_dev *dev, uint32_t command, uint16_t param, uint32_t timeout_us)
{
    uint32_t addr, reg, acked, i, v = 0;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (command > (DCN41_DMUB_GPINT_COMMAND_MASK >> DCN41_DMUB_GPINT_COMMAND_SHIFT))
        return DCN41_E_ARG;
    addr = DCN41_ADDR(dev, DCN41_DMCUB_GPINT_DATAIN1);
    if (addr == DCN41_BAD_OFFSET)
        return DCN41_E_WINDOW;
    /* union dmub_gpint_data_register; reg.bits.status = 1 */
    reg = (1u << DCN41_DMUB_GPINT_STATUS_SHIFT) | ((command << DCN41_DMUB_GPINT_COMMAND_SHIFT) & DCN41_DMUB_GPINT_COMMAND_MASK) |
          (param & DCN41_DMUB_GPINT_PARAM_MASK);
    acked = reg & ~DCN41_DMUB_GPINT_STATUS_MASK;   /* is_gpint_acked: reg.bits.status = 0; test.all == reg.all */
    rc = dcn41_write(dev, addr, reg);
    if (rc)
        return rc;
    for (i = 0; i < timeout_us; ++i) {
        dev->udelay(dev->cookie, 1);
        rc = dcn41_read(dev, addr, &v);
        if (rc)
            return rc;
        if (v == acked)
            return DCN41_OK;
    }
    return DCN41_E_TIMEOUT;
}

int dcn41_dmub_gpint_response(struct dcn41_dev *dev, uint32_t *response)
{
    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!response)
        return DCN41_E_ARG;
    return dcn41_read(dev, DCN41_ADDR(dev, DCN41_DMCUB_SCRATCH7), response);
}

static bool ring_ok(const struct dcn41_dmub_ring *ring)
{
    return ring && ring->magic == DCN41_DMUB_RING_MAGIC && dcn41_ready(ring->dev) && ring->mread && ring->mwrite;
}

static bool ring_ptr_valid(const struct dcn41_dmub_ring *ring, uint32_t ptr)
{
    return ring->capacity % DCN41_DMUB_CMD_SIZE == 0 &&
           ptr % DCN41_DMUB_CMD_SIZE == 0 && ptr < ring->capacity;
}

int dcn41_dmub_ring_attach(struct dcn41_dmub_ring *ring, struct dcn41_dev *dev, const struct dcn41_dmub_state *st,
                           dcn41_mem_read32_fn mread, dcn41_mem_write32_fn mwrite, void *mcookie)
{
    uint32_t i;

    if (!ring || !st || !mread || !mwrite)
        return DCN41_E_ARG;
    for (i = 0; i < sizeof(*ring); i++)
        ((uint8_t *)ring)[i] = 0;
    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (st->verdict != DCN41_DMUB_ALIVE_IDLE || st->ring_map == DCN41_DMUB_MAP_UNKNOWN || st->ring_addr == 0)
        return DCN41_E_STATE;
    ring->dev = dev;
    ring->mread = mread;
    ring->mwrite = mwrite;
    ring->mcookie = mcookie;
    ring->ring_addr = st->ring_addr;
    ring->capacity = st->inbox1_size;
    ring->wptr = st->inbox1_wptr;        /* dmub_srv_sync_inbox1: adopt the hardware pointers */
    ring->rptr = st->inbox1_rptr;
    ring->magic = DCN41_DMUB_RING_MAGIC;
    return DCN41_OK;
}

bool dcn41_dmub_ring_full(const struct dcn41_dmub_ring *ring)
{
    uint32_t data_count;
    if (!ring || ring->capacity == 0)
        return true;
    if (ring->wptr >= ring->rptr)
        data_count = ring->wptr - ring->rptr;
    else
        data_count = ring->capacity - (ring->rptr - ring->wptr);
    return data_count == ring->capacity - DCN41_DMUB_CMD_SIZE;
}

static int ring_stage(const struct dcn41_dmub_ring *ring, uint32_t offset, const struct dcn41_dmub_cmd *cmd)
{
    const uint8_t *src = (const uint8_t *)cmd;
    for (uint32_t i = 0; i < DCN41_DMUB_CMD_SIZE; i += 4) {
        uint32_t word = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8) | ((uint32_t)src[i + 2] << 16) |
                        ((uint32_t)src[i + 3] << 24);
        ring->mwrite(ring->mcookie, ring->ring_addr + offset + i, word);
    }
    /* dmub_rb_flush_pending reads the queued entries back before WPTR moves; we also compare them. */
    for (uint32_t i = 0; i < DCN41_DMUB_CMD_SIZE; i += 4) {
        uint32_t word = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8) | ((uint32_t)src[i + 2] << 16) |
                        ((uint32_t)src[i + 3] << 24);
        if (ring->mread(ring->mcookie, ring->ring_addr + offset + i) != word)
            return DCN41_E_READBACK;
    }
    return DCN41_OK;
}

int dcn41_dmub_ring_submit_batch(struct dcn41_dmub_ring *ring, const struct dcn41_dmub_cmd *cmds, uint32_t count)
{
    uint32_t hw_wptr = 0, hw_rptr = 0, used, next;
    uint32_t wptr_addr, rptr_addr;
    int rc;

    if (!ring_ok(ring))
        return DCN41_E_UNINIT;
    if (!cmds || !count)
        return DCN41_E_ARG;
    wptr_addr = DCN41_ADDR(ring->dev, DCN41_DMCUB_INBOX1_WPTR);
    rptr_addr = DCN41_ADDR(ring->dev, DCN41_DMCUB_INBOX1_RPTR);
    if (wptr_addr == DCN41_BAD_OFFSET || rptr_addr == DCN41_BAD_OFFSET)
        return DCN41_E_WINDOW;
    rc = dcn41_read(ring->dev, wptr_addr, &hw_wptr);
    if (rc)
        return rc;
    if (hw_wptr != ring->wptr)
        return DCN41_E_RACE;
    rc = dcn41_read(ring->dev, rptr_addr, &hw_rptr);
    if (rc)
        return rc;
    if (!ring_ptr_valid(ring, hw_rptr) || !ring_ptr_valid(ring, ring->wptr))
        return DCN41_E_HW;
    ring->rptr = hw_rptr;
    used = ring->wptr >= hw_rptr ? ring->wptr - hw_rptr : ring->capacity - (hw_rptr - ring->wptr);
    if (count >= (ring->capacity - used) / DCN41_DMUB_CMD_SIZE)
        return DCN41_E_FULL;
    next = ring->wptr;
    for (uint32_t index = 0; index < count; ++index) {
        rc = ring_stage(ring, next, &cmds[index]);
        if (rc)
            return rc;
        next += DCN41_DMUB_CMD_SIZE;
        if (next == ring->capacity)
            next = 0;
    }
    rc = dcn41_write(ring->dev, wptr_addr, next);   /* dmub_dcn401_set_inbox1_wptr */
    if (rc)
        return rc;
    ring->wptr = next;
    ring->submitted += count;
    return DCN41_OK;
}

int dcn41_dmub_ring_submit(struct dcn41_dmub_ring *ring, const struct dcn41_dmub_cmd *cmd)
{
    return dcn41_dmub_ring_submit_batch(ring, cmd, 1);
}

int dcn41_dmub_ring_wait_idle(struct dcn41_dmub_ring *ring, uint32_t timeout_us)
{
    uint32_t i, rptr = 0, addr;
    int rc;

    if (!ring_ok(ring))
        return DCN41_E_UNINIT;
    if (!ring_ptr_valid(ring, ring->rptr) || !ring_ptr_valid(ring, ring->wptr))
        return DCN41_E_HW;
    addr = DCN41_ADDR(ring->dev, DCN41_DMCUB_INBOX1_RPTR);
    if (addr == DCN41_BAD_OFFSET)
        return DCN41_E_WINDOW;
    for (i = 0; i < timeout_us; i++) {
        /* dmub_srv_update_inbox_status */
        rc = dcn41_read(ring->dev, addr, &rptr);
        if (rc)
            return rc;
        if (!ring_ptr_valid(ring, rptr))
            return DCN41_E_HW;
        if (ring->rptr > rptr)
            ring->reported += (rptr + ring->capacity - ring->rptr) / DCN41_DMUB_CMD_SIZE;
        else
            ring->reported += (rptr - ring->rptr) / DCN41_DMUB_CMD_SIZE;
        ring->rptr = rptr;
        if (ring->wptr == ring->rptr)        /* dmub_rb_empty */
            return DCN41_OK;
        ring->dev->udelay(ring->dev->cookie, 1);
    }
    return DCN41_E_TIMEOUT;
}

int dcn41_dmub_ring_return_data(struct dcn41_dmub_ring *ring, struct dcn41_dmub_cmd *out)
{
    uint64_t base;
    uint8_t *dst;
    uint32_t i;

    if (!ring_ok(ring))
        return DCN41_E_UNINIT;
    if (!out)
        return DCN41_E_ARG;
    /* dmub_rb_get_return_data: rptr == 0 ? base + capacity - 64 : base + rptr - 64 */
    base = ring->ring_addr + (ring->rptr == 0 ? ring->capacity - DCN41_DMUB_CMD_SIZE : ring->rptr - DCN41_DMUB_CMD_SIZE);
    dst = (uint8_t *)out;
    for (i = 0; i < DCN41_DMUB_CMD_SIZE; i += 4) {
        uint32_t w = ring->mread(ring->mcookie, base + i);
        dst[i] = (uint8_t)w;
        dst[i + 1] = (uint8_t)(w >> 8);
        dst[i + 2] = (uint8_t)(w >> 16);
        dst[i + 3] = (uint8_t)(w >> 24);
    }
    return DCN41_OK;
}

uint32_t dcn41_dmub_header(uint32_t type, uint32_t sub_type, uint32_t payload_bytes, bool ret_status)
{
    return ((type << DCN41_DMUB_HDR_TYPE_SHIFT) & DCN41_DMUB_HDR_TYPE_MASK) |
           ((sub_type << DCN41_DMUB_HDR_SUB_TYPE_SHIFT) & DCN41_DMUB_HDR_SUB_TYPE_MASK) |
           (ret_status ? DCN41_DMUB_HDR_RET_STATUS_MASK : 0) |
           ((payload_bytes << DCN41_DMUB_HDR_PAYLOAD_BYTES_SHIFT) & DCN41_DMUB_HDR_PAYLOAD_BYTES_MASK);
}

void dcn41_dmub_cmd_zero(struct dcn41_dmub_cmd *cmd)
{
    uint32_t i;
    for (i = 0; i < sizeof(*cmd); i++)
        ((uint8_t *)cmd)[i] = 0;
}

static void put8(struct dcn41_dmub_cmd *cmd, uint32_t off, uint8_t v)
{
    cmd->payload[off] = v;
}

static void put32(struct dcn41_dmub_cmd *cmd, uint32_t off, uint32_t v)
{
    cmd->payload[off] = (uint8_t)v;
    cmd->payload[off + 1] = (uint8_t)(v >> 8);
    cmd->payload[off + 2] = (uint8_t)(v >> 16);
    cmd->payload[off + 3] = (uint8_t)(v >> 24);
}

void dcn41_dmub_build_encoder_control(struct dcn41_dmub_cmd *cmd, uint8_t digid, uint8_t action, uint8_t digmode,
                                      uint8_t lanenum, uint32_t pixel_clock_khz, uint8_t atom_bitpercolor,
                                      bool is_hdmi)
{
    uint32_t pclk_10khz = pixel_clock_khz / 10;
    dcn41_dmub_cmd_zero(cmd);
    /* HDMI deep colour: params.pclk_10khz * 30 / 24 (10 bpc), * 36 / 24 (12), * 48 / 24 (16) */
    if (is_hdmi) {
        if (atom_bitpercolor == 0x03)
            pclk_10khz = (pclk_10khz * 30) / 24;
        else if (atom_bitpercolor == 0x04)
            pclk_10khz = (pclk_10khz * 36) / 24;
        else if (atom_bitpercolor == 0x05)
            pclk_10khz = (pclk_10khz * 48) / 24;
    }
    /* header.payload_bytes = sizeof(cmd.digx_encoder_control) - sizeof(header) = sizeof(union ..._v1_5) = 12 */
    cmd->header = dcn41_dmub_header(DCN41_DMUB_CMD_VBIOS, DCN41_DMUB_VBIOS_DIGX_ENCODER_CONTROL,
                                    sizeof(struct dcn41_dmub_encoder_stream_setup_v1_5), false);
    put8(cmd, offsetof(struct dcn41_dmub_encoder_stream_setup_v1_5, digid), digid);
    put8(cmd, offsetof(struct dcn41_dmub_encoder_stream_setup_v1_5, action), action);
    put8(cmd, offsetof(struct dcn41_dmub_encoder_stream_setup_v1_5, digmode), digmode);
    put8(cmd, offsetof(struct dcn41_dmub_encoder_stream_setup_v1_5, lanenum), lanenum);
    put32(cmd, offsetof(struct dcn41_dmub_encoder_stream_setup_v1_5, pclk_10khz), pclk_10khz);
    put8(cmd, offsetof(struct dcn41_dmub_encoder_stream_setup_v1_5, bitpercolor), atom_bitpercolor);
}

void dcn41_dmub_build_transmitter_control(struct dcn41_dmub_cmd *cmd, uint8_t phyid, uint8_t action,
                                          uint8_t mode_laneset, uint8_t lanenum, uint32_t pixel_clock_khz,
                                          uint8_t hpdsel, uint8_t digfe_sel, uint8_t connobj_id,
                                          uint8_t hpo_instance, uint8_t skip_phy_ssc_reduction)
{
    dcn41_dmub_cmd_zero(cmd);
    /* payload_bytes = sizeof(union dmub_cmd_dig1_transmitter_control_data) = 60 */
    cmd->header = dcn41_dmub_header(DCN41_DMUB_CMD_VBIOS, DCN41_DMUB_VBIOS_DIG1_TRANSMITTER_CONTROL,
                                    sizeof(struct dcn41_dmub_transmitter_control_v1_7), false);
    put8(cmd, offsetof(struct dcn41_dmub_transmitter_control_v1_7, phyid), phyid);
    put8(cmd, offsetof(struct dcn41_dmub_transmitter_control_v1_7, action), action);
    put8(cmd, offsetof(struct dcn41_dmub_transmitter_control_v1_7, mode_laneset), mode_laneset);
    put8(cmd, offsetof(struct dcn41_dmub_transmitter_control_v1_7, lanenum), lanenum);
    put32(cmd, offsetof(struct dcn41_dmub_transmitter_control_v1_7, symclk), pixel_clock_khz / 10);
    put8(cmd, offsetof(struct dcn41_dmub_transmitter_control_v1_7, hpdsel), hpdsel);
    put8(cmd, offsetof(struct dcn41_dmub_transmitter_control_v1_7, digfe_sel), digfe_sel);
    put8(cmd, offsetof(struct dcn41_dmub_transmitter_control_v1_7, connobj_id), connobj_id);
    put8(cmd, offsetof(struct dcn41_dmub_transmitter_control_v1_7, hpo_instance), hpo_instance);
    put8(cmd, offsetof(struct dcn41_dmub_transmitter_control_v1_7, skip_phy_ssc_reduction), skip_phy_ssc_reduction);
}

void dcn41_dmub_build_set_pixel_clock(struct dcn41_dmub_cmd *cmd, uint32_t pixclk_100hz, uint8_t pll_id,
                                      uint8_t encoderobjid, uint8_t encoder_mode, uint8_t miscinfo, uint8_t crtc_id,
                                      uint8_t deep_color_ratio)
{
    dcn41_dmub_cmd_zero(cmd);
    cmd->header = dcn41_dmub_header(DCN41_DMUB_CMD_VBIOS, DCN41_DMUB_VBIOS_SET_PIXEL_CLOCK,
                                    sizeof(struct dcn41_dmub_set_pixel_clock_v1_7), false);
    put32(cmd, offsetof(struct dcn41_dmub_set_pixel_clock_v1_7, pixclk_100hz), pixclk_100hz);
    put8(cmd, offsetof(struct dcn41_dmub_set_pixel_clock_v1_7, pll_id), pll_id);
    put8(cmd, offsetof(struct dcn41_dmub_set_pixel_clock_v1_7, encoderobjid), encoderobjid);
    put8(cmd, offsetof(struct dcn41_dmub_set_pixel_clock_v1_7, encoder_mode), encoder_mode);
    put8(cmd, offsetof(struct dcn41_dmub_set_pixel_clock_v1_7, miscinfo), miscinfo);
    put8(cmd, offsetof(struct dcn41_dmub_set_pixel_clock_v1_7, crtc_id), crtc_id);
    put8(cmd, offsetof(struct dcn41_dmub_set_pixel_clock_v1_7, deep_color_ratio), deep_color_ratio);
}

void dcn41_dmub_build_disp_power_gating(struct dcn41_dmub_cmd *cmd, uint8_t disp_pipe_id, uint8_t enable)
{
    dcn41_dmub_cmd_zero(cmd);
    cmd->header = dcn41_dmub_header(DCN41_DMUB_CMD_VBIOS, DCN41_DMUB_VBIOS_ENABLE_DISP_POWER_GATING,
                                    sizeof(struct dcn41_dmub_disp_power_gating_v2_1), false);
    put8(cmd, offsetof(struct dcn41_dmub_disp_power_gating_v2_1, disp_pipe_id), disp_pipe_id);
    put8(cmd, offsetof(struct dcn41_dmub_disp_power_gating_v2_1, enable), enable);
}

void dcn41_dmub_build_query_feature_caps(struct dcn41_dmub_cmd *cmd)
{
    dcn41_dmub_cmd_zero(cmd);
    cmd->header = dcn41_dmub_header(DCN41_DMUB_CMD_QUERY_FEATURE_CAPS, 0, sizeof(struct dcn41_dmub_feature_caps), true);
}

int dcn41_dmub_fw_meta_from_blob(const uint8_t *blob, uint32_t size, uint32_t meta_offset,
                                 struct dcn41_dmub_fw_meta_info *out)
{
    const uint8_t *m;
    uint32_t i;
    if (!blob || !size || !out)
        return DCN41_E_ARG;
    if ((uint64_t)size < 64u + meta_offset)          /* sizeof(union dmub_fw_meta) + meta_offset */
        return DCN41_E_ARG;
    m = blob + size - meta_offset - 64u;
    if (((uint32_t)m[0] | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24)) !=
        DCN41_DMUB_FW_META_MAGIC)
        return DCN41_E_ARG;
    for (i = 0; i < sizeof(*out); i++)
        ((uint8_t *)out)[i] = m[i];
    return DCN41_OK;
}

int dcn41_dmub_fw_meta_scan(const uint8_t *blob, uint32_t size, struct dcn41_dmub_fw_meta_info *out)
{
    uint32_t i;
    for (i = 0; i < 16; ++i)                          /* "Combined metadata region - can be aligned to 16-bytes." */
        if (dcn41_dmub_fw_meta_from_blob(blob, size, i, out) == DCN41_OK)
            return DCN41_OK;
    return DCN41_E_ARG;
}

int dcn41_dmub_fw_meta_from_psp(const uint8_t *signed_blob, uint32_t signed_bytes,
                                struct dcn41_dmub_fw_meta_info *out, uint32_t *code_bytes)
{
    const uint32_t header = 256u;
    const uint32_t footer = 256u;
    if (!signed_blob || !out || signed_bytes < header + footer + 64u)
        return DCN41_E_ARG;
    const uint8_t *code = signed_blob + header;
    uint32_t bytes = signed_bytes - header - footer;
    if (dcn41_dmub_fw_meta_scan(code, bytes, out) != DCN41_OK) {
        if (bytes < footer + 64u)
            return DCN41_E_ARG;
        bytes -= footer;
        if (dcn41_dmub_fw_meta_scan(code, bytes, out) != DCN41_OK)
            return DCN41_E_ARG;
    }
    if (code_bytes)
        *code_bytes = bytes;
    return DCN41_OK;
}

static int wr_reg(struct dcn41_dev *dev, uint32_t off, uint32_t base_idx, uint32_t v)
{
    uint32_t addr = dcn41_abs(dev, off, base_idx);
    if (addr == DCN41_BAD_OFFSET)
        return DCN41_E_WINDOW;
    return dcn41_write(dev, addr, v);
}

static int rd_reg(struct dcn41_dev *dev, uint32_t off, uint32_t base_idx, uint32_t *v)
{
    uint32_t addr = dcn41_abs(dev, off, base_idx);
    if (addr == DCN41_BAD_OFFSET)
        return DCN41_E_WINDOW;
    return dcn41_read(dev, addr, v);
}

int dcn41_dmub_stop_and_reset(struct dcn41_dev *dev, uint32_t timeout_us)
{
    uint32_t cntl, cntl2, scratch, gpint, initial_gpint, elapsed;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    rc = rd_reg(dev, DCN41_DMCUB_CNTL, DCN41_DMCUB_CNTL_BASE_IDX, &cntl);
    if (rc) return rc;
    rc = rd_reg(dev, DCN41_DMCUB_CNTL2, DCN41_DMCUB_CNTL2_BASE_IDX, &cntl2);
    if (rc) return rc;
    if (cntl == UINT32_MAX || cntl2 == UINT32_MAX ||
        !DCN41_FG(DCN41_DMCUB_CNTL, DMCUB_ENABLE, cntl) ||
        DCN41_FG(DCN41_DMCUB_CNTL2, DMCUB_SOFT_RESET, cntl2))
        return DCN41_E_STATE;
    if (timeout_us == 0u)
        return DCN41_E_ARG;

    const uint32_t stop_code = DCN41_DMUB_GPINT_STOP_FW << DCN41_DMUB_GPINT_COMMAND_SHIFT;
    const uint32_t stop_request = stop_code | (1u << DCN41_DMUB_GPINT_STATUS_SHIFT);
    rc = rd_reg(dev, DCN41_DMCUB_SCRATCH7, DCN41_DMCUB_SCRATCH7_BASE_IDX, &scratch);
    if (rc) return rc;
    rc = rd_reg(dev, DCN41_DMCUB_GPINT_DATAIN1, DCN41_DMCUB_GPINT_DATAIN1_BASE_IDX, &initial_gpint);
    if (rc) return rc;
    if ((initial_gpint & DCN41_DMUB_GPINT_STATUS_MASK) ||
        (initial_gpint & DCN41_DMUB_GPINT_COMMAND_MASK) == stop_code ||
        scratch == DCN41_DMUB_GPINT_STOP_FW_RESPONSE || scratch == UINT32_MAX)
        return DCN41_E_STATE;

    rc = wr_reg(dev, DCN41_DMCUB_GPINT_DATAIN1, DCN41_DMCUB_GPINT_DATAIN1_BASE_IDX, stop_request);
    if (rc) return rc;
    for (elapsed = 0; elapsed < timeout_us; ++elapsed) {
        rc = rd_reg(dev, DCN41_DMCUB_GPINT_DATAIN1, DCN41_DMCUB_GPINT_DATAIN1_BASE_IDX, &gpint);
        if (rc) break;
        if (gpint != stop_request && gpint != stop_code && gpint != initial_gpint)
            return DCN41_E_RACE;
        rc = rd_reg(dev, DCN41_DMCUB_SCRATCH7, DCN41_DMCUB_SCRATCH7_BASE_IDX, &scratch);
        if (rc) break;
        rc = rd_reg(dev, DCN41_DMCUB_CNTL, DCN41_DMCUB_CNTL_BASE_IDX, &cntl);
        if (rc) break;
        if (gpint == stop_code && scratch == DCN41_DMUB_GPINT_STOP_FW_RESPONSE &&
            cntl != UINT32_MAX && DCN41_FG(DCN41_DMCUB_CNTL, DMCUB_PWAIT_MODE_STATUS, cntl))
            break;
        dev->udelay(dev->cookie, 1);
    }
    if (rc || elapsed == timeout_us) {
        int clear_rc = rd_reg(dev, DCN41_DMCUB_GPINT_DATAIN1, DCN41_DMCUB_GPINT_DATAIN1_BASE_IDX, &gpint);
        if (!clear_rc && (gpint == stop_request || gpint == stop_code))
            clear_rc = wr_reg(dev, DCN41_DMCUB_GPINT_DATAIN1, DCN41_DMCUB_GPINT_DATAIN1_BASE_IDX, 0u);
        return rc ? rc : (clear_rc ? clear_rc : DCN41_E_TIMEOUT);
    }

    rc = wr_reg(dev, DCN41_DMCUB_CNTL2, DCN41_DMCUB_CNTL2_BASE_IDX,
                cntl2 | DCN41_DMCUB_CNTL2__DMCUB_SOFT_RESET_MASK);
    if (rc) return rc;
    dev->udelay(dev->cookie, 1);
    rc = rd_reg(dev, DCN41_DMCUB_CNTL2, DCN41_DMCUB_CNTL2_BASE_IDX, &cntl2);
    if (rc) return rc;
    if (cntl2 == UINT32_MAX || !DCN41_FG(DCN41_DMCUB_CNTL2, DMCUB_SOFT_RESET, cntl2))
        return DCN41_E_READBACK;
    rc = wr_reg(dev, DCN41_DMCUB_CNTL, DCN41_DMCUB_CNTL_BASE_IDX,
                cntl & ~DCN41_DMCUB_CNTL__DMCUB_ENABLE_MASK);
    if (rc) return rc;
    rc = rd_reg(dev, DCN41_DMCUB_CNTL, DCN41_DMCUB_CNTL_BASE_IDX, &cntl);
    if (rc) return rc;
    if (cntl == UINT32_MAX || DCN41_FG(DCN41_DMCUB_CNTL, DMCUB_ENABLE, cntl))
        return DCN41_E_READBACK;

#define CLEAR_HANDOFF_REG(reg) do { \
    rc = wr_reg(dev, DCN41_##reg, DCN41_##reg##_BASE_IDX, 0u); \
    if (rc) return rc; \
} while (0)
    CLEAR_HANDOFF_REG(DMCUB_REGION3_CW2_TOP_ADDRESS);
    CLEAR_HANDOFF_REG(DMCUB_REGION3_CW3_TOP_ADDRESS);
    CLEAR_HANDOFF_REG(DMCUB_REGION3_CW4_TOP_ADDRESS);
    CLEAR_HANDOFF_REG(DMCUB_REGION3_CW5_TOP_ADDRESS);
    CLEAR_HANDOFF_REG(DMCUB_REGION3_CW6_TOP_ADDRESS);
    CLEAR_HANDOFF_REG(DMCUB_REGION3_CW7_TOP_ADDRESS);
    CLEAR_HANDOFF_REG(DMCUB_INBOX1_RPTR);
    CLEAR_HANDOFF_REG(DMCUB_INBOX1_WPTR);
    CLEAR_HANDOFF_REG(DMCUB_OUTBOX1_RPTR);
    CLEAR_HANDOFF_REG(DMCUB_OUTBOX1_WPTR);
    CLEAR_HANDOFF_REG(DMCUB_OUTBOX0_RPTR);
    CLEAR_HANDOFF_REG(DMCUB_OUTBOX0_WPTR);
    CLEAR_HANDOFF_REG(DMCUB_SCRATCH0);
#undef CLEAR_HANDOFF_REG
    rc = rd_reg(dev, DCN41_DMCUB_GPINT_DATAIN1, DCN41_DMCUB_GPINT_DATAIN1_BASE_IDX, &gpint);
    if (rc) return rc;
    if (gpint == stop_code || gpint == stop_request)
        return wr_reg(dev, DCN41_DMCUB_GPINT_DATAIN1, DCN41_DMCUB_GPINT_DATAIN1_BASE_IDX, 0u);
    if (gpint != 0u)
        return DCN41_E_RACE;
    return DCN41_OK;
}

static int wr_cw(struct dcn41_dev *dev, uint32_t off_lo, uint32_t off_hi, uint32_t reg_base, uint32_t reg_top,
                 uint32_t base_mask, uint32_t top_mask, uint32_t en_mask, const struct dcn41_dmub_window *w)
{
    int rc;
    rc = wr_reg(dev, off_lo, DCN41_DMCUB_CNTL_BASE_IDX, (uint32_t)(w->gpu_addr & 0xFFFFFFFFu));
    if (rc) return rc;
    rc = wr_reg(dev, off_hi, DCN41_DMCUB_CNTL_BASE_IDX, (uint32_t)(w->gpu_addr >> 32));
    if (rc) return rc;
    rc = wr_reg(dev, reg_base, DCN41_DMCUB_CNTL_BASE_IDX, w->base & base_mask);
    if (rc) return rc;
    return wr_reg(dev, reg_top, DCN41_DMCUB_CNTL_BASE_IDX, (w->top & top_mask) | en_mask);
}

int dcn41_dmub_program_windows(struct dcn41_dev *dev, const struct dcn41_dmub_handoff_windows *w)
{
    uint32_t cntl, cntl2;
    uint32_t r5_top, r6_top;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (!w || w->inbox1_base != DCN41_DMUB_CW4_BASE || w->inbox1_size != DCN41_DMUB_RB_SIZE ||
        w->outbox1_base != DCN41_DMUB_CW4_BASE + DCN41_DMUB_RB_SIZE || w->outbox1_size != DCN41_DMUB_RB_SIZE ||
        w->outbox0_base != DCN41_DMUB_OUTBOX0_TRACE_BASE || w->outbox0_size == 0 ||
        w->cw3.base != DCN41_DMUB_CW3_BASE || w->cw4.base != DCN41_DMUB_CW4_BASE ||
        w->cw5.base != DCN41_DMUB_CW5_BASE || w->cw6.base != DCN41_DMUB_CW6_BASE ||
        w->region6.base != DCN41_DMUB_CW6_BASE)
        return DCN41_E_ARG;
    const struct dcn41_dmub_window * const windows[] = { &w->cw3, &w->cw4, &w->cw5, &w->cw6, &w->region6 };
    for (unsigned index = 0; index < sizeof(windows) / sizeof(windows[0]); ++index) {
        const struct dcn41_dmub_window *window = windows[index];
        const uint64_t limit = 1ULL << 48;
        if (window->top <= window->base || window->top - window->base >= 0x1000000u ||
            !window->gpu_addr || (window->gpu_addr & 0xffu) != 0 || window->gpu_addr >= limit ||
            window->top - window->base > limit - window->gpu_addr)
            return DCN41_E_ARG;
    }
    if (w->cw4.top - w->cw4.base != 2u * DCN41_DMUB_RB_SIZE ||
        w->cw5.top - w->cw5.base <= DCN41_DMUB_TRACE_ENTRY_OFFSET ||
        w->outbox0_size != w->cw5.top - w->cw5.base - DCN41_DMUB_TRACE_ENTRY_OFFSET)
        return DCN41_E_ARG;
    rc = rd_reg(dev, DCN41_DMCUB_CNTL, DCN41_DMCUB_CNTL_BASE_IDX, &cntl);
    if (rc) return rc;
    rc = rd_reg(dev, DCN41_DMCUB_CNTL2, DCN41_DMCUB_CNTL2_BASE_IDX, &cntl2);
    if (rc) return rc;
    if (cntl == UINT32_MAX || cntl2 == UINT32_MAX ||
        DCN41_FG(DCN41_DMCUB_CNTL, DMCUB_ENABLE, cntl) ||
        !DCN41_FG(DCN41_DMCUB_CNTL2, DMCUB_SOFT_RESET, cntl2))
        return DCN41_E_STATE;

    rc = wr_cw(dev, DCN41_DMCUB_REGION3_CW3_OFFSET, DCN41_DMCUB_REGION3_CW3_OFFSET_HIGH,
               DCN41_DMCUB_REGION3_CW3_BASE_ADDRESS, DCN41_DMCUB_REGION3_CW3_TOP_ADDRESS,
               DCN41_DMCUB_REGION3_CW3_BASE_ADDRESS__DMCUB_REGION3_CW3_BASE_ADDRESS_MASK,
               DCN41_DMCUB_REGION3_CW3_TOP_ADDRESS__DMCUB_REGION3_CW3_TOP_ADDRESS_MASK,
               DCN41_DMCUB_REGION3_CW3_TOP_ADDRESS__DMCUB_REGION3_CW3_ENABLE_MASK, &w->cw3);
    if (rc) return rc;
    rc = wr_cw(dev, DCN41_DMCUB_REGION3_CW4_OFFSET, DCN41_DMCUB_REGION3_CW4_OFFSET_HIGH,
               DCN41_DMCUB_REGION3_CW4_BASE_ADDRESS, DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS,
               DCN41_DMCUB_REGION3_CW4_BASE_ADDRESS__DMCUB_REGION3_CW4_BASE_ADDRESS_MASK,
               DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS__DMCUB_REGION3_CW4_TOP_ADDRESS_MASK,
               DCN41_DMCUB_REGION3_CW4_TOP_ADDRESS__DMCUB_REGION3_CW4_ENABLE_MASK, &w->cw4);
    if (rc) return rc;
    rc = wr_cw(dev, DCN41_DMCUB_REGION3_CW5_OFFSET, DCN41_DMCUB_REGION3_CW5_OFFSET_HIGH,
               DCN41_DMCUB_REGION3_CW5_BASE_ADDRESS, DCN41_DMCUB_REGION3_CW5_TOP_ADDRESS,
               DCN41_DMCUB_REGION3_CW5_BASE_ADDRESS__DMCUB_REGION3_CW5_BASE_ADDRESS_MASK,
               DCN41_DMCUB_REGION3_CW5_TOP_ADDRESS__DMCUB_REGION3_CW5_TOP_ADDRESS_MASK,
               DCN41_DMCUB_REGION3_CW5_TOP_ADDRESS__DMCUB_REGION3_CW5_ENABLE_MASK, &w->cw5);
    if (rc) return rc;

    r5_top = w->cw5.top - w->cw5.base - 1u;
    rc = wr_reg(dev, DCN41_DMCUB_REGION5_OFFSET, DCN41_DMCUB_REGION5_OFFSET_BASE_IDX, (uint32_t)w->cw5.gpu_addr);
    if (rc) return rc;
    rc = wr_reg(dev, DCN41_DMCUB_REGION5_OFFSET_HIGH, DCN41_DMCUB_REGION5_OFFSET_HIGH_BASE_IDX,
                (uint32_t)(w->cw5.gpu_addr >> 32));
    if (rc) return rc;
    rc = wr_reg(dev, DCN41_DMCUB_REGION5_TOP_ADDRESS, DCN41_DMCUB_REGION5_TOP_ADDRESS_BASE_IDX,
                r5_top | DCN41_DMCUB_REGION5_TOP_ADDRESS__DMCUB_REGION5_ENABLE_MASK);
    if (rc) return rc;

    rc = wr_cw(dev, DCN41_DMCUB_REGION3_CW6_OFFSET, DCN41_DMCUB_REGION3_CW6_OFFSET_HIGH,
               DCN41_DMCUB_REGION3_CW6_BASE_ADDRESS, DCN41_DMCUB_REGION3_CW6_TOP_ADDRESS,
               DCN41_DMCUB_REGION3_CW6_BASE_ADDRESS__DMCUB_REGION3_CW6_BASE_ADDRESS_MASK,
               DCN41_DMCUB_REGION3_CW6_TOP_ADDRESS__DMCUB_REGION3_CW6_TOP_ADDRESS_MASK,
               DCN41_DMCUB_REGION3_CW6_TOP_ADDRESS__DMCUB_REGION3_CW6_ENABLE_MASK, &w->cw6);
    if (rc) return rc;

    r6_top = w->region6.top - w->region6.base - 1u;
    rc = wr_reg(dev, DCN41_DMCUB_REGION6_OFFSET, DCN41_DMCUB_REGION6_OFFSET_BASE_IDX, (uint32_t)w->region6.gpu_addr);
    if (rc) return rc;
    rc = wr_reg(dev, DCN41_DMCUB_REGION6_OFFSET_HIGH, DCN41_DMCUB_REGION6_OFFSET_HIGH_BASE_IDX,
                (uint32_t)(w->region6.gpu_addr >> 32));
    if (rc) return rc;
    rc = wr_reg(dev, DCN41_DMCUB_REGION6_TOP_ADDRESS, DCN41_DMCUB_REGION6_TOP_ADDRESS_BASE_IDX,
                r6_top | DCN41_DMCUB_REGION6_TOP_ADDRESS__DMCUB_REGION6_ENABLE_MASK);
    if (rc) return rc;

    rc = wr_reg(dev, DCN41_DMCUB_OUTBOX0_BASE_ADDRESS, DCN41_DMCUB_OUTBOX0_BASE_ADDRESS_BASE_IDX, w->outbox0_base);
    if (rc) return rc;
    rc = wr_reg(dev, DCN41_DMCUB_OUTBOX0_SIZE, DCN41_DMCUB_OUTBOX0_SIZE_BASE_IDX, w->outbox0_size);
    if (rc) return rc;

    rc = wr_reg(dev, DCN41_DMCUB_INBOX1_BASE_ADDRESS, DCN41_DMCUB_INBOX1_BASE_ADDRESS_BASE_IDX, w->inbox1_base);
    if (rc) return rc;
    rc = wr_reg(dev, DCN41_DMCUB_INBOX1_SIZE, DCN41_DMCUB_INBOX1_SIZE_BASE_IDX, w->inbox1_size);
    if (rc) return rc;
    rc = wr_reg(dev, DCN41_DMCUB_OUTBOX1_BASE_ADDRESS, DCN41_DMCUB_OUTBOX1_BASE_ADDRESS_BASE_IDX, w->outbox1_base);
    if (rc) return rc;
    return wr_reg(dev, DCN41_DMCUB_OUTBOX1_SIZE, DCN41_DMCUB_OUTBOX1_SIZE_BASE_IDX, w->outbox1_size);
}

int dcn41_dmub_release_and_wait(struct dcn41_dev *dev, uint32_t psp_version, uint32_t timeout_us)
{
    uint32_t mmhb = 0, cntl = 0, cntl2 = 0, scratch0 = 0, i;
    const uint32_t ready_mask = DCN41_DMUB_BOOT_DAL_FW | DCN41_DMUB_BOOT_MAILBOX_RDY | DCN41_DMUB_BOOT_HW_POWER_INIT_DONE;
    int rc;

    if (!dcn41_ready(dev))
        return DCN41_E_UNINIT;
    if (timeout_us == 0u)
        return DCN41_E_ARG;
    rc = rd_reg(dev, DCN41_DMCUB_CNTL, DCN41_DMCUB_CNTL_BASE_IDX, &cntl);
    if (rc) return rc;
    rc = rd_reg(dev, DCN41_DMCUB_CNTL2, DCN41_DMCUB_CNTL2_BASE_IDX, &cntl2);
    if (rc) return rc;
    if (cntl == UINT32_MAX || cntl2 == UINT32_MAX ||
        DCN41_FG(DCN41_DMCUB_CNTL, DMCUB_ENABLE, cntl) ||
        !DCN41_FG(DCN41_DMCUB_CNTL2, DMCUB_SOFT_RESET, cntl2))
        return DCN41_E_STATE;

    rc = rd_reg(dev, DCN41_MMHUBBUB_SOFT_RESET, DCN41_MMHUBBUB_SOFT_RESET_BASE_IDX, &mmhb);
    if (rc) return rc;
    if (mmhb == UINT32_MAX)
        return DCN41_E_HW;
    rc = wr_reg(dev, DCN41_MMHUBBUB_SOFT_RESET, DCN41_MMHUBBUB_SOFT_RESET_BASE_IDX,
                mmhb & ~DCN41_MMHUBBUB_SOFT_RESET__DMUIF_SOFT_RESET_MASK);
    if (rc) return rc;
    rc = wr_reg(dev, DCN41_DMCUB_SCRATCH15, DCN41_DMCUB_SCRATCH15_BASE_IDX, psp_version & 0x001100FFu);
    if (rc) return rc;

    rc = wr_reg(dev, DCN41_DMCUB_CNTL, DCN41_DMCUB_CNTL_BASE_IDX,
                cntl | DCN41_DMCUB_CNTL__DMCUB_ENABLE_MASK | DCN41_DMCUB_CNTL__DMCUB_TRACEPORT_EN_MASK);
    if (rc) return rc;
    rc = wr_reg(dev, DCN41_DMCUB_CNTL2, DCN41_DMCUB_CNTL2_BASE_IDX,
                cntl2 & ~DCN41_DMCUB_CNTL2__DMCUB_SOFT_RESET_MASK);
    if (rc) return rc;

    for (i = 0; i < timeout_us; ++i) {
        rc = rd_reg(dev, DCN41_DMCUB_SCRATCH0, DCN41_DMCUB_SCRATCH0_BASE_IDX, &scratch0);
        if (rc) return rc;
        if (scratch0 != UINT32_MAX && (scratch0 & ready_mask) == ready_mask)
            return DCN41_OK;
        dev->udelay(dev->cookie, 1);
    }
    return DCN41_E_TIMEOUT;
}
