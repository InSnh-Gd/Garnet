// garnet_scanout_control.cpp — SDMA GCR rinse, scanout control modes 0..10,
// and VRAM/tiled/staged scanout copy entrypoints (split from scanout_sdma.cpp).
#include "dcn_scanout_internal.hpp"

// =====================================================================================================
// 0.0.416 (an internal design note, G2;) — MODE 7: THE SDMA CACHE-RINSE INSTRUMENT.
//
// READ-ONLY on the source. ONE SDMA COPY_LINEAR moves a 256 KiB window from VRAM offset `vramOff` into a low
// scratch buffer of ours; if `gcr` is asked for, the SDMA GCR_REQ (GL2 write-back + invalidate) goes
// immediately BEFORE the copy in the SAME submission, so it is ordered before the engine reads the source.
// Fenced as every copy here is. Then the scratch (what the engine's copy delivered) and the source (what the
// GPU's own MM window sees) are BOTH read through the MM window and compared per 256-byte line: lines fully
// matching / partly / fully differing, a 64-bit map of which of the 64 4-KiB pages hold a difference, and the
// first 8 differing dwords (offset, MM source, SDMA scratch). `vramOff == 0` is the CONTROL: a low buffer of
// our own is written with a known pattern through the MM window and the SAME comparison runs on it.
//
// The question it asks: does the engine read the same bytes at these addresses as the GPU's own view? If a
// plane window differs plain but matches with the GCR_REQ, cache coherence is the mechanism ('s (A)).
// Every log line is under 512 bytes; every buffer is our own and freed on every exit path.
// =====================================================================================================
#define RDNA4_GCR_WINDOW_BYTES 0x40000u          // 256 KiB
#define RDNA4_GCR_LINE_DWORDS  64u               // one 256-byte line
#define RDNA4_GCR_LINES        (RDNA4_GCR_WINDOW_BYTES / (RDNA4_GCR_LINE_DWORDS * 4u))   // 1024
#define RDNA4_GCR_PAGE_LINES   16u               // 4 KiB / 256 B

static int scanout_bar_index(uint32_t v) {
	static const uint32_t bars[8] = { 0x00ffffffu, 0x00ffff00u, 0x0000ffffu, 0x0000ff00u,
	                                  0x00ff00ffu, 0x00ff0000u, 0x000000ffu, 0x00000000u };
	for (int b = 0; b < 8; b++) if ((v & 0x00ffffffu) == bars[b]) return b;
	return -1;
}

static uint32_t scanout_gcr_case(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                 uint64_t arg, uint64_t *v) {
	amdgpu::VRAMAllocation sc {}, pat {};
	uint32_t st = kScanStOk;
	const bool control = (rdna4_scanout7_off(arg) == 0);
	const bool gcr = rdna4_scanout7_gcr(arg) != 0;
	uint64_t srcOff = rdna4_scanout7_off(arg);
	uint32_t rp = 0;
	st = scanout_queue_check(dev, inst, &rp);
	if (st != kScanStOk) { v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff; return st; }
	if (!control) {
		const uint32_t rr = rdna4_gcr_src_reason(srcOff, RDNA4_GCR_WINDOW_BYTES, dev.vramSizeBytes,
		                                       gBringup.gmc.vram_alloc.base(), gBringup.gmc.vram_alloc.size(),
		                                       gBringup.gmc.vram_alloc_hi.base(), gBringup.gmc.vram_alloc_hi.size());
		if (rr != kRDNA4GcrSrcOk) {
			RDNA4LOG("scanout-gcr: source window %#llx + %u byte(s) REFUSED (%s; allocation pools %#llx+%llu and "
			       "%#llx+%llu, VRAM %llu byte(s))", (unsigned long long)srcOff, (unsigned)RDNA4_GCR_WINDOW_BYTES,
			       rr == kRDNA4GcrSrcAlign ? "not 4 KiB aligned" : rr == kRDNA4GcrSrcRange ? "outside the card's VRAM"
			       : "overlaps one of our own allocations",
			       (unsigned long long)gBringup.gmc.vram_alloc.base(), (unsigned long long)gBringup.gmc.vram_alloc.size(),
			       (unsigned long long)gBringup.gmc.vram_alloc_hi.base(), (unsigned long long)gBringup.gmc.vram_alloc_hi.size(),
			       (unsigned long long)dev.vramSizeBytes);
			v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff;
			return rr == kRDNA4GcrSrcAlign ? kScanStGcrAlign : rr == kRDNA4GcrSrcRange ? kScanStGcrRange : kScanStGcrOverlap;
		}
	}
	if (!gBringup.gmc.vram_alloc.alloc(RDNA4_GCR_WINDOW_BYTES, 65536, &sc) ||
	    (control && !gBringup.gmc.vram_alloc.alloc(RDNA4_GCR_WINDOW_BYTES, 65536, &pat))) {
		RDNA4LOG("scanout-gcr: cannot allocate %u byte(s) scratch (+ control) from gmc.vram_alloc; pool free now %llu "
		       "byte(s) - REFUSING", (unsigned)RDNA4_GCR_WINDOW_BYTES, (unsigned long long)gBringup.gmc.vram_alloc.bytes_free());
		scanout_free_vram(sc); scanout_free_vram(pat);
		v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff;
		return kScanStAlloc;
	}
	const uint64_t scOff = sc.gpu_va - gBringup.gmc.vram_start;
	if (!amdgpu::bar0_range_ok(dev, scOff, RDNA4_GCR_WINDOW_BYTES) ||
	    (control && !amdgpu::bar0_range_ok(dev, pat.gpu_va - gBringup.gmc.vram_start, RDNA4_GCR_WINDOW_BYTES))) {
		scanout_free_vram(sc); scanout_free_vram(pat);
		v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff;
		return kScanStAlloc;
	}
	if (control) {
		// The control source is OUR low buffer, written through the MM window (the GPU's own view) so the same
		// comparison runs on memory we own and know. The pattern is self-naming: index | 0x5a5a0000.
		srcOff = pat.gpu_va - gBringup.gmc.vram_start;
		for (uint64_t i = 0; i < RDNA4_GCR_WINDOW_BYTES / 4u; i += 64u) {
			uint32_t p[64];
			for (uint32_t x = 0; x < 64u; x++) p[x] = 0x5a5a0000u | (uint32_t)((i + x) & 0xffffu);
			if (!rdna4_vram_write_mm(srcOff + i * 4u, p, 64)) {
				RDNA4LOG("scanout-gcr: CONTROL pattern write through the MM window failed at +%#llx - REFUSING",
				       (unsigned long long)(i * 4u));
				scanout_free_vram(sc); scanout_free_vram(pat);
				v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff;
				return kScanStSourceCtl;
			}
		}
		amdgpu::amdgpu_hdp_flush(dev);
	}
	uint64_t us = 0; uint32_t last = 0;
	st = scanout_sdma_copy_linear_gcr(dev, inst, gBringup.gmc.vram_start + srcOff,
	                                  gBringup.gmc.vram_start + scOff, RDNA4_GCR_WINDOW_BYTES, gcr,
	                                  0x5CA12000u, 1000, &us, &last);
	uint32_t full = 0, part = 0, diff = 0, ndiff = 0, nfirst = 0;
	uint32_t nd[8] = { 0 }, nmm[8] = { 0 }, nsd[8] = { 0 };
	uint64_t pageMap = 0;
	if (st == kScanStOk) {
		for (uint32_t l = 0; l < RDNA4_GCR_LINES; l++) {
			const uint64_t off = (uint64_t)l * (RDNA4_GCR_LINE_DWORDS * 4u);
			uint32_t mm[64] = { 0 }, sd[64] = { 0 };
			const bool mok = rdna4_vram_read_mm(srcOff + off, mm, RDNA4_GCR_LINE_DWORDS);
			const bool sok = rdna4_vram_read_mm(scOff + off, sd, RDNA4_GCR_LINE_DWORDS);
			uint32_t bad = 0;
			for (uint32_t k = 0; k < RDNA4_GCR_LINE_DWORDS; k++) {
				if (mok && sok && mm[k] == sd[k]) continue;
				bad++; ndiff++;
				pageMap |= 1ull << (l / RDNA4_GCR_PAGE_LINES);
				if (nfirst < 8u) { nd[nfirst] = l * RDNA4_GCR_LINE_DWORDS + k; nmm[nfirst] = mm[k]; nsd[nfirst] = sd[k]; nfirst++; }
			}
			if (bad == 0) full++;
			else if (bad == RDNA4_GCR_LINE_DWORDS) diff++;
			else part++;
		}
	}
	// out: [1] gcr | control<<1, [2] source VRAM offset, [3] scratch VRAM offset, [4] fence us | copy dwords<<32,
	// [5] lines full | part<<16 | fully-differing<<32, [6] differing dwords, [7] the 64-page difference map.
	v[1] = (uint64_t)gcr | ((uint64_t)control << 1);
	v[2] = srcOff; v[3] = scOff;
	v[4] = us | ((uint64_t)rdna4_sdma_linear_copy_dwords(gcr) << 32);
	v[5] = (uint64_t)full | ((uint64_t)part << 16) | ((uint64_t)diff << 32);
	v[6] = ndiff; v[7] = pageMap;
	RDNA4LOG("scanout-gcr: mode 7 %s (%u-byte window), GCR_REQ %s; source %#llx (MC %#llx) -> scratch %#llx (MC %#llx); "
	       "status %u, fence %#010x after %llu us, COPY_LINEAR+GCR %u dwords; lines full %u, partly %u, fully differing %u "
	       "of %u; differing dwords %u; pages with a difference %#018llx", control ? "CONTROL" : "PLANE",
	       (unsigned)RDNA4_GCR_WINDOW_BYTES, gcr ? "ON" : "off", (unsigned long long)srcOff,
	       (unsigned long long)(gBringup.gmc.vram_start + srcOff), (unsigned long long)scOff,
	       (unsigned long long)(gBringup.gmc.vram_start + scOff), st, last, (unsigned long long)us,
	       (unsigned)rdna4_sdma_linear_copy_dwords(gcr), full, part, diff, (unsigned)RDNA4_GCR_LINES, ndiff,
	       (unsigned long long)pageMap);
	if (nfirst)
		RDNA4LOG("scanout-gcr: first %u differing dword(s) (offset, MM source, SDMA scratch): %u:%#010x/%#010x "
		       "%u:%#010x/%#010x %u:%#010x/%#010x %u:%#010x/%#010x %u:%#010x/%#010x %u:%#010x/%#010x %u:%#010x/%#010x "
		       "%u:%#010x/%#010x", nfirst, nd[0], nmm[0], nsd[0], nd[1], nmm[1], nsd[1], nd[2], nmm[2], nsd[2],
		       nd[3], nmm[3], nsd[3], nd[4], nmm[4], nsd[4], nd[5], nmm[5], nsd[5], nd[6], nmm[6], nsd[6],
		       nd[7], nmm[7], nsd[7]);
	scanout_free_vram(sc); scanout_free_vram(pat);
	return st;
}

