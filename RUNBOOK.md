# Ace 5 (giulia / PKG110) runbook: SMP + var_store persistence

This runbook is for the OnePlus Ace 5 port (`YiKongk/HackiOS-Ace5`). It covers:

- **Phase 0** — backup and hardware extraction (read-only on the phone).
- **Phase 1** — SMP: `cpus=` / `cpumask=` boot arguments, 1 → 2 → 4 → 8 cores.
- **Phase 2** — `var_store` persistence: carve a `darwin` partition, enable UFS writes, persist `/private/var`. **Blocked on PKG110 as of 2026-10-25: `userdata` is metadata-encrypted; see the blocker note in the Phase 2 section.**

Not covered here: full root-on-userdata (needs a writable APFS, which this tree does not have), iOS 18 (a different kernel/rootfs stack), Wi-Fi, audio, camera.

**Environment.** The build needs Linux ARM64, LLVM 18, Theos (`iPhoneOS16.5.sdk`), `7z` 23+, and macOS only for wallpaper conversion. The current checkout on Windows can edit and inspect code but cannot build XNU or run fastboot. Run every `python3 tools/ace5/...` command from the repository root on the build host. If you have no Linux ARM64 host, `docs/GITHUB-ACTIONS.md` builds the same artifacts on a free public ARM64 runner; once `install-artifact.py` has placed them, start at Phase 1. A complete Ubuntu 24.04 ARM64 cloud-host setup is in [docs/BUILD-HOST.md](docs/BUILD-HOST.md).

**Safety.** `tools/ace5/backup.py` backs up 22 boot-chain partitions, not `userdata`. Phase 2 repartitions `userdata` and loses its contents. Do not start Phase 2 without a backup you have tested restoring, or explicit acceptance that the data is disposable.

---

## Host-only checks (no phone, no build)

These run on Windows or Linux and validate the code paths without touching the phone:

```bash
tools/ace5/host-selftest.sh
python3 tools/ace5/check-host.py

ACE5_CPUS=2 python3 tools/ace5/phone.py --serial X run --dry-run --seconds 60 --bootcpu 1
ACE5_CPUMASK=0x7F python3 tools/ace5/phone.py --serial X run --dry-run --persist --seconds 60 --bootcpu 1
```

`--dry-run` prints the exact command line and payload command, then exits before any fastboot call. `host-selftest.sh` also builds a synthetic GPT to test `storage-constants.py` and `apply-storage-constants.py`. `check-host.py` reports which build tools are missing on the current machine.

---

## Phase 0 — baseline, backup, extraction

```bash
export ACE5_SERIAL='<adb serial of the phone>'
adb -s "$ACE5_SERIAL" get-state
python3 tools/ace5/backup.py --serial "$ACE5_SERIAL" \
  --output local/backups/ace5-before-first-boot
tools/ace5/extract-hw.sh local/device
python3 tools/ace5/inspect-dtb.py local/device/giulia.dtb
```

`extract-hw.sh` writes:

| File | Used by |
|---|---|
| `giulia.dtb` | CPU topology, GIC, `ufshc@...` |
| `cpuinfo.txt`, `cpu-list.txt`, `dmesg.txt`, `interrupts.txt` | SMP validation |
| `by-name.txt`, `partitions.txt`, `gpt-head.bin`, `gpt-tail.bin` | Phase 2 carve |
| `userdata-meta-before.bin`, `segment_info-before.txt` | `tools/userdata-carve.py` |
| `fw-list.txt` | later Wi-Fi/audio/camera work |

Check before continuing:

- `inspect-dtb.py` shows the `arm,gic-v3` node at `0x17100000` / `0x17180000` and `redistributor-stride = <0x40000>`.
- `dmesg.txt` shows PSCI and the UFS controller; note the `ufshc@...` address.
- `backup.py` finished with `manifest.json` and 22 `.img` files.

Baseline (unchanged code, one core):

```bash
python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" enter --stage kernel
python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" ramdisk --image local/system-wallpaper.apfs
ACE5_CPUS=1 python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" run \
  --seconds 600 --bootcpu 1 --live-log --live-interval 10 \
  --kernel out/giulia-kernel/mach.ace5-public --root-image local/root-daily.apfs
```

Save the log path printed at the end. This is the control run for every SMP comparison.

---

## Phase 1 — SMP

### What changed

