// ---- GarnetDisplayMachine ---------------------------------------------------------------------------------------------------------------------
// aux 0.0.3 (M11H section 3.2 option C). IOGraphicsAccelerator2::requestProbe(1) calls this with accel+0x368, the provider cast at start, which for us is the
// GarnetMetalNub. The family's framebuffer discovery walk only looks at the children and clients of that argument (getChildIterator /
// copyClientWithCategory("IOFramebuffer") / getClientIterator, M11H section 3.2 and 11h.1 F4), and the nub has no framebuffer below it: RDNA4FB is attached to
// the GPU's IOPCIDevice, a sibling of the bring-up kext. So with display on and a nub argument, the walk starts at the PCI device the bring-up kext hands us
// (ops pci_device); every other case passes the family's own argument through (0.0.2). The walk makes IOService virtual calls only (no PCI config access).
GARNET_R_IOAccelDisplayMachine_267 GarnetDisplayMachine::start(IOPCIDevice *provider) {
	GARNET_AUX_ENTER(IOAccelDisplayMachine::start(provider));
	const bool on = disp_enabled(gOps);
	void *pci = (on && gCtx && gOps->pci_device) ? gOps->pci_device(gCtx) : nullptr;
	void *use = dm_walk_provider(on, garnet_provider_is_nub((IOService *)(void *)provider), (void *)provider, pci);
	garnet_trace(GARNET_TR_DM_START, (uint64_t)(uintptr_t)use, use != (void *)provider ? 1u : 0u);
	return IOAccelDisplayMachine::start((IOPCIDevice *)use);
}
bool GarnetDisplayMachine::displayModeWillChange() { return true; }   // AppleParavirtDisplayMachine 0x142c5608: `movb $1,%al`
bool GarnetDisplayMachine::displayModeDidChange()  { return true; }

// ---- GarnetMemoryMap (only ever created when the bring-up kext enables GARNET_FACT_MEMORYMAP) ------------------------------------------------------
static bool garnet_mm(uint32_t op, void *obj) {
	const bool have = gOps && gOps->mm_hook && gCtx;
	const int rc = have ? gOps->mm_hook(gCtx, op, obj) : -1;
	garnet_trace(GARNET_TR_MM, op, (uint64_t)(int64_t)rc);
	return mm_result(gOps != nullptr, have, rc);
}
bool GarnetMemoryMap::commitIntoGPUPageTable()   { GARNET_AUX_ENTER(false); return garnet_mm(GARNET_MM_COMMIT, this); }
bool GarnetMemoryMap::releaseFromGPUPageTable()  { GARNET_AUX_ENTER(false); return garnet_mm(GARNET_MM_RELEASE, this); }
bool GarnetMemoryMap::updateGPUPageTable()       { GARNET_AUX_ENTER(false); return garnet_mm(GARNET_MM_UPDATE, this); }

// ---- GarnetFramebuffer (aux 0.0.4, M5) ----------------------------------------------------------------------------------------------------------------------------------------------------
// Adapted from RDNA4FB's AMDRDNA4FB (src/RDNA4FB/src/framebuffer.cpp:1908-1941 initForPM, :2194-2442 the IOFramebuffer overrides; BSD-3-Clause, (c) the RDNA4FB authors): the overrides keep RDNA4FB's contract and answers, with the
// console geometry replaced by the immutable snapshot of GarnetDisplayOps.h. The framebuffer is a client of GarnetDisplayNub, which the bring-up kext publishes ONLY on demand (`fbpublish 2`), never at boot.
static_assert(kFbAttrEnable == kConnectionEnable && kFbAttrCheckEnable == kConnectionCheckEnable && kFbAttrFlags == kConnectionFlags && kFbAttrHlDdc == kConnectionSupportsHLDDCSense && kFbAttrPower == kConnectionPower &&
              kFbAttrPower == kIOPowerAttribute && kFbAttrCursor == kIOHardwareCursorAttribute, "the pure header's four-character codes are IOKit's");
static_assert(kFbModeId == 1u, "one mode, id 1");

