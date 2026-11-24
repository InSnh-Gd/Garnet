// ---------------------------------------------------------------------------------------------------------------
// RDNA4DynamicLibrary (Task 3.2, Metal 3 MTLDynamicLibrary) and
// RDNA4FunctionHandle / RDNA4VisibleFunctionTable (Task 3.3, Metal 3 MTLVisibleFunctionTable).
// Backed by '.grdl' container codec (rdna4_dynlib_vft.h), garnetxlate Stage 2 FFI (0073/0074),
// and host-coherent Shared RDNA4Buffer storage matching 0074 (`word[0]=slot_count, word[1+slot]=handle_id`).
//
// Wave 3B integration checklist:
//   1. GarnetDevice.m:
//      a) Place right after `#include "rdna4_binary_archive_impl.inc"` (before render/compute pipelines):
//           #include "rdna4_dynlib_vft_impl.inc"
//      b) Register protocols in +[GarnetDevice load]:
//           { const char *n[] = { "MTLDynamicLibrary" };
//             rdna4_add_protocols([RDNA4DynamicLibrary class], n, 1, "RDNA4DynamicLibrary"); }
//           { const char *n[] = { "MTLFunctionHandle" };
//             rdna4_add_protocols([RDNA4FunctionHandle class], n, 1, "RDNA4FunctionHandle"); }
//           { const char *n[] = { "MTLVisibleFunctionTable", "MTLResource" };
//             rdna4_add_protocols([RDNA4VisibleFunctionTable class], n, 2, "RDNA4VisibleFunctionTable"); }
//      c) Wire MTLDevice dynamic library factories in @implementation GarnetDevice (keep
//         supportsDynamicLibraries / supportsRenderDynamicLibraries / supportsFunctionPointers /
//         supportsFunctionPointersFromRender returning NO until live verification / Task 8.2):
//           - (id<MTLDynamicLibrary>)newDynamicLibrary:(id<MTLLibrary>)library error:(NSError **)error {
//               return (id<MTLDynamicLibrary>)[[RDNA4DynamicLibrary alloc] initWithDevice:self library:library error:error];
//           }
//           - (id<MTLDynamicLibrary>)newDynamicLibraryWithURL:(NSURL *)url error:(NSError **)error {
//               return (id<MTLDynamicLibrary>)[[RDNA4DynamicLibrary alloc] initWithDevice:self url:url error:error];
//           }
//   2. rdna4_compute_pipeline_impl.inc (RDNA4ComputePipelineState):
//      a) Add ivars: `uint64_t _vftOwnerToken; rdna4vft_reflection_t _vft;`
//      b) In initWithDevice:descriptor:options:error:, check `NSError *lkErr = rdna4_lk_error(fn);`
//         and fail immediately if non-nil.
//      c) In initWithDevice:function:spv:meta:fname:error:, initialize `_vftOwnerToken = rdna4_vft_next_owner_token();`
//         and call `if (!rdna4x_parse_vft_from_meta(meta, &_vft, err)) return nil;`.
//      d) In dealloc: `rdna4vft_refl_destroy(&_vft);`.
//      e) Add selectors:
//           - (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function {
//               return rdna4_vft_pipeline_handle(self, _dev, _vftOwnerToken, &_vft, function, 0);
//           }
//           - (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)d {
//               return rdna4_vft_pipeline_new_table(self, _dev, _vftOwnerToken, &_vft, d, 0);
//           }
//   3. rdna4_render_pipeline_impl.inc (RDNA4RenderPipelineState):
//      a) Add ivars: `uint64_t _vftOwnerToken; rdna4vft_reflection_t _vftV, _vftF;`
//      b) In initWithDevice:descriptor:options:noFallback:error:, call
//         `if (!rdna4_vft_validate_render_footnote10(d, NO, NO, NO, err)) return nil;`
//         and check `rdna4_lk_error(d.vertexFunction)` / `rdna4_lk_error(d.fragmentFunction)`.
//      c) After resolving mv_ / mf_, initialize `_vftOwnerToken = rdna4_vft_next_owner_token();`
//         and parse `_vftV` / `_vftF` via `rdna4x_parse_vft_from_meta(...)`.
//      d) In dealloc: `rdna4vft_refl_destroy(&_vftV); rdna4vft_refl_destroy(&_vftF);`.
//      e) Add selectors:
//           - (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function stage:(MTLRenderStages)stage {
//               const rdna4vft_reflection_t *r = (stage == MTLRenderStageVertex) ? &_vftV :
//                   (stage == MTLRenderStageFragment) ? &_vftF : NULL;
//               return rdna4_vft_pipeline_handle(self, _dev, _vftOwnerToken, r, function, stage);
//           }
//           - (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)d stage:(MTLRenderStages)stage {
//               const rdna4vft_reflection_t *r = (stage == MTLRenderStageVertex) ? &_vftV :
//                   (stage == MTLRenderStageFragment) ? &_vftF : NULL;
//               return rdna4_vft_pipeline_new_table(self, _dev, _vftOwnerToken, r, d, stage);
//           }
//   4. rdna4_compute_encoder_impl.inc & rdna4_render_encoder_impl.inc:
//      a) Compute:
//           - (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i {
//               if (!t) { [self setBuffer:nil offset:0 atIndex:i]; return; }
//               if (![t isKindOfClass:[RDNA4VisibleFunctionTable class]]) {
//                   [_cb rdna4Fail:@"compute setVisibleFunctionTable: not an RDNA4VisibleFunctionTable"]; return;
//               }
//               RDNA4VisibleFunctionTable *vft = (RDNA4VisibleFunctionTable *)t;
//               [vft rdna4RetainIntoCommandBuffer:_cb];
//               [self setBuffer:[vft rdna4BackingBuffer] offset:0 atIndex:i];
//           }
//           - (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
//               for (NSUInteger k = 0; k < r.length; k++) [self setVisibleFunctionTable:t[k] atBufferIndex:r.location + k];
//           }
//      b) Render (vertex & fragment): same pattern routing to `rdna4SetBuffer:[vft rdna4BackingBuffer] offset:0 index:i frag:(NO|YES)`.
// ---------------------------------------------------------------------------------------------------------------
#ifndef RDNA4_DYNLIB_VFT_IMPL_INC
#define RDNA4_DYNLIB_VFT_IMPL_INC