// Mode 0 reads, 1 runs the positive control, 2 restores the rectangle the positive control overwrote.
// out[0] status, [1] geometry reason | plan reason<<8 | pre-flight BAR0 pass 2<<16 | BAR0 pass 3 (after the HDP
// flush)<<32 | poison (0xdeadbeef) mismatches<<48, [2] scanout VRAM offset, [3] rect x<<48|y<<32|w<<16|h,
// [4] pre-flight BAR0 pass 1 mismatches | MM-WINDOW mismatches<<32 (0.0.337: the MM window is the verdict),
// [5] pre-flight fence us<<32 | scanout fence us, [6] rect BAR0 mismatches | rect MM-window mismatches<<32,
// [7] first mismatch index<<32 | value read (the PRE-FLIGHT's when status is 9, the rectangle's otherwise),
// [8..11] samples expected<<32|read (row 0 x=16, row 0 x=112, row 0 x=240, row 63 x=16),
// [12] passed | runs<<8 | copies<<24 | refusals<<40.
uint32_t rdna4_scanout_control(uint64_t arg, uint64_t *out, unsigned count) {
	uint64_t v[13] = { 0 };
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	// 0.0.416 (an internal design note, G2): mode 7 packs the mode, the GCR flag and the source VRAM offset
	// into the one ABI scalar (rdna4_scanout7_scalar). Modes 0..6 keep scalar == mode, byte-identical; any other
	// scalar with high bits set is refused exactly as it was before (kScanStPlan).
	const uint32_t mode = (uint32_t)(arg & RDNA4_SCANOUT_MODE_MASK);
	uint32_t st = kScanStOk;
	if (!txn.ok) {
		v[0] = kScanStNoContext;
		if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
		return kScanStNoContext;
	}
	amdgpu::VRAMAllocation a {}, b {}, c {};
	RDNA4ScanoutGeom g {};
	
	uint64_t fbOff = 0;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { st = kScanStNoContext; goto done_nolock; }
	if (arg != (uint64_t)mode && mode != 7u) { st = kScanStPlan; goto done_nolock; }
	// build 0.0.542: `scanout 9` (full) / `scanout 10` (release) take flip mode's lock BEFORE gScanoutLock (the lock order), so
	// they branch here, before this function's own gScanoutLock. v[1] carries scanout_full.h's reason; 0 captured / released.
	if (mode == RDNA4_SF_MODE || mode == RDNA4_SF_MODE_RELEASE) {
		st = rdna4_scanout_full(mode, v) ? kScanStFull : kScanStOk;
		goto done_nolock;
	}
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		st = scanout_geometry(dev, &g);
		if (st != kScanStOk) goto done;
		{
			const uint32_t gr = rdna4_scanout_geom_check(&g, &fbOff);
			v[1] = gr; v[2] = fbOff;
			RDNA4LOG("scanout: %s base %#llx len %#llx %ux%u rowBytes %u depth %u; our BAR0 %#llx size %#llx, "
			       "vramBase %#llx, vram_start(MC) %#llx vramMcBase %#llx -> geometry %s (reason %u), scanout VRAM "
			       "offset %#llx", "AMDGarnetPrimaryFramebuffer", (unsigned long long)g.fbPhys, (unsigned long long)g.fbLen, g.width,
			       g.height, g.rowBytes, g.depth, (unsigned long long)g.bar0Phys, (unsigned long long)g.bar0Size,
			       (unsigned long long)g.vramBase, (unsigned long long)gBringup.gmc.vram_start,
			       (unsigned long long)dev.vramMcBase, gr ? "REFUSED" : "OK", gr, (unsigned long long)fbOff);
			if (gr) { st = kScanStGeom; goto done; }
			if (gBringup.gmc.vram_start != dev.vramMcBase || dev.vramMcBase == 0) { st = kScanStMcBase; goto done; }
		}
		if (mode == 0) goto done;
		// build 0.0.518 (W4): the content read / FNV (3) and the thumbnail (4) read the buffer HUBP0 is scanning (A or flip
		// mode's B), not A. Both only read; every write mode below keeps the console's offset.
		if (mode == 3 || mode == 4) {
			const uint64_t rd = fm_displayed_off(fbOff, g.fbLen);
			if (rd != fbOff) RDNA4LOG("scanout: mode %u reads flip mode's B (vram+%#llx), the buffer SURFACE_EARLIEST_INUSE names",
			                        mode, (unsigned long long)rd);
			fbOff = rd;
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		if (mode == 2) {
			if (!gScanoutSave) { st = kScanStNoSave; goto done; }
			for (uint32_t y = 0; y < gScanoutSaveH; y++)
				for (uint32_t x = 0; x < gScanoutSaveW; x++) {
					const uint64_t o = fbOff + (uint64_t)(gScanoutSaveY + y) * g.rowBytes + (uint64_t)(gScanoutSaveX + x) * 4u;
					if (!rdna4_scanout_row_dst_ok(fbOff, g.fbLen, o, 4)) continue;
					amdgpu::WBAR0_32(dev, o, gScanoutSave[y * gScanoutSaveW + x]);
				}
			RDNA4LOG("scanout: RESTORED the %ux%u rectangle at (%u,%u) from the saved copy (CPU writes through BAR0, "
			       "inside the scanout only)", gScanoutSaveW, gScanoutSaveH, gScanoutSaveX, gScanoutSaveY);
			goto done;
		}
		// 0.0.279: mode 3 READS WHAT THE MONITOR SHOWS, writes nothing. CoreDisplay's legacy present copies
		// WindowServer's shadow into the kIOFBVRAMMemory mapping - RDNA4FB's getVRAMRange, this scanout - so the scanout itself
		// is the evidence. out[1] positive-control rectangle pixels still holding the kext bars (of out[2]), [3] FNV-1a over
		// every 4th pixel of every 4th row, [4] non-black sampled pixels, [5] value changes along the sampled rows, [6] samples,
		// [7] centre, [8] (16,16), [9] (w-17,16), [10] (16,h-17), [11] (w-17,h-17), [12] pixels differing from the bars in the rect.
		if (mode == 3) {
			uint32_t bars = 0, diff = 0;
			for (uint32_t y = 0; y < kScanPcH; y++)
				for (uint32_t x = 0; x < kScanPcW; x++) {
					const uint64_t o = fbOff + (uint64_t)(kScanPcY + y) * g.rowBytes + (uint64_t)(kScanPcX + x) * 4u;
					if (amdgpu::RBAR0_32(dev, o) == rdna4_pc_pixel(x, y, kScanPcW)) bars++; else diff++;
				}
			uint64_t h = 1469598103934665603ull;
			uint32_t nz = 0, changes = 0, samples = 0;
			for (uint32_t y = 0; y < g.height; y += 4) {
				uint32_t prev = amdgpu::RBAR0_32(dev, fbOff + (uint64_t)y * g.rowBytes);
				for (uint32_t x = 0; x < g.width; x += 4) {
					const uint32_t px = amdgpu::RBAR0_32(dev, fbOff + (uint64_t)y * g.rowBytes + (uint64_t)x * 4u);
					h = (h ^ px) * 1099511628211ull;
					if (px & 0x00ffffffu) nz++;
					if (px != prev) { changes++; prev = px; }
					samples++;
				}
			}
			const uint32_t sx[5] = { g.width / 2, 16, g.width - 17, 16, g.width - 17 }, sy[5] = { g.height / 2, 16, 16, g.height - 17, g.height - 17 };
			for (unsigned i = 0; i < 5; i++) v[7 + i] = amdgpu::RBAR0_32(dev, fbOff + (uint64_t)sy[i] * g.rowBytes + (uint64_t)sx[i] * 4u);
			v[1] = bars; v[2] = kScanPcW * kScanPcH; v[3] = h; v[4] = nz; v[5] = changes; v[6] = samples; v[12] = diff;
			RDNA4LOG("scanout: CONTENT READ (writes nothing): control rectangle %ux%u at (%u,%u) still holds the kext bars at %u pixel(s), %u differ; "
			       "sampled %u pixel(s) (every 4th of every 4th row): non-black %u, value changes %u, FNV-1a %#018llx; centre %#010x, "
			       "inset corners TL %#010x TR %#010x BL %#010x BR %#010x", kScanPcW, kScanPcH, kScanPcX, kScanPcY, bars, diff, samples, nz,
			       changes, (unsigned long long)h, (uint32_t)v[7], (uint32_t)v[8], (uint32_t)v[9], (uint32_t)v[10], (uint32_t)v[11]);
			goto done;
		}
		// 0.0.281: mode 4 is a READ-ONLY THUMBNAIL - the scanout averaged into 64x36 cells (30x30 px at 1920x1080,
		// every 3rd pixel of every 3rd row per cell), one log line per cell row as 64 RRGGBB hex triplets, bracketed by BEGIN/END
		// lines; tools/scanout-thumb-png.py turns a log or stream into a PNG. out[1] cols, [2] rows, [3] FNV-1a of the cells,
		// [4] cells with any non-black average, [5] brightest cell luma, [6] mean luma * 1000.
		if (mode == 4) {
			static constexpr uint32_t cols = 64, rows = 36;
			const uint32_t cw = g.width / cols, chh = g.height / rows;
			uint64_t h = 1469598103934665603ull, lumaSum = 0;
			uint32_t lit = 0, maxLuma = 0;
			static uint64_t thumbSeq = 0;
			thumbSeq++;
			RDNA4LOG("scanout-thumb: BEGIN #%llu %ux%u cells of %ux%u px from the %ux%u scanout (writes nothing)", (unsigned long long)thumbSeq,
			       cols, rows, cw, chh, g.width, g.height);
			for (uint32_t r = 0; r < rows; r++) {
				char hex[cols * 6 + 1];
				for (uint32_t col = 0; col < cols; col++) {
					uint64_t sr = 0, sg = 0, sb = 0, n = 0;
					for (uint32_t y = r * chh; y < (r + 1) * chh; y += 3)
						for (uint32_t x = col * cw; x < (col + 1) * cw; x += 3) {
							const uint32_t px = amdgpu::RBAR0_32(dev, fbOff + (uint64_t)y * g.rowBytes + (uint64_t)x * 4u);
							sr += (px >> 16) & 0xffu; sg += (px >> 8) & 0xffu; sb += px & 0xffu; n++;
						}
					const uint32_t ar = n ? (uint32_t)(sr / n) : 0u, ag = n ? (uint32_t)(sg / n) : 0u, ab = n ? (uint32_t)(sb / n) : 0u;
					const uint32_t cell = (ar << 16) | (ag << 8) | ab;
					h = (h ^ cell) * 1099511628211ull;
					const uint32_t luma = (299u * ar + 587u * ag + 114u * ab) / 1000u;
					lumaSum += luma;
					if (cell) lit++;
					if (luma > maxLuma) maxLuma = luma;
					static const char hx[] = "0123456789abcdef";
					for (unsigned k = 0; k < 6; k++) hex[col * 6 + k] = hx[(cell >> (20 - 4 * k)) & 0xfu];
				}
				hex[cols * 6] = 0;
				RDNA4LOG("scanout-thumb: #%llu row %02u %s", (unsigned long long)thumbSeq, r, hex);
			}
			RDNA4LOG("scanout-thumb: END #%llu FNV-1a %#018llx, lit cells %u of %u, brightest luma %u, mean luma %llu.%03llu",
			       (unsigned long long)thumbSeq, (unsigned long long)h, lit, cols * rows, maxLuma,
			       (unsigned long long)(lumaSum / (cols * rows)), (unsigned long long)((lumaSum * 1000u / (cols * rows)) % 1000u));
			v[1] = cols; v[2] = rows; v[3] = h; v[4] = lit; v[5] = maxLuma; v[6] = lumaSum * 1000u / (cols * rows);
			goto done;
		}
		// 0.0.345 — MODE 5: THE UNARMED DETILE PROOF. Touches NOTHING outside three scratch VRAM buffers
		// from our own pool: it never reads or writes the scanout, never needs the positive-control interlock, needs no
		// WindowServer, no Apple accelerator and no display pipe, and costs neither a reboot nor a panic.
		// proved the fence fix this way before any armed run; this is the same discipline for the packet.
		//
		// Four measurements, in order, on a 256 x 256 (2 x 2 blocks of 128 x 128) surface at 32 bpp:
		//   1. THE VERDICT. The CPU lays down a KNOWN TILED image using rdna4_addr3_64kb_2d_off_4bpe (the address
		//      equation read out of vendored addrlib, scanout_copy.h). SDMA detiles it with the new 14-dword packet
		//      into a linear buffer. Every one of the 65536 destination pixels is compared, MM window (the GPU's own
		//      view,'s verdict) and BAR0. The pattern is rdna4_tile_probe_pixel = (y<<16)|x, so a wrong dword
		//      NAMES the source pixel it actually came from, and the first six are logged decoded.
		//   2. THE CROSS-CHECK, and it is the one that makes 1 mean anything about the HARDWARE. SDMA tiles the
		//      verified linear result back (detile = 0) into a third buffer, and every position is compared against
		//      the same equation. Agreement = the hardware's ADDR3_64KB_2D layout IS our equation, measured, not
		//      assumed. 1 alone could have passed with a wrong-but-self-consistent equation only if the hardware
		//      shared the same wrong equation; 2 is what closes that.
		//   3. THE NEGATIVE CONTROL. `differs from linear` counts the positions where the tiled address is NOT the
		//      linear one. If SDMA had ignored the swizzle field and done a straight copy, 1 would still pass and 2
		//      would fail at exactly these positions - so a non-zero count here with 2 passing is what rules out
		//      "the swizzle field did nothing".
		//   4. THE ASYMMETRIC FIELD CHECK. A second detile of the SUB-RECTANGLE at tiled (128,128), 128 x 128, into
		//      linear (0,0). A round trip cannot catch a field the hardware reads differently from us if the error
		//      is symmetric between tile and detile; a sub-window at a non-zero tiled origin can, because it moves
		//      tiled_x/tiled_y, the rect and the linear pitch independently of the surface extent.
		if (mode == 5) {
			static constexpr uint32_t kTW = 256u, kTH = 256u, kTN = kTW * kTH;
			const uint64_t linBytes = (uint64_t)kTN * 4u;
			const uint64_t tilBytes = rdna4_addr3_64kb_2d_bytes_4bpe(kTW, kTH);
			uint32_t rptr = 0;
			st = scanout_queue_check(dev, inst, &rptr);
			RDNA4LOG("scanout-tiled: SDMA0 QUEUE0 inited %d enabled %d wptr %llu (ring index %u), write-back rptr %#x -> %s",
			       inst.inited, inst.enabled, (unsigned long long)inst.wptr,
			       (uint32_t)(inst.wptr & inst.ring_ptr_mask), rptr,
			       st == kScanStOk ? "IDLE" : (st == kScanStQueueBusy ? "BUSY - REFUSING" : "NOT UP - REFUSING"));
			if (st != kScanStOk) goto done;
			if (!gBringup.gmc.vram_alloc.alloc(tilBytes, 65536, &a) ||
			    !gBringup.gmc.vram_alloc.alloc(linBytes, 65536, &b) ||
			    !gBringup.gmc.vram_alloc.alloc(tilBytes, 65536, &c)) { st = kScanStAlloc; goto done; }
			const uint64_t tOff = a.gpu_va - gBringup.gmc.vram_start;     // tiled, CPU-written from the equation
			const uint64_t dOff = b.gpu_va - gBringup.gmc.vram_start;     // linear destination
			const uint64_t t2Off = c.gpu_va - gBringup.gmc.vram_start;    // tiled, SDMA-written (the cross-check)
			if (!amdgpu::bar0_range_ok(dev, tOff, tilBytes) || !amdgpu::bar0_range_ok(dev, dOff, linBytes) ||
			    !amdgpu::bar0_range_ok(dev, t2Off, tilBytes) || (tOff & 0xffffu) || (t2Off & 0xffffu)) { st = kScanStAlloc; goto done; }
			// The known tiled image, plus poison everywhere the copies must write.
			uint32_t nonLinear = 0;
			for (uint32_t y = 0; y < kTH; y++)
				for (uint32_t x = 0; x < kTW; x++) {
					const uint64_t o = rdna4_addr3_64kb_2d_off_4bpe(x, y, kTW);
					if (o != ((uint64_t)y * kTW + x) * 4u) nonLinear++;
					amdgpu::WBAR0_32(dev, tOff + o, rdna4_tile_probe_pixel(x, y));
				}
			for (uint64_t i = 0; i < linBytes / 4u; i++) amdgpu::WBAR0_32(dev, dOff + i * 4u, 0xdeadbeefu);
			for (uint64_t i = 0; i < tilBytes / 4u; i++) amdgpu::WBAR0_32(dev, t2Off + i * 4u, 0xdeadbeefu);
			amdgpu::amdgpu_hdp_flush(dev);
			v[2] = tOff; v[3] = ((uint64_t)kTW << 16) | kTH; v[7] = nonLinear;
			// 0.0.346 — THE TWO CONTROLS 0.0.345 LEFT OUT, and they are not optional: had to add
			// exactly these to the row path after 24 runs could not tell "the copy is wrong" from "the buffer was
			// never what we thought". EVERY source position and EVERY destination dword, both ways, BEFORE the copy.
			{
				uint32_t srcMm = 0, srcBar = 0, poisonBad = 0, firstSrc = 0xffffffffu, firstSrcGot = 0;
				bool mmOk = true;
				for (uint32_t i = 0; i < kTN && mmOk; i += 64) {
					uint32_t mm[64] = { 0 };
					if (!rdna4_vram_read_mm(tOff + (uint64_t)i * 4u, mm, 64)) { mmOk = false; break; }
					for (uint32_t k2 = 0; k2 < 64u; k2++) {
						// Which (x,y) owns tiled dword (i+k2)? Decode the value instead of inverting the equation:
						// the pattern is self-addressing, so re-deriving the address from it is the check.
						const uint32_t got = mm[k2];
						const uint32_t sx = got & 0xffffu, sy = got >> 16;
						const bool ok = sx < kTW && sy < kTH &&
						                rdna4_addr3_64kb_2d_off_4bpe(sx, sy, kTW) == (uint64_t)(i + k2) * 4u;
						if (!ok) {
							if (firstSrc == 0xffffffffu) { firstSrc = i + k2; firstSrcGot = got; }
							srcMm++;
						}
					}
				}
				for (uint32_t i = 0; i < kTN; i++) {
					const uint32_t got = amdgpu::RBAR0_32(dev, tOff + (uint64_t)i * 4u);
					const uint32_t sx = got & 0xffffu, sy = got >> 16;
					if (!(sx < kTW && sy < kTH && rdna4_addr3_64kb_2d_off_4bpe(sx, sy, kTW) == (uint64_t)i * 4u)) srcBar++;
					if (amdgpu::RBAR0_32(dev, dOff + (uint64_t)i * 4u) != 0xdeadbeefu) poisonBad++;
				}
				RDNA4LOG("scanout-tiled: buffers tiled %#llx (MC %#llx, %llu bytes) linear %#llx (MC %#llx) tiled2 %#llx; "
				       "surface %ux%u at 32 bpp, swizzle 3 (ADDR3_64KB_2D); %u of %u position(s) are NOT at their linear "
				       "address (the negative control)", (unsigned long long)tOff, (unsigned long long)a.gpu_va,
				       (unsigned long long)tilBytes, (unsigned long long)dOff, (unsigned long long)b.gpu_va,
				       (unsigned long long)t2Off, kTW, kTH, nonLinear, kTN);
				RDNA4LOG("scanout-tiled: CONTROLS BEFORE THE COPY, every dword both ways: the WHOLE tiled source holds the "
				       "self-addressing pattern - MM WINDOW %s %u of %u wrong, BAR0 %u wrong (first bad tiled dword %u reads "
				       "%#010x); the destination poison landed everywhere - %u of %u dword(s) were NOT 0xdeadbeef. A "
				       "non-zero here voids everything below", mmOk ? "read" : "UNREADABLE", srcMm, kTN, srcBar,
				       firstSrc == 0xffffffffu ? 0u : firstSrc, firstSrcGot, poisonBad, kTN);
				v[9] = (uint64_t)srcMm | ((uint64_t)srcBar << 16) | ((uint64_t)poisonBad << 32);
				if (!mmOk || srcMm || srcBar || poisonBad) { st = kScanStSourceCtl; goto done; }
			}
			RDNA4ScanoutTiledPlan p {};
			p.fbOff = dOff; p.srcOff = tOff; p.surfW = kTW; p.surfH = kTH;
			p.swizzle = amdgpu::kAddr3_64KB_2D; p.elementLog2 = 2u;
			p.srcX = 0; p.srcY = 0; p.dstX = 0; p.dstY = 0;
			p.linPitch = kTW; p.linSlice = kTN; p.w = kTW; p.h = kTH;
			uint32_t detileBad = 0xffffffffu, tileBad = 0xffffffffu, subBad = 0xffffffffu, discBad = 0xffffffffu;
			// 1. THE VERDICT — detile, and when it disagrees, WHERE the bytes came from.
			{
				uint64_t us = 0; uint32_t last = 0;
				st = scanout_sdma_tiled_copy(dev, inst, &p, gBringup.gmc.vram_start + tOff,
				                             gBringup.gmc.vram_start + dOff, true, 0x5CA11D00u, 1000, &us, &last);
				v[4] = us;
				uint32_t mmBad = 0, barBad = 0, poison = 0, firstIdx = 0, firstGot = 0, ns = 0;
				uint32_t sIdx[6] = { 0 }, sGot[6] = { 0 };
				uint32_t bitSet[18] = { 0 }, bucket[32] = { 0 };
				uint64_t loAddr = ~0ull, hiAddr = 0;
				bool mmOk = true;
				if (st == kScanStOk) {
					for (uint32_t i = 0; i < kTN && mmOk; i += 64) {
						uint32_t mm[64] = { 0 };
						if (!rdna4_vram_read_mm(dOff + (uint64_t)i * 4u, mm, 64)) { mmOk = false; break; }
						for (uint32_t k2 = 0; k2 < 64u; k2++) {
							const uint32_t idx = i + k2, dx = idx % kTW, dy = idx / kTW;
							if (mm[k2] == rdna4_tile_probe_pixel(dx, dy)) continue;
							if (!mmBad) { firstIdx = idx; firstGot = mm[k2]; }
							mmBad++;
							if (mm[k2] == 0xdeadbeefu) poison++;
							if (ns < 6) { sIdx[ns] = idx; sGot[ns] = mm[k2]; ns++; }
							// THE MAP: every wrong destination pixel has ONE tiled source address it should have
							// come from. Which bits of that address are always set among the wrong ones NAMES the
							// fault - a single always-set bit is a bit we and the engine disagree about; a
							// contiguous range is a truncation.
							const uint64_t sa = rdna4_addr3_64kb_2d_off_4bpe(dx, dy, kTW);
							if (sa < loAddr) loAddr = sa;
							if (sa > hiAddr) hiAddr = sa;
							for (unsigned bit = 0; bit < 18u; bit++) if (sa & (1ull << bit)) bitSet[bit]++;
							bucket[(sa >> 13) & 31u]++;
						}
					}
					for (uint32_t i = 0; i < kTN; i++)
						if (amdgpu::RBAR0_32(dev, dOff + (uint64_t)i * 4u) != rdna4_tile_probe_pixel(i % kTW, i / kTW)) barBad++;
				}
				detileBad = (st == kScanStOk && mmOk) ? mmBad : 0xffffffffu;
				v[1] = (uint64_t)mmBad | ((uint64_t)barBad << 32);
				v[6] = ((uint64_t)firstIdx << 32) | firstGot;
				RDNA4LOG("scanout-tiled: DETILE (one 14-dword COPY_TILED_SUB_WINDOW, detile 1, dcc 0, CPV 0, + FENCE): status %u, "
				       "fence %#010x after %llu us; destination %ux%u compared EVERY pixel - MM WINDOW %s %u wrong of %u "
				       "(still poison %u), BAR0 %u wrong%s", st, last, (unsigned long long)us, kTW, kTH,
				       mmOk ? "read" : "UNREADABLE", mmBad, kTN, poison, barBad,
				       (st == kScanStOk && mmOk && !mmBad) ? " - THE DETILE IS CORRECT AT EVERY POSITION" : " - FAILED");
				if (ns) {
					// What does the SOURCE actually hold at the address each wrong pixel should have come from?
					// Source right + destination wrong = the engine read somewhere else. Both wrong = our write.
					uint32_t sv[6] = { 0 };
					uint64_t sa[6] = { 0 };
					for (unsigned i2 = 0; i2 < ns; i2++) {
						sa[i2] = rdna4_addr3_64kb_2d_off_4bpe(sIdx[i2] % kTW, sIdx[i2] / kTW, kTW);
						(void)rdna4_vram_read_mm(tOff + sa[i2], &sv[i2], 1);
					}
					RDNA4LOG("scanout-tiled: DETILE first %u mismatch(es) — destination (x,y): value READ / value WANTED / the "
					       "tiled source address it should have come from / what the SOURCE HOLDS there right now -- "
					       "(%u,%u): %#010x/%#010x/%#07llx/%#010x; (%u,%u): %#010x/%#010x/%#07llx/%#010x; "
					       "(%u,%u): %#010x/%#010x/%#07llx/%#010x; (%u,%u): %#010x/%#010x/%#07llx/%#010x; "
					       "(%u,%u): %#010x/%#010x/%#07llx/%#010x; (%u,%u): %#010x/%#010x/%#07llx/%#010x", ns,
					       sIdx[0] % kTW, sIdx[0] / kTW, sGot[0], rdna4_tile_probe_pixel(sIdx[0] % kTW, sIdx[0] / kTW), (unsigned long long)sa[0], sv[0],
					       sIdx[1] % kTW, sIdx[1] / kTW, sGot[1], rdna4_tile_probe_pixel(sIdx[1] % kTW, sIdx[1] / kTW), (unsigned long long)sa[1], sv[1],
					       sIdx[2] % kTW, sIdx[2] / kTW, sGot[2], rdna4_tile_probe_pixel(sIdx[2] % kTW, sIdx[2] / kTW), (unsigned long long)sa[2], sv[2],
					       sIdx[3] % kTW, sIdx[3] / kTW, sGot[3], rdna4_tile_probe_pixel(sIdx[3] % kTW, sIdx[3] / kTW), (unsigned long long)sa[3], sv[3],
					       sIdx[4] % kTW, sIdx[4] / kTW, sGot[4], rdna4_tile_probe_pixel(sIdx[4] % kTW, sIdx[4] / kTW), (unsigned long long)sa[4], sv[4],
					       sIdx[5] % kTW, sIdx[5] / kTW, sGot[5], rdna4_tile_probe_pixel(sIdx[5] % kTW, sIdx[5] / kTW), (unsigned long long)sa[5], sv[5]);
					RDNA4LOG("scanout-tiled: DETILE MAP — of %u wrong pixel(s), the tiled source address spans %#07llx..%#07llx; "
					       "wrong pixels whose source address has bit N set, N=2..17: %u %u %u %u %u %u %u %u %u %u %u %u %u %u "
					       "%u %u (a count equal to %u means EVERY wrong pixel has that bit set, 0 means none do)",
					       mmBad, (unsigned long long)(loAddr == ~0ull ? 0 : loAddr), (unsigned long long)hiAddr,
					       bitSet[2], bitSet[3], bitSet[4], bitSet[5], bitSet[6], bitSet[7], bitSet[8], bitSet[9],
					       bitSet[10], bitSet[11], bitSet[12], bitSet[13], bitSet[14], bitSet[15], bitSet[16], bitSet[17], mmBad);
					RDNA4LOG("scanout-tiled: DETILE MAP — wrong pixels per 8 KiB of tiled source address (32 buckets, %#x total): "
					       "%u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u",
					       (unsigned)tilBytes, bucket[0], bucket[1], bucket[2], bucket[3], bucket[4], bucket[5], bucket[6], bucket[7],
					       bucket[8], bucket[9], bucket[10], bucket[11], bucket[12], bucket[13], bucket[14], bucket[15],
					       bucket[16], bucket[17], bucket[18], bucket[19], bucket[20], bucket[21], bucket[22], bucket[23],
					       bucket[24], bucket[25], bucket[26], bucket[27], bucket[28], bucket[29], bucket[30], bucket[31]);
				}
				if (st != kScanStOk) goto done;
			}
			// 2. THE CROSS-CHECK — tile the verified linear result back and compare against the same equation.
			// 0.0.346: this RUNS EVEN WHEN THE DETILE FAILED. A detile that is wrong while the tile direction is right
			// is a different fault from both being wrong, and 0.0.345 threw that distinction away by aborting first.
			// The linear buffer is re-seeded by the CPU so the tile direction starts from a KNOWN image either way.
			{
				uint64_t us = 0; uint32_t last = 0;
				uint32_t r2 = 0;
				st = scanout_queue_check(dev, inst, &r2);
				if (st != kScanStOk) { RDNA4LOG("scanout-tiled: SDMA0 QUEUE0 is not idle after the detile (status %u, rptr %#x) - "
				                              "REFUSING the cross-check", st, r2); goto done; }
				for (uint32_t y = 0; y < kTH; y++)
					for (uint32_t x = 0; x < kTW; x++)
						amdgpu::WBAR0_32(dev, dOff + ((uint64_t)y * kTW + x) * 4u, rdna4_tile_probe_pixel(x, y));
				amdgpu::amdgpu_hdp_flush(dev);
				st = scanout_sdma_tiled_copy(dev, inst, &p, gBringup.gmc.vram_start + t2Off,
				                             gBringup.gmc.vram_start + dOff, false, 0x5CA11D01u, 1000, &us, &last);
				v[4] |= us << 32;
				uint32_t bad = 0, stillPoison = 0, fx = 0, fy = 0, fgot = 0;
				if (st == kScanStOk)
					for (uint32_t y = 0; y < kTH; y++)
						for (uint32_t x = 0; x < kTW; x++) {
							const uint32_t got = amdgpu::RBAR0_32(dev, t2Off + rdna4_addr3_64kb_2d_off_4bpe(x, y, kTW));
							if (got == rdna4_tile_probe_pixel(x, y)) continue;
							if (!bad) { fx = x; fy = y; fgot = got; }
							if (got == 0xdeadbeefu) stillPoison++;
							bad++;
						}
				tileBad = st == kScanStOk ? bad : 0xffffffffu;
				v[5] = bad;
				RDNA4LOG("scanout-tiled: TILE BACK (detile 0, same packet, same swizzle, from a CPU-written linear image): "
				       "status %u, fence %#010x after %llu us; %u of %u position(s) are NOT where the addrlib equation says "
				       "(%u still hold the 0xdeadbeef poison, i.e. never written) - %s. First disagreement (%u,%u) reads "
				       "%#010x. With %u position(s) differing from linear, a pass here also rules out SDMA ignoring the "
				       "swizzle field", st, last, (unsigned long long)us, bad, kTN, stillPoison,
				       (st == kScanStOk && !bad) ? "THE WRITE SIDE AGREES WITH THE EQUATION" : "FAILED", fx, fy, fgot, nonLinear);
				if (st != kScanStOk) goto done;
			}
			// 2b. 0.0.347 — THE DISCRIMINATOR. Detile the tiled image SDMA ITSELF just wrote, which
			// TILE BACK has verified byte-for-byte against the equation, into the same linear buffer. The ONLY thing
			// that changes from step 1 is WHO WROTE THE TILED SOURCE: the CPU through BAR0, or the engine through
			// its own path. Same packet, same fields, same swizzle, same destination, same addresses.
			//   clean here + dirty in step 1  -> the packet and the equation are correct and the fault is that the
			//                                    engine does not see the CPU's writes to that buffer
			//   dirty here too               -> the tiled READ path itself is wrong and the packet is not usable
			{
				uint64_t us = 0; uint32_t last = 0;
				uint32_t r4 = 0;
				st = scanout_queue_check(dev, inst, &r4);
				if (st != kScanStOk) { RDNA4LOG("scanout-tiled: SDMA0 QUEUE0 is not idle before the discriminator (status %u, "
				                              "rptr %#x) - REFUSING", st, r4); goto done; }
				for (uint64_t i = 0; i < linBytes / 4u; i++) amdgpu::WBAR0_32(dev, dOff + i * 4u, 0xdeadbeefu);
				amdgpu::amdgpu_hdp_flush(dev);
				st = scanout_sdma_tiled_copy(dev, inst, &p, gBringup.gmc.vram_start + t2Off,
				                             gBringup.gmc.vram_start + dOff, true, 0x5CA11D03u, 1000, &us, &last);
				uint32_t bad = 0, poison2 = 0, firstIdx = 0, firstGot = 0;
				bool mmOk = true;
				if (st == kScanStOk)
					for (uint32_t i = 0; i < kTN && mmOk; i += 64) {
						uint32_t mm[64] = { 0 };
						if (!rdna4_vram_read_mm(dOff + (uint64_t)i * 4u, mm, 64)) { mmOk = false; break; }
						for (uint32_t k2 = 0; k2 < 64u; k2++) {
							const uint32_t idx = i + k2;
							if (mm[k2] == rdna4_tile_probe_pixel(idx % kTW, idx / kTW)) continue;
							if (!bad) { firstIdx = idx; firstGot = mm[k2]; }
							if (mm[k2] == 0xdeadbeefu) poison2++;
							bad++;
						}
					}
				discBad = (st == kScanStOk && mmOk) ? bad : 0xffffffffu;
				v[8] = bad;
				RDNA4LOG("scanout-tiled: DISCRIMINATOR — the SAME detile packet, the SAME destination, but the tiled source is "
				       "the one SDMA WROTE (%#llx, verified byte-for-byte by TILE BACK) instead of the one the CPU wrote "
				       "(%#llx): status %u, fence %#010x after %llu us; MM WINDOW %s %u of %u wrong (%u still poison), first "
				       "(%u,%u) reads %#010x. %s", (unsigned long long)t2Off, (unsigned long long)tOff, st, last,
				       (unsigned long long)us, mmOk ? "read" : "UNREADABLE", bad, kTN, poison2,
				       firstIdx % kTW, firstIdx / kTW, firstGot,
				       (st == kScanStOk && mmOk && !bad)
				           ? "CLEAN: the packet and the equation are CORRECT; step 1's failure is the engine not seeing the "
				             "CPU's writes to that buffer, not the tiling"
				           : "ALSO DIRTY: the tiled READ path is wrong independently of who wrote the source");
				if (st != kScanStOk) goto done;
			}
			// 3. THE ASYMMETRIC FIELD CHECK — a sub-window at a non-zero tiled origin, out of the CPU-written source.
			{
				RDNA4ScanoutTiledPlan q = p;
				q.srcX = 128; q.srcY = 128; q.dstX = 0; q.dstY = 0; q.w = 128; q.h = 128;
				uint64_t us = 0; uint32_t last = 0;
				uint32_t r3 = 0;
				st = scanout_queue_check(dev, inst, &r3);
				if (st != kScanStOk) { RDNA4LOG("scanout-tiled: SDMA0 QUEUE0 is not idle after the tile back (status %u, rptr %#x) - "
				                              "REFUSING the sub-window check", st, r3); goto done; }
				for (uint64_t i = 0; i < linBytes / 4u; i++) amdgpu::WBAR0_32(dev, dOff + i * 4u, 0xdeadbeefu);
				amdgpu::amdgpu_hdp_flush(dev);
				st = scanout_sdma_tiled_copy(dev, inst, &q, gBringup.gmc.vram_start + tOff,
				                             gBringup.gmc.vram_start + dOff, true, 0x5CA11D02u, 1000, &us, &last);
				uint32_t bad = 0, outside = 0, fx = 0, fy = 0, fgot = 0;
				bool mmOk = true;
				if (st == kScanStOk) {
					for (uint32_t y = 0; y < q.h && mmOk; y++)
						for (uint32_t x = 0; x < q.w; x += 64) {
							uint32_t mm[64] = { 0 };
							if (!rdna4_vram_read_mm(dOff + ((uint64_t)y * q.linPitch + x) * 4u, mm, 64)) { mmOk = false; break; }
							for (uint32_t k2 = 0; k2 < 64u; k2++) {
								if (mm[k2] == rdna4_tile_probe_pixel(x + k2 + q.srcX, y + q.srcY)) continue;
								if (!bad) { fx = x + k2; fy = y; fgot = mm[k2]; }
								bad++;
							}
						}
					for (uint32_t y = 0; y < 4u; y++) {
						uint32_t one = 0;
						if (rdna4_vram_read_mm(dOff + ((uint64_t)y * q.linPitch + q.w) * 4u, &one, 1) && one != 0xdeadbeefu) outside++;
					}
				}
				subBad = (st == kScanStOk && mmOk) ? bad : 0xffffffffu;
				v[5] |= (uint64_t)bad << 32;
				RDNA4LOG("scanout-tiled: SUB-WINDOW detile of tiled (%u,%u) %ux%u into linear (0,0) pitch %u: status %u, fence "
				       "%#010x after %llu us; MM WINDOW %s %u of %u wrong%s (first (%u,%u) reads %#010x); bytes written past "
				       "the rectangle in the first 4 rows: %u of 4 (must be 0)", q.srcX, q.srcY, q.w, q.h, q.linPitch, st, last,
				       (unsigned long long)us, mmOk ? "read" : "UNREADABLE", bad, q.w * q.h,
				       (st == kScanStOk && mmOk && !bad && !outside) ? " - EVERY PIXEL OF THE SUB-WINDOW IS CORRECT" : " - FAILED",
				       fx, fy, fgot, outside);
				v[10] = outside;
				if (st != kScanStOk) goto done;
			}
			RDNA4LOG("scanout-tiled: SUMMARY — detile (CPU-written source) %u wrong, tile back %u wrong, DISCRIMINATOR "
			       "(SDMA-written source) %u wrong, sub-window %u wrong, of %u each (0xffffffff = the step did not run). "
			       "None of them says whether Apple's composited surface is in THIS mode - that is what an armed run asks",
			       detileBad, tileBad, discBad, subBad, kTN);
			if (detileBad || tileBad || subBad || discBad || v[10]) st = kScanStReadback;
			goto done;
		}
		// 0.0.414 (an internal design note) — MODE 6: THE FULL-GEOMETRY SDMA SELF-TEST.
		// ONE call, two cases IN ORDER (S1): (a) 256x256, the control, and (b) 1920x1080, the live plane's
		// exact packet fields (swizzle 3, pitch 1920, 15 blocks wide, a partial 9th block row). Both run S3/S4's
		// method on OUR OWN three scratch buffers and never touch the scanout or Apple's surfaces. Mode 5's
		// 256x256 proof is byte-for-byte unchanged; this mode is purely additive. The maps and the decoded
		// pixels are in the `scanout-full:` driver-log lines (S6, every line < 512 bytes); out[] carries the
		// per-case counts: 1|2 case0 tile|detile wrong, poison|bar-mm, tile|detile sampled; 4|5|6 the same for
		// case1; 7|8 case tile-status|detile-status.
		if (mode == 6) {
			RDNA4FullCase c0 {}, c1 {};
			// build 0.0.514 B3: case (b) is the LIVE geometry (Console,Width/Height, read above into `g`),
			// 1920x1080 only when it is absent - 0.0.513's constant, so a 1080p boot runs exactly 0.0.513's case.
			const uint32_t lw = rdna4_live_dim(g.width, 1920u), lh = rdna4_live_dim(g.height, 1080u);
			const uint32_t s0 = scanout_full_case(dev, inst, 256u, 256u, 0x5CA11F00u, &c0, false);
			const uint32_t s1 = scanout_full_case(dev, inst, lw, lh, 0x5CA11F10u, &c1, false);
			v[1] = (uint64_t)c0.tileWrong | ((uint64_t)c0.detWrong << 32);
			v[2] = (uint64_t)c0.poison | ((uint64_t)c0.barMm << 32);
			v[3] = (uint64_t)c0.tileSampled | ((uint64_t)c0.detSampled << 32);
			v[4] = (uint64_t)c1.tileWrong | ((uint64_t)c1.detWrong << 32);
			v[5] = (uint64_t)c1.poison | ((uint64_t)c1.barMm << 32);
			v[6] = (uint64_t)c1.tileSampled | ((uint64_t)c1.detSampled << 32);
			v[7] = ((uint64_t)c0.tileStatus << 32) | c0.detStatus;
			v[8] = ((uint64_t)c1.tileStatus << 32) | c1.detStatus;
			st = s0 != kScanStOk ? s0 : (s1 != kScanStOk ? s1 : kScanStOk);
			// The 256x256 control must still pass. A dirty live case is a MEASUREMENT, not a refusal.
			if (st == kScanStOk && (c0.tileWrong || c0.detWrong || c0.poison)) st = kScanStReadback;
			RDNA4LOG("scanout-full: SUMMARY mode 6 — control 256x256 tile %u/%u, detile %u/%u, poison %u; live "
			       "%ux%u tile %u/%u, detile %u/%u, poison %u; statuses %u/%u and %u/%u -> %u",
			       c0.tileWrong, c0.tileSampled, c0.detWrong, c0.detSampled, c0.poison, lw, lh,
			       c1.tileWrong, c1.tileSampled, c1.detWrong, c1.detSampled, c1.poison,
			       c0.tileStatus, c0.detStatus, c1.tileStatus, c1.detStatus, st);
			goto done;
		}
		// D7 (0.0.417, an internal design note) — MODE 8: THE UNIFORM PROBE. Mode 6's live 1920x1080 case
		// EXACTLY - same buffers, same packet fields, same bounded sample set, same `scanout-full:` report lines and
		// the same POISON detection - except the CPU writes RDNA4_TILE_UNIFORM_PIXEL (0xff00ff00) everywhere and every
		// expected value is that value. It is the write-compression half of the DCC fault: with our no-PTE write
		// compression ON a constant 256-byte block is stored as a code the raw MM window reads instead of the pixels;
		// after `sdmadcc 1` the same probe must read 0 wrong. out[4..6] are the unused control slots (0); the live
		// counts are out[7..9] and the statuses out[11], exactly where mode 6 leaves its case-1 counts.
		if (mode == 8) {
			RDNA4FullCase c1 {};
			const uint32_t s1 = scanout_full_case(dev, inst, 1920u, 1080u, 0x5CA11F10u, &c1, true);
			v[4] = (uint64_t)c1.tileWrong | ((uint64_t)c1.detWrong << 32);
			v[5] = (uint64_t)c1.poison | ((uint64_t)c1.barMm << 32);
			v[6] = (uint64_t)c1.tileSampled | ((uint64_t)c1.detSampled << 32);
			v[8] = ((uint64_t)c1.tileStatus << 32) | c1.detStatus;
			st = s1;
			RDNA4LOG("scanout-full: SUMMARY mode 8 (UNIFORM probe %#010x) 1920x1080 tile %u/%u, detile %u/%u, poison %u; "
			       "statuses %u/%u -> %u; the same test with `sdmadcc 1` must read 0 wrong",
			       (unsigned)RDNA4_TILE_UNIFORM_PIXEL, c1.tileWrong, c1.tileSampled, c1.detWrong, c1.detSampled,
			       c1.poison, c1.tileStatus, c1.detStatus, st);
			goto done;
		}
		// 0.0.416 (an internal design note, G2) — MODE 7: the SDMA cache-rinse instrument. READ-ONLY on the
		// source; our own low scratch only; the same interlock-free proof discipline as modes 5/6 (three/four
		// scratch buffers, no WindowServer, no Apple accelerator, no reboot). out: 1 gcr|control<<1, 2 source
		// VRAM offset, 3 scratch VRAM offset, 4 fence us | copy dwords<<32, 5 lines full|part<<16|diff<<32,
		// 6 differing dwords, 7 the 64-page difference map. The maps and first-8 decode are in the log.
		if (mode == 7) {
			st = scanout_gcr_case(dev, inst, arg, v);
			goto done;
		}
		if (mode != 1) { st = kScanStPlan; goto done; }
		gScanoutPcRuns++;
		uint32_t rptr = 0;
		st = scanout_queue_check(dev, inst, &rptr);
		RDNA4LOG("scanout: SDMA0 QUEUE0 inited %d enabled %d wptr %llu (ring index %u), write-back rptr %#x (bytes) -> %s", inst.inited,
		       inst.enabled, (unsigned long long)inst.wptr, (uint32_t)(inst.wptr & inst.ring_ptr_mask), rptr, st == kScanStOk ? "IDLE" : (st == kScanStQueueBusy ? "BUSY - REFUSING" : "NOT UP - REFUSING"));
		if (st != kScanStOk) goto done;
		const uint32_t bytes = kScanPcW * kScanPcH * 4u;
		if (!gBringup.gmc.vram_alloc.alloc(bytes, 16384, &a) || !gBringup.gmc.vram_alloc.alloc(bytes, 16384, &b)) {
			st = kScanStAlloc; goto done;
		}
		const uint64_t aOff = a.gpu_va - gBringup.gmc.vram_start, bOff = b.gpu_va - gBringup.gmc.vram_start;
		if (!amdgpu::bar0_range_ok(dev, aOff, bytes) || !amdgpu::bar0_range_ok(dev, bOff, bytes)) { st = kScanStAlloc; goto done; }
		// The pattern, and a poisoned scratch buffer.
		for (uint32_t y = 0; y < kScanPcH; y++)
			for (uint32_t x = 0; x < kScanPcW; x++) {
				amdgpu::WBAR0_32(dev, aOff + ((uint64_t)y * kScanPcW + x) * 4u, rdna4_pc_pixel(x, y, kScanPcW));
				amdgpu::WBAR0_32(dev, bOff + ((uint64_t)y * kScanPcW + x) * 4u, 0xdeadbeefu);
			}
		amdgpu::amdgpu_hdp_flush(dev);
		// Source positive control, CPU view (every pixel) and the GPU's own view (MM window, row 0).
		{
			uint32_t bad = 0, mm[64] = { 0 }, mmBad = 0;
			for (uint32_t i = 0; i < kScanPcW * kScanPcH; i++)
				if (amdgpu::RBAR0_32(dev, aOff + (uint64_t)i * 4u) != rdna4_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW)) bad++;
			const bool mmOk = rdna4_vram_read_mm(aOff, mm, 64);
			for (uint32_t x = 0; x < 64; x++) if (mm[x] != rdna4_pc_pixel(x, 0, kScanPcW)) mmBad++;
			RDNA4LOG("scanout: pattern buffer VRAM %#llx (MC %#llx), scratch VRAM %#llx (MC %#llx), %u bytes each; source "
			       "control: BAR0 mismatches %u of %u, MM-window row 0 %s mismatches %u of 64 (mm[16] %#010x)",
			       (unsigned long long)aOff, (unsigned long long)a.gpu_va, (unsigned long long)bOff,
			       (unsigned long long)b.gpu_va, bytes, bad, kScanPcW * kScanPcH, mmOk ? "read" : "UNREADABLE", mmBad, mm[16]);
			if (bad || !mmOk || mmBad) { st = kScanStSourceCtl; goto done; }
		}
		// Pre-flight: pattern -> scratch, nothing near the scanout.
		//
		// 0.0.337 — THE INSTRUMENT. left this as the unresolved limitation: the compare was a bare
		// `bad++` with no index, no value and no second opinion, so for 24 runs "our SDMA copy corrupted 7904 of 16384
		// pixels" and "the CPU's BAR0 view of a perfectly copied buffer is stale" were indistinguishable — two faults
		// with nothing in common and no shared fix. The same bytes are now read FOUR ways:
		//   1. BAR0, first pass, with every mismatch CLASSIFIED — 0xdeadbeef is the poison this function wrote before
		//      the copy (so: never written, or a stale read of the pre-copy value); the right colour with a wrong row
		//      byte is an addressing fault; anything else is real corruption — and the first six logged with index,
		//      value read and value expected.
		//   2. BAR0 again immediately, nothing in between: a difference means the first pass was early or stale.
		//   3. BAR0 once more after amdgpu_hdp_flush — the SAME call this function already makes at the top of this
		//      block and again inside scanout_sdma_copies. No new register and no new register offset is written.
		//   4. THE MM WINDOW over the whole buffer — MM_INDEX/MM_DATA, the GPU's own view of VRAM, which this file
		//      already designates as the right instrument for memory written by SDMA rather than by a CPU (the comment
		//      on rdna4_vram_read_mm) and which the source control 20 lines above already uses on this same buffer.
		// THE MM WINDOW IS THE VERDICT. That is not a relaxation of the interlock. What the interlock protects is that
		// OUR SDMA0 QUEUE0 copy puts the right bytes in VRAM, and the MM window reads VRAM; a BAR0 aperture read of
		// memory a GPU engine has just written, in a driver that issues no HDP read-cache invalidate anywhere, is the
		// weaker instrument, not the stronger one. Every BAR0 number is still measured, logged and returned, so the two
		// paths can never disagree silently, and if the copy is genuinely wrong BOTH fail and the interlock still holds.
		{
			uint64_t us = 0; uint32_t last = 0;
			uint32_t bad = 0, bad2 = 0, bad3 = 0, mmBad = 0, poison = 0, colour = 0, other = 0;
			uint32_t firstIdx = 0, firstGot = 0, ns = 0;
			uint32_t sIdx[6] = { 0 }, sGot[6] = { 0 }, sWant[6] = { 0 };
			bool mmOk = true;
			const uint32_t total = kScanPcW * kScanPcH;
			const uint64_t s1 = a.gpu_va, d1 = b.gpu_va; const uint32_t n1 = bytes;
			// 0.0.338: two gaps 0.0.337 left open, closed BEFORE the copy runs.
			//  - `poisonBad` — nothing ever verified that the 0xdeadbeef poison actually LANDED. "poison 0
			//    after the copy" only means "the copy wrote everywhere" if the poison was everywhere first.
			//  - `srcAfter` (below, after the copy) — nothing ever re-read the SOURCE afterwards. If anything
			//    else on this machine writes VRAM under us, the source is where it would show.
			uint32_t poisonBad = 0, srcAfter = 0;
			for (uint32_t i = 0; i < total; i++)
				if (amdgpu::RBAR0_32(dev, bOff + (uint64_t)i * 4u) != 0xdeadbeefu) poisonBad++;
			uint32_t alien = 0, dRowOther = 0, dRowHist[7] = { 0 }, dBarHist[16] = { 0 }, rowBad[kScanPcH] = { 0 };
			char map[kScanPcH + 1];
			st = scanout_sdma_copies(dev, inst, &s1, &d1, &n1, 1, 0x5CA11ED0u, 500, &us, &last);
			if (st == kScanStOk) {
				for (uint32_t i = 0; i < total; i++) {
					const uint32_t want = rdna4_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW);
					const uint32_t got = amdgpu::RBAR0_32(dev, bOff + (uint64_t)i * 4u);
					if (got == want) continue;
					if (!bad) { firstIdx = i; firstGot = got; }
					bad++;
					if (got == 0xdeadbeefu) poison++;
					else if ((got & 0x00ffffffu) == (want & 0x00ffffffu)) colour++;
					else other++;
					if (ns < 6) { sIdx[ns] = i; sGot[ns] = got; sWant[ns] = want; ns++; }
					// Where did this dword come from? Decode the value back into the source pixel that owns it.
					rowBad[i / kScanPcW]++;
					const int sb = scanout_bar_index(got);
					const uint32_t sy = got >> 24;
					if (sb < 0 || sy >= kScanPcH) { alien++; continue; }
					const int dRow = (int)sy - (int)(i / kScanPcW);
					const int dBar = sb - (int)((i % kScanPcW) / 32u);
					if (dRow >= -3 && dRow <= 3) dRowHist[dRow + 3]++; else dRowOther++;
					dBarHist[(dBar + 8) & 15]++;
				}
				for (uint32_t i = 0; i < total; i++)
					if (amdgpu::RBAR0_32(dev, bOff + (uint64_t)i * 4u) != rdna4_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW)) bad2++;
				amdgpu::amdgpu_hdp_flush(dev);
				for (uint32_t i = 0; i < total; i++)
					if (amdgpu::RBAR0_32(dev, bOff + (uint64_t)i * 4u) != rdna4_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW)) bad3++;
				for (uint32_t i = 0; i < total && mmOk; i += 64) {
					uint32_t mm[64] = { 0 };
					if (!rdna4_vram_read_mm(bOff + (uint64_t)i * 4u, mm, 64)) { mmOk = false; break; }
					for (uint32_t k = 0; k < 64; k++)
						if (mm[k] != rdna4_pc_pixel((i + k) % kScanPcW, (i + k) / kScanPcW, kScanPcW)) mmBad++;
				}
				for (uint32_t i = 0; i < total; i++)
					if (amdgpu::RBAR0_32(dev, aOff + (uint64_t)i * 4u) != rdna4_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW)) srcAfter++;
			}
			for (uint32_t y = 0; y < kScanPcH; y++)
				map[y] = rowBad[y] == 0 ? '.' : (rowBad[y] >= kScanPcW ? '#' : "0123456789abcdef"[(rowBad[y] * 16u) / kScanPcW]);
			map[kScanPcH] = 0;
			v[4] = (uint64_t)bad | ((uint64_t)mmBad << 32);
			v[5] = us << 32;
			v[1] |= ((uint64_t)(bad2 & 0xffffu) << 16) | ((uint64_t)(bad3 & 0xffffu) << 32) |
			        ((uint64_t)(poison & 0xffffu) << 48);
			v[7] = ((uint64_t)firstIdx << 32) | firstGot;    // pre-flight only; the rectangle readback overwrites it if we get there
			RDNA4LOG("scanout: PRE-FLIGHT copy pattern -> scratch: status %u, fence %#010x after %llu us; BAR0 pass 1 %u of "
			       "%u wrong (poison 0xdeadbeef %u, right colour wrong row byte %u, other %u), BAR0 pass 2 (nothing in "
			       "between) %u, BAR0 pass 3 (after amdgpu_hdp_flush) %u; MM WINDOW %s %u of %u wrong. THE MM WINDOW IS "
			       "THE VERDICT : it is the GPU's own view of the VRAM the copy wrote, and no HDP "
			       "read-cache invalidate exists on the BAR0 path", st, last, (unsigned long long)us, bad, total,
			       poison, colour, other, bad2, bad3, mmOk ? "read" : "UNREADABLE", mmBad, total);
			RDNA4LOG("scanout: PRE-FLIGHT MAP, one char per 256-pixel row of the scratch buffer ('.' = that row is entirely "
			       "correct, '#' = all 256 wrong, otherwise the hex of wrong*16/256): %s", map);
			RDNA4LOG("scanout: PRE-FLIGHT DISPLACEMENT — every pattern value names its own source (colour = source x/32, top "
			       "byte = source row), so a wrong dword says where it came from: alien (not any bar value) %u; source-row "
			       "delta -3..+3 = %u %u %u %u %u %u %u and |delta|>3 %u; source-bar delta -8..+7 = %u %u %u %u %u %u %u %u "
			       "%u %u %u %u %u %u %u %u. CONTROLS THIS RUN: poison landed everywhere? %u of %u dwords were NOT 0xdeadbeef "
			       "before the copy; source still intact after the copy? %u of %u wrong",
			       alien, dRowHist[0], dRowHist[1], dRowHist[2], dRowHist[3], dRowHist[4], dRowHist[5], dRowHist[6], dRowOther,
			       dBarHist[0], dBarHist[1], dBarHist[2], dBarHist[3], dBarHist[4], dBarHist[5], dBarHist[6], dBarHist[7],
			       dBarHist[8], dBarHist[9], dBarHist[10], dBarHist[11], dBarHist[12], dBarHist[13], dBarHist[14], dBarHist[15],
			       poisonBad, total, srcAfter, total);
			RDNA4LOG("scanout: PRE-FLIGHT first %u mismatch(es) as index: read / expected -- %u: %#010x/%#010x, %u: %#010x/%#010x, "
			       "%u: %#010x/%#010x, %u: %#010x/%#010x, %u: %#010x/%#010x, %u: %#010x/%#010x", ns,
			       sIdx[0], sGot[0], sWant[0], sIdx[1], sGot[1], sWant[1], sIdx[2], sGot[2], sWant[2],
			       sIdx[3], sGot[3], sWant[3], sIdx[4], sGot[4], sWant[4], sIdx[5], sGot[5], sWant[5]);
			if (st != kScanStOk) goto done;
			if (!mmOk || mmBad) { st = kScanStPreflight; goto done; }
		}
		// Plan the rectangle, save what is there, copy, read every pixel back.
		RDNA4ScanoutPlan plan {};
		{
			const uint32_t pr = rdna4_scanout_plan_rows(&g, aOff, bytes, kScanPcW, kScanPcH, kScanPcW * 4u, 0, 0,
			                                          kScanPcX, kScanPcY, kScanPcW, kScanPcH, kScanMaxRows, &plan);
			v[1] |= (uint64_t)pr << 8;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr || plan.w != kScanPcW || plan.h != kScanPcH) { st = kScanStPlan; goto done; }
		}
		if (!gScanoutSave) gScanoutSave = static_cast<uint32_t *>(IOMalloc(kScanPcW * kScanPcH * sizeof(uint32_t)));
		if (gScanoutSave) {
			for (uint32_t y = 0; y < plan.h; y++)
				for (uint32_t x = 0; x < plan.w; x++)
					gScanoutSave[y * plan.w + x] = amdgpu::RBAR0_32(dev, rdna4_scanout_row_dst(&plan, g.rowBytes, y) + (uint64_t)x * 4u);
			gScanoutSaveX = plan.dstX; gScanoutSaveY = plan.dstY; gScanoutSaveW = plan.w; gScanoutSaveH = plan.h;
		}
		{
			uint64_t srcMc[kScanPcH], dstMc[kScanPcH]; uint32_t nb[kScanPcH];
			for (uint32_t i = 0; i < plan.h; i++) {
				const uint64_t d = rdna4_scanout_row_dst(&plan, g.rowBytes, i);
				if (!rdna4_scanout_row_dst_ok(fbOff, g.fbLen, d, plan.rowBytes)) { st = kScanStPlan; goto done; }
				srcMc[i] = gBringup.gmc.vram_start + rdna4_scanout_row_src(&plan, i);
				dstMc[i] = gBringup.gmc.vram_start + d;
				nb[i] = plan.rowBytes;
			}
			uint64_t us = 0; uint32_t last = 0;
			st = scanout_sdma_copies(dev, inst, srcMc, dstMc, nb, plan.h, 0x5CA11ED1u, 1000, &us, &last);
			v[5] |= us;
			RDNA4LOG("scanout: POSITIVE CONTROL copy %u rows x %u bytes, MC %#llx.. -> MC %#llx.. (rect %u,%u %ux%u): "
			       "status %u, fence %#010x after %llu us", plan.h, plan.rowBytes, (unsigned long long)srcMc[0],
			       (unsigned long long)dstMc[0], plan.dstX, plan.dstY, plan.w, plan.h, st, last, (unsigned long long)us);
			if (st != kScanStOk) goto done;
		}
		{
			uint32_t bad = 0, firstIdx = 0, firstVal = 0;
			for (uint32_t y = 0; y < plan.h; y++)
				for (uint32_t x = 0; x < plan.w; x++) {
					const uint32_t got = amdgpu::RBAR0_32(dev, rdna4_scanout_row_dst(&plan, g.rowBytes, y) + (uint64_t)x * 4u);
					if (got != rdna4_pc_pixel(x, y, kScanPcW)) { if (!bad) { firstIdx = y * plan.w + x; firstVal = got; } bad++; }
				}
			// 0.0.337: the same second opinion the pre-flight now takes. The rectangle is inside
			// RDNA4FB's scanout, written by the same SDMA0 QUEUE0 copy and read back through the same BAR0
			// aperture — so if the pre-flight's BAR0 view was stale this one is too. The MM window decides here
			// as well; the BAR0 count and the four named samples are still measured and still reported, so a
			// disagreement between the two paths is visible, never silent.
			uint32_t mmBad = 0; bool mmOk = true;
			for (uint32_t y = 0; y < plan.h && mmOk; y++) {
				const uint64_t rowOff = rdna4_scanout_row_dst(&plan, g.rowBytes, y);
				for (uint32_t x = 0; x < plan.w; x += 64) {
					uint32_t mm[64] = { 0 };
					if (!rdna4_vram_read_mm(rowOff + (uint64_t)x * 4u, mm, 64)) { mmOk = false; break; }
					for (uint32_t k = 0; k < 64; k++)
						if (mm[k] != rdna4_pc_pixel(x + k, y, kScanPcW)) mmBad++;
				}
			}
			v[6] = (uint64_t)bad | ((uint64_t)mmBad << 32); v[7] = ((uint64_t)firstIdx << 32) | firstVal;
			static const uint32_t sx[4] = { 16, 112, 240, 16 }, sy[4] = { 0, 0, 0, 63 };
			for (unsigned i = 0; i < 4; i++) {
				const uint32_t got = amdgpu::RBAR0_32(dev, rdna4_scanout_row_dst(&plan, g.rowBytes, sy[i]) + (uint64_t)sx[i] * 4u);
				v[8 + i] = ((uint64_t)rdna4_pc_pixel(sx[i], sy[i], kScanPcW) << 32) | got;
			}
			RDNA4LOG("scanout: READBACK of the scanout rectangle: BAR0 %u of %u pixels differ from the bars; MM WINDOW %s "
			       "%u of %u differ%s; samples through BAR0 (expected/read) row0 x16 %#010x/%#010x, row0 x112 "
			       "%#010x/%#010x, row0 x240 %#010x/%#010x, row63 x16 %#010x/%#010x", bad, plan.w * plan.h,
			       mmOk ? "read" : "UNREADABLE", mmBad, plan.w * plan.h,
			       (!mmOk || mmBad) ? " - FAILED" : " - EVERY PIXEL MATCHES IN THE GPU'S OWN VIEW",
			       (uint32_t)(v[8] >> 32), (uint32_t)v[8], (uint32_t)(v[9] >> 32), (uint32_t)v[9],
			       (uint32_t)(v[10] >> 32), (uint32_t)v[10], (uint32_t)(v[11] >> 32), (uint32_t)v[11]);
			if (!mmOk || mmBad) { st = kScanStReadback; goto done; }
			gScanoutPcPassed = true;
			gScanoutCopies++;
		}
	}
