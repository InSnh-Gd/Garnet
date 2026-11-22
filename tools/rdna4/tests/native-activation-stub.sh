#!/usr/bin/env bash
set -eu
name=${0##*/}
printf '%s %s\n' "$name" "$*" >> "$RDNA4_ACTIVATION_TEST_TRACE"
case "$name" in
    sw_vers) printf '25G332\n';;
    sysctl)
        case "$*" in
            *kern.bootsessionuuid*) printf 'activation-test-boot\n';;
            *machdep.cpu.features*) printf 'VMM\n';;
            *) printf 'garnet=1 garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-apps=1 garnet-multisession=1\n';;
        esac;;
    publish-metal)
        [ -f "$RDNA4_ACTIVATION_TEST_TRACE.service-ready" ] || { printf 'GarnetDriver service absent\n' >&2; exit 1; }
        printf '42\n';;
    ioreg)
        case "${1:-}" in -*r*) ;; *) exit 65;; esac
        [ "${RDNA4_ACTIVATION_NO_SERVICE:-0}" != 1 ] || exit 1
        service_state=${RDNA4_ACTIVATION_NO_SERVICE:-0}
        if [ "$service_state" = delayed ]; then
            calls=$(awk '$1 == "ioreg" { count++ } END { print count + 0 }' "$RDNA4_ACTIVATION_TEST_TRACE")
            [ "$calls" -gt 2 ] || service_state=unregistered
        fi
        if [ "$service_state" = unregistered ]; then
            printf '+-o GarnetDriver  <class GarnetDriver, id 1, !registered, !matched, active, busy 1, retain 6>\n'
        else
            printf '+-o GarnetDriver  <class GarnetDriver, id 1, registered, matched, active, busy 0, retain 6>\n'
            : > "$RDNA4_ACTIVATION_TEST_TRACE.service-ready"
        fi;;
    kmutil)
        if [ "${1:-}" = showloaded ]; then
            for bundle in org.garnet.driver; do
                [ "$bundle" = "${RDNA4_ACTIVATION_MISSING_BOOT:-}" ] || printf '%s\n' "$bundle"
            done
            [ "${RDNA4_ACTIVATION_LEGACY_FB:-0}" != 1 ] || printf 'com.hackintosh.RDNA4FB\n'
        fi;;
    mtlprobe-direct)
        [ "${1:-}" != "${RDNA4_ACTIVATION_FAIL_PROBE:-}" ] || exit 9;;
    garnettest)
        case "${2:-}" in
            appallow)
                case "$*" in
                    'accel appallow add WallpaperAerialsExtension'|'accel appallow add System Information'|'accel appallow add system_profiler'|'accel appallow add System Settings'|'accel appallow add iconservicesagent'|'accel appallow add Terminal') ;;
                    *) exit 64;;
                esac
                allow=ok
                [ "${4:-}" != "${RDNA4_ACTIVATION_APP_ALLOW_TARGET:-WallpaperAerialsExtension}" ] || allow=${RDNA4_ACTIVATION_APP_ALLOW:-ok}
                case "$allow" in
                    ok) printf 'code : 0 (ok)\n';;
                    already) printf 'code : 4 (already on the list)\n';;
                    full) printf 'code : 3 (FULL (32 entries))\n'; exit 3;;
                    wrong) printf 'code : 5 (not on the list)\n';;
                    transport) exit 9;;
                    *) exit 64;;
                esac;;
            pipeadopt) printf 'status : 0 (OK)\n';;
            pipeagdc) printf 'agdc status : 0 (PUBLISHED)\n';;
            fbname) exit 11;;
            pipearm) printf '0xccf now : 1\n';;
            pipereload) printf 'Restart Window OPEN\n';;
        esac;;
    native-bundle-check|native-translate|touch|chmod|pgrep|killall|sleep) ;;
    *) exit 64;;
esac