#include "rdna4_dynlib_vft.h"

static NSError *rdna4_dl_err(MTLDynamicLibraryError code, NSString *msg, NSError *underlying) {
    NSMutableDictionary *info = [NSMutableDictionary dictionaryWithObject:(msg ?: @"Garnet dynamic library error")
                                                                   forKey:NSLocalizedDescriptionKey];
    if (underlying) info[NSUnderlyingErrorKey] = underlying;
    return [NSError errorWithDomain:MTLDynamicLibraryDomain code:(NSInteger)code userInfo:info];
}

// Guard private/SPI selector probes by verifying the runtime method actually returns an ObjC object ('@').
static id rdna4_dl_objc_prop(id obj, const char *selName) {
    if (!obj || !selName) return nil;
    SEL sel = sel_registerName(selName);
    Method m = class_getInstanceMethod(object_getClass(obj), sel);
    if (!m) return nil;
    char rt[8] = "";
    method_getReturnType(m, rt, sizeof rt);
    if (rt[0] != '@') return nil;
    return ((id (*)(id, SEL))objc_msgSend)(obj, sel);
}

// For Cocoa +1 ownership selectors (`new*`/`copy*`), transfer ownership via CFBridgingRelease so ARC does not leak.
static id rdna4_dl_objc_new1(id obj, const char *selName, id arg) {
    if (!obj || !selName) return nil;
    SEL sel = sel_registerName(selName);
    Method m = class_getInstanceMethod(object_getClass(obj), sel);
    if (!m) return nil;
    char rt[8] = "";
    method_getReturnType(m, rt, sizeof rt);
    if (rt[0] != '@') return nil;
    void *raw = ((void *(*)(id, SEL, id))objc_msgSend)(obj, sel, arg);
    return raw ? CFBridgingRelease(raw) : nil;
}

static uint32_t rdna4_dl_detect_payload_kind(NSData *data) {
    if (!data.length) return 0u;
    if (rdna4dl_is_bitcode_magic(data.bytes, data.length)) return RDNA4DL_PAYLOAD_AIR_BITCODE;
    if (rdna4dl_is_metallib_magic(data.bytes, data.length)) return RDNA4DL_PAYLOAD_METALLIB;
    if (memchr(data.bytes, '\0', data.length) == NULL) {
        NSString *txt = [[NSString alloc] initWithBytes:data.bytes length:data.length encoding:NSUTF8StringEncoding];
        if (txt && ([txt containsString:@"define "] || [txt containsString:@"target "] || [txt containsString:@"; ModuleID"]))
            return RDNA4DL_PAYLOAD_LLVM_IR;
    }
    return 0u;
}

static BOOL rdna4_dl_append_unique_module(rdna4dl_container_t *container,
                                          NSMutableSet<NSString *> *seenShas,
                                          NSData *payload,
                                          uint32_t kind,
                                          NSError **err) {
    if (!payload.length || !rdna4dl_payload_kind_ok(kind)) return YES;
    NSString *sha = rdna4_sha256hex(payload);
    if ([seenShas containsObject:sha]) return YES;
    rdna4dl_status_t st = rdna4dl_add_module(container, kind, payload.bytes, payload.length);
    if (st != RDNA4DL_OK) {
        if (err) *err = rdna4_dl_err(MTLDynamicLibraryErrorCompilationFailure,
            [NSString stringWithFormat:@"Failed to add dynamic library payload module: %s", rdna4dl_status_str(st)], nil);
        return NO;
    }
    [seenShas addObject:sha];
    return YES;
}

// =================================================================================================
// RDNA4DynamicLibrary (MTLDynamicLibrary)
// =================================================================================================
@interface RDNA4DynamicLibrary : NSObject <RDNA4DynamicLibraryInternal> {
    id _dev;
    NSString *_lbl;
    NSString *_installName;
    rdna4dl_container_t _container;
    NSArray<NSDictionary *> *_modulesCache;
    NSArray<NSString *> *_symbolsCache;
    NSLock *_lock;
}
- (instancetype)initWithDevice:(id)dev library:(id<MTLLibrary>)library error:(NSError **)error;
- (instancetype)initWithDevice:(id)dev url:(NSURL *)url error:(NSError **)error;
- (id)device;
- (NSString *)installName;
- (NSString *)label;
- (void)setLabel:(NSString *)label;
- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error;
- (NSArray<NSDictionary *> *)rdna4PayloadModules;
- (NSArray<NSString *> *)rdna4ExportedSymbols;
- (BOOL)rdna4HasExportedSymbol:(NSString *)symbol;
@end

@implementation RDNA4DynamicLibrary