done:
	scanout_free_vram(a);
	scanout_free_vram(b);
	scanout_free_vram(c);
	if (st != kScanStOk && mode != 0) { gScanoutRefusals++; gScanoutLastReason = st; }
	IOLockUnlock(gScanoutLock);
done_nolock:
	v[0] = st;
	if (mode != 3 && mode != 4 && mode != RDNA4_SF_MODE && mode != RDNA4_SF_MODE_RELEASE)   // 0.0.280: modes 3/4 (0.0.542: 9/10) keep their own out[12]
		v[12] = (gScanoutPcPassed ? 1u : 0u) | ((uint64_t)(gScanoutPcRuns & 0xffff) << 8) |
		        ((uint64_t)(gScanoutCopies & 0xffff) << 24) | ((uint64_t)(gScanoutRefusals & 0xffff) << 40);
	RDNA4LOG("scanout: mode %u -> status %u; interlock %s (positive-control runs %u, verified copies %u, refusals %u)",
	       mode, st, gScanoutPcPassed ? "PASSED" : "not passed", gScanoutPcRuns, gScanoutCopies, gScanoutRefusals);
	if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
	return st;
}

// 0.0.412 — S2's RICHER READBACK. One sample per 16x16 destination cell (256 samples), compared with the
// source at the same geometric position: the tiled path detiles with the SAME equation the copy used, the linear path
// reads (stride, x). Read-only. The caller has already bounded how often this runs (rdna4_dpg_verify_due). Prints ONE
// line, under 480 body bytes: how many samples differ, the 256-bit cell map as four 64-bit words, and the cell and
// dst/src values of the first 8 differing samples. The cells, centres and bit packing are display_pipe_guard.h's pure
// helpers; this function only does the reads.
void rdna4_scanout_verify_cells(amdgpu::DeviceContext &dev, uint64_t fbOff, uint32_t dstX, uint32_t dstY,
                                     uint32_t w, uint32_t h, uint32_t linPitch, const RDNA4VerifySource &s)
{
	if (w == 0 || h == 0 || linPitch == 0) return;
	uint64_t map[RDNA4_DPG_VERIFY_MAP_WORDS] = { 0, 0, 0, 0 };
	uint32_t differ = 0, fx[RDNA4_DPG_VERIFY_FIRST] = { 0 }, fd[RDNA4_DPG_VERIFY_FIRST] = { 0 },
	         fs[RDNA4_DPG_VERIFY_FIRST] = { 0 };
	for (uint32_t cy = 0; cy < RDNA4_DPG_VERIFY_CELLS; cy++) {
		for (uint32_t cx = 0; cx < RDNA4_DPG_VERIFY_CELLS; cx++) {
			uint32_t x = 0, y = 0;
			rdna4_dpg_cell16_centre(cx, cy, w, h, &x, &y);
			uint32_t sv = 0;
			const uint64_t so = s.tiled
			    ? s.off + rdna4_addr3_64kb_2d_off_4bpe(s.srcX + x, s.srcY + y, s.surfW)
			    : s.off + (uint64_t)(s.srcY + y) * s.stride + (uint64_t)(s.srcX + x) * 4u;
			const bool sok = rdna4_vram_read_mm(so, &sv, 1);
			const uint64_t d = fbOff + ((uint64_t)(dstY + y) * linPitch + (dstX + x)) * 4u;
			const uint32_t dv = amdgpu::RBAR0_32(dev, d);
			if (!sok || dv != sv) {
				rdna4_dpg_verify_map_set(map, rdna4_dpg_cell16(x, y, w, h));
				if (differ < RDNA4_DPG_VERIFY_FIRST) { fx[differ] = cy * RDNA4_DPG_VERIFY_CELLS + cx; fd[differ] = dv; fs[differ] = sv; }
				differ++;
			}
		}
	}
	RDNA4LOG("scanout-copy: S2 16x16 %s %ux%u: %u of %u samples differ; map %016llx%016llx%016llx%016llx; first %u: "
	       "c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x "
	       "(cell=dst/src, src=%s)", s.tiled ? "tiled" : "linear", w, h, differ, RDNA4_DPG_VERIFY_SAMPLES,
	       (unsigned long long)map[0], (unsigned long long)map[1], (unsigned long long)map[2], (unsigned long long)map[3],
	       differ, fx[0], fd[0], fs[0], fx[1], fd[1], fs[1], fx[2], fd[2], fs[2], fx[3], fd[3], fs[3],
	       fx[4], fd[4], fs[4], fx[5], fd[5], fs[5], fx[6], fd[6], fs[6], fx[7], fd[7], fs[7],
	       s.tiled ? "tiled equation" : "linear rows");
}

