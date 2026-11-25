#include "garnet_argument_public_sampler.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned checks;

static void require(int condition, const char *message) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "ARGUMENT_PUBLIC_SAMPLER_FAIL %s\n", message);
        exit(1);
    }
}

static void test_limit_calculation(void) {
    rdna4_argument_public_sampler_plan plan;
    require(rdna4_argument_public_sampler_limit(
                1, 96, 100000, 2, 2, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                plan.table_limit == 47 && plan.object_limit == 49999 &&
                plan.public_limit == 47,
            "mixed 96-slot table reserves the dummy and dual-use region");
    require(rdna4_argument_public_sampler_limit(
                0, 96, 100000, 1, 1, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                plan.table_limit == 95 && plan.object_limit == 99999 &&
                plan.public_limit == 95,
            "normal table reserves one dummy row and one object");

    require(rdna4_argument_public_sampler_limit(
                1, 5, 100, 0, 2, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                plan.public_limit == 1,
            "one live dual-use configuration after dummy");
    require(rdna4_argument_public_sampler_limit(
                1, 2, 100, 0, 2, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                plan.public_limit == 0,
            "dummy-only dual-use table admits no live configuration");
    require(rdna4_argument_public_sampler_limit(
                1, 1, 100, 0, 2, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_NO_CONFIGURATIONS,
            "descriptor capacity below the dummy is rejected");

    require(rdna4_argument_public_sampler_limit(
                1, 96, 96, 2, 2, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                plan.object_limit == 47 && plan.public_limit == 47,
            "last legal dual object budget");
    require(rdna4_argument_public_sampler_limit(
                1, 96, 95, 2, 2, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                plan.object_limit == 46 && plan.public_limit == 46,
            "one object below the dual budget");
    require(rdna4_argument_public_sampler_limit(
                1, 96, 100, 7, 2, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                plan.object_limit == 46 && plan.public_limit == 46,
            "reserved dummy/static/direct objects reduce object headroom");
    require(rdna4_argument_public_sampler_limit(
                1, 96, 144, 3, 3, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                plan.table_limit == 47 && plan.object_limit == 47 &&
                plan.public_limit == 47,
            "ABI8 pixel worst case counts three objects per configuration");
    require(rdna4_argument_public_sampler_limit(
                1, 96, 143, 3, 3, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                plan.object_limit == 46 && plan.public_limit == 46,
            "one object below the ABI8 pixel worst-case budget");
    require(rdna4_argument_public_sampler_limit(
                0, 96, 96, 1, 1, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                plan.table_limit == 95 && plan.object_limit == 95 &&
                plan.public_limit == 95,
            "normal object budget still reserves the dummy object");
    require(rdna4_argument_public_sampler_limit(
                1, 96, 1, 1, 2, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK &&
                plan.object_limit == 0 && plan.public_limit == 0,
            "reserve covering all objects leaves no public object limit");

    require(rdna4_argument_public_sampler_limit(
                0, 0, 100, 0, 1, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE,
            "zero descriptor capacity is unavailable");
    require(rdna4_argument_public_sampler_limit(
                1, 96, 0, 0, 2, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE,
            "unqueried sampler object limit is unavailable");
    require(rdna4_argument_public_sampler_limit(
                1, UINT64_C(1) << 32, 100, 0, 2, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OVERFLOW,
            "descriptor capacity above the table representation is rejected");
    require(rdna4_argument_public_sampler_limit(
                2, 96, 100, 0, 2, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID,
            "comparison representation must be boolean");
    require(rdna4_argument_public_sampler_limit(
                1, 96, 100, 0, 0, &plan) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID,
            "zero objects per configuration is rejected");
}

static void test_admission(void) {
    int admitted = -1;
    require(rdna4_argument_public_sampler_admit(
                47, 46, 1, 0, 2, 100, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK && admitted,
            "last legal new public configuration");
    require(rdna4_argument_public_sampler_admit(
                47, 47, 1, 0, 2, 100, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_AT_LIMIT && !admitted,
            "new public configuration above the limit is rejected");
    require(rdna4_argument_public_sampler_admit(
                47, 47, 0, 0, 2, 100, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK && admitted,
            "reuse at the public limit is allowed");
    require(rdna4_argument_public_sampler_admit(
                47, 20, 1, 98, 2, 100, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK && admitted,
            "last legal sampler object pair");
    require(rdna4_argument_public_sampler_admit(
                47, 20, 1, 99, 2, 100, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OBJECT_LIMIT && !admitted,
            "sampler object exhaustion rejects before admission");
    require(rdna4_argument_public_sampler_admit(
                47, 46, 1, 141, 3, 144, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK && admitted,
            "last legal ABI8 pixel three-object reservation");
    require(rdna4_argument_public_sampler_admit(
                47, 46, 1, 142, 3, 144, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OBJECT_LIMIT && !admitted,
            "ABI8 pixel three-object reservation cannot exceed the backend");
    require(rdna4_argument_public_sampler_admit(
                47, 20, 1, 101, 0, 100, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_OBJECT_LIMIT && !admitted,
            "corrupt live object count cannot wrap the object budget");
    require(rdna4_argument_public_sampler_admit(
                0, 0, 1, 0, 0, 100, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_AT_LIMIT && !admitted,
            "zero public limit rejects a new configuration");
    require(rdna4_argument_public_sampler_admit(
                47, 0, 1, 0, 0, 0, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID && !admitted,
            "unqueried object limit cannot admit");
    require(rdna4_argument_public_sampler_admit(
                47, 0, 2, 0, 0, 100, &admitted) ==
                RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID && !admitted,
            "invalid uniqueness flag is rejected");
}

int main(void) {
    test_limit_calculation();
    test_admission();
    printf("ARGUMENT_PUBLIC_SAMPLER_PASS checks=%u gpu_execution=0\n", checks);
    return 0;
}
