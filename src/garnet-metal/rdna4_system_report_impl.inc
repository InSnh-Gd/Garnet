// SPDisplaysReporter merges only the PCI function's immediate children. Our accelerator
// is below Bringup/MetalNub, so its metadata is lost behind the NDRV framebuffer.
static pthread_mutex_t rdna4sp_mu = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSString *, NSDictionary *> *rdna4sp_devices;
static _Atomic(IMP) rdna4sp_original;
static Ivar rdna4sp_properties;

static NSDictionary *rdna4sp_merge(NSDictionary *report, NSDictionary *properties,
                                  NSDictionary *devices) {
    NSString *path = properties[@"io_registry_entry_path_key"];
    if (![path isKindOfClass:[NSString class]]) return report;
    NSDictionary *device = devices[path];
    id framebufferOwner = properties[@"CFBundleIdentifier"];
    if (!device || (![framebufferOwner isEqual:@"org.garnet.driver"] &&
                    ![framebufferOwner isEqual:@"com.apple.iokit.IONDRVSupport"]))
        return report;
    NSMutableDictionary *result = [report mutableCopy];
    NSString *model = device[@"model"];
    if (model.length) {
        result[@"sppci_model"] = model;
        result[@"_name"] = model;
    }
    // Report the verified accelerator's actual owner, without changing the framebuffer's registry identity.
    result[@"sppci_kextinfo"] = device[@"bundle"];
    result[@"spdisplays_mtlgpufamilysupport"] = device[@"family"];
    [result removeObjectForKey:@"spdisplays_metal"];
    result[@"_metal_path"] = device[@"acceleratorPath"];
    result[@"_spdisplays_regid"] = device[@"registryID"];
    return result;
}
static id rdna4sp_dictionary(id object, SEL command) {
    IMP original = atomic_load_explicit(&rdna4sp_original, memory_order_acquire);
    NSDictionary *report = ((id (*)(id, SEL))original)(object, command);
    id properties = object_getIvar(object, rdna4sp_properties);
    if (![report isKindOfClass:[NSDictionary class]] || ![properties isKindOfClass:[NSDictionary class]])
        return report;
    pthread_mutex_lock(&rdna4sp_mu);
    NSDictionary *devices = [rdna4sp_devices copy];
    pthread_mutex_unlock(&rdna4sp_mu);
    return rdna4sp_merge(report, properties, devices);
}
static void rdna4sp_install(void) {
    pthread_mutex_lock(&rdna4sp_mu);
    if (!atomic_load(&rdna4sp_original)) {
        Class cls = NSClassFromString(@"SPDisplaysDevice");
        Method method = cls ? class_getInstanceMethod(cls, sel_getUid("dictionaryRepresentation")) : NULL;
        Ivar properties = cls ? class_getInstanceVariable(cls, "properties") : NULL;
        Dl_info image;
        IMP original = method ? method_getImplementation(method) : NULL;
        if (original && properties && ivar_getTypeEncoding(properties)[0] == '@' &&
            !strcmp(method_getTypeEncoding(method), "@16@0:8") &&
            dladdr((void *)original, &image) && image.dli_fname &&
            !strcmp(image.dli_fname, "/System/Library/SystemProfiler/SPDisplaysReporter.spreporter/Contents/MacOS/SPDisplaysReporter")) {
            rdna4sp_properties = properties;
            atomic_store_explicit(&rdna4sp_original, original, memory_order_release);
            method_setImplementation(method, (IMP)rdna4sp_dictionary);
        }
    }
    pthread_mutex_unlock(&rdna4sp_mu);
}
static id rdna4sp_property(io_registry_entry_t entry, CFStringRef key) {
    return CFBridgingRelease(IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0));
}
static uint32_t rdna4sp_pci_word(id value) {
    uint32_t word = 0;
    if ([value isKindOfClass:[NSData class]] && [value length] == sizeof word) memcpy(&word, [value bytes], sizeof word);
    return word;
}
static void rdna4sp_register(id<MTLDevice> device, io_service_t accelerator) {
    if (strcmp(getprogname(), "system_profiler") && strcmp(getprogname(), "System Information")) return;
    uint64_t registryID = 0;
    if (!IOObjectConformsTo(accelerator, "GarnetAccelerator") ||
        IORegistryEntryGetRegistryEntryID(accelerator, &registryID) != KERN_SUCCESS ||
        registryID != device.registryID) return;
    id bundle = rdna4sp_property(accelerator, CFSTR("CFBundleIdentifier"));
    if (![bundle isEqual:@"org.garnet.accel"] ||
        ![rdna4sp_property(accelerator, CFSTR("MetalPluginName")) isEqual:@"GarnetMetal"] ||
        ![rdna4sp_property(accelerator, CFSTR("MetalPluginClassName")) isEqual:@"GarnetDevice"]) return;
    io_registry_entry_t entry = accelerator;
    if (IOObjectRetain(entry) != KERN_SUCCESS) return;
    io_string_t pciPath, acceleratorPath;
    BOOL found = NO;
    for (unsigned depth = 0; depth < 16; depth++) {
        if (IOObjectConformsTo(entry, "IOPCIDevice")) {
            found = rdna4sp_pci_word(rdna4sp_property(entry, CFSTR("vendor-id"))) == 0x1002 &&
                rdna4sp_pci_word(rdna4sp_property(entry, CFSTR("device-id"))) == 0x7590 &&
                IORegistryEntryGetPath(entry, kIOServicePlane, pciPath) == KERN_SUCCESS;
            break;
        }
        io_registry_entry_t parent = 0;
        kern_return_t result = IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent);
        IOObjectRelease(entry); entry = parent;
        if (result != KERN_SUCCESS || !entry) break;
    }
    if (entry) IOObjectRelease(entry);
    if (!found || IORegistryEntryGetPath(accelerator, kIOServicePlane, acceleratorPath) != KERN_SUCCESS) return;
    NSString *family = nil;
    if ([device supportsFamily:(MTLGPUFamily)5002]) family = @"spdisplays_metal4";
    else if ([device supportsFamily:(MTLGPUFamily)5001]) family = @"spdisplays_metal3";
    else if ([device supportsFamily:MTLGPUFamilyMac2]) family = @"spdisplays_metal2";
    NSMutableDictionary *metadata = [@{ @"bundle": bundle, @"acceleratorPath": @(acceleratorPath),
        @"registryID": [NSString stringWithFormat:@"%llx", (unsigned long long)registryID] } mutableCopy];
    metadata[@"family"] = family;
    metadata[@"model"] = device.name;
    pthread_mutex_lock(&rdna4sp_mu);
    if (!rdna4sp_devices) rdna4sp_devices = [NSMutableDictionary dictionary];
    rdna4sp_devices[@(pciPath)] = [metadata copy];
    pthread_mutex_unlock(&rdna4sp_mu);
    // GUI rendering can initialize Metal before the reporter bundle is loaded.
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        (void)[NSNotificationCenter.defaultCenter addObserverForName:NSBundleDidLoadNotification
            object:nil queue:nil usingBlock:^(NSNotification *note) { (void)note; rdna4sp_install(); }];
    });
    rdna4sp_install();
}
