#!/bin/bash
set -euo pipefail
name=${0##*/}
printf '%s %s\n' "$name" "$*" >> "$RDNA4_DEPLOY_TEST_TRACE"
case "$name" in
    uname) printf 'Darwin\n';;
    id) [ "${1:-}" = -u ]; printf '0\n';;
    sw_vers) printf '25G332\n';;
    sysctl)
        [ "${*: -1}" = machdep.cpu.features ]
        if [ "${RDNA4_DEPLOY_TEST_VMM:-0}" = 1 ]; then printf 'VMM\n'; else printf 'VMX\n'; fi
        ;;
    sync)
        if [ "${RDNA4_DEPLOY_STUB_FAIL:-}" = sync ]; then exit 9; fi
        ;;
    launchctl)
        state=${RDNA4_DEPLOY_TEST_TRACE:-${TMPDIR:-/tmp}/rdna4-deploy-stub}.launchctl-state
        if [ "${RDNA4_DEPLOY_TEST_LOADED_JOBS+x}" = x ]; then
            initial=$RDNA4_DEPLOY_TEST_LOADED_JOBS
        else
            initial='<unset>'
        fi
        marker=$state.initial
        if [ ! -f "$marker" ] || [ "$(cat "$marker")" != "$initial" ]; then
            printf '%s\n' "$initial" > "$marker"
            : > "$state"
            for label in ${RDNA4_DEPLOY_TEST_LOADED_JOBS:-}; do
                printf '%s\n' "$label" >> "$state"
            done
        fi
        case "${1:-}" in
            print)
                label=${2#system/}
                grep -Fqx "$label" "$state" || exit 113
                ;;
            bootout)
                label=${2#system/}
                grep -Fvx "$label" "$state" > "$state.next" || :
                mv "$state.next" "$state"
                ;;
            bootstrap)
                [ "${2:-}" = system ] || exit 64
                label=${3##*/}
                label=${label%.plist}
                grep -Fqx "$label" "$state" || printf '%s\n' "$label" >> "$state"
                ;;
            *) ;;
        esac
        ;;
    codesign|chown) ;;
    plutil)
        python3 - "$@" <<'PY'
import base64
import plistlib
import sys
args = sys.argv[1:]
with open(args[-1], "rb") as stream:
    root = plistlib.load(stream)
if args[0] == "-insert" and args[2] == "-data":
    parts = args[1].split(".")
    parent = root
    for part in parts[:-1]:
        parent = parent[part]
    if parts[-1] in parent:
        sys.exit("key already exists")
    parent[parts[-1]] = base64.b64decode(args[3], validate=True)
    with open(args[-1], "wb") as stream:
        plistlib.dump(root, stream)
elif args[0] != "-lint":
    sys.exit("unsupported plutil command")
PY
        ;;
    kmutil)
        if [ "${1:-}" = create ] && [ "${RDNA4_DEPLOY_STUB_FAIL:-}" = kmutil-create ]; then exit 9; fi
        if [ "${1:-}" = inspect ] && [ "${RDNA4_DEPLOY_STUB_FAIL:-}" = kmutil-inspect-second ]; then
            state=${RDNA4_DEPLOY_TEST_TRACE:-${TMPDIR:-/tmp}/rdna4-deploy-stub}.inspect-count
            count=0
            [ ! -f "$state" ] || count=$(cat "$state")
            count=$((count + 1))
            printf '%s\n' "$count" > "$state"
            [ "$count" -lt 2 ] || exit 9
        fi
        python3 - "$@" <<'PY'
import os
import plistlib
import sys
args = sys.argv[1:]
if args[0] == "create":
    out = args[args.index("-A") + 1]
    entries = []
    for i, arg in enumerate(args):
        if arg == "-p":
            path = args[i + 1]
            with open(path + "/Contents/Info.plist", "rb") as stream:
                info = plistlib.load(stream)
            entries.append({"CFBundleIdentifier": info["CFBundleIdentifier"],
                            "_PrelinkBundlePath": path})
    if os.environ.get("RDNA4_DEPLOY_STUB_FAIL") == "kmutil-create-drop-foreign":
        entries = [item for item in entries
                   if item["CFBundleIdentifier"] != "com.example.foreign"]
    with open(out, "wb") as stream:
        plistlib.dump({"_PrelinkInfoDictionary": entries}, stream)
elif args[0] == "inspect":
    if "--show-prelink-info" in args:
        with open(args[args.index("-A") + 1], "rb") as stream:
            sys.stdout.buffer.write(stream.read())
    else:
        print("stub extension info")
else:
    sys.exit("unsupported kmutil command")
PY
        ;;
    *) exit 64;;
esac
