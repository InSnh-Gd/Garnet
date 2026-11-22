//
//  fbname.cpp — rename our framebuffer's IOKit class at RUNTIME. DEFAULT OFF. See
//  fbname.hpp for what and fbname_scan.h for why the offset is searched rather than assumed.
//
//  THE WHOLE MECHANISM, IN ONE PARAGRAPH
//
//  CoreDisplay asks IOKit for the framebuffer's class name and searches it for "AMD".
//  IOObjectCopyClass resolves to OSObject::getMetaClass()->getClassName(), and OSMetaClass keeps that
//  name as a single `const OSSymbol *className` field. Replacing that pointer with an interned symbol
//  for a name containing "AMD" changes what CoreDisplay reads — with no kext install, no kernel
//  collection rebuild, no KDK and no reboot, and it is undone by writing the saved pointer back.
//
//  WHY IT IS SAFE TO ATTEMPT, stated as the four things that must all hold before a single byte moves:
//    1. The symbol we are looking for is obtained through public KPI, not guessed: OSSymbol is
//       INTERNED, so OSSymbol::withCString("RDNA4FB") returns the very object the metaclass holds.
//    2. Its offset in the metaclass is FOUND by searching, and the search must match EXACTLY ONCE.
//       Zero matches or two matches are refusals. A wrong offset can therefore never become a write.
//       (Host-tested: fbname_scan_test.cpp, 24 checks.)
//    3. The word immediately before it must be a plausible pointer AND must be an OSMetaClass whose
//       own class name is "IOFramebuffer" — i.e. superClassLink. That is a positive control on having
//       found `className` in a real OSMetaClass rather than a coincidence.
//    4. The object being written is `gMetaClass`, which lives in `__DATA,__common` — ordinary
//       writable data, verified with `nm -m` on the built kext. Not __DATA_CONST.
//  After the write, the public API getClassName() must return the new name, or the original pointer
//  is put straight back and the arm reports failure.
//
//  WHAT HAPPENS WHILE ARMED (corrected by the the reviewer review; see  — my first
//  write-up of this had it BACKWARDS in both directions):
//  metaCast / checkMetaCastWithName are a dictionary lookup followed by a POINTER walk up the
//  superclass chain. They never compare getClassName() strings, and the global class dictionary is
//  keyed by the symbol used at REGISTRATION, which this patch does not rekey. So
//  metaCast("RDNA4FB") keeps working and metaCast("AMDRDNA4FB") never starts working.
//  Identity therefore SPLITS CLEANLY: everything that MATCHES or RESOLVES (IOKit matching,
//  IOServiceMatching, registerService re-matching, our nub lookups) continues to use the OLD name and
//  is unaffected; only the STRING READERS (getClassName, IOObjectCopyClass, CFStringFind, ioreg) see
//  the new one. That is exactly the split this patch needs, and it is safer than I had claimed.
//
#include "fbname.hpp"
#include "fbname_scan.h"
#include "GarnetDriver.hpp"
#include "amd/native_display_identity.h"

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSMetaClass.h>