// The surface copy, called from the flush hook (GarnetAccelPeer.cpp) AFTER Apple's own selector 10 returned.
// Source = a VRAM buffer [srcOff, srcOff+srcLen) holding srcW x srcH pixels at srcStride. Copies the rectangle
// (0,0,w,h) of it to (dstX,dstY), verifies rows 0 and h-1 in full (destination through BAR0, source through
// BAR0 when it is inside the aperture, else the MM window) and nine samples against the test client's pattern.
// out[0] status, [1] plan reason, [2] srcOff, [3] rect, [4] fence us, [5] row mismatches, [6] rows compared (px),
// [7] client-pattern matches<<32 | samples, [8..10] samples dst<<32|src (0,0), (w/2,h/2), (w-1,h-1).
uint32_t rdna4_scanout_copy_vram(uint64_t srcOff, uint64_t srcLen, uint32_t srcW, uint32_t srcH, uint32_t srcStride,
                                  uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h, uint64_t *out, unsigned count,
                                  bool verify) {
	uint64_t v[11] = { 0 };
	uint32_t st = kScanStOk;
	v[2] = srcOff;
	bool logit = false;   // 0.0.278
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) { v[0] = kScanStNoContext; st = kScanStNoContext; goto fin; }
	logit = gScanoutCopyCalls++ < kScanCopyLogBudget;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { v[0] = kScanStNoContext; goto fin; }
	if (!gScanoutPcPassed) { v[0] = kScanStInterlock; gScanoutRefusals++; gScanoutLastReason = kScanStInterlock; goto fin; }
	if (__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) { v[0] = kScanStFlipMode; __atomic_fetch_add(&gFm.writersHeld, 1ull, __ATOMIC_RELAXED); goto fin; }   // 0.0.518: flip mode owns A/B
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		RDNA4ScanoutGeom g {};  uint64_t fbOff = 0;
		RDNA4ScanoutPlan plan {};
		st = scanout_geometry(dev, &g);
		if (st == kScanStOk && rdna4_scanout_geom_check(&g, &fbOff)) st = kScanStGeom;
		if (st == kScanStOk && gBringup.gmc.vram_start != dev.vramMcBase) st = kScanStMcBase;
		if (st == kScanStOk) {
			const uint32_t pr = rdna4_scanout_plan_rows(&g, srcOff, srcLen, srcW, srcH, srcStride, 0, 0, dstX, dstY, w, h,
			                                          kScanCopyMaxRows, &plan);
			v[1] = pr;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr) st = kScanStPlan;
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		uint32_t rptr = 0;
		if (st == kScanStOk) st = scanout_queue_check(dev, inst, &rptr);
		if (st == kScanStOk) {
			uint64_t *srcMc = static_cast<uint64_t *>(IOMalloc(plan.h * sizeof(uint64_t)));
			uint64_t *dstMc = static_cast<uint64_t *>(IOMalloc(plan.h * sizeof(uint64_t)));
			uint32_t *nb = static_cast<uint32_t *>(IOMalloc(plan.h * sizeof(uint32_t)));
			if (!srcMc || !dstMc || !nb) st = kScanStAlloc;
			for (uint32_t i = 0; st == kScanStOk && i < plan.h; i++) {
				const uint64_t d = rdna4_scanout_row_dst(&plan, g.rowBytes, i);
				if (!rdna4_scanout_row_dst_ok(fbOff, g.fbLen, d, plan.rowBytes)) { st = kScanStPlan; break; }
				srcMc[i] = gBringup.gmc.vram_start + rdna4_scanout_row_src(&plan, i);
				dstMc[i] = gBringup.gmc.vram_start + d;
				nb[i] = plan.rowBytes;
			}
			uint64_t us = 0; uint32_t last = 0, chunks = 0;
			if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: at the write
			if (st == kScanStOk)
				st = scanout_sdma_copies_chunked(dev, inst, srcMc, dstMc, nb, plan.h, 0x5CA11E00u, 1000, &us, &last, &chunks);
			v[4] = us;
			if (logit || (st != kScanStOk && gScanoutCopyFailLogs++ < kScanCopyFailLogBudget))
				RDNA4LOG("scanout-copy: surface VRAM %#llx (+%#llx) %ux%u stride %u -> rect (%u,%u) %ux%u: %u packet(s) in %u "
				       "submission(s), first MC %#llx -> %#llx; status %u, last fence %#010x, %llu us waited (copy call #%u)",
				       (unsigned long long)srcOff, (unsigned long long)srcLen, srcW, srcH, srcStride, plan.dstX, plan.dstY,
				       plan.w, plan.h, plan.h, chunks, (unsigned long long)(srcMc ? srcMc[0] : 0),
				       (unsigned long long)(dstMc ? dstMc[0] : 0), st, last, (unsigned long long)us, gScanoutCopyCalls);
			if (srcMc) IOFree(srcMc, plan.h * sizeof(uint64_t));
			if (dstMc) IOFree(dstMc, plan.h * sizeof(uint64_t));
			if (nb) IOFree(nb, plan.h * sizeof(uint32_t));
		}
		// 0.0.288: the pipe shim's present asks for the copy WITHOUT this readback (a BAR0 read is ~0.57 MiB/s,).
		// The copy above - plan, per-row rdna4_scanout_row_dst_ok, fence - is identical either way; only the comparison
		// after it is skipped, and an unverified copy does NOT count towards gScanoutCopies.
		if (st == kScanStOk && !verify) {
			v[5] = 0; v[6] = 0; v[7] = 0;
		} else if (st == kScanStOk) {
			// Rows 0 and h-1 in full, 64 dwords per source read.
			const bool srcInBar = amdgpu::bar0_range_ok(dev, srcOff, srcLen);
			uint32_t bad = 0, compared = 0, clientOk = 0, samples = 0;
			const uint32_t rows[2] = { 0, plan.h - 1 };
			for (unsigned r = 0; r < 2; r++) {
				uint64_t so = rdna4_scanout_row_src(&plan, rows[r]);
				const uint64_t dor = rdna4_scanout_row_dst(&plan, g.rowBytes, rows[r]);
				for (uint32_t x = 0; x < plan.w; x += 64) {
					uint32_t chunk[64] = { 0 };
					const uint32_t nw = plan.w - x < 64 ? plan.w - x : 64;
					bool okr = true;
					if (srcInBar) { for (uint32_t i = 0; i < nw; i++) chunk[i] = amdgpu::RBAR0_32(dev, so + (uint64_t)(x + i) * 4u); }
					else okr = rdna4_vram_read_mm(so + (uint64_t)x * 4u, chunk, nw);
					for (uint32_t i = 0; i < nw; i++) {
						compared++;
						if (!okr || amdgpu::RBAR0_32(dev, dor + (uint64_t)(x + i) * 4u) != chunk[i]) bad++;
					}
				}
				(void)so;
			}
			const uint32_t px[3] = { 0, plan.w / 2, plan.w - 1 }, py[3] = { 0, plan.h / 2, plan.h - 1 };
			for (unsigned i = 0; i < 3; i++) {
				const uint32_t d = amdgpu::RBAR0_32(dev, rdna4_scanout_row_dst(&plan, g.rowBytes, py[i]) + (uint64_t)px[i] * 4u);
				uint32_t sv = 0;
				const uint64_t so = rdna4_scanout_row_src(&plan, py[i]) + (uint64_t)px[i] * 4u;
				if (srcInBar) sv = amdgpu::RBAR0_32(dev, so); else rdna4_vram_read_mm(so, &sv, 1);
				v[8 + i] = ((uint64_t)d << 32) | sv;
				samples++;
				if (sv == rdna4_client_pixel(px[i], py[i])) clientOk++;
			}
			v[5] = bad; v[6] = compared; v[7] = ((uint64_t)clientOk << 32) | samples;
			if (logit || bad)
			RDNA4LOG("scanout-copy: READBACK rows 0 and %u, %u pixel(s) compared destination (BAR0) against source (%s): "
			       "%u differ%s; samples dst/src (0,0) %#010x/%#010x (%u,%u) %#010x/%#010x (%u,%u) %#010x/%#010x; "
			       "the source holds the test client's pattern at %u of %u samples", plan.h - 1, compared,
			       srcInBar ? "BAR0" : "MM window", bad, bad ? " - FAILED" : " - ROWS MATCH",
			       (uint32_t)(v[8] >> 32), (uint32_t)v[8], px[1], py[1], (uint32_t)(v[9] >> 32), (uint32_t)v[9],
			       px[2], py[2], (uint32_t)(v[10] >> 32), (uint32_t)v[10], clientOk, samples);
			if (bad) st = kScanStReadback; else gScanoutCopies++;
			// 0.0.412: the 16x16 cell map over the destination, source read linearly (this path's
			// own addressing). The linear copy is byte-faithful, so a uniform source reads uniform here whatever the
			// source's true layout - which is exactly what makes S1's forced-linear result decidable.
			{
				RDNA4VerifySource vs;
				vs.off = plan.srcOff; vs.stride = plan.srcStride; vs.surfW = 0; vs.srcX = plan.srcX; vs.srcY = plan.srcY;
				vs.tiled = 0;
				rdna4_scanout_verify_cells(dev, plan.fbOff, plan.dstX, plan.dstY, plan.w, plan.h, g.rowBytes / 4u, vs);
			}
			// 0.0.278: WHAT the frame holds (read from the scanout, which the readback just matched to the source):
			// non-black pixels and value changes in rows 0, h/2 and h-1.
			if (logit && !bad) {
				const uint32_t rowsC[3] = { 0, plan.h / 2, plan.h - 1 };
				uint32_t nz[3] = { 0 }, ch3[3] = { 0 };
				for (unsigned r = 0; r < 3; r++) {
					const uint64_t dor = rdna4_scanout_row_dst(&plan, g.rowBytes, rowsC[r]);
					uint32_t prev = amdgpu::RBAR0_32(dev, dor);
					for (uint32_t x = 0; x < plan.w; x++) {
						const uint32_t pxv = amdgpu::RBAR0_32(dev, dor + (uint64_t)x * 4u);
						if (pxv & 0x00ffffffu) nz[r]++;
						if (pxv != prev) { ch3[r]++; prev = pxv; }
					}
				}
				RDNA4LOG("scanout-copy: FRAME CONTENT (scanout after the copy): rows 0/%u/%u non-black pixels %u/%u/%u of %u, value "
				       "changes along the row %u/%u/%u", plan.h / 2, plan.h - 1, nz[0], nz[1], nz[2], plan.w, ch3[0], ch3[1], ch3[2]);
			}
		}
		if (st != kScanStOk) { gScanoutRefusals++; gScanoutLastReason = st; }
		v[0] = st;
	}
	IOLockUnlock(gScanoutLock);