static bool garnet_is_class(IOService *p, const char *want) {
	const OSMetaClass *mc = p ? p->getMetaClass() : nullptr;
	const char *nm = mc ? mc->getClassName() : nullptr;
	if (!nm) return false;
	for (; *want; ++want, ++nm) if (*want != *nm) return false;
	return *nm == 0;
}
static const GarnetDispOps *garnet_get_dops(IOService *nub) {
	if (!nub) return nullptr;
	const OSSymbol *fn = OSSymbol::withCString(GARNET_DISP_FN_SYMBOL);
	if (!fn) return nullptr;
	const GarnetDispOps *o = nullptr;
	const IOReturn rc = nub->callPlatformFunction(fn, /*waitForFunction=*/false, (void *)&o, nullptr, nullptr, nullptr);
	fn->release();
	return rc == kIOReturnSuccess ? o : nullptr;
}

// 0.0.6: the display index of the nub (its GarnetDisplayIndex OSNumber, the very property the personality matched on): 1 = the monitor B, 2 = the monitor A; 0 = none / unknown
static uint32_t garnet_provider_index(IOService *nub) {
	OSNumber *n = nub ? OSDynamicCast(OSNumber, nub->getProperty(GARNET_DISP_INDEX_KEY)) : nullptr;
	return fb_provider_index(n != nullptr, n ? (uint64_t)n->unsigned64BitValue() : 0u);
}

bool GarnetFramebuffer::start(IOService *provider) {
	GARNET_AUX_ENTER(false);
	uint32_t wsv = 0, m6v = 0;
	const bool metalWsArg = PE_parse_boot_argn("garnet-metal-ws", &wsv, sizeof(wsv));      // present with ANY value: that boot gives the DP to the GPU desktop
	const bool m6On = m6_on(PE_parse_boot_argn("garnet-m6", &m6v, sizeof(m6v)), m6v);       // 0.0.7 (M6 Stage 1a, R1): garnet-m6=1 lifts the refusal (the bring-up kext's routing guard replaces it)
	const bool metalWs = fb_metal_ws_blocks(metalWsArg, m6On);                                // what the verdict below judges: metal-ws present AND the M6 latch OFF
	const bool isNub = garnet_is_class(provider, GARNET_DISP_NUB_CLASS);
	const bool layoutOk = !metalWs && isNub && garnet_layout_gate();         // the runtime layout gate runs HERE too: on an fb2 boot no accelerator is probed (the gate's other caller), and the framebuffer must never start unproven
	const GarnetDispOps *o = layoutOk ? garnet_get_dops(provider) : nullptr;
	const uint32_t pidx = garnet_provider_index(provider);                   // 0.0.6: which display this nub is (1 = the monitor B, 2 = the monitor A)
	const uint32_t ov = layoutOk ? garnetdisp_ops_check_for(o, pidx) : (uint32_t)GARNET_DOV_NULL;      // index 2 needs an ABI-2 table: an ABI-1 bring-up kext is refused here
	GarnetDispSnap s;
	bzero(&s, sizeof(s));
	const int rc = ov == GARNET_DOV_OK ? o->snapshot((void *)provider, &s) : -1;
	const uint32_t sv = rc == 0 ? fb_snap_for_index(&s, pidx) : (uint32_t)GARNET_DSV_NULL;      // valid for ITS OWN index's geometry and carrying exactly that index
	const FbStart v = fb_start_verdict(garnet_aux_on(), metalWs, isNub, layoutOk, ov, rc, sv);
	if (o && ov == GARNET_DOV_OK && o->trace) o->trace(v == kFbStartOk ? GARNET_DTR_START_OK : GARNET_DTR_START_REFUSED, (uint64_t)v, ((uint64_t)ov << 32) | sv);
	if (v != kFbStartOk) return false;
	snap = s; dops = o; powerState = 2u; connPower = 1u; pmRegistered = false;
	const bool ok = IOFramebuffer::start(provider);
	if (o->trace) o->trace(ok ? GARNET_DTR_START_OK : GARNET_DTR_START_REFUSED, (uint64_t)(uintptr_t)this, ok ? 0u : 0xFFu);
	return ok;
}