- (void)rdna4RebuildCachesLocked {
    NSMutableArray<NSDictionary *> *mods = [NSMutableArray arrayWithCapacity:_container.module_count];
    for (uint32_t i = 0; i < _container.module_count; i++) {
        const rdna4dl_module_t *m = &_container.modules[i];
        NSData *d = [NSData dataWithBytes:m->data length:m->size];
        if (!d) continue;
        NSString *sha = rdna4_sha256hex(d);
        NSString *mname = (_container.module_count == 1u)
            ? (_installName ?: @"dylib")
            : [NSString stringWithFormat:@"%@#%u", _installName ?: @"dylib", i];
        [mods addObject:@{ @"name": mname, @"kind": @(m->kind), @"data": d, @"sha": sha }];
    }
    _modulesCache = [mods copy];

    NSMutableArray<NSString *> *syms = [NSMutableArray arrayWithCapacity:_container.symbol_count];
    for (uint32_t i = 0; i < _container.symbol_count; i++) {
        NSString *s = [NSString stringWithUTF8String:_container.symbols[i]];
        if (s) [syms addObject:s];
    }
    _symbolsCache = [syms copy];
}

- (instancetype)initWithDevice:(id)dev library:(id<MTLLibrary>)library error:(NSError **)error {
    if (error) *error = nil;
    if (!dev || !library) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorInvalidFile,
            @"newDynamicLibrary:error: requires a non-nil device and MTLLibrary", nil);
        return nil;
    }
    if ([library respondsToSelector:@selector(device)]) {
        id libDev = [library device];
        if (libDev && libDev != dev) {
            if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorCompilationFailure,
                @"MTLLibrary belongs to a different MTLDevice", nil);
            return nil;
        }
    }
    if ([library respondsToSelector:@selector(type)]) {
        MTLLibraryType ltype = [library type];
        if (ltype != MTLLibraryTypeDynamic) {
            if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorCompilationFailure,
                @"MTLDynamicLibrary requires an MTLLibrary compiled with MTLLibraryTypeDynamic", nil);
            return nil;
        }
    }
    NSString *iname = [library respondsToSelector:@selector(installName)] ? [library installName] : nil;
    if (![iname isKindOfClass:[NSString class]] || !iname.length) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorUnresolvedInstallName,
            @"MTLLibrary has no installName for dynamic library creation", nil);
        return nil;
    }

    self = [super init];
    if (!self) return nil;
    _dev = dev;
    _lock = [NSLock new];
    _installName = [iname copy];
    if ([library respondsToSelector:@selector(label)] && [library label])
        _lbl = [[library label] copy];
    rdna4dl_init(&_container);

    const char *inameUtf8 = _installName.UTF8String;
    rdna4dl_status_t st = rdna4dl_set_install_name(&_container, inameUtf8 ?: "", inameUtf8 ? strlen(inameUtf8) : 0u);
    if (st != RDNA4DL_OK) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorUnresolvedInstallName,
            [NSString stringWithFormat:@"Invalid dynamic library installName: %s", rdna4dl_status_str(st)], nil);
        return nil;
    }

    NSMutableArray<NSString *> *externNames = [NSMutableArray array];
    NSMutableArray<NSString *> *funcNames = [NSMutableArray array];
    NSMutableArray<NSString *> *allNames = [NSMutableArray array];
    NSMutableSet<NSString *> *seenNames = [NSMutableSet set];
    {
        id extArr = rdna4_dl_objc_prop(library, "externFunctionNames");
        if ([extArr isKindOfClass:[NSArray class]]) {
            for (id item in (NSArray *)extArr) {
                if ([item isKindOfClass:[NSString class]] && [(NSString *)item length] && ![seenNames containsObject:item]) {
                    [seenNames addObject:item];
                    [externNames addObject:item];
                    [allNames addObject:item];
                }
            }
        }
        id fnArr = rdna4_dl_objc_prop(library, "functionNames");
        if ([fnArr isKindOfClass:[NSArray class]]) {
            for (id item in (NSArray *)fnArr) {
                if ([item isKindOfClass:[NSString class]] && [(NSString *)item length] && ![seenNames containsObject:item]) {
                    [seenNames addObject:item];
                    [funcNames addObject:item];
                    [allNames addObject:item];
                }
            }
        }
    }

    NSMutableSet<NSString *> *seenShas = [NSMutableSet set];
    NSData *metallibFallback = nil;

    // Extract whole-library bitcodeData FIRST: on macOS 26+, MTLLibraryTypeDynamic carries the full
    // AIR bitcode here while calling newExternFunctionWithName: on a [[visible]] functionNames entry
    // aborts inside Metal.framework (validateMTLFunctionType).
    id libBcObj = rdna4_dl_objc_prop(library, "bitcodeData");
    if ([libBcObj isKindOfClass:[NSData class]]) {
        NSData *libBc = (NSData *)libBcObj;
        uint32_t kind = rdna4_dl_detect_payload_kind(libBc);
        if (kind == RDNA4DL_PAYLOAD_AIR_BITCODE || kind == RDNA4DL_PAYLOAD_LLVM_IR) {
            if (!rdna4_dl_append_unique_module(&_container, seenShas, libBc, kind, error)) return nil;
        } else if (kind == RDNA4DL_PAYLOAD_METALLIB) {
            metallibFallback = libBc;
        }
    }

    // Only probe per-function bitcode when whole-library AIR/IR bitcode was not already extracted.
    // Call newExternFunctionWithName: strictly on externFunctionNames (NEVER on functionNames).
    if (_container.module_count == 0u) {
        for (NSString *fnName in externNames) {
            id fn = rdna4_dl_objc_new1(library, "newExternFunctionWithName:", fnName);
            id bcObj = rdna4_dl_objc_prop(fn, "bitcodeData");
            if (![bcObj isKindOfClass:[NSData class]]) continue;
            NSData *bc = (NSData *)bcObj;
            uint32_t kind = rdna4_dl_detect_payload_kind(bc);
            if (kind == RDNA4DL_PAYLOAD_AIR_BITCODE || kind == RDNA4DL_PAYLOAD_LLVM_IR) {
                if (!rdna4_dl_append_unique_module(&_container, seenShas, bc, kind, error)) return nil;
            }
        }
        if ([library respondsToSelector:@selector(newFunctionWithName:)]) {
            for (NSString *fnName in funcNames) {
                id fn = [library newFunctionWithName:fnName];
                id bcObj = rdna4_dl_objc_prop(fn, "bitcodeData");
                if (![bcObj isKindOfClass:[NSData class]]) continue;
                NSData *bc = (NSData *)bcObj;
                uint32_t kind = rdna4_dl_detect_payload_kind(bc);
                if (kind == RDNA4DL_PAYLOAD_AIR_BITCODE || kind == RDNA4DL_PAYLOAD_LLVM_IR) {
                    if (!rdna4_dl_append_unique_module(&_container, seenShas, bc, kind, error)) return nil;
                }
            }
        }
    }

    if (_container.module_count == 0u && metallibFallback) {
        if (!rdna4_dl_append_unique_module(&_container, seenShas, metallibFallback, RDNA4DL_PAYLOAD_METALLIB, error)) return nil;
    }

    if (_container.module_count == 0u) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorCompilationFailure,
            [NSString stringWithFormat:@"Dynamic library '%@' has no extractable AIR/LLVM bitcode modules", _installName], nil);
        return nil;
    }

    // Enumerate exported symbols via garnetx_list_exported_symbols when the translator is available.
    BOOL translatorEnumerated = NO;
    for (uint32_t i = 0; i < _container.module_count; i++) {
        const rdna4dl_module_t *m = &_container.modules[i];
        if (m->kind != RDNA4DL_PAYLOAD_AIR_BITCODE && m->kind != RDNA4DL_PAYLOAD_LLVM_IR) continue;
        NSArray<NSString *> *syms = nil;
        NSError *symErr = nil;
        if (rdna4x_list_exported_symbols_for_module(m->data, m->size, m->kind, &syms, &symErr)) {
            translatorEnumerated = YES;
            for (NSString *s in syms) {
                const char *u = s.UTF8String;
                if (!u || !u[0]) continue;
                rdna4dl_status_t ast = rdna4dl_add_symbol_len(&_container, u, strlen(u), 0);
                if (ast != RDNA4DL_OK) {
                    if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorCompilationFailure,
                        [NSString stringWithFormat:@"Exported symbol table overflow: %s", rdna4dl_status_str(ast)], nil);
                    return nil;
                }
            }
        } else if (rdna4x_stage2_symbols_available()) {
            // Translator is loaded and explicitly refused the module's AIR/IR.
            if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorCompilationFailure,
                [NSString stringWithFormat:@"Dynamic library '%@' failed symbol enumeration: %@",
                 _installName, symErr.localizedDescription ?: @"unknown error"], symErr);
            return nil;
        }
    }
    if (!translatorEnumerated) {
        for (NSString *nm in allNames) {
            const char *u = nm.UTF8String;
            if (u && u[0]) (void)rdna4dl_add_symbol_len(&_container, u, strlen(u), 0);
        }
    }

    [self rdna4RebuildCachesLocked];
    RDNA4LOG("RDNA4DynamicLibrary %p '%s': %u module(s), %u exported symbol(s)",
             (__bridge void *)self, _installName.UTF8String, _container.module_count, _container.symbol_count);
    return self;
}