namespace {

struct FbnameState {
	bool             armed        { false };
	IOService       *fb           { nullptr };   // retained while we hold it
	OSMetaClass     *mc           { nullptr };   // NOT retained: metaclasses are static
	int              wordIndex    { -1 };
	const OSSymbol  *origSymbol   { nullptr };   // the pointer we saved, to write back
	const OSSymbol  *newSymbol    { nullptr };   // retained while armed
	uint32_t         lastStatus   { 0 };
	uint64_t         arms         { 0 }, disarms { 0 }, refusals { 0 };
	char             seenName[64] { 0 };
};

FbnameState gFb;

// Status codes, reported as out[1] and in the log.
enum {
	FBN_OK              = 0,
	FBN_NO_FRAMEBUFFER  = 1,   // neither class name matched a live IOFramebuffer
	FBN_NO_METACLASS    = 2,
	FBN_NO_SYMBOL       = 3,   // OSSymbol::withCString failed
	FBN_NOT_FOUND       = 4,   // the symbol is nowhere in the scanned words
	FBN_AMBIGUOUS       = 5,   // more than one word held it
	FBN_LAYOUT          = 6,   // word 0, or the preceding word is not a pointer
	FBN_SUPERCLASS      = 7,   // the preceding word is not IOFramebuffer's metaclass
	FBN_READBACK        = 8,   // the write did not take: restored, nothing left behind
	FBN_ALREADY         = 9,
	FBN_BAD_ARG         = 10,
	FBN_UNPROVEN_LEAF   = 11,  // generic Apple/shared or otherwise unproven leaf metaclass
};

const char *status_name(uint32_t s) {
	switch (s) {
	case FBN_OK: return "OK";
	case FBN_NO_FRAMEBUFFER: return "no framebuffer";
	case FBN_NO_METACLASS: return "no metaclass";
	case FBN_NO_SYMBOL: return "no symbol";
	case FBN_NOT_FOUND: return "className NOT FOUND in the scanned words";
	case FBN_AMBIGUOUS: return "AMBIGUOUS - more than one word matched";
	case FBN_LAYOUT: return "layout check failed";
	case FBN_SUPERCLASS: return "the preceding word is not IOFramebuffer's metaclass";
	case FBN_READBACK: return "read-back failed - RESTORED";
	case FBN_ALREADY: return "already in that state";
	case FBN_UNPROVEN_LEAF: return "leaf metaclass is not the proven local GarnetPrimaryFramebuffer";
	default: return "bad argument";
	}
}

// The identity helper is the only lookup path: it proves the target GPU, the unique console
// framebuffer and OTG0 before this mutation is considered. Returns RETAINED, or nullptr.
IOService *find_primary() {
	IOService *fb = nullptr;
	if (rdna4_display_primary(&fb)) return fb;
	if (fb) fb->release();
	return nullptr;
}

// Put the original pointer back. Unconditional, idempotent, and it verifies itself.
uint32_t do_restore(const char *why) {
	if (!gFb.armed || !gFb.mc || gFb.wordIndex < 0 || !gFb.origSymbol) {
		gFb.armed = false;
		return FBN_OK;
	}
	uintptr_t *words = reinterpret_cast<uintptr_t *>(gFb.mc);
	words[gFb.wordIndex] = reinterpret_cast<uintptr_t>(gFb.origSymbol);
	const char *now = gFb.mc->getClassName();
	const char *origName = gFb.origSymbol->getCStringNoCopy();
	const bool ok = now && origName && !strcmp(now, origName);
	RDNA4LOG("fbname: RESTORE (%s) -> className now \"%s\" %s", why, now ? now : "(null)",
	       ok ? "(verified)" : "*** NOT THE ORIGINAL ***");
	gFb.armed = false;
	gFb.disarms++;
	// Release BOTH extra references we took. The metaclass keeps its own reference to the original
	// symbol (from class registration), so dropping ours cannot free it; the new symbol is freed here
	// because nothing points at it any more. the reviewer review: origSymbol was leaking.
	if (gFb.newSymbol) { gFb.newSymbol->release(); gFb.newSymbol = nullptr; }
	if (gFb.origSymbol) { gFb.origSymbol->release(); }
	if (gFb.fb) { gFb.fb->release(); gFb.fb = nullptr; }
	gFb.mc = nullptr;
	gFb.wordIndex = -1;
	gFb.origSymbol = nullptr;   /* released just above */
	return ok ? FBN_OK : FBN_READBACK;
}

uint32_t do_arm(void) {
	if (gFb.armed) return FBN_ALREADY;

	IOService *fb = find_primary();
	if (!fb) { RDNA4LOG("fbname: no verified target-GPU console primary - REFUSING"); return FBN_NO_FRAMEBUFFER; }

	const OSMetaClass *cmc = fb->getMetaClass();
	const char *leaf = cmc ? cmc->getClassName() : nullptr;
	if (!cmc || !leaf) { fb->release(); RDNA4LOG("fbname: no metaclass - REFUSING"); return FBN_NO_METACLASS; }
	snprintf(gFb.seenName, sizeof(gFb.seenName), "%s", leaf);
	if (!rdna4fbname::fbname_leaf_is_local(leaf)) {
		RDNA4LOG("fbname: REFUSING to rename %s%s; only the local GarnetPrimaryFramebuffer/AMDGarnetPrimaryFramebuffer leaf is proven, and a shared Apple metaclass must never be mutated",
		         leaf, rdna4fbname::fbname_leaf_is_apple_shared(leaf) ? " (Apple-shared class)" : " (unproven leaf)");
		fb->release();
		gFb.refusals++;
		return FBN_UNPROVEN_LEAF;
	}
	if (strcmp(leaf, RDNA4_PRIMARY_FBNAME_NEW) == 0) {
		RDNA4LOG("fbname: the framebuffer's class is already \"%s\" - REFUSING", leaf);
		fb->release();
		return FBN_ALREADY;
	}
	const char *wantStr = leaf;
	const char *freshStr = RDNA4_PRIMARY_FBNAME_NEW;

	// (1) the exact symbol, through public KPI: OSSymbol is interned.
	const OSSymbol *want = OSSymbol::withCString(wantStr);
	if (!want) { fb->release(); return FBN_NO_SYMBOL; }
	// The class name alone is not enough: a shared metaclass renamed elsewhere could impersonate
	// it. Resolve the registered local class and require the live object's metaclass to be that
	// exact pointer. This is the leaf-ownership proof, not an interface-layout claim.
	const OSMetaClass *registered = OSMetaClass::getMetaClassWithName(want);
	if (!registered || cmc != registered) {
		RDNA4LOG("fbname: the live %s object is not backed by the registered local %s metaclass (registered %p, live %p) - REFUSING",
		         leaf, wantStr, registered, cmc);
		want->release(); fb->release(); gFb.refusals++;
		return FBN_UNPROVEN_LEAF;
	}

	// (2) find it, and require exactly one match.
	OSMetaClass *mc = const_cast<OSMetaClass *>(cmc);
	const uintptr_t *words = reinterpret_cast<const uintptr_t *>(mc);
	const int idx = rdna4_fbname_find_unique(words, RDNA4_FBNAME_SCAN_WORDS, reinterpret_cast<uintptr_t>(want));
	if (idx < 0) {
		const uint32_t st = (idx == RDNA4_FBNAME_FOUND_AMBIGUOUS) ? FBN_AMBIGUOUS : FBN_NOT_FOUND;
		RDNA4LOG("fbname: %s (scanned %u words of the OSMetaClass at %p) - REFUSING, nothing written",
		       status_name(st), RDNA4_FBNAME_SCAN_WORDS, mc);
		want->release(); fb->release(); gFb.refusals++;
		return st;
	}
	// (3) layout + the positive control on the superclass.
	if (!rdna4_fbname_layout_ok(words, RDNA4_FBNAME_SCAN_WORDS, idx)) {
		RDNA4LOG("fbname: className appears at word %d, whose predecessor is not a pointer - REFUSING", idx);
		want->release(); fb->release(); gFb.refusals++;
		return FBN_LAYOUT;
	}
	const OSMetaClass *super = reinterpret_cast<const OSMetaClass *>(words[idx - 1]);
	const char *superName = super ? super->getClassName() : nullptr;
	if (!superName || strcmp(superName, "IOFramebuffer") != 0) {
		RDNA4LOG("fbname: the word before className is a metaclass named \"%s\", not IOFramebuffer "
		       "- REFUSING (we have not found what we think we have)", superName ? superName : "(null)");
		want->release(); fb->release(); gFb.refusals++;
		return FBN_SUPERCLASS;
	}

	const OSSymbol *fresh = OSSymbol::withCString(freshStr);
	if (!fresh) { want->release(); fb->release(); return FBN_NO_SYMBOL; }

	// Commit. gMetaClass is __DATA,__common - ordinary writable data.
	gFb.fb = fb;                 // keep the retain
	gFb.mc = mc;
	gFb.wordIndex = idx;
	gFb.origSymbol = want;       // keep the retain: this is what we write back
	gFb.newSymbol = fresh;
	gFb.armed = true;
	reinterpret_cast<uintptr_t *>(mc)[idx] = reinterpret_cast<uintptr_t>(fresh);

	// (4) read-back through the public API, or put it straight back.
	const char *now = mc->getClassName();
	if (!now || strcmp(now, freshStr) != 0) {
		RDNA4LOG("fbname: read-back says \"%s\", expected \"%s\" - RESTORING", now ? now : "(null)", freshStr);
		(void)do_restore("read-back failed");
		gFb.refusals++;
		return FBN_READBACK;
	}
	gFb.arms++;
	RDNA4LOG("fbname: ARMED - className at word %d of the OSMetaClass %p swapped; superclass verified as "
	       "IOFramebuffer; getClassName() now returns \"%s\". CoreDisplay should resolve vendor bit 0x4. "
	       "Disarm with `accel fbname 0`.", idx, mc, now);
	return FBN_OK;
}

}  // namespace