// RDNA4FB's initForPM verbatim: three power states (0 sleep, 1 doze, 2 wake), system sleep VETOED from every state (the monitor B's pixels cannot be restored after the GPU loses power), registered with the policy maker, no sleep until
// the displays allow it, not below doze until system sleep. The only difference: the "IOPMIsPowerManaged" marker goes on our own provider (the nub), not on a PCI device.
void GarnetFramebuffer::initForPM() {
	GARNET_AUX_ENTER_V();
	if (pmRegistered) return;
	pmRegistered = true;
	static IOPMPowerState powerStates[3] = {
		{ 1, 0,                0,            0,            0, 0, 0, 0, 0, 0, 0, 0 },
		{ 1, 0,                0,            kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
		{ 1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
	};
	for (auto &state : powerStates) state.capabilityFlags |= kIOPMPreventSystemSleep;
	registerPowerDriver(this, powerStates, 3);
	temporaryPowerClampOn();
	changePowerStateTo(1);
	IOService *p = getProvider();
	if (p) p->setProperty("IOPMIsPowerManaged", true);
}

IOReturn GarnetFramebuffer::enableController() {
	GARNET_AUX_ENTER(kIOReturnNotReady);
	initForPM();
	if (dops && dops->trace) dops->trace(GARNET_DTR_ENABLE, (uint64_t)(uintptr_t)this, 0u);
	return kIOReturnSuccess;
}

IODeviceMemory *GarnetFramebuffer::getApertureRange(IOPixelAperture aperture) {
	GARNET_AUX_ENTER(nullptr);
	uint64_t phys = 0, len = 0;
	if (!fb_aperture(&snap, aperture == kIOFBSystemAperture, &phys, &len)) return nullptr;
	return IODeviceMemory::withRange((IOPhysicalAddress)phys, (IOPhysicalLength)len);      // a fresh instance (retained) per the caller's contract
}
IODeviceMemory *GarnetFramebuffer::getVRAMRange() {
	GARNET_AUX_ENTER(nullptr);
	uint64_t phys = 0, len = 0;
	if (!fb_aperture(&snap, true, &phys, &len)) return nullptr;
	return IODeviceMemory::withRange((IOPhysicalAddress)phys, (IOPhysicalLength)len);
}

const char *GarnetFramebuffer::getPixelFormats() {
	GARNET_AUX_ENTER(nullptr);
	static const char formats[] = IO32BitDirectPixels "\0";         // NUL-separated, double-NUL terminated
	return formats;
}
IOItemCount GarnetFramebuffer::getDisplayModeCount() {
	GARNET_AUX_ENTER(0);
	return fb_mode_count();
}
IOReturn GarnetFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes) {
	GARNET_AUX_ENTER(kIOReturnNotReady);
	if (!allDisplayModes) return kIOReturnBadArgument;
	allDisplayModes[0] = (IODisplayModeID)kFbModeId;
	return kIOReturnSuccess;
}
IOReturn GarnetFramebuffer::getInformationForDisplayMode(IODisplayModeID displayMode, IODisplayModeInformation *info) {
	GARNET_AUX_ENTER(kIOReturnNotReady);
	FbModeInfo m;
	if (!info || !fb_mode_info(&snap, (uint32_t)displayMode, &m)) return kIOReturnBadArgument;
	bzero(info, sizeof(*info));
	info->nominalWidth = m.w; info->nominalHeight = m.h; info->refreshRate = m.refresh1616; info->maxDepthIndex = (IOIndex)m.maxDepthIndex;
	info->flags = (m.valid ? kDisplayModeValidFlag : 0u) | (m.safe ? kDisplayModeSafeFlag : 0u) | (m.isDefault ? kDisplayModeDefaultFlag : 0u);
	return kIOReturnSuccess;
}
UInt64 GarnetFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) {
	GARNET_AUX_ENTER(0);
	return 0;                                                       // obsolete: must return 0
}
IOReturn GarnetFramebuffer::getPixelInformation(IODisplayModeID displayMode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pixelInfo) {
	GARNET_AUX_ENTER(kIOReturnNotReady);
	FbPixel p;
	if (!pixelInfo || depth < 0 || !fb_pixel_info(&snap, (uint32_t)displayMode, (uint32_t)depth, aperture == kIOFBSystemAperture, &p)) return kIOReturnBadArgument;
	bzero(pixelInfo, sizeof(*pixelInfo));
	pixelInfo->bytesPerRow = p.bytesPerRow; pixelInfo->bitsPerPixel = p.bitsPerPixel; pixelInfo->pixelType = kIORGBDirectPixels;
	pixelInfo->componentCount = p.componentCount; pixelInfo->bitsPerComponent = p.bitsPerComponent;
	for (unsigned i = 0; i < 3u; i++) pixelInfo->componentMasks[i] = p.masks[i];
	for (unsigned i = 0; i < sizeof(p.format) && i < sizeof(pixelInfo->pixelFormat); i++) pixelInfo->pixelFormat[i] = p.format[i];
	pixelInfo->activeWidth = p.w; pixelInfo->activeHeight = p.h;
	return kIOReturnSuccess;
}

