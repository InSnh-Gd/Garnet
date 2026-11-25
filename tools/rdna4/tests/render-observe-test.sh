#!/bin/bash
set -euo pipefail
tests=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$tests/../../.." && pwd)
activation=${1:-$tests/../activate-native.sh}
scratch_root=${TMPDIR:-$repo/temp/scratch}
mkdir -p "$scratch_root"
work=$(mktemp -d "${scratch_root%/}/garnet-render-observe.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir "$work/persistent" "$work/ephemeral"
touch "$work/persistent/garnet-observe"
sed -e "s|/private/var/tmp/garnet-observe|$work/persistent/garnet-observe|g" \
    -e "s|/private/tmp/garnet-|$work/ephemeral/garnet-|g" \
    "$activation" > "$work/activate-native.sh"
for boot in 1 2; do
    bash "$tests/native-activation-test.sh" "$work/activate-native.sh" > "$work/boot-$boot.log"
    for flag in crc cblog surfaces; do
        [ -f "$work/ephemeral/garnet-$flag" ] && [ ! -L "$work/ephemeral/garnet-$flag" ]
    done
    [ "$(cat "$work/ephemeral/garnet-crc")" = '128 noslot' ]
    printf 'OBSERVE_BOOT_PASS boot=%s ephemeral-markers=recreated\n' "$boot"
    rm "$work/ephemeral/garnet-crc" "$work/ephemeral/garnet-cblog" "$work/ephemeral/garnet-surfaces"
done
printf 'unchanged\n' > "$work/sentinel"
ln -s "$work/sentinel" "$work/ephemeral/garnet-cblog"
if bash "$tests/native-activation-test.sh" "$work/activate-native.sh" > "$work/link.log" 2>&1; then
    printf 'OBSERVE_FAIL untrusted marker was accepted\n' >&2
    exit 1
fi
[ "$(cat "$work/sentinel")" = unchanged ]
printf 'OBSERVE_LINK_REFUSAL_PASS\n'