- `tools/ace5/phone.py` now takes one CPU selector from the environment:
  - `ACE5_CPUS=1..8` → `cpus=N`
  - `ACE5_CPUMASK=<number>` → `cpumask=<number>`; **bits are positions in the loader's CPU list, not physical core numbers**.
- The loader lists only cores that are off, and omits the core it stays on (CPU0, which serves USB). With `bootcpu=1`, the kernel therefore gets at most 7 listed cores, and `cpus=8` is already the normal way to use all of them. Check the printed `cores for the kernel, in the order it takes them:` line; use `cpumask` only to drop a specific listed position.
- XNU **panics if both `cpus=` and `cpumask=` are present**, so the script emits exactly one. Do not set both variables.
- The duplicate `cpus=1` in the old command line was removed.
- `common/devices/giulia.conf` now records `BOOTARGS="-v cpus=8"`; the real boot path is `phone.py`.

`platform_giulia` already has `cores=8`, `core_shift=8`, `small_cores=0x03`, GIC `0x17100000`/`0x17180000`, stride `0x40000`. No loader-platform change is needed for the core count.

### Build

Skip this section if you used the GitHub Actions workflow: its artifact already contains `mach.ace5-public` and both boot images (`docs/GITHUB-ACTIONS.md`).

```bash
xnu-deps/setup.sh --stage1
xnu/apply.sh
xnu-deps/setup.sh --stage2
./xnu-build.sh -j2 build
mkdir -p out/giulia-kernel
cp xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  out/giulia-kernel/mach.ace5-public

tools/ace5/build-loader.sh kernel
python3 tools/ace5/pack-boot.py --mu <Mu-Silicium> --dtb local/device/giulia.dtb \
  --stage kernel --output out/giulia-kernel/hackios-giulia-kernel.img
```

UEFI itself is built per `docs/UEFI.zh-CN.md`; `pack-boot.py` consumes the resulting Mu-Silicium tree.

### Test ladder

Stage the system image once after `enter`: it is the `rd=md0` root and carries `/sbin/launchd`. Without it the kernel mounts the small `root-daily.apfs` as `rd=md0` and panics with `Process 1 exec of /sbin/launchd failed, errno 2` right after the SMP bring-up (the core lines below are still valid).

```bash
python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" ramdisk --image local/system-metal.apfs
```

Run the same command with `ACE5_CPUS=2`, then `4`, then `8`. The loader prints `cores for the kernel, in the order it takes them:`; CPU0 (the USB observer core) should not be in that list. If it is and USB dies, use `cpumask` with the bit for **that position** cleared (bit 0 is the first listed core), not `0xFE`.

```bash
ACE5_CPUS=2 python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" run \
  --seconds 600 --bootcpu 1 --live-log --live-interval 10 \
  --kernel out/giulia-kernel/mach.ace5-public --root-image local/root-daily.apfs

# Escape hatch (positional): read the printed order first, then clear the
# position you want to drop. There is no fixed mask that means "physical CPU0".
# ACE5_CPUMASK=0x... python3 tools/ace5/phone.py ...
```

The script prints `CPU boot arg: ...` before building the payload; the same token must appear in the saved log.

### Acceptance

```bash
grep -iE 'cpu boot arg|psci|gic|redistributor|cpu [0-9]' logs/giulia/<timestamp>-run.log
```

- `PSCI_SMP_CPU_ASKED` / `IN_KERNEL` / `RUNNING` appear for each started core.
- No `panic(cpu ...)`, no `gic: no redistributor for MPIDR ...`.
- USB log and screenshot still arrive; the UI frame time does not regress versus the Phase 0 control.
- `hw.ncpu` may report `logical_cpu_max` (8) even when fewer cores are listed; count `RUNNING` cores in the log instead.

If `gic: no redistributor for MPIDR` appears, the ADT is missing `redistributor-stride = <0x40000>`; fix the loader's ADT generation before trying more cores.

### Measured on 2026-10-25 (CI kernel `ea64dbc`)

| `ACE5_CPUS` | registered | online | notes |
| --- | --- | --- | --- |
| 1 | cpu 0 | cpu 0 | control |
| 2 | cpu 0–1 | cpu 1 | |
| 4 | cpu 0–3 | cpu 1–3 | |
| 8 | cpu 0–6 | cpu 1–6 | the loader lists 7 cores; `cpus=8` uses all of them |

No `gic: no redistributor for MPIDR` in any run, and USB stayed alive (logs and screenshots came back). The only panic was the missing `/sbin/launchd` above, which is the system image not being staged, not an SMP failure.

