# GarnetAccel (`org.garnet.accel`) Installation Notes

Never overwrite a running Auxiliary Kernel Extension bundle in `/Library/Extensions/` while loaded; bump to the new versioned directory, remove superseded auxiliary kext bundles if present, rebuild the Aux KC, and load explicitly:

NEVER `kmutil install --update-all`

## 0.0.8 (native error preservation at IOService 5-arg newUserClient)

1. Remove any previous `GarnetAccel*.kext` from `/Library/Extensions/` once rebooted safely, and stage `build/GarnetAccel.kext` as `/Library/Extensions/GarnetAccel.kext`.
2. Verify ownership (`root:wheel`), `755` permissions, `xattr -cr`, and run `verify.sh` before and after `garnetnub publish`.

## 0.0.7 (M6 Stage 1a: multi-display framebuffer latch `garnet-m6=1`)

1. Back up the pre-install Aux KC to `backup/auxkc-pre-0.0.7` before modifying `/Library/Extensions/`.
2. Move `/Library/Extensions/GarnetAccel-0.0.6.kext` out of `/Library/Extensions/` and install the new build at `/Library/Extensions/GarnetAccel-0.0.7.kext`:
   - `sudo cp -R build/GarnetAccel.kext /Library/Extensions/GarnetAccel-0.0.7.kext && sudo chown -R root:wheel /Library/Extensions/GarnetAccel-0.0.7.kext`
   - `sudo chmod -R go-w /Library/Extensions/GarnetAccel-0.0.7.kext`
   - `sudo xattr -cr /Library/Extensions/GarnetAccel-0.0.7.kext`
3. Rebuild the Aux KC (Step 4b): approve System Settings Privacy & Security if prompted (`org.garnet.accel` is preserved), NEVER `kmutil install --update-all`.
4. Boot with `stage17-native-1440-metal-disp-amfi-m6.plist` (`garnet-m6=1`), then load and activate in order:
   - `sudo kmutil load -p /Library/Extensions/GarnetAccel-0.0.7.kext`
   - `accel fbpublish 2`
   - `accel pipeadopt`
   - `accel pipeagdc 1`
   - `accel m6stat`
5. Verify `/Library/Extensions/GarnetAccel-0.0.7.kext` via `verify.sh` (`org.garnet.driver` boot-loaded from ESP, auxiliary kext loaded).

    Kill switch at any time: boot-arg `garnet-aux=0` (both framebuffers refuse to start). With garnet-m6=1 they START only when the routing guard is active.

## 0.0.6 (Run B: Monitor A display index 2 personality + ABI 2 snapshot)

1. Back up the pre-install Aux KC to `backup/auxkc-pre-0.0.6`.
2. Install to `/Library/Extensions/GarnetAccel-0.0.6.kext`:
   - `sudo cp -R build/GarnetAccel.kext /Library/Extensions/GarnetAccel-0.0.6.kext && sudo chown -R root:wheel /Library/Extensions/GarnetAccel-0.0.6.kext`
   - `sudo chmod -R go-w /Library/Extensions/GarnetAccel-0.0.6.kext`
   - `sudo xattr -cr /Library/Extensions/GarnetAccel-0.0.6.kext`
3. Rebuild the Aux KC (Step 4b); NEVER `kmutil install --update-all`.
4. Load the new kext:
   - `sudo kmutil load -p /Library/Extensions/GarnetAccel-0.0.6.kext`
5. Run B display activation order for Monitor B (`2`) and Monitor A (`1`):
   - `fbhold 2`
   - `fbhold 1`
   - `fbpublish 2`
   - `fbpublish 1`
   - `sudo killall -9 WindowServer`
6. Inspect `/Library/Extensions/GarnetAccel-0.0.6.kext` registration in IORegistry (`GarnetFramebuffer` on `GarnetDisplayNub` index 1 and 2).

    Kill switch at any time: boot-arg `garnet-aux=0` (both framebuffers refuse to start). The framebuffers also refuse to start with boot-arg garnet-metal-ws present.

## 0.0.5 (G6: accelerator `'GRNT'` native user client route)

1. Back up the pre-install Aux KC to `backup/auxkc-pre-0.0.5`.
2. Install to `/Library/Extensions/GarnetAccel-0.0.5.kext`:
   - `sudo cp -R build/GarnetAccel.kext /Library/Extensions/GarnetAccel-0.0.5.kext && sudo chown -R root:wheel /Library/Extensions/GarnetAccel-0.0.5.kext`
   - `sudo chmod -R go-w /Library/Extensions/GarnetAccel-0.0.5.kext`
   - `sudo xattr -cr /Library/Extensions/GarnetAccel-0.0.5.kext`
3. Rebuild the Aux KC (Step 4b); NEVER `kmutil install --update-all`.
4. Load and verify the new kext:
   - `sudo kmutil load -p /Library/Extensions/GarnetAccel-0.0.5.kext`
   - Verify `/Library/Extensions/GarnetAccel-0.0.5.kext` via `verify.sh` and confirm sandboxed clients open `'GRNT'` on `GarnetAccelerator`.

    Kill switch at any time: boot-arg `garnet-aux=0` (a 'GRNT' open on the accelerator then returns kIOReturnUnsupported).

## 0.0.4 (M5: Monitor B secondary `IOFramebuffer` on `GarnetDisplayNub`)

1. Install at the NEW Aux KC path `/Library/Extensions/GarnetAccel-0.0.4.kext` (`0.0.4`, reusing approved bundle identifier `org.garnet.accel`).
2. Rebuild only the Aux KC collection for `/Library/Extensions/GarnetAccel-0.0.4.kext`:
   NEVER `kmutil install --update-all` (see step 4b).
3. Load with `sudo kmutil load -p /Library/Extensions/GarnetAccel-0.0.4.kext`.
