#!/bin/bash
set -euo pipefail
umask 077
if [ "${1:-}" = --help ]; then
    printf 'Usage: sudo bash activate-native.sh\nActivates native Metal after readiness, compute, render and IOSurface pixel checks.\n'
    exit 0
fi
[ "$#" -eq 0 ] || { printf 'Use --help for usage.\n' >&2; exit 64; }
root=$(cd "$(dirname "$0")" && pwd)
printf 'RDNA4 boot activation: %s\n' "$(date -u +%FT%TZ)"
boot=$(sysctl -n kern.bootsessionuuid)
product_ver=$(sw_vers -productVersion)
case "$product_ver" in
    26|26.*) ;;
    *) printf 'Unsupported macOS version: %s (macOS 26.x required)\n' "$product_ver" >&2; exit 1;;
esac
[ "$(uname -m)" = x86_64 ] || { printf 'Unsupported architecture: %s (x86_64 required)\n' "$(uname -m)" >&2; exit 1; }
profile=$(cat "$root/profile")
case "$profile" in
    vm-load-only) style=start-only;;
    native) style=start-and-match;;
    *) printf 'Unknown activation profile\n' >&2; exit 1;;
esac
# Garnet.kext must be loaded at boot to own early BAR/PCI setup and PrimaryFramebuffer.
loaded=$(kmutil showloaded --list-only)
for bundle_id in org.garnet.driver; do
    printf '%s\n' "$loaded" | awk -v wanted="$bundle_id" '
        { for (i = 1; i <= NF; i++) if ($i == wanted) found = 1 }
        END { exit !found }
    ' || { printf 'Required boot component is not loaded: %s\n' "$bundle_id" >&2; exit 1; }
done
if printf '%s\n' "$loaded" | awk '
    { for (i = 1; i <= NF; i++) if ($i ~ /^(com\.hackintosh\.RDNA4FB|com\.hackintosh\.AMDRadeonRDNA4|com\.rdna4\.bringup|com\.rdna4\.accelprobe|com\.navi48\.bringup)$/) found = 1 }
    END { exit !found }
'; then
    printf 'Conflicting legacy RDNA4/Navi48 kext is loaded\n' >&2
    exit 1
fi
kmutil load -b org.garnet.accel --load-style "$style" --verbose
"$root/native-bundle-check" /Library/GPUBundles/GarnetMetal.bundle
if [ "$profile" = vm-load-only ]; then
    case " $(sysctl -n machdep.cpu.features) " in *' VMM '*) ;; *) exit 1;; esac
    printf 'VM_LOAD_ONLY_PASS: kexts and Metal bundle loaded; no GPU execution tested.\n'
    exit 0
fi
args=$(sysctl -n kern.bootargs)
for flag in garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-apps=1 garnet-multisession=1; do
    case " $args " in *" $flag "*) ;; *) printf 'Missing %s\n' "$flag" >&2; exit 1;; esac
done

# kmutil can list org.garnet.driver as loaded before its GarnetDriver IOService has finished
# start() and called registerService(). publish-metal would then fail with "service absent".
wait_for_bringup() {
    local i
    for ((i = 0; i < 180; i++)); do
        if ioreg -rc GarnetDriver -d1 2>/dev/null | grep -q '<class GarnetDriver,.* registered, matched, active,'; then
            sleep 1
            ioreg -rc GarnetDriver -d1 2>/dev/null | grep -q '<class GarnetDriver,.* registered, matched, active,' && return 0
        fi
        sleep 1
    done
    return 1
}