After a panic the loader stays at `xnuloader`, so the next `enter` fails with `Device <serial> did not appear`. Either run the next `run` directly (the loader is still ready) or `phone.py reboot` to go back to Android first.

### Full boot (system image)

```bash
python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" ramdisk --image local/system-metal.apfs
ACE5_CPUS=8 python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" run \
  --seconds 300 --bootcpu 1 --live-log --live-interval 10 \
  --kernel out/giulia-kernel/mach.ace5-public --root-image local/root-daily.apfs
```

Measured 2026-10-25 with the CI kernel (`ea64dbc`): 7 cores online, SpringBoard home screen, no panic, framebuffer 10 fps (51 frames per 5 s, ~2.5 ms in the driver per frame). Screenshot: `logs/giulia/20261025-164108-screen.png`. Touch is off unless the run also passes `--touch`; Wi-Fi, audio and camera are not ported.

### Touch (S3910)

`--touch` adds `ace5_touch=1` and starts the S3910 driver in the kernel. Measured 2026-10-25: `s3910: ready; max=12639,27799, 10 contacts, polled SPI`, then 849 reports with 0 errors after a finger slide. Log: `logs/giulia/20261025-165751-run.log`.

### GPU (Adreno 750v2)

The kernel reads the GPU firmware from `fw/` in the small root image, so a firmware-bearing root is required:

```bash
# root is needed; /vendor/firmware is not readable by the adb shell user.
MSYS_NO_PATHCONV=1 adb -s "$ACE5_SERIAL" exec-out su -c 'cat /vendor/firmware/gmu_gen70900.bin' > local/firmware/gmu_gen70900.bin
# repeat for gen70900_sqe.fw, gen70900_aqe.fw, gen70900_zap.mbn
mkdir -p local/root-gmu/fw
cp local/firmware/gmu_gen70900.bin local/firmware/gen70900_sqe.fw \
   local/firmware/gen70900_aqe.fw local/firmware/gen70900_zap.mbn local/root-gmu/fw/
python3 tools/apfs/mkapfs.py -z --plain fw -o local/root-gmu.apfs redwood=local/root-gmu
```

```bash
python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" ramdisk --image local/system-metal.apfs
ACE5_CPUS=8 python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" run --seconds 300 \
  --bootcpu 1 --gpu-probe 20 --gpu-direct --live-log --live-interval 10 \
  --kernel out/giulia-kernel/mach.ace5-public --root-image local/root-gmu.apfs
```

Measured 2026-10-25: stages 1–20 pass with the rebuilt kernel (`ae42f3e3…`). Stage 17 writes the direct-ring fences and `scratch=0xa7500001`; stage 18 completes PAS13 ZAP authentication; stage 19 does nonsecure-mode GPU DMA; stage 20 writes `BR=0xa75000b1`, `BV=0xa75000b2`, fence 5 at RPTR 112. Log: `logs/giulia/20261025-171542-run.log`.

Kernel `6f2542d0…` replaces the exact firmware-size gate with structural validation (size range, `%4`, `fw[0]`/`fw[1]` version words, FNV fingerprint), so both this device's 85300-byte SQE and the upstream 85364-byte image are accepted and identified. Cold and back-to-back warm stage-20 runs both pass; the second run logs `waking retained RSC sleep state` (`logs/giulia/20261025-175732-run.log`, `logs/giulia/20261025-175752-run.log`).

The probe tears the GPU down at the end (`probe ended; CX off=1; software
rendering continues`) unless `--gpu-keep` is set: then the owner thread sleeps
in `a750_keep_wait`, the backend stays published (`persistent startup ready=1`)
and the GPU is torn down only when the system stops. Use `--gpu-keep --metal`
with the **full** `root-daily` plus its `fw/` directory; the minimal
`root-gmu.apfs` has no `metal_props` or `display_test`. The loader's return
timer starts when the loader boots, so `enter` + `ramdisk` (~75 s) already
consume part of it: use `--seconds 600` for a keep run, or the timer can expire
right after the ~100 s probe and return to the loader before the UI paints.

**Repacking the root on Windows.** The GPU test needs `fw/` in the small root.
The CI-built `root-daily.apfs` is correct; a repack from the artifact tree on
Windows is not, because the zip loses empty directories and the Unix modes.
Repack with a **tab-separated** manifest (the parser splits on tabs, not
spaces), 0755 for `bin/` and `sbin/`, and the `hal/RedwoodAudio.driver` symlink:

