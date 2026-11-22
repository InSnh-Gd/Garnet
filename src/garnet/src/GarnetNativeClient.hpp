//
//  GarnetNativeClient.hpp - the native user client (type 'GRNT', kext 0.0.601, native step S1c).
//
//  0.0.612: the C++ class is IOAccelGarnetNativeClient (this file and GarnetNativeClient.cpp keep their names). WindowServer's sandbox admits iokit-open only for user-client classes
//  whose name starts with "IOAccel"; the service (GarnetDriver) and the type ('GRNT') are wired cleanly.
//
//  A separate IOUserClient class from the legacy GarnetUserClient (type 0): its own selector numbering 0..8 (ABI 1.0) plus 9..14, the native scanout (ABI 1.1, 0.0.603), its own ABI
//  header (GarnetNativeABI.h, shared with the Mesa Darwin backend), one client per boot at a time (0.0.627: up to four with boot-arg garnet-multisession=1), and a gate that
//  refuses to open unless this boot's S1b self-test reported POSITIVE PASS. All state and every rule live in amd/native_s1c.{h,cpp};
//  this class only checks the shape of each call (scalar counts and struct sizes EXACTLY, every struct inline) and forwards it.
//
#ifndef GarnetNativeClient_hpp
#define GarnetNativeClient_hpp

#include <IOKit/IOUserClient.h>
#include "GarnetNativeABI.h"
#include "amd/native_open_policy_pure.h"   // 0.0.656 (G6): rdna4native::policy::Route

class GarnetDriver;

class IOAccelGarnetNativeClient : public IOUserClient {
	OSDeclareDefaultStructors(IOAccelGarnetNativeClient)
	using super = IOUserClient;

public:
	// Create, privilege-check, gate and open in one step. Returns kIOReturnSuccess with *handler set, or the error IOServiceOpen must
	// see: NotPrivileged (non-root), NotReady (gate / S1b / HUNG), ExclusiveAccess (a client is open), NoMemory.
	// 0.0.612: latch the boot-arg garnet-metal-ws once (GarnetDriver::runStages calls it at start; initWithTask latches it itself if that never ran). First writer wins; never re-read.
	static void latchBootArgs();

	// 0.0.656 (G6): `attachTo` is the service the client is attached to and started on, per `route`: GarnetDriver itself (kRouteBringup, attachTo == owner, what GarnetDriver::newUserClient does) or the aux accelerator the
	// ops table's native_open hands in (kRouteAccelerator). `owner` keeps ALL engine state either way.
	static IOReturn create(GarnetDriver *owner, IOService *attachTo, rdna4native::policy::Route route, task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
	                       IOUserClient **handler);

	bool     initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties) override;
	bool     start(IOService *provider) override;   // 0.0.656: accepts only the provider the route names (rdna4native::policy::start_provider_ok)
	void     stop(IOService *provider) override;
	IOReturn clientClose() override;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target,
	                        void *reference) override;
	// memoryType = the BO handle (contract 3.4).
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;

private:
	GarnetDriver  *owner  { nullptr };
	rdna4native::policy::Route route { rdna4native::policy::kRouteBringup };   // 0.0.656 (G6): the service this client is attached to (set by create before attach / start; the accelerator route reaches the APP selector set only)
	task_t         task   { nullptr };
	bool           opened { false };   // this object owns the (single) native session
	bool           privileged { false };
	bool           adminClient { false };   // 0.0.612: admitted as an administrator (root). false = admitted by the uid-88 rule: it reaches only the selectors of rdna4native::policy::selector_allowed
	bool           wsSession { false };     // 0.0.627 (G2): admitted by the uid-88 (WindowServer) rule (policy reason kReasonWindowServer): THIS client's session is WindowServer's
	bool           appSession { false };    // 0.0.640 (G4): admitted by the allow-list rule (policy reason kReasonApp): an ordinary user application, limited to rdna4native::policy::selector_allowed_for_app
	uint64_t       appKey { 0 };            // 0.0.641: the opener's executable-name key (GarnetAppKey.h) when admitted as an APP; the strike count's key
	uint32_t       sessSlot { 0 };          // 0.0.627 (G2): this client's session (amdgpu::N1cRef): slot 0..7 = VMID 8..15 ...
	uint32_t       sessId { 0 };            // ... and the id of its open (0 = none). Every engine call carries both; a stale pair is refused.
};

#endif /* GarnetNativeClient_hpp */
