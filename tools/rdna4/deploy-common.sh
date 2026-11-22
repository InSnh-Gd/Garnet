# Shared by install-native.sh and uninstall-native.sh (sourced; bash 3.2 compatible).
# Every absolute path goes through $R so tests/install-native-test.sh can run both scripts against a scratch tree
# with stub commands. On a real machine RDNA4_DEPLOY_ROOT is unset and $R is empty.
R=${RDNA4_DEPLOY_ROOT:-}
buddy=${RDNA4_PLISTBUDDY:-/usr/libexec/PlistBuddy}
guid=7C436110-AB2A-4BBB-A880-FE41995C9F82
support="$R/Library/Application Support/RDNA4/native"
aux="$R/Library/KernelCollections/AuxiliaryKernelExtensions.kc"

# The standalone Garnet components this delivery owns:
# - Garnet.kext (org.garnet.driver) lives in /Library/Extensions (Aux KC),
#   because AMDGarnetPrimaryFramebuffer inherits IOFramebuffer (IOGraphicsFamily),
#   which lives in SystemKernelExtensions.kc (not BootKernelExtensions.kc).
# - GarnetAccel.kext (org.garnet.accel) lives in /Library/Extensions (Aux KC)
# - GarnetMetal.bundle (org.garnet.metal) lives in /Library/GPUBundles
# Retired pre-Garnet components (AMDRadeonRDNA4, RDNA4Bringup, RDNA4Accel, RDNA4FB,
# RDNA4Metal, Navi48*) are backup-and-remove only; never rewrite another vendor's kext
# (including WhateverGreen or a user-installed Lilu).
GARNET_BUNDLE=Garnet.kext
GARNET_BUNDLE_ID=org.garnet.driver
GARNET_EXECUTABLE=Garnet
ACCEL_BUNDLE=GarnetAccel.kext
ACCEL_BUNDLE_ID=org.garnet.accel
ACCEL_EXECUTABLE=GarnetAccel
METAL_BUNDLE=GarnetMetal.bundle
METAL_BUNDLE_ID=org.garnet.metal
DELIVERY_MANIFEST_FORMAT=garnet-delivery-v1

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
note() { printf '%s\n' "$*"; }
launch_job_loaded() { launchctl print "system/$1" >/dev/null 2>&1; }

# Boot-args the native profile sets. garnet-apps-all is deliberately NOT here (it bypasses the application
# allow-list); add it by hand only for a diagnostic boot.
RDNA4_ARGS_BASE='garnet=1 garnet-interrupts=0'
RDNA4_ARGS_NATIVE='garnet-stage=17 garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 amfi=0x80'
RDNA4_ARGS_VM='garnet-stage=0'
# Controlled runs use the matched GPU stack but deliberately do not install
# boot launch jobs or opt into the desktop activation ladder. garnet-metal=1 is
# required for the root/in-process Metal-nub publish selector; it does not
# publish at boot. Display-pipe, WindowServer, app and multisession latches stay
# off, and root admission is unchanged.
RDNA4_ARGS_CONTROLLED='garnet-stage=17 garnet-native=1 garnet-metal=1'

# Drop every token this project owns, in current or retired spellings, plus the ones the installer adds (amfi, -radvesa).
strip_args() {  # $1 boot-args -> stdout
    local kept= flag
    for flag in $1; do
        case "$flag" in garnet|garnet=*|garnet-*|rdna4bringup*|rdna4-*|navi48bringup*|navi48-*|amfi=*|-radvesa) ;; *) kept="$kept $flag";; esac
    done
    printf '%s\n' "${kept# }"
}
profile_args() {  # $1 old boot-args, $2 profile -> stdout
    local base; base=$(strip_args "$1")
    case "$2" in
        native) printf '%s\n' "${base:+$base }$RDNA4_ARGS_BASE $RDNA4_ARGS_NATIVE";;
        vm-load-only) printf '%s\n' "${base:+$base }$RDNA4_ARGS_BASE $RDNA4_ARGS_VM";;
        controlled) printf '%s\n' "${base:+$base }$RDNA4_ARGS_BASE $RDNA4_ARGS_CONTROLLED";;
        *) return 1;;
    esac
}
legacy_args() {  # $1 boot-args -> pre-Garnet tokens still present (GarnetBootArgs.h honours rdna4-* with a log line)
    local out= flag
    for flag in $1; do case "$flag" in rdna4bringup*|rdna4-*|navi48bringup*|navi48-*) out="$out $flag";; esac; done
    printf '%s\n' "${out# }"
}