fin:
	if (out) for (unsigned i = 0; i < count && i < 11; i++) out[i] = v[i];
	return (uint32_t)v[0];
}

// 0.0.345 — THE TILED SURFACE COPY, the shim's present path. Same contract as rdna4_scanout_copy_vram
// above (same interlock, same lock, same geometry, same fence), but ONE COPY_TILED_SUB_WINDOW packet for the whole
// frame instead of one COPY_LINEAR per row, because CONFIRMED the source is tiled and no row address into it is
// meaningful. `swizzle` is the GFX12 ADDR3 enum, `surfW`/`surfH` are Apple's CB_COLOR0_ATTRIB2 MIP0_WIDTH/HEIGHT + 1 as
// the shim read them out of the resource - both come from the caller, neither is guessed here.
// out[0] status, [1] plan reason, [2] srcOff, [3] rect, [4] fence us, [5] readback mismatches, [6] pixels compared,
// [7] swizzle | surfW<<16 | surfH<<32, [8..10] samples dst<<32|src at (0,0), (w/2,h/2), (w-1,h-1).
// 0.0.416 (an internal design note, G3): `gcr` prepends the five-dword GCR_REQ to the SAME submission as the
// tiled copy (and so grows the ring traffic from 18 to 23 dwords). false is the 0.0.415 behaviour, byte for byte.
uint32_t rdna4_scanout_copy_tiled(uint64_t srcOff, uint64_t srcLen, uint32_t surfW, uint32_t surfH, uint32_t swizzle,
                                   uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h, uint64_t *out, unsigned count,
                                   bool verify, bool gcr) {
	uint64_t v[11] = { 0 };
	uint32_t st = kScanStOk;
	v[2] = srcOff;
	v[7] = (uint64_t)swizzle | ((uint64_t)surfW << 16) | ((uint64_t)surfH << 32);
	bool logit = false;
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) { v[0] = kScanStNoContext; st = kScanStNoContext; goto fin; }
	logit = gScanoutCopyCalls++ < kScanCopyLogBudget;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { v[0] = kScanStNoContext; goto fin; }
	if (!gScanoutPcPassed) { v[0] = kScanStInterlock; gScanoutRefusals++; gScanoutLastReason = kScanStInterlock; goto fin; }
	if (__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) { v[0] = kScanStFlipMode; __atomic_fetch_add(&gFm.writersHeld, 1ull, __ATOMIC_RELAXED); goto fin; }   // 0.0.518: flip mode owns A/B
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		RDNA4ScanoutGeom g {};  uint64_t fbOff = 0;
		RDNA4ScanoutTiledPlan plan {};
		st = scanout_geometry(dev, &g);
		if (st == kScanStOk && rdna4_scanout_geom_check(&g, &fbOff)) st = kScanStGeom;
		if (st == kScanStOk && gBringup.gmc.vram_start != dev.vramMcBase) st = kScanStMcBase;
		if (st == kScanStOk) {
			const uint32_t pr = rdna4_scanout_plan_tiled(&g, srcOff, srcLen, surfW, surfH, swizzle, 0, 0,
			                                           dstX, dstY, w, h, &plan);
			v[1] = pr;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr) st = kScanStPlan;
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		uint32_t rptr = 0;
		if (st == kScanStOk) st = scanout_queue_check(dev, inst, &rptr);
		// The emission-time check, repeated here exactly as the row path repeats rdna4_scanout_row_dst_ok per packet:
		// every byte this packet may write must lie inside [fbOff, fbOff + fbLen).
		if (st == kScanStOk && !rdna4_scanout_tiled_dst_ok(&plan, g.fbLen)) st = kScanStPlan;
		uint64_t us = 0; uint32_t last = 0;
		if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: at the write
		if (st == kScanStOk)
			st = scanout_sdma_tiled_copy(dev, inst, &plan, gBringup.gmc.vram_start + plan.srcOff,
			                             gBringup.gmc.vram_start + plan.fbOff, true, 0x5CA11E80u, 1000, &us, &last, gcr);
		v[4] = us;
		if (logit || (st != kScanStOk && gScanoutCopyFailLogs++ < kScanCopyFailLogBudget))
			RDNA4LOG("scanout-copy: TILED surface VRAM %#llx (+%#llx) %ux%u swizzle %u -> rect (%u,%u) %ux%u: ONE 14-dword "
			       "COPY_TILED_SUB_WINDOW (detile 1, dcc 0, CPV 0) + FENCE%s, %u dwords in all, tiled MC %#llx -> linear MC "
			       "%#llx pitch %u; status %u, last fence %#010x, %llu us waited (copy call #%u)", (unsigned long long)srcOff,
			       (unsigned long long)srcLen, surfW, surfH, swizzle, plan.dstX, plan.dstY, plan.w, plan.h,
			       gcr ? " + GCR_REQ (GL2 WB+INV) before it in the same submission" : "",
			       (unsigned)rdna4_sdma_tiled_copy_dwords(gcr),
			       (unsigned long long)(gBringup.gmc.vram_start + plan.srcOff),
			       (unsigned long long)(gBringup.gmc.vram_start + plan.fbOff), plan.linPitch, st, last,
			       (unsigned long long)us, gScanoutCopyCalls);
		if (st == kScanStOk && verify) {
			// Rows 0 and h-1 of the DESTINATION against the TILED source read through the same address equation.
			// Note what this can and cannot settle: it reads a LIVE source AFTER the copy, so a difference is
			// "the copy is wrong" OR "the source was re-rendered in between" ( left exactly that unresolved).
			uint32_t bad = 0, compared = 0;
			const uint32_t rows[2] = { 0, plan.h - 1u };
			for (unsigned r = 0; r < 2; r++)
				for (uint32_t x = 0; x < plan.w; x++) {
					uint32_t sv = 0;
					const uint64_t so = plan.srcOff + rdna4_addr3_64kb_2d_off_4bpe(x + plan.srcX, rows[r] + plan.srcY, plan.surfW);
					const uint64_t d = plan.fbOff + ((uint64_t)(plan.dstY + rows[r]) * plan.linPitch + plan.dstX + x) * 4u;
					if (!rdna4_vram_read_mm(so, &sv, 1)) { bad++; compared++; continue; }
					compared++;
					if (amdgpu::RBAR0_32(dev, d) != sv) bad++;
				}
			const uint32_t px[3] = { 0, plan.w / 2u, plan.w - 1u }, py[3] = { 0, plan.h / 2u, plan.h - 1u };
			for (unsigned i = 0; i < 3; i++) {
				uint32_t sv = 0;
				const uint64_t so = plan.srcOff + rdna4_addr3_64kb_2d_off_4bpe(px[i] + plan.srcX, py[i] + plan.srcY, plan.surfW);
				const uint64_t d = plan.fbOff + ((uint64_t)(plan.dstY + py[i]) * plan.linPitch + plan.dstX + px[i]) * 4u;
				(void)rdna4_vram_read_mm(so, &sv, 1);
				v[8 + i] = ((uint64_t)amdgpu::RBAR0_32(dev, d) << 32) | sv;
			}
			v[5] = bad; v[6] = compared;
			RDNA4LOG("scanout-copy: TILED READBACK rows 0 and %u, %u pixel(s) compared destination (BAR0) against the tiled "
			       "source detiled by our own addrlib equation (MM window): %u differ%s; samples dst/src (0,0) "
			       "%#010x/%#010x (%u,%u) %#010x/%#010x (%u,%u) %#010x/%#010x", plan.h - 1u, compared, bad,
			       bad ? " - FAILED" : " - EVERY COMPARED PIXEL MATCHES", (uint32_t)(v[8] >> 32), (uint32_t)v[8],
			       px[1], py[1], (uint32_t)(v[9] >> 32), (uint32_t)v[9], px[2], py[2], (uint32_t)(v[10] >> 32), (uint32_t)v[10]);
			// Same counter semantics as the row path (0.0.288): an UNVERIFIED copy does not count towards
			// gScanoutCopies, so `verified copies` keeps meaning what its name says.
			if (bad) st = kScanStReadback; else gScanoutCopies++;
			// 0.0.412: the 16x16 cell map over the destination, source detiled by the SAME addrlib
			// equation the copy was built from. This is the map that must reproduce the 1/16 grid on the glass when
			// S1 is off, and read uniform when S1 forces the linear row copy.
			{
				RDNA4VerifySource vs;
				vs.off = plan.srcOff; vs.stride = 0; vs.surfW = plan.surfW; vs.srcX = plan.srcX; vs.srcY = plan.srcY;
				vs.tiled = 1;
				rdna4_scanout_verify_cells(dev, plan.fbOff, plan.dstX, plan.dstY, plan.w, plan.h, plan.linPitch, vs);
			}
		}
		if (st != kScanStOk) { gScanoutRefusals++; gScanoutLastReason = st; }
		v[0] = st;
	}
	IOLockUnlock(gScanoutLock);
