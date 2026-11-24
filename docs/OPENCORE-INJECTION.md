# Loading the driver from OpenCore - design (2026-10-26)

**Status 2026-11-06: not possible as planned (see Constraints 1). U1 (built-in firmware) stays; U2-U4 are stopped.**

Goal: OpenCore loads the GPU driver at boot (Kernel -> Add), from the EFI partition or a USB stick, instead of macOS
loading it from `/Library/Extensions`.

## What it solves

| Problem today | With OpenCore loading |
|---|---|
| Testing a new build means installing it into a macOS volume; a bad one (Tahoe, 10-26) leaves that macOS unbootable until the kext is removed from another system | The driver lives on the USB stick. Boot **without** the stick (internal EFI has no driver) = plain macOS, always |
| One build per macOS version has to be installed into each volume | One EFI, one entry per version: `MinKernel`/`MaxKernel` 24.x = Sequoia build, 25.x = Tahoe build |
| The kext is loaded from the auxiliary collection ~6 s into the boot, after WindowServer started on the firmware screen: the logo hold, the WindowServer restart, the ~6 s of black | NMXGPU attaches in the first second, before WindowServer: WindowServer opens our framebuffer as its first and only display |
| New kexts need approval in Privacy & Security and an auxiliary-collection rebuild on every update | OpenCore injects them directly; updating = replacing the file on the EFI |
| Firmware read from `/Library/NullMoth/firmware` (must be installed per volume) | Firmware built into the kext |

## Constraints

1. **OpenCore can only add kexts to the boot kernel collection.** Every library a kext lists in `OSBundleLibraries`
   must itself be in the boot collection.
   - NMXGPU needs `kpi.*`, IOPCIFamily, IOACPIFamily, IOGraphicsFamily. **2026-11-06 (nmx-bootkc-check.sh, Sequoia
     15.8.1): IOGraphicsFamily is in the SYSTEM collection, not the boot one - NMXGPU (an IOFramebuffer) cannot be
     injected.** IOPCIFamily, IOACPIFamily and IOSurface are in the boot collection; IOAcceleratorFamily2,
     AppleGraphicsDevicePolicy and AppleGraphicsControl are in the system collection.
   - NMXAccel needs IOAcceleratorFamily2 and IOSurface. Whether those are in the boot collection is **unknown** -
     `tools/macos-helpers/nmx-bootkc-check.sh` answers it on the real machine (Sequoia and Tahoe).
2. **No file system at attach time.** The root volume is mounted later. Everything NMXGPU reads at attach must come
   from the kext, NVRAM, the IORegistry or ACPI: the firmware (b184) is the only file it reads.
3. **No duplicate.** The same bundle id must not also be in `/Library/Extensions` (the auxiliary copy would be refused
   or would race). Installing for OpenCore = removing it there.
4. **The boot logo hold and the auto-arm helper stay** as long as NMXAccel loads from `/Library/Extensions` (after
   WindowServer starts). They go away only if NMXAccel can be injected too (step U3).

## Steps

| Step | Work | Done when |
|---|---|---|
| U1 | **b184:** firmware built into NMXGPU (`gen_fw_embedded.py` -> `xnu/nmx_fw_blobs.inc`: `tgl_dmc_ver2_12`, `tgl_guc_70.1.1`, `tgl_huc_7.9.3`, checked against `app/Resources/nullmoth-gpu-firmware.txt`). The loader tries the built-in copy first, the folder second; with built-in firmware the attach no longer waits for the root volume | log: `firmware ... from the driver itself` on Sequoia with the kext still in `/Library/Extensions` |
| U2 | Sequoia: NMXGPU in OpenCore (USB EFI first, `MinKernel 24.0.0 MaxKernel 24.99.99`), removed from `/Library/Extensions`; NMXAccel, NVRMAGDC, the plugin and the auto-arm helper unchanged | desktop accelerated as before; `nmx-boottime.sh`: NMXGPU attach < 2 s, before WindowServer |
| U3 | If the boot collection has IOAcceleratorFamily2 + IOSurface: NMXAccel (+ NVRMAGDC) injected too; arming at WindowServer's first open instead of the restart | no WindowServer restart; lock screen accelerated from its first frame |
| U4 | Tahoe: the same USB EFI gets the Tahoe builds (`25.0.0`-`25.99.99`); Tahoe's own volume never holds our kexts | Tahoe boots with the stick; without it, plain Tahoe |

## Risks

- **Attach before the root volume:** anything in the attach path that touches files besides firmware fails quietly
  (b184 makes the folder loader return `ENOENT` before a root vnode exists instead of looking up a path).
- **Earlier attach = earlier i915 init:** the console (verbose boot text) moves to our framebuffer earlier; the boot
  guard (NVRAM `nullmoth-gpu-attaching`) still parks the driver after an attach that never returned.
- **Kext size:** the firmware adds about 1 MB to a 13.6 MB kext; the boot collection has room.
- **Licence:** Intel's firmware is redistributable unmodified under `LICENSE.i915` (linux-firmware); it is fetched at
  build time and is not stored in this repository.