namespace rdna4fbname {

void restore_on_unload(void) {
	if (gFb.armed) {
		RDNA4LOG("fbname: the kext is going away while armed - restoring the original class name");
		(void)do_restore("kext unload");
	}
}

uint32_t control(uint64_t arg, uint64_t *out, unsigned outCount) {
	uint32_t st;
	if (arg == 0) {
		st = gFb.armed ? do_restore("explicit fbname 0") : FBN_OK;
	} else if (arg == 1) {
		st = do_arm();
	} else {
		RDNA4LOG("fbname: argument %llu is not 0 or 1 - REFUSING", (unsigned long long)arg);
		st = FBN_BAD_ARG;
	}
	gFb.lastStatus = st;

	// Report the name as it stands NOW, read fresh through the public API rather than remembered.
	const char *nameNow = "(no framebuffer)";
	IOService *fb = find_primary();
	if (fb) {
		const OSMetaClass *mc = fb->getMetaClass();
		if (mc && mc->getClassName()) nameNow = mc->getClassName();
		fb->release();
	}
	RDNA4LOG("fbname: arg %llu -> %s (%u); armed %d; class name NOW \"%s\"; arms %llu disarms %llu refusals %llu",
	       (unsigned long long)arg, status_name(st), st, gFb.armed ? 1 : 0, nameNow,
	       (unsigned long long)gFb.arms, (unsigned long long)gFb.disarms, (unsigned long long)gFb.refusals);

	if (out) {
		if (outCount > 0) out[0] = gFb.armed ? 1u : 0u;
		if (outCount > 1) out[1] = st;
		if (outCount > 2) out[2] = (uint64_t)(int64_t)gFb.wordIndex;
		if (outCount > 3) out[3] = gFb.arms;
		if (outCount > 4) out[4] = gFb.disarms;
		if (outCount > 5) out[5] = gFb.refusals;
		// out[6]: the first 8 bytes of the CURRENT class name, so userspace can print it without a
		// string ABI. (Only 8 - rdna4test therefore prints "AMDRDNA4" for "AMDRDNA4FB"; ioreg and
		// the driver log carry the full name. Reporting limit, not truncation - .)
		if (outCount > 6) {
			uint64_t v = 0;
			for (unsigned i = 0; i < 8 && nameNow[i]; i++) {
				v |= (uint64_t)(uint8_t)nameNow[i] << (8 * i);
			}
			out[6] = v;
		}
		if (outCount > 7) out[7] = (uint64_t)(uintptr_t)gFb.mc;
	}
	return st;
}

}  // namespace rdna4fbname
