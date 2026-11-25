#include <stdio.h>
#include <string.h>
#include "garnet_nocopy.h"

int main(void) {
    char text[4]; int fails = 0;
    rdna4nc_protection_text(0, text);
    if (strcmp(text, "---")) { printf("FAIL no access: %s\n", text); fails++; }
    rdna4nc_protection_text(RDNA4NC_PROT_READ, text);
    if (strcmp(text, "r--")) { printf("FAIL read only: %s\n", text); fails++; }
    rdna4nc_protection_text(RDNA4NC_PROT_READ | RDNA4NC_PROT_WRITE, text);
    if (strcmp(text, "rw-")) { printf("FAIL read/write: %s\n", text); fails++; }
    rdna4nc_protection_text(RDNA4NC_PROT_READ | RDNA4NC_PROT_WRITE | RDNA4NC_PROT_EXEC, text);
    if (strcmp(text, "rwx")) { printf("FAIL all access: %s\n", text); fails++; }
    if (!fails) puts("PASS test-nocopy-protection (4 checks)");
    return fails ? 1 : 0;
}
