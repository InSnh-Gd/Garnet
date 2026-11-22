#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: sudo bash agdc-live-install-vm.sh PREPARED_PROBE_DIR\nInstalls the VM probe and rebuilds only the test VM Aux KC. Reboot the VM to run it.\n'
    exit 0
fi
[ "$#" -eq 1 ] && [ "$(uname -s)" = Darwin ] && [ "$(id -u)" -eq 0 ] || exit 2
[ "$(sw_vers -buildVersion)" = 25G332 ] || exit 2
case " $(sysctl -n machdep.cpu.features) " in *' VMM '*) ;; *) exit 2;; esac
cd "$1"
old=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
[ ! -e AuxiliaryKernelExtensions-before-live.kc ] || exit 2
cp -p "$old" AuxiliaryKernelExtensions-before-live.kc
kmutil inspect -A "$old" --show-prelink-info --no-header > aux-before-live.plist
ditto RDNA4AGDCLive.kext /Library/Extensions/RDNA4AGDCLive.kext
chown -R root:wheel /Library/Extensions/RDNA4AGDCLive.kext
paths=(-p /Library/Extensions/RDNA4AGDCLive.kext)
i=0
while path=$(/usr/libexec/PlistBuddy -c "Print :_PrelinkInfoDictionary:$i:_PrelinkBundlePath" aux-before-live.plist 2>/dev/null); do
    [ -d "$path" ]
    if [ "$path" != /Library/Extensions/RDNA4AGDCLive.kext ]; then paths+=(-p "$path"); fi
    i=$((i + 1))
done
[ "$i" -ge 3 ] || exit 2
kmutil create -n aux -x -z --force --allow-missing-kdk \
    -B /System/Library/KernelCollections/BootKernelExtensions.kc \
    -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
    -A ./AuxiliaryKernelExtensions-with-live.kc "${paths[@]}"
kmutil inspect -A ./AuxiliaryKernelExtensions-with-live.kc --show-extension-info
cp ./AuxiliaryKernelExtensions-with-live.kc "$old"
chown root:wheel "$old"
chmod 644 "$old"
sync
printf 'AGDC_LIVE_INSTALLED backup=%s/AuxiliaryKernelExtensions-before-live.kc\n' "$PWD"
