# Build freestanding x86_64 PE32+ UEFI application GarnetRecover.efi and run pure C host tests.
CC ?= clang
HOST_CC ?= $(CC)
LLD_LINK ?= lld-link
BUILD ?= build

EFI_CFLAGS := -target x86_64-unknown-windows -ffreestanding -fno-builtin \
	-fno-stack-protector -mno-red-zone -mno-stack-arg-probe -fshort-wchar \
	-O2 -Wall -Wextra -Werror -std=c11

EFI_LDFLAGS := /subsystem:efi_application /entry:efi_main /nodefaultlib /opt:ref

HOST_CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined

.PHONY: all efi test clean

all: efi

efi: $(BUILD)/GarnetRecover.efi

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/GarnetRecover.obj: GarnetRecover.c garnet_recover_pure.h | $(BUILD)
	$(CC) $(EFI_CFLAGS) -c -o $@ GarnetRecover.c

$(BUILD)/GarnetRecover.efi: $(BUILD)/GarnetRecover.obj | $(BUILD)
	$(LLD_LINK) $(EFI_LDFLAGS) /out:$@ $<

$(BUILD)/recover_pure_test: tests/recover_pure_test.c garnet_recover_pure.h | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -I. -o $@ tests/recover_pure_test.c

test: $(BUILD)/recover_pure_test
	$(BUILD)/recover_pure_test

clean:
	rm -rf $(BUILD)