fin:
	if (out) for (unsigned i = 0; i < count && i < 11; i++) out[i] = v[i];
	return (uint32_t)v[0];
}


// 0.0.273: the STAGED surface copy. flush2 measured the test client's flushed buffer as a VidMemory with NO
// VRAM placement (getPhysicalSegment 0/0) while the client's pixels sat in the resource's system-memory backing
// (IOAccelResource2 +0x80, the IOAccelSysMemory lockForCPUAccess maps, 0x145a81ef/0x145a81fd). The GPU cannot reach that
// memory from VMID 0, so the rows are staged by the CPU into a BAR0-pool VRAM buffer (never the scanout) and copied into the
// scanout by SDMA0 QUEUE0 through the same checked plan, fence and readback as the VRAM path. Every scanout write is still
// an SDMA packet whose destination passed rdna4_scanout_row_dst_ok.
// out as rdna4_scanout_copy_vram, plus [2] the staging VRAM offset and status 17 (backing not prepared / too short).
uint32_t rdna4_scanout_copy_staged(IOMemoryDescriptor *md, uint32_t srcStride, uint32_t srcW, uint32_t srcH,
                                    uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h, uint64_t *out, unsigned count) {
	uint64_t v[11] = { 0 };
	uint32_t st = kScanStOk;
	amdgpu::VRAMAllocation stg {};
	uint32_t *row = nullptr;
	bool prepared = false;
	bool logit = false;   // 0.0.278
	rdna4_display_txn txn(rdna4peer::kLeaseLocalDisplay);
	if (!txn.ok) { v[0] = kScanStNoContext; st = kScanStNoContext; goto fin; }
	logit = gScanoutCopyCalls++ < kScanCopyLogBudget;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { v[0] = kScanStNoContext; goto fin; }
	if (!gScanoutPcPassed) { v[0] = kScanStInterlock; gScanoutRefusals++; gScanoutLastReason = kScanStInterlock; goto fin; }
	if (__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) { v[0] = kScanStFlipMode; __atomic_fetch_add(&gFm.writersHeld, 1ull, __ATOMIC_RELAXED); goto fin; }   // 0.0.518: flip mode owns A/B
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		RDNA4ScanoutGeom g {};  uint64_t fbOff = 0;
		RDNA4ScanoutPlan plan {};
		const uint32_t cw = w < srcW ? w : srcW, ch = h < srcH ? h : srcH;
		const uint64_t need = (uint64_t)(srcH - 1) * srcStride + (uint64_t)srcW * 4u;
		st = scanout_geometry(dev, &g);
		if (st == kScanStOk && rdna4_scanout_geom_check(&g, &fbOff)) st = kScanStGeom;
		if (st == kScanStOk && gBringup.gmc.vram_start != dev.vramMcBase) st = kScanStMcBase;
		if (st == kScanStOk && (!md || md->getLength() < need || cw == 0 || ch == 0 || srcStride < srcW * 4u))
			st = 17;
		// 0.0.274: flush3 measured the backing as an IOBufferMemoryDescriptor with preparationID 0 (not wired).
		// The IOKit contract for reading such memory is prepare() first and complete() after, balanced, from thread
		// context; readBytes walks physical segments that exist only while it is prepared.
		if (st == kScanStOk) {
			const IOReturn pr = md->prepare();
			prepared = (pr == kIOReturnSuccess);
			if (logit || !prepared)
				RDNA4LOG("scanout-staged: backing %p prepare() -> %#x, preparationID now %#llx", md, pr,
				       (unsigned long long)md->getPreparationID());
			if (!prepared) st = 17;
		}
		const uint32_t bytes = cw * ch * 4u;
		if (st == kScanStOk && (!gBringup.gmc.vram_alloc.alloc(bytes, 16384, &stg) ||
		                        stg.gpu_va < gBringup.gmc.vram_start ||
		                        !amdgpu::bar0_range_ok(dev, stg.gpu_va - gBringup.gmc.vram_start, bytes)))
			st = kScanStAlloc;
		const uint64_t stgOff = stg.size ? stg.gpu_va - gBringup.gmc.vram_start : 0;
		v[2] = stgOff;
		if (st == kScanStOk) {
			const uint32_t pr = rdna4_scanout_plan_rows(&g, stgOff, bytes, cw, ch, cw * 4u, 0, 0, dstX, dstY, cw, ch,
			                                          kScanCopyMaxRows, &plan);
			v[1] = pr;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr || plan.w != cw || plan.h != ch) st = kScanStPlan;
		}
		// Stage: read each source row from the backing and write it to the staging buffer through BAR0.
		if (st == kScanStOk) {
			row = static_cast<uint32_t *>(IOMalloc(cw * sizeof(uint32_t)));
			if (!row) st = kScanStAlloc;
			for (uint32_t y = 0; st == kScanStOk && y < ch; y++) {
				if (md->readBytes((IOByteCount)y * srcStride, row, cw * 4u) != cw * 4u) { st = 17; break; }
				amdgpu::bar0_memcpy_to_vram(dev, stgOff + (uint64_t)y * cw * 4u, row, cw * 4u);
			}
			amdgpu::amdgpu_hdp_flush(dev);
			// Source control: the staging buffer reads back as the backing's first and last rows.
			if (st == kScanStOk) {
				uint32_t bad = 0;
				const uint32_t rows2[2] = { 0, ch - 1 };
				for (unsigned r = 0; r < 2; r++) {
					md->readBytes((IOByteCount)rows2[r] * srcStride, row, cw * 4u);
					for (uint32_t x = 0; x < cw; x++)
						if (amdgpu::RBAR0_32(dev, stgOff + ((uint64_t)rows2[r] * cw + x) * 4u) != row[x]) bad++;
				}
				if (logit || bad)
					RDNA4LOG("scanout-staged: backing %p (%s, length %#llx) %ux%u stride %u -> staging VRAM %#llx (MC %#llx) %ux%u: "
					       "staged rows 0 and %u read back with %u mismatch(es)", md, md->getMetaClass()->getClassName(),
					       (unsigned long long)md->getLength(), srcW, srcH, srcStride, (unsigned long long)stgOff,
					       (unsigned long long)stg.gpu_va, cw, ch, ch - 1, bad);
				if (bad) st = kScanStSourceCtl;
				// 0.0.278: WHAT the frame holds, so the report can say what the screen shows - non-zero pixels and
				// distinct values in five rows, and the value at the centre and at four inset corners.
				if (logit && st == kScanStOk) {
					const uint32_t rowsC[5] = { 0, ch / 4, ch / 2, (3 * ch) / 4, ch - 1 };
					uint32_t nz[5] = { 0 }, first[5] = { 0 }, distinctish[5] = { 0 };
					for (unsigned r = 0; r < 5; r++) {
						md->readBytes((IOByteCount)rowsC[r] * srcStride, row, cw * 4u);
						first[r] = row[0];
						uint32_t prev = row[0];
						for (uint32_t x = 0; x < cw; x++) {
							if (row[x] & 0x00ffffffu) nz[r]++;
							if (row[x] != prev) { distinctish[r]++; prev = row[x]; }
						}
					}
					const uint32_t sx[5] = { cw / 2, 16, cw - 17, 16, cw - 17 }, sy[5] = { ch / 2, 16, 16, ch - 17, ch - 17 };
					uint32_t sv[5] = { 0 };
					for (unsigned i = 0; i < 5; i++) md->readBytes((IOByteCount)sy[i] * srcStride + (IOByteCount)sx[i] * 4u, &sv[i], 4);
					RDNA4LOG("scanout-staged: FRAME CONTENT (backing, BGRA little-endian dwords): rows 0/%u/%u/%u/%u non-black pixels "
					       "%u/%u/%u/%u/%u of %u, value changes along the row %u/%u/%u/%u/%u, first pixel %#010x/%#010x/%#010x/%#010x/%#010x; "
					       "centre %#010x, inset corners TL %#010x TR %#010x BL %#010x BR %#010x", ch / 4, ch / 2, (3 * ch) / 4, ch - 1,
					       nz[0], nz[1], nz[2], nz[3], nz[4], cw, distinctish[0], distinctish[1], distinctish[2], distinctish[3],
					       distinctish[4], first[0], first[1], first[2], first[3], first[4], sv[0], sv[1], sv[2], sv[3], sv[4]);
				}
			}
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		uint32_t rptr = 0;
		if (st == kScanStOk) st = scanout_queue_check(dev, inst, &rptr);
		if (st == kScanStOk) {
			uint64_t *srcMc = static_cast<uint64_t *>(IOMalloc(plan.h * sizeof(uint64_t)));
			uint64_t *dstMc = static_cast<uint64_t *>(IOMalloc(plan.h * sizeof(uint64_t)));
			uint32_t *nb = static_cast<uint32_t *>(IOMalloc(plan.h * sizeof(uint32_t)));
			if (!srcMc || !dstMc || !nb) st = kScanStAlloc;
			for (uint32_t i = 0; st == kScanStOk && i < plan.h; i++) {
				const uint64_t d = rdna4_scanout_row_dst(&plan, g.rowBytes, i);
				if (!rdna4_scanout_row_dst_ok(fbOff, g.fbLen, d, plan.rowBytes)) { st = kScanStPlan; break; }
				srcMc[i] = gBringup.gmc.vram_start + rdna4_scanout_row_src(&plan, i);
				dstMc[i] = gBringup.gmc.vram_start + d;
				nb[i] = plan.rowBytes;
			}
			uint64_t us = 0; uint32_t last = 0, chunks = 0;
			if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: at the write
			if (st == kScanStOk)
				st = scanout_sdma_copies_chunked(dev, inst, srcMc, dstMc, nb, plan.h, 0x5CA11F00u, 1000, &us, &last, &chunks);
			v[4] = us;
			if (logit || (st != kScanStOk && gScanoutCopyFailLogs++ < kScanCopyFailLogBudget))
				RDNA4LOG("scanout-staged: SDMA copy %u rows x %u bytes in %u submission(s), staging MC %#llx.. -> scanout MC %#llx.. "
				       "(rect %u,%u %ux%u): status %u, last fence %#010x, %llu us waited (copy call #%u)", plan.h, plan.rowBytes,
				       chunks, (unsigned long long)(srcMc ? srcMc[0] : 0), (unsigned long long)(dstMc ? dstMc[0] : 0), plan.dstX,
				       plan.dstY, plan.w, plan.h, st, last, (unsigned long long)us, gScanoutCopyCalls);
			if (srcMc) IOFree(srcMc, plan.h * sizeof(uint64_t));
			if (dstMc) IOFree(dstMc, plan.h * sizeof(uint64_t));
			if (nb) IOFree(nb, plan.h * sizeof(uint32_t));
		}
		// Readback: the scanout rows 0 and h-1 against the BACKING (the client's own bytes), and nine samples against the
		// client's pattern.
		if (st == kScanStOk) {
			uint32_t bad = 0, compared = 0, clientOk = 0;
			const uint32_t rows2[2] = { 0, plan.h - 1 };
			for (unsigned r = 0; r < 2; r++) {
				md->readBytes((IOByteCount)rows2[r] * srcStride, row, cw * 4u);
				const uint64_t dor = rdna4_scanout_row_dst(&plan, g.rowBytes, rows2[r]);
				for (uint32_t x = 0; x < plan.w; x++) { compared++; if (amdgpu::RBAR0_32(dev, dor + (uint64_t)x * 4u) != row[x]) bad++; }
			}
			const uint32_t px[3] = { 0, plan.w / 2, plan.w - 1 }, py[3] = { 0, plan.h / 2, plan.h - 1 };
			for (unsigned i = 0; i < 3; i++) {
				const uint32_t d = amdgpu::RBAR0_32(dev, rdna4_scanout_row_dst(&plan, g.rowBytes, py[i]) + (uint64_t)px[i] * 4u);
				uint32_t sv = 0;
				md->readBytes((IOByteCount)py[i] * srcStride + (IOByteCount)px[i] * 4u, &sv, 4);
				v[8 + i] = ((uint64_t)d << 32) | sv;
				if (sv == rdna4_client_pixel(px[i], py[i])) clientOk++;
			}
			v[5] = bad; v[6] = compared; v[7] = ((uint64_t)clientOk << 32) | 3u;
			if (logit || bad)
			RDNA4LOG("scanout-staged: READBACK rows 0 and %u of the scanout (BAR0) against the surface BACKING: %u of %u pixel(s) "
			       "differ%s; samples dst/backing (0,0) %#010x/%#010x (%u,%u) %#010x/%#010x (%u,%u) %#010x/%#010x; the backing holds "
			       "the test client's pattern at %u of 3 samples", plan.h - 1, bad, compared, bad ? " - FAILED" : " - ROWS MATCH",
			       (uint32_t)(v[8] >> 32), (uint32_t)v[8], px[1], py[1], (uint32_t)(v[9] >> 32), (uint32_t)v[9], px[2], py[2],
			       (uint32_t)(v[10] >> 32), (uint32_t)v[10], clientOk);
			if (bad) st = kScanStReadback; else gScanoutCopies++;
		}
		if (row) IOFree(row, cw * sizeof(uint32_t));
		if (prepared) md->complete();
		scanout_free_vram(stg);
		if (st != kScanStOk) { gScanoutRefusals++; gScanoutLastReason = st; }
		v[0] = st;
	}
	IOLockUnlock(gScanoutLock);