wait_for_bringup || { printf 'GarnetDriver IOService did not appear\n' >&2; exit 1; }
"$root/native-translate" --wait-ready 30
rid=$("$root/publish-metal")
export RDNA4_METAL_BUNDLE=/Library/GPUBundles/GarnetMetal.bundle RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1
"$root/mtlprobe-direct" compute --registry-id "$rid"
# Successful compute and fences do not establish that the render backend writes pixels.
"$root/mtlprobe-direct" quad --registry-id "$rid"
"$root/mtlprobe-direct" sysdraw --registry-id "$rid"
"$root/mtlprobe-direct" windowserver-lut --registry-id "$rid"
step() {
    local expected=$1 output status=0
    shift
    output=$("$root/garnettest" accel "$@") || status=$?
    printf '%s\n' "$output"
    if [ "$status" -ne 0 ]; then
        "$root/garnettest" log || :
        return "$status"
    fi
    printf '%s\n' "$output" | grep -Eq "$expected"
}
# The active aerial wallpaper uses an ordinary-user Metal service after login.
step 'code[[:space:]]*: (0 \(ok\)|4 \(already on the list\))' appallow add WallpaperAerialsExtension
# System Information draws its sidebar through Metal and enumerates devices as the console user.
step 'code[[:space:]]*: (0 \(ok\)|4 \(already on the list\))' appallow add 'System Information'
step 'code[[:space:]]*: (0 \(ok\)|4 \(already on the list\))' appallow add system_profiler
# Settings and its user-side icon renderer open their own Metal sessions.
step 'code[[:space:]]*: (0 \(ok\)|4 \(already on the list\))' appallow add 'System Settings'
step 'code[[:space:]]*: (0 \(ok\)|4 \(already on the list\))' appallow add iconservicesagent
# Terminal uses RGhA CA Whippet IOSurfaces on wide-gamut (P3) displays, which macOS 26 drops to zero without Metal.
step 'code[[:space:]]*: (0 \(ok\)|4 \(already on the list\))' appallow add Terminal
# Persistent application denylist: load proc: entries into kernel gAppDeny (bundle: entries are enforced by GarnetMetal.bundle).
deny_file=
if [ -L "$root/app-denylist.txt" ]; then
    printf 'Refusing symbolic-link denylist: %s/app-denylist.txt\n' "$root" >&2
    exit 1
elif [ -f "$root/app-denylist.txt" ]; then
    deny_file="$root/app-denylist.txt"
elif [ -f "$root/default-app-denylist.txt" ] && [ ! -L "$root/default-app-denylist.txt" ]; then
    deny_file="$root/default-app-denylist.txt"
fi
if [ -n "$deny_file" ]; then
    while IFS= read -r raw_deny || [ -n "$raw_deny" ]; do
        deny_line=${raw_deny%$'\r'}
        deny_line=${deny_line#"${deny_line%%[!$' \t']*}"}
        deny_line=${deny_line%"${deny_line##*[!$' \t']}"}
        case "$deny_line" in
            ''|\#*) continue;;
            proc:*)
                deny_proc=${deny_line#proc:}
                deny_proc=${deny_proc#"${deny_proc%%[!$' \t']*}"}
                [ -n "$deny_proc" ] || continue
                step 'code[[:space:]]*: (0 \(ok\)|4 \(already on the denylist\))' appdeny add "$deny_proc"
                ;;
            bundle:*) ;;
        esac
    done < "$deny_file"
fi
# /tmp is cleared at boot; an owned persistent opt-in restores observational diagnostics.
if [ -f /private/var/tmp/rdna4m-observe ] && [ ! -L /private/var/tmp/rdna4m-observe ] && [ -O /private/var/tmp/rdna4m-observe ]; then
    observe_flag() {
        local path=$1 content=$2
        if [ -e "$path" ] || [ -L "$path" ]; then
            [ -f "$path" ] && [ ! -L "$path" ] && [ -O "$path" ] || return 1
        else
            (set -o noclobber; umask 022; printf '%s\n' "$content" > "$path") || return 1
        fi
        chmod 644 "$path"
    }
    observe_flag /private/tmp/rdna4m-crc '128 noslot'
    observe_flag /private/tmp/rdna4m-cblog ''
    observe_flag /private/tmp/rdna4m-surfaces ''
    printf 'RDNA4 render observation enabled for this boot.\n'
fi
step 'status +: 0 \(OK\)|status 0 \(OK\)' pipeadopt
step 'agdc status +: 0 \(PUBLISHED\)' pipeagdc 1
touch /private/tmp/rdna4m-headless-no
chmod 644 /private/tmp/rdna4m-headless-no
step '0xccf now +: 1' pipearm 1
step '[Rr]estart [Ww]indow' pipereload
if pgrep -x WindowServer >/dev/null; then
    killall -9 WindowServer
fi
printf 'NATIVE_ACTIVATION_ATTEMPTED boot=%s registry-id=%s; inspect WindowServer and GPU results.\n' "$boot" "$rid"