```bash
python3 local/tools/mkroot-manifest.py local/root-daily local/root-daily.manifest
python3 tools/apfs/mkapfs.py -z --plain fw -o local/root-daily-fw.apfs \
  redwood=local/root-daily,manifest=local/root-daily.manifest
```

Without the manifest, `stage0` fails with `cannot run /redwood/sbin/init:
Permission denied` and the kernel halts right after `load_init_program`.

**Live compositor fixed 2026-10-27 (kernel `ab9839f`).** The ghosting/torn
frames came from the CP context reset that ran on *every* submit:
`CP_RESET_CONTEXT_STATE 0xf` clears the BR/BV counters and the resource table
that a7xx binning keeps across a frame. UniXNU's A740 driver runs it only when
the client context changes; this port has one context, so it now runs on the
first submission of a session (`giulia_gpu_live.h`), and a map no longer
invalidates the TLB (it only fills invalid leaves; unmap keeps the invalidate).
With `--gpu-keep --metal` the window server composites correctly: icons,
labels, Dock, status bar and the display scaler are pixel-aligned, with no
torn/duplicated/stale rectangles (`logs/giulia/20261027-115244-screen.png`).
`RedwoodGPUDisplay` scales 1125x2436 → 1264x2780 in 4.6 ms, and the framebuffer
reports 10 fps with ~0.9 ms waiting for the GPU. What remains is content, not
composition: the Weather and Calendar widget slots are pure white (the widget
layer never drew), a separate content-service issue.

If lock timeouts or watchdog messages appear at 4/8 cores, add `timeout_scale = 2` to `platform_giulia` in `xnuloader/loader.c` and rebuild the loader.

### Rollback

Revert `tools/ace5/phone.py`, `common/devices/giulia.conf`, and the optional `timeout_scale`. No storage was written in Phase 1.

---

## Phase 2 — var_store persistence (destructive gate)

**Stop here until Phase 1 is stable and the backup is verified.**

> **Executed 2026-10-26 (destructive step done):** `userdata` is metadata-encrypted,
> but the f2fs superblock is plaintext on the mapper device (`/dev/block/dm-N`),
> not on `/dev/block/by-name/userdata` (= `sda15`, ciphertext).
> `tools/ace5/extract-hw.sh` resolves the mapper device and reads the superblock
> and `segment_info` there. The tail 20 GiB held 4.18 GiB of data (2142 segments,
> the last one at the very end), so the pre-shrink dry run refused as designed.
> The 6.1.75 kernel's `f2fs_resize_fs` then GC'd that data before shrinking:
> `f2fs-resize /data 52157952` took 24 s and moved `block_count` 57,400,861 ->
> 52,157,981. The table was made from the post-shrink state, and Android boots
> with `/data` at 198.97 GiB and every `fsck.f2fs` check OK. Result:
> **userdata** entry 14 ends at 56,078,330; **darwin** entry 15 is
> lba 56,078,336..61,321,210 (20.000 GiB, APFS container type, unique GUID
> `086cbf2b-c897-40e5-a2eb-f08ff566fb75`).

The persistence stack exists but is disabled in the diagnostic/kernel stages:

- `tools/ace5/build-loader.sh` compiles `HACKIOS_STORAGE_WRITES=0` for them.
- `phone.py` requires `storage-writes=disabled` there and a manifest allowing only `logdump`.
- The persist stage (`build-loader.sh persist`) enables `ufs=2 ufs_write=1 var_store=1`.

### 2.1 Carve the `darwin` partition

```bash
# 1. The GPT alone: both copies, userdata last, a free entry after it. No phone needed.
python3 tools/userdata-carve.py local/device 20 darwin --preflight

# 2. The f2fs superblock and segment map, from the mapper device (plaintext only there).
ACE5_SERIAL="$ACE5_SERIAL" tools/ace5/extract-hw.sh local/device
od -An -tx1 -j 1024 -N 4 local/device/userdata-meta-before.bin   # want 10 20 f5 f2
```

