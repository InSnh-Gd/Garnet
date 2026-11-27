// Drive the actual pipeline helper with a delayed worker operation; no GPU is opened.
#include "GarnetDevice.m"

static unsigned checks;
static _Atomic unsigned lowerCalls;
static _Atomic size_t lowerStack;
static unsigned lowerDelay;

static void require(BOOL ok, const char *message) {
    checks++;
    if (!ok) { fprintf(stderr, "ARGUMENT_POSTPASS_FAIL %s\n", message); exit(1); }
}

static int lower(void *context, const uint8_t *spv, size_t sl, const uint8_t *meta, size_t ml,
    uint8_t **code, size_t *cl, uint8_t **json, size_t *jl, char *error, size_t errorBytes) {
    (void)context; (void)error; (void)errorBytes;
    atomic_fetch_add(&lowerCalls, 1); atomic_store(&lowerStack, pthread_get_stacksize_np(pthread_self()));
    if (lowerDelay) usleep(lowerDelay * 1000);
    if (!spv || !meta || sl != 64 || ml < 2) return 3;
    const char output[] = "{\"bindings\":[],\"test_postpass\":true}";
    *cl = sl; *code = malloc(sl); *jl = sizeof output - 1; *json = malloc(*jl);
    if (!*code || !*json) return 3;
    memcpy(*code, spv, sl); (*code)[20] = 91; memcpy(*json, output, *jl);
    return 0;
}

static void release(void *context, uint8_t *bytes, size_t length) { (void)context; (void)length; free(bytes); }

static NSDictionary *inputMetadata(NSUInteger marker) {
    return @{ @"bindings": @[@{@"kind": @"EmbeddedArgBufferTexture",
        @"descriptor": @{@"set": @0, @"binding": @32, @"count": @1}}], @"marker": @(marker) };
}

int main(int argc, const char **argv) {
    if (argc != 2) return 2;
    alarm(20);
    @autoreleasepool {
        require(rdna4x_active(), "runner enables the existing translation gate");
        NSString *directory = @(argv[1]); NSError *error = nil;
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:NO attributes:nil error:&error], "fresh cache directory");
        RDNA4XL.dirNS = directory; RDNA4XL.dirState = 1;
        rdna4x_deps dependencies = {.release = release, .lower_arguments = lower};
        rdna4x_engine_init(&RDNA4XL.eng, &dependencies, NULL, NULL, NULL, RDNA4X_WORKER_STACK);
        RDNA4XL.engineInit = 1; RDNA4R.gpuArgumentTables = YES;
        // Model the pipeline caller's opened backend without creating a Vulkan device.
        RDNA4R.ok = YES;
        uint8_t bytes[64] = {3, 2, 0x23, 7}; bytes[20] = 37;
        NSData *input = [NSData dataWithBytes:bytes length:sizeof bytes];
        NSDictionary *meta = inputMetadata(1);
        NSData *result = rdna4_gpu_argument_spv(input, &meta, rdna4_now() + 2000000000ull, &error);
        require(result.length == input.length && ((const uint8_t *)result.bytes)[20] == 91 && [meta[@"test_postpass"] boolValue] && !error,
            "pipeline helper consumes the complete worker result");
        require(atomic_load(&lowerStack) >= RDNA4X_WORKER_STACK && atomic_load(&lowerCalls) == 1, "pipeline helper lowers on the shared worker");
        NSMutableDictionary *equivalent = [NSMutableDictionary dictionary];
        equivalent[@"marker"] = @1; equivalent[@"bindings"] = inputMetadata(1)[@"bindings"]; meta = equivalent;
        result = rdna4_gpu_argument_spv(input, &meta, rdna4_now(), &error);
        require(result.length == input.length && atomic_load(&lowerCalls) == 1 && !error, "canonical metadata cache hit needs no wait or second lowering");

        meta = inputMetadata(2); lowerDelay = 600;
        uint64_t start = rdna4_now();
        result = rdna4_gpu_argument_spv(input, &meta, start + 20000000ull, &error);
        require(!result && error.code == 49 && [error.localizedDescription containsString:@"code 9"] &&
            rdna4_now() - start < 350000000ull, "pipeline caller observes its deadline and a useful timeout error");
        error = nil; meta = inputMetadata(2);
        result = rdna4_gpu_argument_spv(input, &meta, rdna4_now() + 2000000000ull, &error);
        require(result.length == input.length && atomic_load(&lowerCalls) == 2 && !error, "retry shares the in-flight postpass and installs its completed result");
        lowerDelay = 0; bytes[20] = 38; input = [NSData dataWithBytes:bytes length:sizeof bytes]; meta = inputMetadata(2);
        result = rdna4_gpu_argument_spv(input, &meta, rdna4_now() + 2000000000ull, &error);
        require(result && atomic_load(&lowerCalls) == 3, "SPIR-V content participates in the postpass cache identity");
        RDNA4R.gpuArgumentTables = NO; meta = inputMetadata(3);
        require(rdna4_gpu_argument_spv(input, &meta, rdna4_now(), &error) == input && atomic_load(&lowerCalls) == 3, "backend without resource tables retains the static ABI");

        NSString *plain = [directory stringByAppendingPathComponent:@"plain"], *link = [directory stringByAppendingPathComponent:@"link"];
        require([[@"{}" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:plain atomically:YES], "cache read fixture");
        require(symlink(plain.fileSystemRepresentation, link.fileSystemRepresentation) == 0, "cache symlink fixture");
        require(rdna4_argument_cached_data(plain, 2).length == 2 && !rdna4_argument_cached_data(plain, 1) &&
            !rdna4_argument_cached_data(link, 2), "cache reads reject symlinks and oversized files");
        require(!RDNA4R.tried && RDNA4R.dev == VK_NULL_HANDLE,
            "helper tests did not initialize a GPU backend");
        RDNA4R.ok = NO;
        printf("ARGUMENT_POSTPASS_PASS checks=%u gpu_execution=0\n", checks);
    }
    return 0;
}
