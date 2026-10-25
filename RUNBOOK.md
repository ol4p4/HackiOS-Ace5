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

If lock timeouts or watchdog messages appear at 4/8 cores, add `timeout_scale = 2` to `platform_giulia` in `xnuloader/loader.c` and rebuild the loader.

### Rollback

Revert `tools/ace5/phone.py`, `common/devices/giulia.conf`, and the optional `timeout_scale`. No storage was written in Phase 1.

---

## Phase 2 — var_store persistence (destructive gate)

**Stop here until Phase 1 is stable and the backup is verified.**

> **Blocker (2026-10-25, PKG110):** `userdata` is mounted from `/dev/block/dm-74` and metadata-encrypted (`ro.crypto.metadata.enabled=true`); the raw f2fs superblock at `/dev/block/sda15` is unreadable, so `tools/userdata-carve.py` refuses with "userdata does not begin with a plain f2fs of 4 KB blocks". The device has 219 GiB userdata with 131 GiB used; the only other free partitions are `hybridswap` 1 GiB, `rawdump` 256 MiB and `splash_odm` 32 MiB. Do not run the carve as written. A dm-aware variant (read the superblock from dm-74, shrink `/data`, rewrite the GPT) or the 1 GiB `hybridswap` fallback is required. Phase 1 does not need either.

The persistence stack exists but is disabled today:

- `tools/ace5/build-loader.sh` compiled `HACKIOS_STORAGE_WRITES=0`.
- `phone.py` required `storage-writes=disabled` and a manifest allowing only `logdump`.
- The run command line had no `ufs=2 ufs_write=1 var_store=1`.

This branch enables it as a separate `persist` stage; the diagnostic and kernel loaders stay write-disabled.

### 2.1 Carve the `darwin` partition

```bash
# Reads the saved GPT and f2fs superblock; writes gpt-head.new / gpt-tail.new only.
python3 tools/userdata-carve.py local/device 20 darwin
```

Follow the printed instructions **in one Android session, no reboot between steps**:

1. Run `f2fs-resize` on `/data` with the printed block count.
2. Save the new first block as `local/device/sb-after.bin`.
3. Re-run `userdata-carve.py` to verify the shrunk size and produce the final table.
4. Write the two GPT copies with the printed `dd` commands.

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
