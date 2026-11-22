// Authored sampler patterns shared by CPU arithmetic and actual GPU fixtures.
#ifndef RDNA4_TEST_SAMPLER_PATTERN_ORACLE_H
#define RDNA4_TEST_SAMPLER_PATTERN_ORACLE_H

typedef struct {
    float r, g, b, a;
} OracleColor;

typedef enum {
    OracleAddressClamp,
    OracleAddressRepeat,
    OracleAddressBorder,
    OracleAddressBorderBlack,
    OracleAddressBorderWhite,
    OracleAddressZero,
    OracleAddressMirroredRepeat,
    OracleAddressMirrorClamp,
} OracleAddress;

typedef enum {
    OracleFilterNearest,
    OracleFilterLinear,
    OracleFilterMinLinear,
    OracleFilterMagLinear,
} OracleFilter;

typedef enum {
    OracleMipNearest,
    OracleMipLinear,
    OracleMipNone,
} OracleMipFilter;

// A hand-authored 2x2 base level and 1x1 mip. The mip is deliberately not a
// box-filter of the base level so a forced LOD cannot hide behind base colors.
static const OracleColor oracleBase[2][2] = {
    {{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
    {{0.0f, 0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
};
static const OracleColor oracleMip0 = {1.0f, 0.75f, 0.0f, 1.0f};
static const OracleColor oracleClear = {0.0f, 0.0f, 0.0f, 0.0f};

typedef struct {
    const char *name;
    int pixelCoordinates;
    OracleAddress address;
    float u, v, lod, lodMin, lodMax;
    OracleFilter filter;
    OracleMipFilter mipFilter;
    OracleColor expected;
    int expectedLowLevel;
    int expectedHighLevel;
} OracleCase;

static const OracleCase oracleCasesTable[] = {
    {"nearest-clamp-normalized", 0, OracleAddressClamp, 0.25f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {1.0f, 0.0f, 0.0f, 1.0f}, 0, 0},
    {"linear-clamp-normalized", 0, OracleAddressClamp, 0.50f, 0.50f, 0.0f, 0.0f, 1.0f,
        OracleFilterLinear, OracleMipNearest, {0.5f, 0.5f, 0.5f, 1.0f}, 0, 0},
    {"lod-clamp-forces-mip1", 0, OracleAddressClamp, 0.25f, 0.25f, 0.0f, 1.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {1.0f, 0.75f, 0.0f, 1.0f}, 1, 1},
    {"lod-nearest-selects-mip1", 0, OracleAddressClamp, 0.25f, 0.25f, 0.75f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {1.0f, 0.75f, 0.0f, 1.0f}, 1, 1},
    {"nearest-interior-coordinate", 0, OracleAddressClamp, 0.625f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {0.0f, 1.0f, 0.0f, 1.0f}, 0, 0},
    {"mip-linear-blend", 0, OracleAddressClamp, 0.25f, 0.25f, 0.5f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipLinear, {1.0f, 0.375f, 0.0f, 1.0f}, 0, 1},
    {"repeat-wraps-normalized", 0, OracleAddressRepeat, 1.25f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {1.0f, 0.0f, 0.0f, 1.0f}, 0, 0},
    {"clamp-wraps-outside", 0, OracleAddressClamp, -0.25f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {1.0f, 0.0f, 0.0f, 1.0f}, 0, 0},
    {"border-returns-transparent", 0, OracleAddressBorder, -0.25f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {0.0f, 0.0f, 0.0f, 0.0f}, 0, 0},
    {"same-coordinate-normalized", 0, OracleAddressClamp, 0.75f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {0.0f, 1.0f, 0.0f, 1.0f}, 0, 0},
    {"same-coordinate-pixel", 1, OracleAddressClamp, 0.75f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {1.0f, 0.0f, 0.0f, 1.0f}, 0, 0},
    {"nearest-low-mip", 0, OracleAddressClamp, 0.25f, 0.25f, 0.25f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {1.0f, 0.0f, 0.0f, 1.0f}, 0, 0},
    {"lod-max-clamp", 0, OracleAddressClamp, 0.25f, 0.25f, 1.0f, 0.0f, 0.0f,
        OracleFilterNearest, OracleMipNearest, {1.0f, 0.0f, 0.0f, 1.0f}, 0, 0},
    {"mip-none", 0, OracleAddressClamp, 0.25f, 0.25f, 1.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNone, {1.0f, 0.0f, 0.0f, 1.0f}, 0, 0},
    {"linear-fractional", 0, OracleAddressClamp, 0.625f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterLinear, OracleMipNearest, {0.25f, 0.75f, 0.0f, 1.0f}, 0, 0},
    {"linear-pixel", 1, OracleAddressClamp, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
        OracleFilterLinear, OracleMipNone, {0.5f, 0.5f, 0.5f, 1.0f}, 0, 0},
    {"minification-linear-only", 0, OracleAddressClamp, 0.5f, 0.5f, 0.25f, 0.0f, 1.0f,
        OracleFilterMinLinear, OracleMipNearest, {0.5f, 0.5f, 0.5f, 1.0f}, 0, 0},
    {"magnification-linear-only", 0, OracleAddressClamp, 0.5f, 0.5f, -1.0f, 0.0f, 1.0f,
        OracleFilterMagLinear, OracleMipNearest, {0.5f, 0.5f, 0.5f, 1.0f}, 0, 0},
    {"repeat-negative", 0, OracleAddressRepeat, -0.25f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {0.0f, 1.0f, 0.0f, 1.0f}, 0, 0},
    {"mirror-repeat-positive", 0, OracleAddressMirroredRepeat, 1.25f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {0.0f, 1.0f, 0.0f, 1.0f}, 0, 0},
    {"mirror-repeat-negative", 0, OracleAddressMirroredRepeat, -0.25f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {1.0f, 0.0f, 0.0f, 1.0f}, 0, 0},
    {"mirror-clamp", 0, OracleAddressMirrorClamp, -1.25f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {0.0f, 1.0f, 0.0f, 1.0f}, 0, 0},
    {"clamp-upper-edge", 0, OracleAddressClamp, 1.0f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {0.0f, 1.0f, 0.0f, 1.0f}, 0, 0},
    {"border-opaque-black", 0, OracleAddressBorderBlack, -1.25f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {0.0f, 0.0f, 0.0f, 1.0f}, 0, 0},
    {"border-opaque-white", 0, OracleAddressBorderWhite, -1.25f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {1.0f, 1.0f, 1.0f, 1.0f}, 0, 0},
    {"clamp-zero", 0, OracleAddressZero, -1.25f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNearest, {0.0f, 0.0f, 0.0f, 0.0f}, 0, 0},
    {"border-linear-edge", 0, OracleAddressBorder, 0.0f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterLinear, OracleMipNone, {0.5f, 0.0f, 0.0f, 0.5f}, 0, 0},
    {"border-pixel", 1, OracleAddressBorder, -1.0f, 0.25f, 0.0f, 0.0f, 1.0f,
        OracleFilterNearest, OracleMipNone, {0.0f, 0.0f, 0.0f, 0.0f}, 0, 0},
};

static int oracle_color_close(OracleColor a, OracleColor b) {
    const float epsilon = 1.0f / 4096.0f;
    return fabsf(a.r - b.r) <= epsilon && fabsf(a.g - b.g) <= epsilon &&
        fabsf(a.b - b.b) <= epsilon && fabsf(a.a - b.a) <= epsilon;
}

static int oracle_mirror(int coordinate, int size) {
    int repeated = coordinate % (2 * size);
    if (repeated < 0) repeated += 2 * size;
    return repeated < size ? repeated : 2 * size - 1 - repeated;
}

static OracleColor oracle_texel(int level, int x, int y, unsigned resource) {
    OracleColor result = level ? oracleMip0 : oracleBase[y][x];
    result.r *= (float)(resource + 1);
    result.g *= (float)(resource + 1);
    result.b *= (float)(resource + 1);
    return result;
}

static OracleColor oracle_fetch(int level, int x, int y, OracleAddress address,
                                unsigned resource, int *inside) {
    int width = level == 0 ? 2 : 1;
    int height = level == 0 ? 2 : 1;
    *inside = 1;
    int border = address == OracleAddressBorder || address == OracleAddressBorderBlack ||
        address == OracleAddressBorderWhite || address == OracleAddressZero;
    if (border && (x < 0 || y < 0 || x >= width || y >= height)) {
        *inside = 0;
        if (address == OracleAddressBorderBlack) return (OracleColor){0, 0, 0, 1};
        if (address == OracleAddressBorderWhite) return (OracleColor){1, 1, 1, 1};
        return oracleClear;
    }
    if (address == OracleAddressRepeat) {
        x %= width;
        if (x < 0) x += width;
        y %= height;
        if (y < 0) y += height;
    } else if (address == OracleAddressMirroredRepeat) {
        x = oracle_mirror(x, width);
        y = oracle_mirror(y, height);
    } else {
        if (address == OracleAddressMirrorClamp) {
            if (x < 0) x = -x - 1;
            if (y < 0) y = -y - 1;
        }
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= width) x = width - 1;
        if (y >= height) y = height - 1;
    }
    return oracle_texel(level, x, y, resource);
}

static OracleColor oracle_sample_nearest(int level, float u, float v,
                                         int pixelCoordinates,
                                         OracleAddress address, unsigned resource, int *inside) {
    int width = level == 0 ? 2 : 1;
    int height = level == 0 ? 2 : 1;
    float x = pixelCoordinates ? u : u * (float)width;
    float y = pixelCoordinates ? v : v * (float)height;
    return oracle_fetch(level, (int)floorf(x), (int)floorf(y), address, resource, inside);
}

static OracleColor oracle_sample_linear(int level, float u, float v,
                                        int pixelCoordinates,
                                        OracleAddress address, unsigned resource) {
    int width = level == 0 ? 2 : 1;
    int height = level == 0 ? 2 : 1;
    float x = (pixelCoordinates ? u : u * (float)width) - 0.5f;
    float y = (pixelCoordinates ? v : v * (float)height) - 0.5f;
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    float fx = x - (float)x0;
    float fy = y - (float)y0;
    int inside;
    OracleColor c00 = oracle_fetch(level, x0, y0, address, resource, &inside);
    OracleColor c10 = oracle_fetch(level, x0 + 1, y0, address, resource, &inside);
    OracleColor c01 = oracle_fetch(level, x0, y0 + 1, address, resource, &inside);
    OracleColor c11 = oracle_fetch(level, x0 + 1, y0 + 1, address, resource, &inside);
    OracleColor top = {
        c00.r + (c10.r - c00.r) * fx,
        c00.g + (c10.g - c00.g) * fx,
        c00.b + (c10.b - c00.b) * fx,
        c00.a + (c10.a - c00.a) * fx,
    };
    OracleColor bottom = {
        c01.r + (c11.r - c01.r) * fx,
        c01.g + (c11.g - c01.g) * fx,
        c01.b + (c11.b - c01.b) * fx,
        c01.a + (c11.a - c01.a) * fx,
    };
    return (OracleColor){
        top.r + (bottom.r - top.r) * fy,
        top.g + (bottom.g - top.g) * fy,
        top.b + (bottom.b - top.b) * fy,
        top.a + (bottom.a - top.a) * fy,
    };
}

static OracleColor oracle_sample_level(const OracleCase *test, int level, unsigned resource) {
    int inside = 1;
    int linear = test->filter == OracleFilterLinear ||
        (test->filter == OracleFilterMinLinear && test->lod > 0) ||
        (test->filter == OracleFilterMagLinear && test->lod <= 0);
    if (!linear)
        return oracle_sample_nearest(level, test->u, test->v,
            test->pixelCoordinates, test->address, resource, &inside);
    return oracle_sample_linear(level, test->u, test->v,
        test->pixelCoordinates, test->address, resource);
}

static OracleColor oracle_evaluate_resource(const OracleCase *test, unsigned resource,
                                           int *lowLevel, int *highLevel) {
    if (test->pixelCoordinates || test->mipFilter == OracleMipNone) {
        *lowLevel = *highLevel = 0;
        return oracle_sample_level(test, 0, resource);
    }

    float lod = test->lod;
    if (lod < test->lodMin) lod = test->lodMin;
    if (lod > test->lodMax) lod = test->lodMax;
    if (lod < 0.0f) lod = 0.0f;
    if (lod > 1.0f) lod = 1.0f;

    *lowLevel = (int)floorf(lod);
    if (test->mipFilter == OracleMipNearest) *lowLevel = (int)floorf(lod + 0.5f);
    *highLevel = *lowLevel;
    if (test->mipFilter == OracleMipLinear) {
        *highLevel = *lowLevel < 1 ? *lowLevel + 1 : 1;
        float fraction = lod - (float)*lowLevel;
        OracleColor low = oracle_sample_level(test, *lowLevel, resource);
        OracleColor high = oracle_sample_level(test, *highLevel, resource);
        return (OracleColor){
            low.r + (high.r - low.r) * fraction,
            low.g + (high.g - low.g) * fraction,
            low.b + (high.b - low.b) * fraction,
            low.a + (high.a - low.a) * fraction,
        };
    }
    return oracle_sample_level(test, *lowLevel, resource);
}

static void run_cpu_texture_oracle(void) {
    for (unsigned i = 0; i < sizeof oracleCasesTable / sizeof oracleCasesTable[0]; ++i) {
        const OracleCase *test = &oracleCasesTable[i];
        int low = -1, high = -1;
        OracleColor actual = oracle_evaluate_resource(test, 0, &low, &high);
        require(oracle_color_close(actual, test->expected), test->name);
        require(low == test->expectedLowLevel && high == test->expectedHighLevel,
            "oracle selected an unexpected mip");
        oracleCases++;
    }
}

#endif