- (instancetype)initWithDevice:(id)dev url:(NSURL *)url error:(NSError **)error {
    if (error) *error = nil;
    if (!dev || !url || ![url isFileURL] || !url.path.length) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorInvalidFile,
            @"newDynamicLibraryWithURL:error: requires a valid file URL", nil);
        return nil;
    }
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    _lock = [NSLock new];
    rdna4dl_init(&_container);

    int ioErr = 0;
    rdna4dl_status_t st = rdna4dl_read_file(url.fileSystemRepresentation, &_container, &ioErr);
    if (st != RDNA4DL_OK) {
        NSError *under = ioErr ? [NSError errorWithDomain:NSPOSIXErrorDomain code:ioErr userInfo:nil] : nil;
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorInvalidFile,
            [NSString stringWithFormat:@"Failed to load Garnet GRDL dynamic library '%@': %s",
             url.path, rdna4dl_status_str(st)], under);
        return nil;
    }
    _installName = [NSString stringWithUTF8String:_container.install_name];
    if (!_installName.length) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorUnresolvedInstallName,
            @"Loaded GRDL dynamic library has invalid UTF-8 installName", nil);
        return nil;
    }
    [self rdna4RebuildCachesLocked];
    RDNA4LOG("RDNA4DynamicLibrary %p loaded '%s' from %s (%u module(s), %u symbol(s))",
             (__bridge void *)self, _installName.UTF8String, url.fileSystemRepresentation,
             _container.module_count, _container.symbol_count);
    return self;
}

- (void)dealloc {
    rdna4dl_destroy(&_container);
}

RDNA4_DNR(RDNA4DynamicLibrary)

- (id)device { return _dev; }
- (NSString *)installName { return _installName; }
- (NSString *)label { [_lock lock]; NSString *l = _lbl; [_lock unlock]; return l; }
- (void)setLabel:(NSString *)label { [_lock lock]; _lbl = [label copy]; [_lock unlock]; }
- (NSArray<NSDictionary *> *)rdna4PayloadModules { return _modulesCache ?: @[]; }
- (NSArray<NSString *> *)rdna4ExportedSymbols { return _symbolsCache ?: @[]; }

- (BOOL)rdna4HasExportedSymbol:(NSString *)symbol {
    if (!symbol.length) return NO;
    [_lock lock];
    BOOL found = rdna4dl_has_symbol(&_container, symbol.UTF8String) ? YES : NO;
    [_lock unlock];
    return found;
}

- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error {
    if (error) *error = nil;
    if (!url || ![url isFileURL] || !url.path.length) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorInvalidFile,
            @"serializeToURL:error: requires a valid file URL", nil);
        return NO;
    }
    [_lock lock];
    int ioErr = 0;
    rdna4dl_status_t st = rdna4dl_write_file_atomic(&_container, url.fileSystemRepresentation, &ioErr);
    [_lock unlock];
    if (st != RDNA4DL_OK) {
        NSError *under = ioErr ? [NSError errorWithDomain:NSPOSIXErrorDomain code:ioErr userInfo:nil] : nil;
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorInvalidFile,
            [NSString stringWithFormat:@"Failed to serialize GRDL dynamic library '%@' to '%@': %s",
             _installName, url.path, rdna4dl_status_str(st)], under);
        return NO;
    }
    return YES;
}

@end

// =================================================================================================
// RDNA4FunctionHandle (MTLFunctionHandle)
// =================================================================================================
@interface RDNA4FunctionHandle : NSObject {
    id _dev;
    __weak id _ownerPipeline;
    uint64_t _ownerToken;
    NSUInteger _stage;
    MTLFunctionType _functionType;
    NSString *_name;
    uint32_t _handleId;
}
- (instancetype)initWithDevice:(id)dev
                 ownerPipeline:(id)pso
                    ownerToken:(uint64_t)token
                         stage:(NSUInteger)stage
                  functionType:(MTLFunctionType)ftype
                          name:(NSString *)name
                      handleId:(uint32_t)handleId;
- (id)device;
- (MTLFunctionType)functionType;
- (NSString *)name;
- (uint32_t)rdna4HandleId;
- (uint64_t)rdna4OwnerToken;
- (NSUInteger)rdna4Stage;
- (id)rdna4OwnerPipeline;
@end

@implementation RDNA4FunctionHandle

- (instancetype)initWithDevice:(id)dev
                 ownerPipeline:(id)pso
                    ownerToken:(uint64_t)token
                         stage:(NSUInteger)stage
                  functionType:(MTLFunctionType)ftype
                          name:(NSString *)name
                      handleId:(uint32_t)handleId {
    if (!dev || !token || !name.length || handleId == RDNA4VFT_HANDLE_NULL) return nil;
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    _ownerPipeline = pso;
    _ownerToken = token;
    _stage = stage;
    _functionType = ftype;
    _name = [name copy];
    _handleId = handleId;
    return self;
}

RDNA4_DNR(RDNA4FunctionHandle)

- (id)device { return _dev; }
- (MTLFunctionType)functionType { return _functionType; }
- (NSString *)name { return _name; }
- (uint32_t)rdna4HandleId { return _handleId; }
- (uint64_t)rdna4OwnerToken { return _ownerToken; }
- (NSUInteger)rdna4Stage { return _stage; }
- (id)rdna4OwnerPipeline { return _ownerPipeline; }

@end

// =================================================================================================
// RDNA4VisibleFunctionTable (MTLVisibleFunctionTable + MTLResource)
// =================================================================================================
@interface RDNA4VisibleFunctionTable : _MTLResource {
    id _dev;
    NSString *_lbl;
    uint64_t _ownerToken;
    NSUInteger _stage;
    rdna4vft_table_t _table;
    RDNA4Buffer *_backing;
    RDNA4Buffer *_attachedBuffers[RDNA4VFT_MAX_ATTACH_BUFFERS];
    NSUInteger _attachedBufferOffsets[RDNA4VFT_MAX_ATTACH_BUFFERS];
    id _nestedTables[RDNA4VFT_MAX_NESTED_TABLES];
    NSLock *_lock;
    RDNA4_RES_IVARS
}
- (instancetype)initWithDevice:(id)dev
                    ownerToken:(uint64_t)token
                         stage:(NSUInteger)stage
                 functionCount:(NSUInteger)count
                   maxHandleId:(uint32_t)maxHandleId
                         error:(NSError **)error;
- (void)setFunction:(id<MTLFunctionHandle>)function atIndex:(NSUInteger)index;
- (void)setFunctions:(const id<MTLFunctionHandle> __unsafe_unretained *)functions withRange:(NSRange)range;
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range;
- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)functionTable atBufferIndex:(NSUInteger)bufferIndex;
- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)functionTables withBufferRange:(NSRange)bufferRange;
- (RDNA4Buffer *)rdna4BackingBuffer;
- (NSUInteger)rdna4FunctionCount;
- (uint32_t)rdna4HandleIdAtIndex:(NSUInteger)index;
- (uint64_t)rdna4OwnerToken;
- (NSUInteger)rdna4Stage;
- (RDNA4Buffer *)rdna4AttachedBufferAtIndex:(NSUInteger)index offset:(NSUInteger *)offsetOut;
- (id)rdna4NestedTableAtBufferIndex:(NSUInteger)index;
- (void)rdna4RetainIntoCommandBuffer:(id)cb;
@end

@implementation RDNA4VisibleFunctionTable

- (BOOL)rdna4SyncBackingLocked {
    void *ptr = _backing ? [_backing contents] : NULL;
    if (!ptr) return NO;
    size_t capWords = [_backing length] / sizeof(uint32_t);
    size_t written = 0;
    return rdna4vft_encode_buffer(&_table, (uint32_t *)ptr, capWords, &written) == RDNA4VFT_OK;
}