IOReturn GarnetFramebuffer::getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) {
	GARNET_AUX_ENTER(kIOReturnNotReady);
	if (displayMode) *displayMode = (IODisplayModeID)kFbModeId;
	if (depth) *depth = 0;
	return kIOReturnSuccess;
}
IOReturn GarnetFramebuffer::getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) {
	GARNET_AUX_ENTER(kIOReturnNotReady);
	if (displayMode) *displayMode = (IODisplayModeID)kFbModeId;      // the base getter returns 0xE00002C7
	if (depth) *depth = 0;
	return kIOReturnSuccess;
}
IOReturn GarnetFramebuffer::setDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
	GARNET_AUX_ENTER(kIOReturnNotReady);
	if (dops && dops->trace) dops->trace(GARNET_DTR_SETMODE, (uint64_t)displayMode, (uint64_t)(int64_t)depth);
	return (depth >= 0 && fb_mode_ok((uint32_t)displayMode, (uint32_t)depth)) ? kIOReturnSuccess : kIOReturnUnsupported;      // only (1, 0)
}

IOReturn GarnetFramebuffer::getAttribute(IOSelect attribute, uintptr_t *value) {
	GARNET_AUX_ENTER(kIOReturnNotReady);
	const FbAttr a = fb_attr((uint32_t)attribute);              // 'crsr' = 0: the software cursor (no hardware cursor plane in M5)
	if (a.rc == kFbRcSuper) return IOFramebuffer::getAttribute(attribute, value);
	if (a.hasValue && value) *value = a.value;
	return kIOReturnSuccess;
}
IOReturn GarnetFramebuffer::setAttribute(IOSelect attribute, uintptr_t value) {
	GARNET_AUX_ENTER(kIOReturnNotReady);
	if (!fb_power_attr((uint32_t)attribute)) return IOFramebuffer::setAttribute(attribute, value);
	powerState = (uint32_t)value;                                // RECORDED only: no hardware action in M5 (the monitor B keeps its last image)
	if (dops && dops->power) (void)dops->power((void *)getProvider(), (uint32_t)value);
	if (dops && dops->trace) dops->trace(GARNET_DTR_POWER, (uint64_t)value, 0u);
	return kIOReturnSuccess;
}
IOItemCount GarnetFramebuffer::getConnectionCount() {
	GARNET_AUX_ENTER(0);
	return 1;
}
IOReturn GarnetFramebuffer::getAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t *value) {
	GARNET_AUX_ENTER(kIOReturnNotReady);
	const FbAttr a = fb_connection_attr(&snap, (uint32_t)attribute);
	if (a.rc == kFbRcSuper) return IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value);
	if (a.rc == kFbRcUnsupported) return kIOReturnUnsupported;
	if (a.hasValue && value) *value = a.value;
	return kIOReturnSuccess;
}
IOReturn GarnetFramebuffer::setAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t value) {
	GARNET_AUX_ENTER(kIOReturnNotReady);
	if (!fb_power_attr((uint32_t)attribute)) return IOFramebuffer::setAttributeForConnection(connectIndex, attribute, value);
	connPower = (uint32_t)value;                                 // display-sleep path of IODisplay: RECORDED only
	return kIOReturnSuccess;
}

bool GarnetFramebuffer::hasDDCConnect(IOIndex connectIndex) {
	GARNET_AUX_ENTER(false);
	return fb_has_ddc(&snap, (int32_t)connectIndex);
}
IOReturn GarnetFramebuffer::getDDCBlock(IOIndex connectIndex, UInt32 blockNumber, IOSelect blockType, IOOptionBits, UInt8 *data, IOByteCount *length) {
	GARNET_AUX_ENTER(kIOReturnNotReady);
	const uint8_t *src = nullptr; uint64_t n = 0;
	const FbDdc r = fb_ddc_block(&snap, (int32_t)connectIndex, (uint32_t)blockNumber, blockType == kIODDCBlockTypeEDID, data != nullptr && length != nullptr, length ? (uint64_t)*length : 0u, &src, &n);
	if (r == kFbDdcUnsupported) return kIOReturnUnsupported;
	if (r == kFbDdcNotFound) return kIOReturnNotFound;
	memcpy(data, src, (size_t)n);
	*length = (IOByteCount)n;
	return kIOReturnSuccess;
}