# Rename legacy tokens in place (navi48bringup/rdna4bringup -> garnet, navi48-X/rdna4-X -> garnet-X); a token whose
# current spelling is already set is dropped. *-apps-all is dropped (allow-list bypass is per-boot diagnostic only).
migrate_args() {  # $1 boot-args -> stdout
    local out= flag name new
    for flag in $1; do
        case "$flag" in
            navi48-apps-all*|rdna4-apps-all*) continue;;
            navi48bringup*) new="garnet${flag#navi48bringup}";;
            rdna4bringup*) new="garnet${flag#rdna4bringup}";;
            navi48-*) new="garnet-${flag#navi48-}";;
            rdna4-*) new="garnet-${flag#rdna4-}";;
            *) out="$out $flag"; continue;;
        esac
        name=${new%%=*}
        case " $1 " in *" $name="*|*" $name "*) continue;; esac
        out="$out $new"
    done
    printf '%s\n' "${out# }"
}

# Retired pre-Garnet artefacts. Code, plists and kexts are moved into the backup; caches and dump dirs are moved/deleted.
LEGACY_DAEMONS='com.navi44.native com.navi44.translate com.navi48.translate com.navi44.desktop-test com.rdna4.translate com.rdna4.native com.rdna4.native-translate'
legacy_moves() {
    local d
    for d in $LEGACY_DAEMONS; do printf '%s\n' "$R/Library/LaunchDaemons/$d.plist"; done
    printf '%s\n' \
        "$R/Library/Extensions/Navi48Accel.kext" \
        "$R/Library/Extensions/Navi48Bringup.kext" \
        "$R/Library/Extensions/Navi48FB.kext" \
        "$R/Library/GPUBundles/Navi48Metal.bundle" \
        "$R/Library/Extensions/RDNA4Accel.kext" \
        "$R/Library/Extensions/RDNA4Bringup.kext" \
        "$R/Library/Extensions/RDNA4FB.kext" \
        "$R/Library/Extensions/AMDRadeonRDNA4.kext" \
        "$R/Library/GPUBundles/RDNA4Metal.bundle" \
        "$R/usr/local/navi48" \
        "$R/usr/local/rdna4" \
        "$R/Library/Application Support/Navi44/native" \
        "$R/Library/Application Support/RDNA4/native/rdna4test"
}
legacy_deletes() {
    local p
    for p in "$R"/private/tmp/n48m* "$R"/private/var/tmp/n48m*; do
        [ -e "$p" ] || [ -L "$p" ] && printf '%s\n' "$p"
    done
    return 0
}
legacy_present() {  # stdout: every legacy path that exists now
    local p
    { legacy_moves; legacy_deletes; } | while IFS= read -r p; do [ -e "$p" ] || [ -L "$p" ] && printf '%s\n' "$p"; done
    return 0
}
remove_legacy() {  # $1 backup dir
    local d p n=0 dest
    for d in $LEGACY_DAEMONS; do
        if [ -e "$R/Library/LaunchDaemons/$d.plist" ]; then
            launchctl bootout "system/$d" >/dev/null 2>&1 || :
            note "legacy: stopped $d"
        fi
    done
    mkdir -p "$1/legacy"
    : > "$1/legacy-map.tsv"
    while IFS= read -r p; do
        [ -e "$p" ] || [ -L "$p" ] || continue
        n=$((n+1))
        dest="$1/legacy/$n-${p##*/}"
        mv "$p" "$dest"
        printf '%s\t%s\n' "$p" "$dest" >> "$1/legacy-map.tsv"
        note "legacy: moved $p -> $dest"
    done < <(legacy_moves)
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        n=$((n+1))
        dest="$1/legacy-cache/$n-${p##*/}"
        mkdir -p "${dest%/*}"
        mv "$p" "$dest"
        printf '%s\t%s\n' "$p" "$dest" >> "$1/legacy-map.tsv"
        note "legacy: moved cache $p -> $dest"
    done < <(legacy_deletes)
}