- (instancetype)initWithDevice:(id)dev
                    ownerToken:(uint64_t)token
                         stage:(NSUInteger)stage
                 functionCount:(NSUInteger)count
                   maxHandleId:(uint32_t)maxHandleId
                         error:(NSError **)error {
    if (error) *error = nil;
    if (!dev || count > RDNA4VFT_MAX_SLOTS) {
        if (error) *error = rdna4_err(49, @"visible function table descriptor has invalid device or functionCount");
        return nil;
    }
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    _ownerToken = token;
    _stage = stage;
    _lock = [NSLock new];

    rdna4vft_status_t st = rdna4vft_init(&_table, (uint32_t)count, maxHandleId);
    if (st != RDNA4VFT_OK) {
        if (error) *error = rdna4_err(49, [NSString stringWithFormat:@"visible function table init failed: %s", rdna4vft_status_str(st)]);
        return nil;
    }

    // Backing Shared RDNA4Buffer holds [slot_count, handle_id_0..handle_id_{N-1}] matching 0074 (>= 16 B aligned).
    size_t needBytes = rdna4vft_buffer_bytes((uint32_t)count);
    size_t allocBytes = (needBytes + 15u) & ~(size_t)15u;
    if (allocBytes < 16u) allocBytes = 16u;
    _backing = [[RDNA4Buffer alloc] initWithDevice:dev length:allocBytes options:MTLResourceStorageModeShared error:error];
    void *ptr = _backing ? [_backing contents] : NULL;
    if (!ptr) {
        rdna4vft_destroy(&_table);
        return nil;
    }
    memset(ptr, 0, allocBytes);
    if (![self rdna4SyncBackingLocked]) {
        rdna4vft_destroy(&_table);
        if (error) *error = rdna4_err(49, @"failed to initialize visible function table backing buffer");
        return nil;
    }
    RDNA4LOGR("RDNA4VisibleFunctionTable %p: slots=%lu maxHandle=%u buf=%lu B",
              (__bridge void *)self, (unsigned long)count, maxHandleId, (unsigned long)allocBytes);
    return self;
}

- (void)dealloc {
    rdna4vft_destroy(&_table);
}

RDNA4_DNR(RDNA4VisibleFunctionTable)

- (BOOL)rdna4ValidateHandleLocked:(id)function handleIdOut:(uint32_t *)hidOut {
    *hidOut = RDNA4VFT_HANDLE_NULL;
    if (!function) return YES;
    if (![function isKindOfClass:[RDNA4FunctionHandle class]]) {
        RDNA4LOG("RDNA4VisibleFunctionTable: rejected non-RDNA4FunctionHandle object");
        return NO;
    }
    RDNA4FunctionHandle *fh = (RDNA4FunctionHandle *)function;
    if ([fh device] != _dev || [fh rdna4OwnerToken] != _ownerToken || [fh rdna4Stage] != _stage) {
        RDNA4LOG("RDNA4VisibleFunctionTable: rejected handle '%s' from mismatched pipeline/stage/device",
                 [fh name].UTF8String ?: "?");
        return NO;
    }
    uint32_t hid = [fh rdna4HandleId];
    if (hid == RDNA4VFT_HANDLE_NULL || hid > _table.max_handle_id) {
        RDNA4LOG("RDNA4VisibleFunctionTable: rejected out-of-range handle id %u (max %u)", hid, _table.max_handle_id);
        return NO;
    }
    *hidOut = hid;
    return YES;
}

- (void)setFunction:(id<MTLFunctionHandle>)function atIndex:(NSUInteger)index {
    [_lock lock];
    if (index >= _table.slot_count) {
        RDNA4LOG("RDNA4VisibleFunctionTable setFunction:atIndex:%lu out of bounds (count=%u)",
                 (unsigned long)index, _table.slot_count);
        [_lock unlock];
        return;
    }
    uint32_t hid = RDNA4VFT_HANDLE_NULL;
    if ([self rdna4ValidateHandleLocked:function handleIdOut:&hid]) {
        if (rdna4vft_set_slot(&_table, (uint32_t)index, hid) == RDNA4VFT_OK)
            (void)[self rdna4SyncBackingLocked];
    }
    [_lock unlock];
}

- (void)setFunctions:(const id<MTLFunctionHandle> __unsafe_unretained *)functions withRange:(NSRange)range {
    if (range.length == 0) return;
    if (!functions) {
        RDNA4LOG("RDNA4VisibleFunctionTable setFunctions:withRange: NULL array");
        return;
    }
    [_lock lock];
    if (range.location > _table.slot_count || range.length > _table.slot_count - range.location) {
        RDNA4LOG("RDNA4VisibleFunctionTable setFunctions:withRange:[%lu,+%lu) out of bounds (count=%u)",
                 (unsigned long)range.location, (unsigned long)range.length, _table.slot_count);
        [_lock unlock];
        return;
    }
    uint32_t *ids = (uint32_t *)malloc(range.length * sizeof(uint32_t));
    if (!ids) { [_lock unlock]; return; }
    BOOL valid = YES;
    for (NSUInteger i = 0; i < range.length; i++) {
        if (![self rdna4ValidateHandleLocked:functions[i] handleIdOut:&ids[i]]) {
            valid = NO;
            break;
        }
    }
    if (valid && rdna4vft_set_range(&_table, (uint32_t)range.location, (uint32_t)range.length, ids) == RDNA4VFT_OK)
        (void)[self rdna4SyncBackingLocked];
    free(ids);
    [_lock unlock];
}

- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index >= RDNA4VFT_MAX_ATTACH_BUFFERS) {
        RDNA4LOG("RDNA4VisibleFunctionTable setBuffer:atIndex:%lu out of bounds", (unsigned long)index);
        return;
    }
    if (buffer && (![buffer isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)buffer device] != _dev)) {
        RDNA4LOG("RDNA4VisibleFunctionTable setBuffer: invalid buffer class or device");
        return;
    }
    [_lock lock];
    _attachedBuffers[index] = (RDNA4Buffer *)buffer;
    _attachedBufferOffsets[index] = buffer ? offset : 0u;
    [_lock unlock];
}

- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)buffers
           offsets:(const NSUInteger *)offsets
         withRange:(NSRange)range {
    if (range.length == 0) return;
    if (!buffers || !offsets ||
        range.location > RDNA4VFT_MAX_ATTACH_BUFFERS ||
        range.length > RDNA4VFT_MAX_ATTACH_BUFFERS - range.location) {
        RDNA4LOG("RDNA4VisibleFunctionTable setBuffers:offsets:withRange: invalid range or NULL array");
        return;
    }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}

- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)functionTable atBufferIndex:(NSUInteger)bufferIndex {
    if (bufferIndex >= RDNA4VFT_MAX_NESTED_TABLES) {
        RDNA4LOG("RDNA4VisibleFunctionTable setVisibleFunctionTable:atBufferIndex:%lu out of bounds",
                 (unsigned long)bufferIndex);
        return;
    }
    if (functionTable == (id)self) {
        RDNA4LOG("RDNA4VisibleFunctionTable: rejected cyclic self-attachment at buffer index %lu",
                 (unsigned long)bufferIndex);
        return;
    }
    if (functionTable &&
        (![functionTable isKindOfClass:[RDNA4VisibleFunctionTable class]] ||
         [(RDNA4VisibleFunctionTable *)functionTable device] != _dev)) {
        RDNA4LOG("RDNA4VisibleFunctionTable: rejected invalid nested table class or device");
        return;
    }
    [_lock lock];
    _nestedTables[bufferIndex] = functionTable;
    [_lock unlock];
}

- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)functionTables
                 withBufferRange:(NSRange)bufferRange {
    if (bufferRange.length == 0) return;
    if (!functionTables ||
        bufferRange.location > RDNA4VFT_MAX_NESTED_TABLES ||
        bufferRange.length > RDNA4VFT_MAX_NESTED_TABLES - bufferRange.location) {
        RDNA4LOG("RDNA4VisibleFunctionTable setVisibleFunctionTables:withBufferRange: invalid range or NULL array");
        return;
    }
    for (NSUInteger i = 0; i < bufferRange.length; i++)
        [self setVisibleFunctionTable:functionTables[i] atBufferIndex:bufferRange.location + i];
}

- (RDNA4Buffer *)rdna4BackingBuffer { return _backing; }
- (NSUInteger)rdna4FunctionCount { return (NSUInteger)_table.slot_count; }
- (uint64_t)rdna4OwnerToken { return _ownerToken; }
- (NSUInteger)rdna4Stage { return _stage; }

- (uint32_t)rdna4HandleIdAtIndex:(NSUInteger)index {
    [_lock lock];
    uint32_t hid = index <= UINT32_MAX ? rdna4vft_get_slot(&_table, (uint32_t)index) : RDNA4VFT_HANDLE_NULL;
    [_lock unlock];
    return hid;
}

- (RDNA4Buffer *)rdna4AttachedBufferAtIndex:(NSUInteger)index offset:(NSUInteger *)offsetOut {
    if (offsetOut) *offsetOut = 0;
    if (index >= RDNA4VFT_MAX_ATTACH_BUFFERS) return nil;
    [_lock lock];
    RDNA4Buffer *b = _attachedBuffers[index];
    if (offsetOut) *offsetOut = _attachedBufferOffsets[index];
    [_lock unlock];
    return b;
}

- (id)rdna4NestedTableAtBufferIndex:(NSUInteger)index {
    if (index >= RDNA4VFT_MAX_NESTED_TABLES) return nil;
    [_lock lock];
    id nested = _nestedTables[index];
    [_lock unlock];
    return nested;
}

- (void)rdna4RetainIntoCommandBuffer:(id)cb depth:(unsigned)depth {
    if (!cb || depth > 8u) return;
    SEL retainSel = sel_registerName("rdna4RetainEncoded:");
    if (![cb respondsToSelector:retainSel]) return;
    void (*retainFn)(id, SEL, id) = (void (*)(id, SEL, id))objc_msgSend;
    retainFn(cb, retainSel, self);
    if (_backing) retainFn(cb, retainSel, _backing);
    [_lock lock];
    for (uint32_t i = 0; i < RDNA4VFT_MAX_ATTACH_BUFFERS; i++) {
        if (_attachedBuffers[i]) retainFn(cb, retainSel, _attachedBuffers[i]);
    }
    NSMutableArray *nested = nil;
    for (uint32_t i = 0; i < RDNA4VFT_MAX_NESTED_TABLES; i++) {
        if (_nestedTables[i]) {
            if (!nested) nested = [NSMutableArray array];
            [nested addObject:_nestedTables[i]];
        }
    }
    [_lock unlock];
    for (RDNA4VisibleFunctionTable *nt in nested)
        [nt rdna4RetainIntoCommandBuffer:cb depth:depth + 1u];
}

- (void)rdna4RetainIntoCommandBuffer:(id)cb {
    [self rdna4RetainIntoCommandBuffer:cb depth:0u];
}

// ---- MTLResource surface ----
- (id)device { return _dev; }
- (NSString *)label { [_lock lock]; NSString *l = _lbl; [_lock unlock]; return l; }
- (void)setLabel:(NSString *)label { [_lock lock]; _lbl = [label copy]; [_lock unlock]; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeDefault; }
- (MTLResourceOptions)resourceOptions { return MTLResourceStorageModeShared; }
- (NSUInteger)allocatedSize { return _backing ? [_backing allocatedSize] : 0u; }
- (RDNA4Heap *)rdna4ResourceHeap { return nil; }
- (NSUInteger)rdna4ResourceHeapOffset { return 0; }
- (BOOL)rdna4ResourceAliasable { return NO; }
- (void)rdna4ResourceMakeAliasable {}
- (BOOL)rdna4ResourceAliases:(id)other { (void)other; return NO; }
RDNA4_RESOURCE_SPI