If the tail is free, `python3 tools/userdata-carve.py local/device 20 darwin`
prints the target and the dd commands. **On this phone it refuses** ("segments
past the new end hold data"); that is the expected first state. Let the kernel
move the data first, in one Android session with no reboot:

```bash
# 3a. Shrink the f2fs by 20 GiB (the kernel GCs the tail; 24 s measured).
adb push local/tools/f2fs-resize /data/local/tmp/f2fs-resize
adb shell su -c '/data/local/tmp/f2fs-resize /data 52157952'

# 3b. Capture the post-shrink state (mapper device, resolved fresh).
adb shell su -c 'dd if=/dev/block/dm-74 bs=4096 count=1 2>/dev/null' > local/device/sb-after.bin
adb shell su -c 'cat /proc/fs/f2fs/dm-74/segment_info' > local/device/segment_info-after.txt

# 3c. Make the table from the post-shrink state: no further shrink (0 GiB),
#     the free tail becomes darwin.
mkdir -p local/device-after
cp local/device/gpt-head.bin local/device/gpt-tail.bin local/device-after/
cp local/device/sb-after.bin local/device-after/userdata-meta-before.bin
cp local/device/segment_info-after.txt local/device-after/segment_info-before.txt
python3 tools/userdata-carve.py local/device-after 0 darwin
```

Then write the two GPT copies with the printed `dd` commands. Use **absolute
paths**: `adb shell su -c 'cd DIR && dd ...'` is split at `&&` by the device
shell, and the dd would run as the shell user. The three writes are:

```bash
adb push local/device-after/gpt-head.new /data/local/tmp/gpt-head.new
adb push local/device-after/gpt-tail.new /data/local/tmp/gpt-tail.new
adb shell su -c 'dd if=/data/local/tmp/gpt-head.new of=/dev/block/sda bs=4096 skip=2 seek=2 count=1 conv=notrunc,fsync'
adb shell su -c 'dd if=/data/local/tmp/gpt-head.new of=/dev/block/sda bs=4096 skip=1 seek=1 count=1 conv=notrunc,fsync'
adb shell su -c 'dd if=/data/local/tmp/gpt-tail.new of=/dev/block/sda bs=4096 skip=3 seek=61321211 count=5 conv=notrunc,fsync'
```

Read the three ranges back and compare with the new files before rebooting.
After the reboot, `df /data` shows 198.97 GiB, `/dev/block/by-name/darwin` is
`sda16`, and `fsck.f2fs` reports every check OK.

This is the destructive step: it shrinks `userdata` and gives the tail to `darwin`. If `userdata-carve.py` refuses, do not force it; the f2fs may hold data past the new end.

### 2.2 Fill the constants

```bash
python3 tools/ace5/storage-constants.py local/device
```

Copy the printed values into:

- `xnuloader/loader.c`: `BLOCK_DARWIN_FIRST`, `BLOCK_DARWIN_LAST`, `BLOCK_DARWIN_UNIQ`
- `xnu/files/bsd/dev/redwood_ufs.c`: `UFS_DARWIN_FIRST`, `UFS_DARWIN_BLOCKS`

`tools/ace5/apply-storage-constants.py local/device` prints the five replacements; add `--write` to edit both files in place. Run it without `--write` first and check the output.

Keep `BLOCK_WRITE_FIRST` / `UFS_WRITE_FIRST` at `2042368` for var_store-only: the store lives at 8 GiB (`STORE_AT 8589934592`), above that floor. Re-derive the floor only for a full root layout.

Verify the UFS base in `local/device/giulia.dtb`; `xnuloader/loader.c`'s `HC_PA` and `xnu/files/bsd/dev/redwood_ufshc.c`'s `UFSHC_PHYS` must equal the DT's `ufshc@...`. Both are currently `0x1d84000`.

The type GUID stays the fixed APFS-container GUID; only `BLOCK_DARWIN_UNIQ` comes from the carve.

### 2.3 Build the persistence loader

The CI workflow already builds this image and ships it in the `ace5-kernel`
artifact (`out/giulia-persist/hackios-giulia-persist.img`). Rebuild locally only
after editing the loader or the constants:

```bash
tools/ace5/build-loader.sh persist
python3 tools/ace5/pack-boot.py --mu <Mu-Silicium> --dtb local/device/giulia.dtb \
  --stage persist --output out/giulia-persist/hackios-giulia-persist.img
```

`build-loader.sh persist` sets `HACKIOS_STORAGE_WRITES=1`; `pack-boot.py --stage persist` expects `storage_writes=true` and writes `allowed_partition_write: ["logdump", "darwin"]`.

### 2.4 Boot and verify

```bash
python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" enter --stage persist
python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" ramdisk --persist \
  --image local/system-wallpaper.apfs
python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" run --persist \
  --seconds 600 --bootcpu 1 --live-log --live-interval 10 \
  --kernel out/giulia-kernel/mach.ace5-public --root-image local/root-daily.apfs
```

`--persist` adds `ufs=2 ufs_write=1 var_store=1` to the command line. The root still comes from RAM; only `/private/var` is persisted.

In the run log or over the shell:

```bash
ls -l /dev/rufs_darwin
var_store show
var_store stamp /private/var/redwood-stamp
var_store save
# reboot / re-enter the loader, then:
var_store load
cat /private/var/redwood-stamp
```

Acceptance:

- `/dev/rufs_darwin` exists and `var_store show` reports both slots.
- A stamped file survives a loader return and a new run.
- Android still boots after `phone.py reboot`.
- No write outside `darwin`: the loader's `block_is_darwin()` and the kernel's name/LBA checks must both pass; if either is edited, re-check the other.

**Measured 2026-10-27** (persist loader `a23a8d09…`):

- Boot 1, empty store: `var_store: no store yet: /private/var stays as init made it`.
  On return: `saving into slot 0 (slot -1 holds generation 0)`, `slot 0: generation 1,
  11004 entries, 38181328 bytes, crc 7e36e5ec`, `8298 blocks into the flash, 1024 left
  alone`; the kernel counted `11 writes into darwin, 33996800 bytes, read back the
  same; 0 refused`; the saver finished in 1,680 ms.
- Boot 2, fresh loader after `reboot` and a re-staged system image:
  `var_store: slot 0: generation 1 of … seconds, 11004 entries, 38181328 bytes`, then
  `4950 directories, 903 files (36526240 bytes), 2 links back in /private/var`.
  On return: `saving into slot 1 (slot 0 holds generation 1)`, `slot 1: generation 2,
  10993 entries, 38362760 bytes, crc 9cdd6eff`, `86 entries and 23541530 bytes of it
  were written since the last save (306 seconds ago)`, `8342 blocks into the flash`.
- Android still boots afterwards: `/data` 199G, `by-name/darwin` → `sda16`.

Two operational notes from the run:

- A second `run` in the **same** loader session fails at `core 100: the firmware
  answers 0 to the request to start it`; reboot, re-enter the persist loader and
  re-stage the system image between runs (the RAM root does not survive the reboot).
- `phone.py` prints `not enough values to unpack (expected 3, got 0)` after a clean
  return (the screenshot fetch); the run and the save are unaffected.

### Rollback

- Restore the boot chain from `local/backups/ace5-before-first-boot` with the documented fastboot/MiFlash procedure.
- Restore the original GPT from `gpt-head.bin` / `gpt-tail.bin` only if you also restore the f2fs size; a GPT restore alone leaves userdata smaller than its old filesystem. The safest path is the full userdata backup or the official fastboot package.
- The persistence loader can be replaced by the diagnostic/kernel loader at any time; storage writes stop as soon as the write-disabled loader is entered.

---

## Troubleshooting

| Symptom | Check |
|---|---|
| `ACE5_CPUS must be an integer 1..8` | `phone.py` rejects out-of-range values before boot. |
| `panic` mentioning both `cpus` and `cpumask` | Only one selector may be present; unset `ACE5_CPUS` when using `ACE5_CPUMASK`. |
| `gic: no redistributor for MPIDR` | ADT missing `redistributor-stride = <0x40000>`; inspect the DTB. |
| USB dies when the second core starts | Check `cores for the kernel`: the loader should omit its own core (CPU0). If CPU0 is listed, clear **that position** with `cpumask` (bit 0 = first listed core), or move USB off CPU0 (not in this runbook). |
| `storage-writes=disabled` on `--persist` | You entered the kernel/diagnostic loader; rebuild and enter `--stage persist`. |
| `allowed_partition_write` mismatch | The image manifest was built by the old `pack-boot.py`; rebuild with `--stage persist`. |
| `var_store` writes fail | Check `UFS_WRITE_FIRST`, the darwin name/GUID/LBAs in both loader and kernel, and that the carve completed. |

## Deferred

- **Full root-on-userdata**: `rd=ufsroot0` does not exist in this tree; `root_apfs=` mounts read-only; APFS has no allocator/transaction. Requires a writable filesystem, months of work.
- **iOS 18**: Bergamota's Renegade (PAC strip) + xnu-11215/11417 + Chlorophyll. A separate branch, not this runbook.
