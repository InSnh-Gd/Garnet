/*
 * GarnetRecover.c — Standalone OpenCore Misc -> Tools UEFI safe-boot & recovery tool.
 *
 * MIT License. Zero ESP/disk filesystem writes; communicates solely via UEFI NVRAM:
 *   [1] Safe Boot Once: strips Garnet kill-switches, appends
 *       "garnet=0 garnet-aux=0 garnet-rebar-compat=0", clears GarnetRecoverAction.
 *   [2] Safe Boot & Uninstall Garnet: same safe boot-args + sets
 *       6E727447-6172-4E65-9452-65636F766572:GarnetRecoverAction = "uninstall".
 *   [Esc]/[Q] Cancel: modifies zero NVRAM variables and returns to OpenCore Picker.
 */
#include <stddef.h>
#include <stdint.h>

#include "garnet_recover_pure.h"

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t EFI_STATUS;
typedef uint64_t UINTN;
typedef uint16_t CHAR16;
typedef uint8_t  BOOLEAN;
typedef void    *EFI_HANDLE;
typedef void    *EFI_EVENT;

#define EFI_ERROR_BIT        (1ULL << 63)
#define EFI_SUCCESS          0ULL
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_BIT | 5ULL)
#define EFI_NOT_FOUND        (EFI_ERROR_BIT | 14ULL)

#define EFI_VARIABLE_NON_VOLATILE       0x00000001u
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x00000002u
#define EFI_VARIABLE_RUNTIME_ACCESS     0x00000004u

#define EFI_ATTR_NV_BS_RT (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
#define EFI_ATTR_BS_RT    (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    uint16_t ScanCode;
    CHAR16   UnicodeChar;
} EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);
    EFI_EVENT WaitForKey;
};

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, const CHAR16 *String);
    EFI_STATUS (EFIAPI *TestString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, const CHAR16 *String);
    void *QueryMode;
    void *SetMode;
    EFI_STATUS (EFIAPI *SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
    EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
};

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime;
    void *SetTime;
    void *GetWakeupTime;
    void *SetWakeupTime;
    void *SetVirtualAddressMap;
    void *ConvertPointer;
    EFI_STATUS (EFIAPI *GetVariable)(
        const CHAR16 *VariableName,
        EFI_GUID     *VendorGuid,
        uint32_t     *Attributes,
        UINTN        *DataSize,
        void         *Data
    );
    void *GetNextVariableName;
    EFI_STATUS (EFIAPI *SetVariable)(
        const CHAR16 *VariableName,
        EFI_GUID     *VendorGuid,
        uint32_t      Attributes,
        UINTN         DataSize,
        const void   *Data
    );
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    void *AllocatePages;
    void *FreePages;
    void *GetMemoryMap;
    void *AllocatePool;
    void *FreePool;
    void *CreateEvent;
    void *SetTimer;
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    uint32_t                         FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
} EFI_SYSTEM_TABLE;

static void print_str(EFI_SYSTEM_TABLE *st, const CHAR16 *s) {
    if (st && st->ConOut && st->ConOut->OutputString) {
        st->ConOut->OutputString(st->ConOut, s);
    }
}

static EFI_STATUS wait_key(EFI_SYSTEM_TABLE *st, EFI_INPUT_KEY *key) {
    if (!st || !st->ConIn || !st->ConIn->ReadKeyStroke) {
        return EFI_NOT_FOUND;
    }
    for (;;) {
        if (st->BootServices && st->BootServices->WaitForEvent && st->ConIn->WaitForKey) {
            UINTN idx = 0;
            EFI_EVENT ev = st->ConIn->WaitForKey;
            (void)st->BootServices->WaitForEvent(1, &ev, &idx);
        }
        EFI_STATUS rc = st->ConIn->ReadKeyStroke(st->ConIn, key);
        if (rc == EFI_SUCCESS) {
            return EFI_SUCCESS;
        }
    }
}

static EFI_STATUS set_nvram_var(
    EFI_RUNTIME_SERVICES *rt,
    const CHAR16         *name,
    EFI_GUID             *guid,
    const void           *data,
    UINTN                 size
) {
    EFI_STATUS rc = rt->SetVariable(name, guid, EFI_ATTR_NV_BS_RT, size, data);
    if (rc != EFI_SUCCESS) {
        rc = rt->SetVariable(name, guid, EFI_ATTR_BS_RT, size, data);
    }
    return rc;
}

static void clear_nvram_var(EFI_RUNTIME_SERVICES *rt, const CHAR16 *name, EFI_GUID *guid) {
    EFI_STATUS rc = rt->SetVariable(name, guid, 0u, 0u, NULL);
    if (rc != EFI_SUCCESS && rc != EFI_NOT_FOUND) {
        (void)rt->SetVariable(name, guid, EFI_ATTR_NV_BS_RT, 0u, NULL);
    }
}