@end

// =================================================================================================
// Pipeline & Footnote 10 integration helpers (used by Wave 3B compute/render/mesh pipeline wiring)
// =================================================================================================
static _Atomic uint64_t rdna4_vft_owner_seq;

__attribute__((unused)) static uint64_t rdna4_vft_next_owner_token(void) {
    return atomic_fetch_add(&rdna4_vft_owner_seq, 1u) + 1u;
}

__attribute__((unused)) static BOOL rdna4_dynlib_vft_translator_ready(void) {
    return rdna4x_active() && rdna4x_stage2_symbols_available();
}

__attribute__((unused)) static id<MTLFunctionHandle> rdna4_vft_pipeline_handle(
    id pso, id dev, uint64_t ownerToken, const rdna4vft_reflection_t *refl,
    id<MTLFunction> function, NSUInteger stage)
{
    if (!pso || !dev || !ownerToken || !refl || !refl->present || !function) return nil;
    NSString *nm = [function respondsToSelector:@selector(name)] ? [function name] : nil;
    if (!nm.length) return nil;
    uint32_t hid = rdna4vft_refl_find_handle(refl, nm.UTF8String);
    if (hid == RDNA4VFT_HANDLE_NULL) return nil;
    MTLFunctionType ftype = [function respondsToSelector:@selector(functionType)]
        ? [function functionType] : MTLFunctionTypeVisible;
    return (id<MTLFunctionHandle>)[[RDNA4FunctionHandle alloc]
        initWithDevice:dev ownerPipeline:pso ownerToken:ownerToken stage:stage
          functionType:ftype name:nm handleId:hid];
}

__attribute__((unused)) static id<MTLVisibleFunctionTable> rdna4_vft_pipeline_new_table(
    id pso, id dev, uint64_t ownerToken, const rdna4vft_reflection_t *refl,
    MTLVisibleFunctionTableDescriptor *descriptor, NSUInteger stage)
{
    if (!pso || !dev || !ownerToken || !descriptor) return nil;
    NSUInteger count = descriptor.functionCount;
    uint32_t maxHid = (refl && refl->present) ? refl->max_handle_id : 0u;
    NSError *e = nil;
    RDNA4VisibleFunctionTable *tbl = [[RDNA4VisibleFunctionTable alloc]
        initWithDevice:dev ownerToken:ownerToken stage:stage functionCount:count maxHandleId:maxHid error:&e];
    if (!tbl && e) RDNA4LOG("newVisibleFunctionTableWithDescriptor failed: %s", e.localizedDescription.UTF8String);
    return (id<MTLVisibleFunctionTable>)tbl;
}

static BOOL rdna4_vft_linked_nonempty(id linked) {
    if (!linked) return NO;
    if (rdna4x_array_nonempty(linked, "functions") ||
        rdna4x_array_nonempty(linked, "privateFunctions") ||
        rdna4x_array_nonempty(linked, "binaryFunctions"))
        return YES;
    id gr = rdna4x_prop(linked, "groups");
    return [gr isKindOfClass:[NSDictionary class]] && [(NSDictionary *)gr count] > 0;
}

__attribute__((unused)) static BOOL rdna4_vft_validate_render_footnote10(
    id desc, BOOL isMeshDesc, BOOL hasVftMeta, BOOL hasRenderRT, NSError **error)
{
    uint32_t flags = 0u;
    if (isMeshDesc || rdna4x_prop(desc, "meshFunction") != nil) flags |= RDNA4FN10_HAS_MESH_STAGE;
    if (rdna4x_prop(desc, "objectFunction") != nil) flags |= RDNA4FN10_HAS_OBJECT_STAGE;
    if (rdna4_vft_linked_nonempty(rdna4x_prop(desc, "vertexLinkedFunctions"))) flags |= RDNA4FN10_HAS_VERTEX_FN_PTRS;
    if (rdna4_vft_linked_nonempty(rdna4x_prop(desc, "fragmentLinkedFunctions")) || hasVftMeta) flags |= RDNA4FN10_HAS_FRAGMENT_FN_PTRS;
    if (rdna4_vft_linked_nonempty(rdna4x_prop(desc, "objectLinkedFunctions"))) flags |= RDNA4FN10_HAS_OBJECT_FN_PTRS;
    if (rdna4_vft_linked_nonempty(rdna4x_prop(desc, "meshLinkedFunctions"))) flags |= RDNA4FN10_HAS_MESH_FN_PTRS;
    if (rdna4x_bool_prop(desc, "supportAddingVertexBinaryFunctions") ||
        rdna4x_bool_prop(desc, "supportAddingFragmentBinaryFunctions") ||
        rdna4x_bool_prop(desc, "supportAddingObjectBinaryFunctions") ||
        rdna4x_bool_prop(desc, "supportAddingMeshBinaryFunctions"))
        flags |= RDNA4FN10_SUPPORT_ADD_BIN_FNS;
    if (hasRenderRT) flags |= RDNA4FN10_HAS_RENDER_RAYTRACING;

    const char *why = NULL;
    rdna4vft_status_t st = rdna4vft_check_footnote10(flags, &why);
    if (st != RDNA4VFT_OK) {
        if (error) *error = rdna4_err(49, [NSString stringWithUTF8String:why ? why : rdna4vft_status_str(st)]);
        return NO;
    }
    return YES;
}

#endif // RDNA4_DYNLIB_VFT_IMPL_INC