fin:
	if (out) for (unsigned i = 0; i < count && i < 11; i++) out[i] = v[i];
	return (uint32_t)v[0];
}

bool rdna4_scanout_pc_passed(void) { return gScanoutPcPassed; }

// =====================================================================================================================
// build 0.0.542 (dcn/scanout_full.h; the push condition's full-res readback) — `accel scanout 9` / `scanout full`: THE
// SURFACE THE DISPLAY ENGINE IS SCANNING OUT, WHOLE, into a kernel capture buffer the CLI pulls through kRDNA4SelReadScanFull.
// scanout_full.h holds the decision, the gate, the copy's order and the header; this is only the I/O it is given:
//   - the display: rdna4dcn::roScanSurface (the READ-ONLY device; nothing here writes a DCN register);
//   - the console: RDNA4FB's Console,* geometry (scanout_geometry, rdna4_scanout_geom_check), as every scanout path reads it;
//   - flip mode's record, read under gFmLock, which this verb TRY-locks (a present or a restore running = FLIP_BUSY) and holds
//     for the whole copy, so flip mode can neither flip nor write A or B under it;
//   - the staging: switch 89's GART read-back buffer (gFc89Rb, 1 MiB, its read-direction positive control passed), under
//     the fast-copy lock (fc_lock_get), TRY-locked (busy = STAGING_BUSY). Not yet bound this boot: REFUSED as STAGING (0.0.543 E1:
//     never bound here, under gFmLock; `gfxneuter 345` binds and proves it);
//   - each chunk: fastcopy.h's rdna4_fc_submit_wait (gScanoutLock inside it; the fence wait bounded from the doorbell) with sf_kick
//     below (fc89_kick's packet with this verb's gate), the fence in 63's write-back dword with a value never used before; a
//     timeout RETIRES the read-back buffer (89 goes inert) and latches 63 off (rdna4_fc_fence_outcome), as 89's control does.
// Lock order: gFmLock (try) -> the fast-copy lock (try) -> gScanoutLock (per chunk, inside rdna4_fc_submit_wait); gSfLock only around
// the published capture's pointer. OFF / inert: nothing below runs unless `scanout 9`, `scanout 10` or the read selector is called.
// =====================================================================================================================
