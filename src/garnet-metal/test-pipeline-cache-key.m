// The production key derivation, with only bundle discovery replaced by a fixture.
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
static NSString *fixturePath;
@interface GarnetDevice : NSObject @end
@implementation GarnetDevice @end
@interface CacheBundle : NSObject
+ (id)bundleForClass:(Class)cls;
- (id)objectForInfoDictionaryKey:(NSString *)key;
- (NSString *)resourcePath;
@end
@implementation CacheBundle
+ (id)bundleForClass:(Class)cls { (void)cls; return [CacheBundle new]; }
- (id)objectForInfoDictionaryKey:(NSString *)key { (void)key; return @"0.0.test"; }
- (NSString *)resourcePath { return fixturePath; }
@end
static struct { uint8_t key[32]; } RDNA4PC;
#define NSBundle CacheBundle
#include "derive-key.inc"
int main(int argc, const char *argv[]) { @autoreleasepool {
    if (argc != 2) return 2;
    fixturePath = @(argv[1]);
    memset(RDNA4PC.key, 0xa5, sizeof RDNA4PC.key);
    if (!rdna4_pc_derive_key()) return 3;
    for (unsigned i = 0; i < sizeof RDNA4PC.key; i++) printf("%02x", RDNA4PC.key[i]);
    puts("");
    return 0;
} }