static EFI_STATUS apply_safe_boot_args(EFI_SYSTEM_TABLE *st) {
    static const CHAR16 kBootArgsName[] = u"boot-args";
    EFI_GUID apple_guid = GARNET_APPLE_BOOT_GUID_INIT;
    char existing[GARNET_BOOT_ARGS_BUF_SIZE];
    char updated[GARNET_BOOT_ARGS_BUF_SIZE];
    UINTN existing_size = sizeof(existing) - 1u;
    uint32_t attrs = 0u;

    for (size_t i = 0; i < sizeof(existing); ++i) {
        existing[i] = '\0';
        updated[i] = '\0';
    }

    EFI_STATUS rc = st->RuntimeServices->GetVariable(
        kBootArgsName, &apple_guid, &attrs, &existing_size, existing
    );
    if (rc == EFI_NOT_FOUND) {
        existing_size = 0u;
    } else if (rc != EFI_SUCCESS) {
        print_str(st, u"ERROR: Failed to read NVRAM boot-args (buffer too large or firmware error).\r\n");
        return rc;
    }

    size_t updated_len = 0u;
    if (!garnet_recover_build_safe_boot_args(existing, (size_t)existing_size, updated, sizeof(updated), &updated_len)) {
        print_str(st, u"ERROR: Updated boot-args would exceed 1023-byte limit; NVRAM unchanged.\r\n");
        return EFI_BUFFER_TOO_SMALL;
    }

    /* Write exact ASCII bytes without trailing NUL so macOS nvram -p stays clean. */
    rc = set_nvram_var(st->RuntimeServices, kBootArgsName, &apple_guid, updated, (UINTN)updated_len);
    if (rc != EFI_SUCCESS) {
        print_str(st, u"ERROR: Failed to write NVRAM boot-args.\r\n");
    }
    return rc;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    static const CHAR16 kActionName[] = u"GarnetRecoverAction";
    static const char kUninstall[] = GARNET_RECOVER_ACTION_UNINSTALL;
    EFI_GUID garnet_guid = GARNET_VENDOR_GUID_INIT;
    (void)ImageHandle;

    if (!SystemTable || !SystemTable->RuntimeServices || !SystemTable->ConIn || !SystemTable->ConOut) {
        return EFI_NOT_FOUND;
    }

    if (SystemTable->ConOut->ClearScreen) {
        (void)SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    }

    print_str(SystemTable, u"============================================================\r\n");
    print_str(SystemTable, u" Garnet Safe Boot & Recovery Tool (Zero ESP Disk Writes)\r\n");
    print_str(SystemTable, u"============================================================\r\n\r\n");
    print_str(SystemTable, u"  [1] Safe Boot Once\r\n");
    print_str(SystemTable, u"      Disable Garnet kexts for this boot only; keep driver installed.\r\n\r\n");
    print_str(SystemTable, u"  [2] Safe Boot & Uninstall Garnet\r\n");
    print_str(SystemTable, u"      Disable Garnet kexts and uninstall system-disk files on next macOS boot.\r\n");
    print_str(SystemTable, u"      (Stays on safe desktop; restore config.plist via Garnet.app / restore-oc.)\r\n\r\n");
    print_str(SystemTable, u"  [Esc] / [Q] Cancel and return to OpenCore Picker\r\n\r\n");
    print_str(SystemTable, u"Select [1], [2], or [Esc]: ");

    EFI_INPUT_KEY key = { 0u, 0u };
    garnet_recover_choice_t choice = GARNET_MENU_IGNORE;
    while (choice == GARNET_MENU_IGNORE) {
        if (wait_key(SystemTable, &key) != EFI_SUCCESS) {
            return EFI_NOT_FOUND;
        }
        choice = garnet_recover_decode_key(key.ScanCode, key.UnicodeChar);
    }

    print_str(SystemTable, u"\r\n\r\n");

    if (choice == GARNET_MENU_CANCEL) {
        print_str(SystemTable, u"Cancelled. No NVRAM or disk changes made.\r\n");
        return EFI_SUCCESS;
    }

    EFI_STATUS rc = apply_safe_boot_args(SystemTable);
    if (rc != EFI_SUCCESS) {
        print_str(SystemTable, u"\r\nPress any key to return to OpenCore Picker...");
        (void)wait_key(SystemTable, &key);
        return rc;
    }

    if (choice == GARNET_MENU_SAFE_BOOT_ONCE) {
        clear_nvram_var(SystemTable->RuntimeServices, kActionName, &garnet_guid);
        print_str(SystemTable, u"OK: Safe boot-args set (garnet=0 garnet-aux=0 garnet-rebar-compat=0).\r\n");
        print_str(SystemTable, u"    Pending uninstall flag cleared. Your ESP/config.plist was NOT modified.\r\n\r\n");
        print_str(SystemTable, u"IMPORTANT: Boot macOS directly from OpenCore Picker now (do NOT reboot first).\r\n");
    } else {
        rc = set_nvram_var(
            SystemTable->RuntimeServices,
            kActionName,
            &garnet_guid,
            kUninstall,
            (UINTN)(sizeof(kUninstall) - 1u)
        );
        if (rc != EFI_SUCCESS) {
            print_str(SystemTable, u"ERROR: Safe boot-args were set, but failed to write GarnetRecoverAction.\r\n");
            print_str(SystemTable, u"       You can still boot macOS safely and run: sudo garnetctl uninstall\r\n");
            print_str(SystemTable, u"\r\nPress any key to return to OpenCore Picker...");
            (void)wait_key(SystemTable, &key);
            return rc;
        }
        print_str(SystemTable, u"OK: Safe boot-args set and GarnetRecoverAction=uninstall armed.\r\n");
        print_str(SystemTable, u"    Your ESP/config.plist was NOT modified.\r\n\r\n");
        print_str(SystemTable, u"IMPORTANT: Boot macOS directly from OpenCore Picker now (do NOT reboot first).\r\n");
        print_str(SystemTable, u"           macOS will uninstall Garnet system-disk files and stay on safe desktop.\r\n");
    }

    print_str(SystemTable, u"\r\nPress any key to return to OpenCore Picker...");
    (void)wait_key(SystemTable, &key);
    return EFI_SUCCESS;
}
