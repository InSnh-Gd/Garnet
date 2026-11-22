#ifndef RDNA4_SHADER_CAPTURE_FLAG
#define RDNA4_SHADER_CAPTURE_FLAG "/private/tmp/rdna4m-shader-capture"
#endif
static int rdna4x_capture_request(NSString *directory, NSString *sha, NSString *name,
    const char *role, NSData *air, NSArray *dependencies) {
    struct stat flag;
    if (lstat(RDNA4_SHADER_CAPTURE_FLAG, &flag) || !S_ISREG(flag.st_mode) ||
        (flag.st_uid != 0 && flag.st_uid != geteuid()) || !directory || !sha || !air.length) return 0;
    char requested[65];
    int marker = open(RDNA4_SHADER_CAPTURE_FLAG, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (marker < 0) return 0;
    ssize_t count = read(marker, requested, sizeof requested);
    close(marker);
    if (count != (ssize_t)sizeof requested || requested[64] != '\n' ||
        memcmp(requested, sha.UTF8String, 64)) return 0;
    NSString *path = [directory stringByAppendingPathComponent:[sha stringByAppendingString:@".capture.plist"]];
    if (lstat(path.fileSystemRepresentation, &flag) == 0) return 1;
    NSDictionary *request = @{@"sha256": sha, @"function": name, @"role": [NSString stringWithUTF8String:role],
        @"air": air, @"dependencies": dependencies ?: @[]};
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:request
        format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    if (!data) return -1;
    return rdna4_dump_write(path.fileSystemRepresentation, data) ? 1 : -1;
}