# OpenCore Kernel.Add: 0..n entries. Prints "index<TAB>BundlePath<TAB>Enabled".
kernel_add_list() {  # $1 config
    local i b e
    for ((i=0; ; i++)); do
        if ! b=$($buddy -c "Print :Kernel:Add:$i:BundlePath" "$1" 2>/dev/null); then
            # A valid list ends here. An entry that exists but has no BundlePath
            # is corruption and must not be mistaken for a short list.
            $buddy -c "Print :Kernel:Add:$i" "$1" >/dev/null 2>&1 && return 1
            break
        fi
        if ! e=$($buddy -c "Print :Kernel:Add:$i:Enabled" "$1" 2>/dev/null); then
            $buddy -c "Print :Kernel:Add:$i" "$1" >/dev/null 2>&1 && return 1
            e=false
        fi
        printf '%s\t%s\t%s\n' "$i" "$b" "$e"
    done
}
kernel_indices_bundle() {  # $1 config, $2 exact BundlePath -> one index per line
    local i b e list
    list=$(kernel_add_list "$1") || return 1
    while IFS=$'\t' read -r i b e; do
        [ "$b" != "$2" ] || printf '%s\n' "$i"
    done <<< "$list"
}
kernel_get() {  # $1 config, $2 index, $3 key
    $buddy -c "Print :Kernel:Add:$2:$3" "$1"
}
kernel_unique_index() {  # $1 config, $2 bundle basename -> index; nonzero if absent/duplicate
    local indices count
    indices=$(kernel_indices_bundle "$1" "$2")
    [ -n "$indices" ] || return 1
    count=$(printf '%s\n' "$indices" | wc -l | tr -d ' ')
    [ "$count" -eq 1 ] || return 2
    printf '%s\n' "$indices"
}
kernel_delete_bundle() {  # $1 config, $2 exact BundlePath
    local indices value k
    local -a items
    indices=$(kernel_indices_bundle "$1" "$2")
    [ -n "$indices" ] || return 0
    items=($indices)
    for ((k=${#items[@]}-1; k>=0; k--)); do
        value=$(kernel_get "$1" "${items[k]}" BundlePath) || value=
        note "config: remove managed entry ${items[k]} ($value)"
        $buddy -c "Delete :Kernel:Add:${items[k]}" "$1"
    done
}
kernel_disable_bundle() {  # $1 config, $2 exact BundlePath
    local i b e list
    list=$(kernel_add_list "$1") || return 1
    while IFS=$'\t' read -r i b e; do
        [ "$b" != "$2" ] || $buddy -c "Set :Kernel:Add:$i:Enabled false" "$1"
    done <<< "$list"
}

# A build must provide this machine-readable record next to MANIFEST.txt.
# Accepted lines are KEY=VALUE, KEY: VALUE, or KEY<TAB>VALUE.
manifest_value() {  # $1 file, $2 key
    awk -v want="$2" '
        BEGIN { n = length(want) }
        index($0, want "=") == 1 { print substr($0, n + 2); exit }
        index($0, want ":") == 1 {
            value = substr($0, n + 2)
            sub(/^[ \t]+/, "", value)
            print value
            exit
        }
        index($0, want "\t") == 1 { print substr($0, n + 2); exit }
    ' "$1"
}
manifest_any() {  # $1 file, $2.. aliases
    local file=$1 value key
    shift
    for key in "$@"; do
        value=$(manifest_value "$file" "$key")
        if [ -n "$value" ]; then
            printf '%s\n' "$value"
            return 0
        fi
    done
    return 1
}
manifest_require() {  # $1 file, $2 logical name, $3.. aliases
    local file=$1 name=$2 value
    shift 2
    value=$(manifest_any "$file" "$@") || fail "Build manifest $file is missing $name"
    printf '%s\n' "$value"
}
delivery_manifest() {  # $1 payload dir
    local file
    for file in "$1/DELIVERY-MANIFEST.txt" "$1/BUILD-MANIFEST.txt" "$1/build-manifest.txt" "$1/MANIFEST.txt"; do
        [ -f "$file" ] || continue
        [ "$(manifest_any "$file" FORMAT BUILD_MANIFEST_FORMAT)" = "$DELIVERY_MANIFEST_FORMAT" ] || continue
        printf '%s\n' "$file"
        return 0
    done
    fail "No $DELIVERY_MANIFEST_FORMAT build manifest in $1"
}
sha256_file() { shasum -a 256 "$1" | awk '{print $1}'; }
plist_value() {  # $1 plist, $2 key
    $buddy -c "Print :$2" "$1"
}
bundle_value() {  # $1 kext/bundle, $2 Info.plist key
    plist_value "$1/Contents/Info.plist" "$2"
}
verify_bundle_identity() {  # $1 bundle, $2 id, $3 executable, $4 label
    local bundle=$1 expected_id=$2 expected_executable=$3 label=$4 actual
    [ -d "$bundle" ] && [ ! -L "$bundle" ] || fail "$label is missing or is a symbolic link: $bundle"
    [ -f "$bundle/Contents/Info.plist" ] && [ ! -L "$bundle/Contents/Info.plist" ] \
        || fail "$label has no regular Contents/Info.plist"
    [ -f "$bundle/Contents/MacOS/$expected_executable" ] \
        && [ ! -L "$bundle/Contents/MacOS/$expected_executable" ] \
        || fail "$label executable is missing: $expected_executable"
    actual=$(bundle_value "$bundle" CFBundleIdentifier) || fail "$label has no CFBundleIdentifier"
    [ "$actual" = "$expected_id" ] || fail "$label CFBundleIdentifier is $actual, expected $expected_id"
    actual=$(bundle_value "$bundle" CFBundleExecutable) || fail "$label has no CFBundleExecutable"
    [ "$actual" = "$expected_executable" ] || fail "$label CFBundleExecutable is $actual, expected $expected_executable"
}
verify_legacy_bundles() {
    local path
    path="$R/Library/Extensions/Navi48Accel.kext"
    [ ! -e "$path" ] && [ ! -L "$path" ] || \
        verify_bundle_identity "$path" com.navi48.accelprobe Navi48Accel 'legacy Navi48Accel'
    path="$R/Library/Extensions/Navi48Bringup.kext"
    [ ! -e "$path" ] && [ ! -L "$path" ] || \
        verify_bundle_identity "$path" com.navi48.bringup Navi48Bringup 'legacy Navi48Bringup'
    path="$R/Library/GPUBundles/Navi48Metal.bundle"
    [ ! -e "$path" ] && [ ! -L "$path" ] || \
        verify_bundle_identity "$path" com.navi48.metal Navi48Metal 'legacy Navi48Metal'
    path="$R/Library/Extensions/RDNA4Accel.kext"
    [ ! -e "$path" ] && [ ! -L "$path" ] || \
        verify_bundle_identity "$path" com.rdna4.accelprobe RDNA4Accel 'legacy RDNA4Accel'
    path="$R/Library/Extensions/RDNA4Bringup.kext"
    [ ! -e "$path" ] && [ ! -L "$path" ] || \
        verify_bundle_identity "$path" com.rdna4.bringup RDNA4Bringup 'legacy RDNA4Bringup'
    path="$R/Library/Extensions/RDNA4FB.kext"
    [ ! -e "$path" ] && [ ! -L "$path" ] || \
        verify_bundle_identity "$path" com.hackintosh.RDNA4FB RDNA4FB 'legacy RDNA4FB'
    path="$R/Library/Extensions/AMDRadeonRDNA4.kext"
    [ ! -e "$path" ] && [ ! -L "$path" ] || \
        verify_bundle_identity "$path" com.hackintosh.AMDRadeonRDNA4 AMDRadeonRDNA4 'legacy AMDRadeonRDNA4'
    path="$R/Library/GPUBundles/RDNA4Metal.bundle"
    [ ! -e "$path" ] && [ ! -L "$path" ] || \
        verify_bundle_identity "$path" com.rdna4.metal RDNA4Metal 'legacy RDNA4Metal'
}
verify_delivery_manifest() {  # $1 payload, $2 manifest -> sets MF_* globals
    local payload=$1 manifest=$2 actual
    [ "$(manifest_any "$manifest" FORMAT BUILD_MANIFEST_FORMAT)" = "$DELIVERY_MANIFEST_FORMAT" ] \
        || fail "Unsupported build manifest format: $manifest"
    MF_GARNET_BUNDLE_ID=$(manifest_require "$manifest" GARNET_BUNDLE_ID GARNET_BUNDLE_ID)
    MF_GARNET_VERSION=$(manifest_require "$manifest" GARNET_VERSION GARNET_VERSION)
    MF_GARNET_EXECUTABLE_SHA256=$(manifest_require "$manifest" GARNET_EXECUTABLE_SHA256 GARNET_EXECUTABLE_SHA256)
    MF_GARNET_INFO_SHA256=$(manifest_require "$manifest" GARNET_INFO_SHA256 GARNET_INFO_SHA256)
    MF_GARNET_STANDALONE=$(manifest_require "$manifest" GARNET_STANDALONE GARNET_STANDALONE)
    MF_ACCEL_BUNDLE_ID=$(manifest_require "$manifest" ACCEL_BUNDLE_ID ACCEL_BUNDLE_ID)
    MF_ACCEL_VERSION=$(manifest_require "$manifest" ACCEL_VERSION ACCEL_VERSION)
    MF_ACCEL_EXECUTABLE_SHA256=$(manifest_require "$manifest" ACCEL_EXECUTABLE_SHA256 ACCEL_EXECUTABLE_SHA256)
    MF_ACCEL_INFO_SHA256=$(manifest_require "$manifest" ACCEL_INFO_SHA256 ACCEL_INFO_SHA256)
    MF_ACCEL_STANDALONE=$(manifest_require "$manifest" ACCEL_STANDALONE ACCEL_STANDALONE)

    [ "$MF_GARNET_BUNDLE_ID" = "$GARNET_BUNDLE_ID" ] \
        || fail "Manifest Garnet bundle id is $MF_GARNET_BUNDLE_ID, expected $GARNET_BUNDLE_ID"
    [ "$MF_ACCEL_BUNDLE_ID" = "$ACCEL_BUNDLE_ID" ] \
        || fail "Manifest Accel bundle id is $MF_ACCEL_BUNDLE_ID, expected $ACCEL_BUNDLE_ID"
    [ "$MF_GARNET_STANDALONE" = true ] && [ "$MF_ACCEL_STANDALONE" = true ] \
        || fail 'Manifest does not record standalone Garnet kexts'

    verify_bundle_identity "$payload/$GARNET_BUNDLE" "$MF_GARNET_BUNDLE_ID" "$GARNET_EXECUTABLE" \
        "payload $GARNET_BUNDLE"
    actual=$(bundle_value "$payload/$GARNET_BUNDLE" CFBundleVersion) \
        || fail "payload $GARNET_BUNDLE has no CFBundleVersion"
    [ "$actual" = "$MF_GARNET_VERSION" ] \
        || fail "Payload Garnet version is $actual, manifest says $MF_GARNET_VERSION"
    actual=$(sha256_file "$payload/$GARNET_BUNDLE/Contents/MacOS/$GARNET_EXECUTABLE")
    [ "$actual" = "$MF_GARNET_EXECUTABLE_SHA256" ] \
        || fail "Payload Garnet executable hash differs from build manifest"
    actual=$(sha256_file "$payload/$GARNET_BUNDLE/Contents/Info.plist")
    [ "$actual" = "$MF_GARNET_INFO_SHA256" ] \
        || fail "Payload Garnet Info.plist hash differs from build manifest"
    if actual=$(bundle_value "$payload/$GARNET_BUNDLE" OSBundleLibraries:as.vit9696.Lilu 2>/dev/null) && [ -n "$actual" ]; then
        fail "Payload $GARNET_BUNDLE must not declare a Lilu dependency"
    fi

    verify_bundle_identity "$payload/$ACCEL_BUNDLE" "$MF_ACCEL_BUNDLE_ID" "$ACCEL_EXECUTABLE" \
        "payload $ACCEL_BUNDLE"
    actual=$(bundle_value "$payload/$ACCEL_BUNDLE" CFBundleVersion) \
        || fail "payload $ACCEL_BUNDLE has no CFBundleVersion"
    [ "$actual" = "$MF_ACCEL_VERSION" ] \
        || fail "Payload Accel version is $actual, manifest says $MF_ACCEL_VERSION"
    actual=$(sha256_file "$payload/$ACCEL_BUNDLE/Contents/MacOS/$ACCEL_EXECUTABLE")
    [ "$actual" = "$MF_ACCEL_EXECUTABLE_SHA256" ] \
        || fail "Payload Accel executable hash differs from build manifest"
    actual=$(sha256_file "$payload/$ACCEL_BUNDLE/Contents/Info.plist")
    [ "$actual" = "$MF_ACCEL_INFO_SHA256" ] \
        || fail "Payload Accel Info.plist hash differs from build manifest"
    if actual=$(bundle_value "$payload/$ACCEL_BUNDLE" OSBundleLibraries:as.vit9696.Lilu 2>/dev/null) && [ -n "$actual" ]; then
        fail "Payload $ACCEL_BUNDLE must not declare a Lilu dependency"
    fi
}
validate_kernel_config() {  # $1 next config, $2 garnet bundle basename
    local config=$1 garnet=$2 i b e list
    list=$(kernel_add_list "$config") || fail 'Cannot read the prepared OpenCore Kernel.Add list'
    while IFS=$'\t' read -r i b e; do
        case "$b" in
            AMDRadeonRDNA4.kext|RDNA4Bringup.kext|RDNA4FB.kext|Navi48FB.kext|Navi48Bringup.kext|RDNA4Accel.kext|Navi48Accel.kext)
                [ "$e" != true ] || fail "Retired kext remains enabled in Kernel.Add: $b";;
            "$garnet")
                [ "$e" != true ] || fail "$garnet must stay in the Aux KC, not Kernel.Add";;
            "$ACCEL_BUNDLE")
                [ "$e" != true ] || fail "$ACCEL_BUNDLE must stay in the Aux KC, not Kernel.Add";;
        esac
    done <<< "$list"
}
# Aux KC bundle ids/paths via kmutil inspect. Prints "id<TAB>path".
aux_list() {  # $1 kc, $2 scratch plist
    local i id path
    [ -f "$1" ] || return 0
    kmutil inspect -A "$1" --show-prelink-info --no-header > "$2" \
        || return 1
    for ((i=0; ; i++)); do
        if ! id=$($buddy -c "Print :_PrelinkInfoDictionary:$i:CFBundleIdentifier" "$2" 2>/dev/null); then
            $buddy -c "Print :_PrelinkInfoDictionary:$i" "$2" >/dev/null 2>&1 && return 1
            break
        fi
        path=$($buddy -c "Print :_PrelinkInfoDictionary:$i:_PrelinkBundlePath" "$2" 2>/dev/null) \
            || return 1
        printf '%s\t%s\n' "$id" "$path"
    done
}
# Rebuild the Aux KC: keep every foreign extension, drop ours and retired ones, add $3.. (kext paths).
# $1 stage dir, $2 output kc. Returns 2 when nothing would remain (caller removes the Aux KC).
AUX_OURS="$ACCEL_BUNDLE_ID $GARNET_BUNDLE_ID com.rdna4.accelprobe com.hackintosh.RDNA4FB com.hackintosh.AMDRadeonRDNA4 com.rdna4.bringup com.navi48.accelprobe com.navi48.bringup"
rebuild_aux() {
    local stage=$1 out=$2 id path keep o list expected rc=0; shift 2
    local paths=()
    list="$stage/aux-current.tsv"
    expected="$stage/aux-expected.tsv"
    : > "$expected"
    aux_list "$aux" "$stage/aux-current.plist" > "$list" \
        || fail 'Aux KC inspect failed; no existing extension was discarded'
    while IFS=$'\t' read -r id path; do
        [ -n "$id" ] || continue
        keep=1; for o in $AUX_OURS; do [ "$id" != "$o" ] || keep=0; done
        [ "$keep" -eq 1 ] || continue
        [ -d "$path" ] && [ ! -L "$path" ] || fail "Cannot preserve Aux KC extension: $id ($path)"
        printf '%s\t%s\n' "$id" "$path" >> "$expected"
        paths+=(-p "$path")
    done < "$list"
    for p in "$@"; do
        path="$R$p"
        case "$p" in
            "/Library/Extensions/$GARNET_BUNDLE")
                verify_bundle_identity "$path" "$GARNET_BUNDLE_ID" "$GARNET_EXECUTABLE" "Aux KC addition $p"
                printf '%s\t%s\n' "$GARNET_BUNDLE_ID" "$path" >> "$expected"
                ;;
            "/Library/Extensions/$ACCEL_BUNDLE")
                verify_bundle_identity "$path" "$ACCEL_BUNDLE_ID" "$ACCEL_EXECUTABLE" "Aux KC addition $p"
                printf '%s\t%s\n' "$ACCEL_BUNDLE_ID" "$path" >> "$expected"
                ;;
            *)
                fail "Unexpected Aux KC addition: $p"
                ;;
        esac
        paths+=(-p "$path")
    done
    [ -s "$expected" ] || return 2
    kmutil create -n aux -x -z --force --allow-missing-kdk \
        -B /System/Library/KernelCollections/BootKernelExtensions.kc \
        -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
        -A "$out" "${paths[@]}" || rc=$?
    [ "$rc" -eq 0 ] || return "$rc"
    aux_list "$out" "$stage/aux-rebuilt.plist" > "$stage/aux-rebuilt.tsv" \
        || fail 'Aux KC inspect failed after rebuild'
    sort "$expected" > "$stage/aux-expected.sorted"
    sort "$stage/aux-rebuilt.tsv" > "$stage/aux-rebuilt.sorted"
    cmp -s "$stage/aux-expected.sorted" "$stage/aux-rebuilt.sorted" \
        || fail 'Aux KC rebuild did not preserve the requested extension set'
}
aux_has_id() {  # $1 kc, $2 id
    local stage id path
    [ -f "$1" ] || return 1
    stage=$(mktemp -d "${TMPDIR:-/tmp}/garnet-aux-check.XXXXXXXX")
    aux_list "$1" "$stage/aux.plist" > "$stage/list" || {
        rm -rf "$stage"
        return 2
    }
    while IFS=$'\t' read -r id path; do
        if [ "$id" = "$2" ]; then
            rm -rf "$stage"
            return 0
        fi
    done < "$stage/list"
    rm -rf "$stage"
    return 1
}
validate_aux() {  # $1 aux KC, $2 scratch dir; require Garnet + GarnetAccel and no legacy kexts
    local kc=$1 stage=$2 id path garnet=0 accel=0
    aux_list "$kc" "$stage/aux-check.plist" > "$stage/aux-check.tsv" \
        || fail 'Aux KC inspect failed after rebuild'
    while IFS=$'\t' read -r id path; do
        case "$id" in
            "$GARNET_BUNDLE_ID")
                [ "$path" = "$R/Library/Extensions/$GARNET_BUNDLE" ] \
                    || fail "Aux KC contains $GARNET_BUNDLE from an unexpected path: $path"
                garnet=1;;
            "$ACCEL_BUNDLE_ID")
                [ "$path" = "$R/Library/Extensions/$ACCEL_BUNDLE" ] \
                    || fail "Aux KC contains $ACCEL_BUNDLE from an unexpected path: $path"
                accel=1;;
            com.hackintosh.AMDRadeonRDNA4|com.hackintosh.RDNA4FB|com.rdna4.accelprobe|com.rdna4.bringup|com.navi48.accelprobe|com.navi48.bringup)
                fail "Aux KC still carries retired extension: $id";;
        esac
    done < "$stage/aux-check.tsv"
    [ "$garnet" -eq 1 ] || fail "Aux KC did not adopt $GARNET_BUNDLE"
    [ "$accel" -eq 1 ] || fail "Aux KC did not adopt $ACCEL_BUNDLE"
}
