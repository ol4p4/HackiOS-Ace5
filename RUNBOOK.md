# Ace 5 (giulia / PKG110) runbook: SMP + var_store persistence

This runbook is for the OnePlus Ace 5 port (`YiKongk/HackiOS-Ace5`). It covers:

- **Phase 0** — backup and hardware extraction (read-only on the phone).
- **Phase 1** — SMP: `cpus=` / `cpumask=` boot arguments, 1 → 2 → 4 → 8 cores.
- **Phase 2** — `var_store` persistence: carve a `darwin` partition, enable UFS writes, persist `/private/var`. **Executed 2026-10-26; the `userdata` metadata-encryption concern was resolved by reading the plaintext f2fs superblock on the mapper device — see the Phase 2 section.**

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

**Compositor state 2026-10-27 (kernel `ab9839f`).** The per-submit CP context
reset was a real defect and is fixed: `CP_RESET_CONTEXT_STATE 0xf` clears the
BR/BV counters and the resource table a7xx binning keeps across a frame, and
UniXNU's A740 driver runs it only on a client context change. This port has one
context, so it now runs on the first submission of a session; a map no longer
invalidates the TLB (it fills invalid leaves; unmap keeps the invalidate).

With `--gpu-keep --metal` the **home screen** composites correctly: icons,
labels, Dock, status bar and the scaler are pixel-aligned, no
torn/duplicated/stale rectangles (`logs/giulia/20261027-115244-screen.png`);
`RedwoodGPUDisplay` scales 1125x2436 → 1264x2780 in 4.6 ms and the framebuffer
reports 10 fps with ~0.9 ms waiting for the GPU.

**Control Center is still corrupted, and that is the open defect.** With
`--metal`: two large tile-quantized black masses (upper-right and lower-right),
a stray purple triangle over Bluetooth, clipped sliders, and the Now Playing
module and status-bar clock missing
(`logs/giulia/20261027-121100-screen.png`). Without `--metal` (GL/Mesa): a
full-height center stripe of black/white/noise and a tile-quantized colour-noise
block at the bottom (`logs/giulia/20261027-122049-screen.png`). Both paths miss
the status-bar clock and Now Playing content. The corruption is confined to
large/blurred/multi-pass surfaces — the home screen's opaque icons are fine — so
the next step is the a7xx binning/GMEM tile path in the userspace driver, not
scanout or the submit ring. The kernel's GET_PARAM mapping matches msm_drm.h
(GMEM_SIZE 3 MiB, CHIP_ID 0x443051401, GMEM_BASE 0x1000000, HIGHEST_BANK_BIT
16, UBWC_SWIZZLE 6, MACROTILE_MODE 1); a full-screen triangle/quad test and a
comparison of the Metal frontend's tile stores with UniXNU AdrenoMetal are the
next probes. The Weather/Calendar widget slots are also still pure white (a
content-service issue, separate from the Control Center corruption).

**Measured 2026-10-27 13:20, and the tiling theory is wrong.** The same Metal
boot was repeated with `FD_MESA_DEBUG=notile` in SpringBoard's and backboardd's
job environment (`local/root-daily/LaunchDaemons/*.plist`, repacked with
`local/tools/mkroot-manifest.py` + `tools/apfs/mkapfs.py`). Two things changed:
the purple wedge over the Bluetooth toggle disappeared (13 852 px, the button
then looked right), and the bottom black panel turned into grey/white/yellow
horizontal bands. Everything else was **pixel-identical** — the diagonal black
wedge (boundary within 0.85 px), the axis-aligned black rectangle below it, the
brightness slider's hard-cut top edge, the missing clock and the battery's 4x25
px red bar. So tiling of Mesa's internal buffers accounts for the purple wedge
only, not for the 49.9 % of the frame that is black.

What the pixels say about the wedge: its edge runs from (93, 200) to (1040,
2239) — a smooth ~25° line (slope dx/dy 0.475, 1-2 px steps, not tile-sized
steps), and the bottom black rectangle starts at x 586, y 2255 with exact
horizontal and vertical edges. A straight diagonal plus axis-aligned edges is
the outline of **one quadrilateral drawn with a wrong vertex**, not of a surface
read with a wrong layout: the blur/backdrop is a full-screen quad, and one
corner of it lands somewhere else. `notile` leaves vertex buffers untouched,
which is why the shape did not move. The GL path's 266 px vertical band is the
same class of fault with a different producer.

Two facts that point the search at vertex data rather than at raster state: the
defect is byte-stable across runs and across two builds, and simple geometry is
fine (`gl_test`, `display_test` 9/9), so it is a specific buffer or address that
is wrong, not a general rasterisation fault. The kernel's live map refuses a
leaf that is already valid (`a750_live_map`), so a VA reused before its unmap
would silently keep the old page — worth checking before anything in the
userspace driver. `glskip=N` / `glshow=WxH` (boot-args read by `eagl.c`) are the
cheapest way to name the program that paints the black region.

**Measured 2026-10-27 14:26: the kept-buffer path is not the cause.** The same
Metal boot with `gpu_keep=0` (every buffer made afresh, none reused) is
pixel-identical to `gpu_keep=1`: 51.4 % of the frame black against 51.3 %, and
0.49 % of pixels differ, which is the noise between two independent frames of the
same UI. The `fKept` reuse in `iokit/Kernel/RedwoodGPU.cpp` (a buffer of the
program's own given back and given again uncleared) is therefore exonerated, and
so is everything that depends on it.

**The trace probes work, but only where `eagl` draws.** Two traps cost a run
each:

- `gltrace_at=SECONDS` counts from the moment `eagl`'s constructor runs in that
  process, and the compositor stops drawing when nothing has changed: the last
  frame of a 480 s run is 820, and a `gltrace_at` later than that never fires.
  Use `gltrace_draws=N` instead — it fires on the frame after one with exactly
  N draws, and Control Center's settled frames have a stable count (199 without
  `--metal`, 205 with it) while the home screen has 10.
- With `--metal` the big black wedge is **SpringBoard's**, drawn by RedwoodMetal,
  which does not go through `eagl` at all; `eagl` sees only backboardd's GL
  compositing. Trace the GL path (no `--metal`) or instrument `redwoodmetal.m`.

**One traced GL frame, 2026-10-27 14:53** (`gltrace_draws=199 gltrace_frames=1
gltrace_lit=1`, frame 752, 199 draws, 19 clears, log
`logs/giulia/20261027-145323-run.log`): the display surface (framebuffer 6,
1125x2436) reaches **98.5 % non-black** after draw #83 (program 37, the
full-screen backdrop) and then falls to **85.9 %** — 346 891 pixels are turned
black *after* the backdrop is in place. Every large drop is **program 62**:

| draw | program | texture sampled | pixels blackened |
| --- | --- | --- | --- |
| 124 | 62 | 391x392 | 8 823 |
| 130 | 62 | 455x456 | 108 924 |
| 152 | 62 | 285x613 | 37 734 |
| 173 | 62 | 1131x2442 (a snapshot of the screen) | 199 098 |
| 200 | 7 | 205x456 | 42 714 |

So program 62 is the backdrop/vibrancy pass: it draws a full-screen quad into
the display, sampling one of the small offscreen blur targets (and once a
1131x2442 snapshot), and it writes black where the thing it samples is black or
where its own geometry/texcoords do not cover the target. The blur chain that
fills those targets is the next thing to read (framebuffers 94, 93, 60, 98, 41,
90, 26, 99 — cleared and drawn within the same frame, all under the scissor
`(0, 96, 1125, 1920)`).

**Fixed 2026-10-27, `userland/src/redwoodmetal.m`: the clear was made once per
draw, not once per pass.** `_clears` (and `_clearsDepth`, `_clearsStencil`) was
set from the pass descriptor and never reset, and `recordDraw:` copies it into
every work item, so `rwa_render_draw` ran `pipe->clear()` before *each* draw. A
pass whose load action is Clear therefore wiped its attachments again before
every draw and only the **last** draw of a multi-draw pass survived — everything
the earlier ones put down was gone, which reads exactly like those draws never
having been made, in exactly the multi-draw passes (a blur chain, a card made of
several quads) where the picture first goes wrong. The load action now belongs to
the first draw of the pass and to nothing after it.

**Measured 2026-10-27 19:55: that improvement did NOT reproduce, and the
attribution above is withdrawn.** The same root, same `rc.jobs`, same `--metal`,
240 s: **51.4 % black, mean 41.6 — the same numbers as the 14:26 baseline to the
first decimal** (`logs/giulia/20261027-195509-screen.png`). Fifty-four minutes
earlier the same build had given **4.0 % black, mean 83.8**
(`20261027-190136-screen.png`). So the picture is **not deterministic across
boots** — three mutually identical runs earlier in the day made it look as if it
were — and the 21.5 % frame was a better-condition sample of the same
distribution, not a fix taking effect. The per-pass clear is still a real bug and
its fix stays in, but nothing measured so far shows it changing the picture.

**And the Metal frontend is not drawing the UI at all.** With an unconditional
"the first draw of this process" line in `recordDraw:`, written through
`/dev/console` opened by name, switched on by a file in the read-only root *and*
by the job's environment — it never appears, in any run. The UI is drawn by
`eagl`/Mesa in backboardd (its `redwood-gl: frame` lines are the only renderer
that speaks), and both the 4.0 % and the 51.4 % frames come from that one path.
RedwoodMetal is present and can be exercised by the `metal_*` tools, but
CoreAnimation is not using it on this device — most likely because `metal_props
on` runs ~60 s into the session (`rc.jobs` puts `rwtouch wait 60` first) while
SpringBoard's CoreAnimation asks for its device in the first seconds. That is the
next thing to test: put `metal_props on` first in `rc.jobs` and see whether the
Metal frontend then draws (its first-draw line would say so) and whether the
picture changes.

**Staged 2026-10-27 20:31, awaiting a device session.** The reorder is in:
`local/root-daily/etc/rc.jobs` now runs `metal_props on` first (the `wait 60`
moved behind it), and `local/root-daily-fw.apfs` is repacked from that tree
(manifest `local/root-daily.manifest`, fw plaintext, switches and SpringBoard
plist verified inside the image). The timing that makes this the right test, read
off `20261027-195508-run.log`: the GPU probe holds the first ~108 s of kernel
time, the GPU backend publishes (`persistent startup ready=1`, kernel 108.381 s),
and only then does launchd spawn `com.redwood.demo` (wall 11:51:07.587) — so
`rc.jobs`'s first line now runs right after the service exists and ~2 s before
SpringBoard's window server asks for a device. The two outcomes:

- `metal_props: no IOAcceleratorES service in the registry` — the launchd job can
  start before the kernel publishes the service; add a bounded wait or a retry to
  `metal_props` and re-run.
- The properties are set near kernel t≈109 s: the first-draw line either appears
  (timing hypothesis confirmed; its `RW_METAL_TRACE=` field then says whether the
  environment channel works) or still does not, and the not-drawing question
  moves to CoreAnimation's own path.

One 300 s `--metal` run decides it:

```bash
export ACE5_SERIAL=b4cf8812   # oplusboot.serialno
python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" enter --stage kernel
python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" ramdisk --image local/system-metal.apfs
ACE5_CPUS=8 python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" run --seconds 300 \
  --bootcpu 1 --live-log --live-interval 10 --gpu-keep --metal --gpu-probe 20 \
  --gpu-direct --kernel out/giulia-kernel/mach.ace5-public \
  --root-image local/root-daily-fw.apfs
```

Read it with `grep -a "metal_props\|the first draw of this process\|metal draw"`.
`local/root-daily.apfs` is the stale CI image (no `fw/`, no swipe step, no
switches file); the `--gpu-keep --metal` runs must keep using `root-daily-fw.apfs`.

**Measured 2026-10-28 13:03 (`20261028-130831-run.log`): the timing hypothesis
is confirmed and superseded in one run.** The properties were set at kernel
121.3 s — before backboardd's CoreAnimation asked — and the service was found
(no registry race). What followed was not the first-draw line but a new state:

- **SpringBoard took the Metal path.** `redwoodmetal[SpringBoard 7]: blend: …`
  lines prove it created real Metal pipelines through RedwoodMetal (labels
  `SiriBlitNearestAdditive`, `SiriSphereMask`, `SiriSphereComposite`, ~27 blend
  trace lines per instance). backboardd never created a Metal device — on iOS
  16.5 it is **SpringBoard, the client, that opens Metal**, and the property
  timing is the renderer-selection switch: set before SpringBoard's device ask
  → Metal; after (or never) → backboardd's GL path.
- **Every SpringBoard instance then died at MPS init**: `[MPSImageGaussianBlur
  initWithDevice:] Failed to get MPSDevice.` (`MPSKernel.mm:95` failed
  assertion → SIGABRT), 34 times — pids 7, 35, 37, … 73 crash-looping until the
  kernel returned. The screen was 100 % black; backboardd's GL renderer drew
  only its 3 boot frames and stopped once no client lived.
- **The probe never spoke because SpringBoard died before its first draw** —
  the first-draw line sits in `recordDraw:`, and no draw was ever recorded. The
  `redwood_metal_say` channel itself is fine: the blend lines reach the log from
  inside sandboxed SpringBoard.

So the Metal frontend's first on-device blocker is now precise: **MPSDevice
creation fails on RedwoodMetal** (`MPSImageGaussianBlur` is in SpringBoard's
startup path — the wallpaper blur). Next step: an MPS probe in the metal suite
(create `MPSImageGaussianBlur` with our device, log every selector MPS asks and
the answer) and bisect the capability query it fails on; `metal_caps`' sweep is
the checklist of what we answer today.

**Measured 2026-10-28 13:45 (`20261028-134527-run.log`): the blocker was the
feature profile, and one boot-arg clears it.** The driver comment predicted it
exactly — profile 0 claims Apple1 alone, and **Apple2 is the lowest rung MPS
asks about**. With `ACE5_CMDLINE="metal_profile=1"` (the existing
`metal_profile=` knob in `-featureProfile`, no rebuild): 0 MPS aborts (was 34),
0 SIGABRTs, and **the first-draw line finally appeared** — `metal: the first
draw of this process: into 465x465 x2, 4 indices, 0 textures`. SpringBoard made
5 Metal draws in total, one small surface, no scissors. chronod/RenderBox
opened its own device and built real pipelines (label `plane_color`, two colour
attachments), and the "2 colour attachments ask for a clear and this driver has
one clear colour" line fired there — **Bergamota's per-attachment clear colours
(fix 1) is now a live requirement, not a checklist item**. backboardd opened a
Metal client too but stopped before pipelines. Two loose ends from the same
run: the first-draw line's `RW_METAL_TRACE=` field read `(unset)` — the plist
EnvironmentVariables do not reach `getenv`, though the `etc/metal.switches`
file channel works (4 windowed trace lines) — and the `surfaceBuffer`
client-memory refusal never fired (0 lines), so the black regions are not the
Control Center platters.

The picture (`134527-screen.png`, 45.2 % black): the GL-path speckle noise and
RGB blocks are **gone** — the client layers that produced them are now
Metal-rendered and correct — but a stepped black wedge remains on the right.

**And the attribution run (`135809`, GL `rc.jobs` order, same
`metal_profile=1`, 25.7 % black) pins that wedge on the compositor, not on
Metal.** The wedge's stepped geometry is the same in both screenshots, so it is
the `--metal`-boot compositor black mass at yet another sample of its
non-deterministic size (4.0–51.4 % across boots), while the speckle and RGB
garbage returns exactly when SpringBoard is back on GL sampling. Two recipes:

- Metal experiment boot: `metal_props on` **first** in `rc.jobs` plus
  `ACE5_CMDLINE="metal_profile=1"`.
- Daily GL boot: `rwtouch wait 60` first (the state `rc.jobs` is left in).

Next steps, cheapest first: per-attachment clear colours in `redwoodmetal.m`
(userland — chronod's pipeline already hits the defect), then the GL
compositor's uncovered blur-target corners (program 62), which remain the
product-path bug. Userland changes ride `build-ace5.yml`; kernel changes (the
writable alias for client-memory surfaces, if a platter ever refuses) ride the
kernel workflow.

**2026-10-28 14:20, three things read off the same `134527` log.**

- **The clock widget path works; the status-bar clock was never chronod's.**
  RenderBox (in chronod) failed to open its *precompiled* pipeline cache
  (`default.pipelinelib/pipelines.Adreno750(chip0x43051401,Redwood)` — the
  system image ships caches keyed by Apple GPU names only) and Metal then
  compiled the pipelines from the framework's `default.metallib` at runtime —
  the `plane_color` blend lines — and **drew**: the first-draw line and all 5
  Metal draws of that boot are chronod's, into a 465x465 ×2 surface (a widget
  platter). The missing status-bar clock is SpringBoard's own status-bar
  rendering, a separate item.
- **The two-attachment clear defect is fixed in-tree** (`ba66189`): the
  `rwa_draw` contract gains `clear_colors` + `clear_color_mask` (appended, so
  every offset the driver and library already read stays), `redwoodmetal.m`
  collects each attachment's own colour, and `air_metal.c` issues one Gallium
  clear per attachment through `PIPE_CLEAR_COLOR0 << i`, falling back to the
  single-colour shape when the driver predates the field. The "one clear colour
  for the whole pass" complaint is retired. Userland + Mesa side: it builds in
  the `build-ace5.yml` image; the next CI run carries it, and the first Metal
  boot after that should show the clock widget's second plane intact.
- **RenderBox's pipeline-cache miss is worth naming**: if a pipelinelib under
  our GPU name ever becomes needed (Metal does fall back to compiling from the
  metallib, as this boot proves), the name to build it under is exactly what
  `targetDeviceInfo` answers: `pipelines.Adreno750(chip0x43051401,Redwood)`.

### Control Center's rendering, decomposed 2026-10-28 14:33

Asked what to do about Control Center, the evidence now splits it three ways:

1. **The client layers' garbage (blue speckle columns, RGB blocks) is a
   GL-path read of never-written memory, and it is boot-to-boot random.**
   `glskip=62` was run to test the old program-62 attribution and **program 62
   never drew** (0 skip lines) — yet the frame came out nearly clean
   (`143338-screen.png`: Control Center composited correctly, only the narrow
   top-centre black stripe and one red/cyan blob at the home indicator left).
   The same build has given 4.0 %, 21.5 % and 51.4 % black on other boots. So
   the garbage is not one program: it is whatever memory CoreAnimation's blur
   chain happened to read before writing it. With SpringBoard on Metal
   (`metal_profile=1` + properties early) those client layers render
   correctly; what stays is the compositor wedge, whose geometry is boot-lottery
   too (same wedge in the GL and Metal runs of 13:45/13:58).
2. **eagl's own variable-blur chain is dormant on this CoreAnimation, and that
   is now shown rather than suspected.** The clean run's log has **zero**
   "blur whose radius varies" lines — neither the level builds (`vblur_level`)
   nor the mask reads (`vblur_draw_begin`) fire — and, decisively, **no
   "CoreAnimation gives its function 38/39 numbers" line has appeared in any
   logged run** (today's or yesterday's), while functions 66, 61, 26 and 18 do
   log and the vibrant path (which reads the same function id from the same
   hard-coded CA context offset) creates its programs. So the probe is fine;
   this CA build never invokes the `vrd`/`vrb` path eagl intercepts — the blur
   is CoreAnimation's own programs sampling CA's own downsample targets, and
   eagl's protection (unmade levels bound to the picture) can never apply. The
   `libredwood-gl.dylib` on the phone is byte-identical across all artifacts
   (`1312ecaa…`), so this is not a regression: the chain has never engaged on
   this build. Fixing the coverage therefore cannot ride the vblur path; it has
   to come from the zero-fill below or from making CA's own chain write what it
   reads.
3. **The fix that matches the platform, committed as `d6484e9`.** An iPhone's
   fresh pages read zero; ours read the heap's last user. `my_glTexImage2D` now
   hands the driver a zero buffer for every empty RGBA/BGRA upload
   (half-floats too), which turns the random speckle into defined black,
   removes the boot-to-boot variance from every future measurement, and makes
   the remaining defect — the uncovered region itself — the only variable
   left. `glzero=0` boots the old behaviour for a side-by-side. This rides the
   next `build-ace5.yml` image.

After that build, the checks in order: one boot with `glzero=1` and one with
`glzero=0` (the difference *is* the uninitialized-memory hypothesis, shown
rather than argued); then the compositor wedge itself, which is the same defect
on both paths and should now be deterministic enough to bisect with
`gltrace_place` over the wedge and `gltrace_look=1`. The pending userland
changes now number three and all wait on one CI build: `d6484e9` (zero fill),
`ba66189` (per-attachment clears), and the runbook-only material in between.

**Measured 2026-10-28 15:20–16:00: the zero-fill build is on the phone and the
hypothesis it was built for is REFUTED.** CI built `a404ab9c` and its artifact
(`local/ci-artifact-new`) carries both fixes — the running library proves it
(`libredwood-gl.dylib` 541960 bytes in the boot log's `/redwood` listing, the
zero-fill string present, the old "one clear colour" complaint gone from
`libredwoodmetal.dylib`). Result: the blue speckle noise on Control Center is
**unchanged** — 82 912 pure-blue pixels this boot against 25 241 before the
fix — and, decisively, the blue-pixel masks of three GL boots overlap by only
**0.9–1.8 %** of their union, so the dots are per-boot heap content and just
differently shaped each time. Two consequences: the fix stays in (it is still
correct platform behaviour and it makes `TexImage2D`-specified textures
deterministic), and **the garbage lives in memory the library never sees
through `glTexImage2D`** — the candidates left are Mesa/freedreno's BO cache
reusing a render target that is never fully written (the original uncovered-
target defect), or the IOSurface partial refill (`a copy of wrapped memory`
textures appear in the trace). MiniIOSurface is exonerated by its own source:
kept memory is bzeroed before reuse and `surf_clear=0` is the only way to get
old content back.

Trace recipe corrections, learned the hard way in the same hour:

- `gltrace_at=SECONDS` did **not** fire (two boots, zero lines); `gltrace_draws=N`
  compares for **exact equality** against a cumulative count that moves in
  90-205-draw steps, so an arbitrary N is a lottery and usually misses.
  `gltrace=FIRST-LAST` (absolute frame numbers) is the one that works: 160
  `gl-at:` lines at frames 70–71 with `gltrace_place=200,0,600,2780`.
- The frame counter of a 300 s session reaches ~800 (the frame line prints only
  every 20th after the first 40), so "the settled Control Center" is around
  frame 600+, not 79 — the earlier "79 frames" reading was a grep artefact.
- When counting trace lines, `grep '^gl:'` — `redwood-gl:` contains `gl:` too.
- A settled-frame trace with the same place rectangle produced **zero** `gl-at:`
  lines at frames 600–601 while the boot frames produced 160, so the place
  test is not yet understood for the settled picture (different drawing
  coordinates or framebuffer); the next trace should use the whole screen
  (`gltrace_place=-4000,-4000,8000,8000`, corners 4) and filter the log by the
  blue bands' coordinates afterwards, as the 15:03 run did.

CI and push notes for the next session: **git push over HTTPS to github.com:443
does not connect from this host** (api.github.com works). The five files were
pushed with the Git Data API (blob → tree → commit → `PATCH /git/refs/heads/main`)
as `a404ab9c`, and the workflow is `workflow_dispatch`, so it must be started
with `gh workflow run build-ace5.yml -R ol4p4/HackiOS-Ace5 --ref main`. The
remote main's history has different SHAs from this checkout's (an earlier
rewrite), so a future `git push` would be refused even with the network fixed —
either fetch/compare first or keep using the API path.

**The blue band's painter, found 2026-10-28 16:34 (`20261028-163440-run.log`,
`gltrace=26-45`, whole-screen place, 3328 `gl-at:` lines).** Frames 26–45 are
the Control Center's opening (the picture is the 1125×2436 iPhone-X canvas), and
the top-centre band is drawn by the same family of draws on every one of them:

- `program 15`, rect (204,682)–(398,744): a 194×62 label;
- `program 17`, rect (90,234)–(510,654) and `program 19`, rect (68,211)–(533,676),
  48 triangles: **the band itself**, all three sampling texture **21 (194 × 62,
  0x1906 = RGB565)** as `[0]` and texture 13 (32 × 32, BGRA) as `[1]`, with the
  texture coordinates at the first corner printed as **-0.77 and -1** — outside
  the texture, so clamp-to-edge smears the edge texels across the whole quad.
  The right-hand band has the same shape with texture 24 (204 × 62).
- Neither texture 21 nor 24 is refilled from an IOSurface anywhere in the traced
  frames, and nothing draws into them there — their content comes from before
  frame 26. Whatever their edges hold (garbage from the heap, per the mask
  evidence above) is stretched over the band; if the edges are clean the band
  is clean, which is exactly the boot lottery.

Next probes, in order: `gltrace=15-26` with the same whole-screen place to catch
who fills texture 21 (a `TexSubImage2D`, an earlier surface refill, or a draw
into it) and what its edge texels hold; then `glshow=194x62` once the gate that
kept it silent is understood, to dump the texture itself (`show_texture()` is
called only for an image that is both `.target` and `.drawn` — texture 21 is
neither, which is why the documented recipe never produced a line).

**"Who fills it" is answered (2026-10-28 17:06, `20261028-170628-run.log`,
whole-frame trace of frames 20–27): CoreAnimation uploads it with real data.**
The call sequence is `glBindTexture(24)` → `glPixelStorei(UNPACK_ALIGNMENT, 1)`
→ `glPixelStorei(UNPACK_ROW_LENGTH, 224)` →
`glTexImage2D(GL_TEXTURE_2D, 0, 0x1906 /* RGB */, 194, 62, 0, 0x1906, 0x1401 /* UBYTE */, 0x1164fc000)` → `glUseProgram(15)` → `glDrawElements` into
the picture, sampling that texture with `wrap 0x812f/0x812f` (clamp) and
`mag 0x2601` (linear). Two consequences:

- The band's texture is a **194×62 RGB crop of a 224-pixel-wide source buffer,
  uploaded from CPU memory with a non-NULL pointer** — so it is not an empty
  allocation and the zero-fill never applies to it. What the band shows is the
  *content of that source buffer*, stretched over the tile.
- Texture ids drift between boots (the same 194×62 shape was id 21 in the
  16:34 trace and id 24 here) — pair observations by size, program and rect,
  never by id.

So the garbage is in a CPU-side buffer that CoreAnimation uploads from — most
likely a copy of a partially-written surface (the 224-pixel stride says it is a
crop of a wider image), and per-boot because that surface's unwritten part holds
the heap's leftovers. The next diagnostic needs a build: `my_glTexImage2D` can
tell when an upload's source pointer range overlaps one of its own wrapped
surfaces (it knows them), which would name the surface and offset the band is
copied from; then the fix is at that surface's producer (write it fully before
the copy) rather than in the upload path.

**That diagnostic is in and returns nothing (2026-10-28 18:24,
`20261028-182414-run.log`).** `my_glTexImage2D` now prints, for any upload
whose pixels land inside the memory of a surface this context knows, the
surface id, its size and the offset. A full session: **0 lines** — so the band's
194×62 crop is not a copy from any surface eagl wraps; the source is
CoreAnimation's own memory, one layer further upstream than the compositor, and
nothing in this driver can name it. The same boot also measured the two defect
variants apart from each other: **51.4 % black, 976 blue pixels** — the
wedge-only frame (zero-fill turns the unfilled regions black, the right-hand
stairs are unchanged) against the speckle frames' 25 000–83 000 blue pixels on
identical code. Two independent defects remain, then: the compositor wedge
(51.4 % here, geometry boot-stable) and the occasional speckle (CA's own
uploaded crop, boot-lottery content).

Where that leaves the Control Center work: the GL path cannot clean CA's own
upload; the options are (a) the wedge, which is ours and is now deterministic
enough to bisect with the trace recipe above, or (b) move the client layers to
Metal, which already renders them correctly (`metal_profile=1`), leaving the
wedge as the single remaining defect on that path too. The `glzero=0` A/B that
was staged has effectively been run by the boot lottery itself: with the fix,
unwritten GL-allocated memory is black; the speckle survived, and that is what
proved the source is not a GL allocation at all.

**Mid-session screenshots are possible, and one boot's post-return frame was
byte-identical to its mid-run frame (2026-10-28 18:36/18:37).** `phone.py
screenshot` answers from the loader while the kernel runs (the same USB channel
the live log uses), so a frame can be taken before the return timer fires:
`20261028-183647-screen.png` at t≈245 s against `20261028-183744-screen.png`
after the return — the same picture, the same coloured blocks, no wedge. Two
things follow: the teardown repaint does **not** alter the last presented frame
(at least in this variant), so post-return screenshots are representative and
the previous measurements stand; and this boot is a third variant — **fully
composited Control Center (cards, sliders, Focus tile all correct) with large
red/green/blue garbage blocks** where the blur content is garbage, no black
wedge anywhere. Variants seen so far on the same build, all mid/presented
frames: clean, coloured noise, black wedge. The wedge and the noise are the
same class (garbage or zeros where a blurred region should be), differing only
in what the unfilled memory held and which of CA's uploads/copies carried it.

**rc.jobs must be LF, and one Windows write of it broke `metal_props` silently
(2026-10-28 18:53).** A `write_text()` without `newline=''` turned the file into
CRLF; the init's argument splitter keeps the `\r`, so `metal_props on\r` printed
`usage:` and set nothing — SpringBoard then took the GL path and the run looked
like "Metal lost the race". The symptom that names it is
`metal_props: usage: metal_props [show | on [NAME CLASS] | off]` in the log.
Write `rc.jobs` with `newline=''` (or the Edit tool), and check that line in
every Metal boot's log before reading anything into "Metal did not engage".

**Metal-on Today View boot, with the per-attachment clear build
(`20261028-190229-run.log`).** With `metal_props on` first (LF file) and
`metal_profile=1`: **0 MPS aborts, 0 SIGABRTs**, and Metal in three processes —
SpringBoard 31 trace lines, chronod/RenderBox 18, backboardd 4 — plus the
first-draw line (`into 465x465 x2`) and 14 traced draws. The picture
(`190148-screen.png`, mid-session): the Today View, **status bar clock rendering
(`11:01`)**, three widget cards as white placeholders. Two corrections to the
record: the status-bar clock was never missing — it was hidden under the wedge
in the Control Center frames, and it renders on the GL path as well (the 18:53
boot with `metal_props` broken showed `10:52`); and the widget cards' blank
content is placeholder/data, not compositing — RenderBox draws its 465×465
platter without crashing and without a visible second-plane loss.

The daily root is back on the GL order with the Control Center swipe (LF,
verified), repacked as `root-daily-fw.apfs`.

**Wedge geometry, measured (2026-10-28 19:10, from `182415-screen.png`).** The
wedge's boundary is a **smooth diagonal, slope about 1:1** — stepping 1–7 px per
4 rows, no 16/32/64-px quantization — so it is not a GMEM tile boundary and the
earlier `notile` result stands for the right reason. A straight diagonal over a
half-screen region is what a rotated or wrongly transformed quad paints, not
what a missed tile store paints. In the traced band-noise boot (`163440`) there
are **no draws with off-canvas coordinates** (0 of 3328), but that trace belongs
to the band variant, not the wedge: the wedge has not been captured in a traced
boot yet. Its capture needs the retry loop — set `gltrace` on the swipe frames
26–45 (whole-screen place), take a mid-session screenshot, and keep the run
only if the picture shows the wedge; the band-noise boot proves the recipe
records what draws the variant that *is* present.

Checked and exonerated on the way: `target_to_surface()`'s `glReadPixels` copy
needs no explicit wait (Mesa syncs readback per the GL contract), and nothing in
the driver intercepts CoreAnimation's own `glReadPixels`, so the 194×62 upload's
source is CA's memory throughout.

**The garbage and the wedge are the framebuffer-fetch path — proven by A/B on
2026-10-28 19:20 (`192531`, `193317`).**

- `glenv=FD_MESA_DEBUG=noreorder`: the env line is set
  (`redwood-gl: in the environment: FD_MESA_DEBUG=noreorder`) and the picture is
  **unchanged** — same coloured blocks — so batch reordering is not the cause,
  and the in-tree `fd6-fb-barrier-reorder.patch` (a Chlorophyll/Bergamota fix
  whose own comment names the exact symptom — "a framebuffer-fetch draw read
  half-written memory: one triangle of each of Control Center's blur quads came
  out stale") is not what this boot needs. It lives inside
  `fd6_texture_barrier`, and **eagl never calls `glTextureBarrier`**, so that
  path is never taken on our stack.
- `glfetch=0` (the extension is not advertised, CA's and eagl's shaders use
  plain sampling): **the Control Center is clean** — no coloured blocks, no
  wedge, correct blur backdrop — and it stays clean: mid-session and post-return
  frames are identical, 286 GL frames, 0 crashes
  (`193128-screen.png`, `193317-screen.png`). What remains is different in kind:
  the connectivity module's four circles draw as white discs (the vibrant
  material's fallback without the fetch program), and the Now Playing card is
  still empty.

So the fetch path is where the garbage comes from, and the family's known fix
(cache invalidation around the first read after a write — K90's CCHE on a8xx,
CCU/UCHE on a7xx) is the right direction; our tree's two attempts at it cover
only `glTextureBarrier` and a same-batch transition, not the fetch draws we
actually issue. The workable next step is to make the invalidation happen for
the draws that use `gl_LastFragData` on this stack — either in the driver (flush
CCU + invalidate UCHE when a fetch draw's target was written by a previous
batch) or by making eagl emit a texture barrier before those draws — rather than
disabling fetch. Until then, `glfetch=0` is a clean-picture workaround with the
white-disc fallback as its cost.

**First attempt at that fix, built 2026-10-28 20:5x (`722f60e1`, artifact in
`local/ci-artifact-3`).** The inline first-fetch barrier used
`FD6_FLUSH_CCU_COLOR | FD6_INVALIDATE_CACHE`; upstream's `fd6_texture_barrier`
always had `| FD6_FLUSH_CACHE` as well, and the missing writeback is exactly what
lets an invalidate throw away the previous draws' pixels. `freedreno-darwin.patch`
now carries `FD6_FLUSH_CCU_COLOR | FD6_FLUSH_CACHE | FD6_INVALIDATE_CACHE` for
the first fb-read draw of a batch, and the new `libOSMesa.8.dylib` (14 822 264
bytes, md5 `59aaf486…`) is merged into `local/root-daily` and repacked as
`root-daily-fw.apfs`. **The boot that tests it has not run yet** — the phone was
handed back to Android for the user. Next session: `enter --stage kernel`,
`ramdisk system-metal.apfs`, `run --seconds 300 --gpu-keep --metal --gpu-probe
20 --gpu-direct --root-image local/root-daily-fw.apfs` (GL order, fetch on), and
a mid-session screenshot; what to look for is the coloured blocks and the
diagonal wedge staying gone while `glfetch` is **1**.

One build lesson: extending a patch's comment inside a hunk changes the `@@
-hunk,count +hunk,count @@` numbers — forgetting that produced "malformed patch
at line 161" and a 12-minute failed CI run. After editing any `tools/mesa/*.patch`,
verify with `patch -p1 --dry-run` against the upstream file before pushing.

**VERIFIED 2026-10-28 22:22 (`222307`): the fix works, and the Control Center
problem is closed.** With fetch **on** and the `FD6_FLUSH_CACHE` build:
Control Center renders completely correctly — backdrop blur, all icons, the
connectivity module's four circles with their material shading (the
`glfetch=0` white-disc fallback is no longer needed and that workaround is
retired), sliders, Focus tile — **black coverage 0.3 %** (wedge variants were
25–51 %), **1 762 saturated pixels** (the coloured-garbage variants carried
tens of thousands), mid-session and post-return frames identical, 79 GL frames,
0 panics or SIGABRTs (`222209-screen.png`, `222308-screen.png`). The coloured
blocks and the diagonal wedge were the same defect all along: the first
framebuffer-fetch draw after ordinary draws read memory whose earlier writes had
been invalidated instead of written back. What remains on that screen is
content, not compositing: the Now Playing card is empty (no media) and the
status bar's clock was hidden only by the wedge before.

The daily root is back on the GL order: `local/root-daily/etc/rc.jobs` has
`rwtouch wait 60` first again, repacked and re-verified on hardware
(`20261028-132636-run.log`: 79 GL frames, 0 MPS aborts, 0 SIGABRTs — identical
to the 10-27 baseline; Control Center painted with the known GL defects,
`20261028-132637-screen.png`). **To boot the Metal experiment again, move
`metal_props on` back to the first line, repack, and add
`ACE5_CMDLINE="metal_profile=1"`.** Also read off this boot: the hand-written
`com.apple.chronod.plist` works — chronod spawned (pid 8) and stayed; with the
Metal profile raised, its RenderBox pipelines compile (`plane_color`), and
whether the status-bar clock then appears is the next screenshot check.

Black coverage 51.4 % → **21.5 %**, mean brightness 41.6 → 83.2, and the band
y 600-1300 that was 30-65 % black is **0.0 %** black in every 100-row slice
(`logs/giulia/20261027-170627-screen.png`). The huge diagonal wedge is gone and
the wallpaper is drawn where it was; the brightness slider is back to full
height; the calculator and camera cards have their fills again.

What is left, measured the same way:

- the top band y 0-500 is still black (100 % / 97.5 % / 74 % / 81 % / 82 % per
  100 rows) with a stepped lower edge, and the status-bar clock is still missing
  (that is the chronod/RenderBox issue, not this one);
- two saturated-colour blocks that were *not* visible before: a red/orange speck
  blob on the calculator/camera card edge (x 0.60-0.90, y 0.635-0.72) and a
  red/cyan/magenta/yellow block around the home indicator (x 0.47-0.90,
  y 0.83-0.99). Saturated pixels 0.06 % → **5.07 %**. Repeated clearing used to
  wipe these regions every draw, so they were hidden rather than absent;
- a grey stepped smear where the old wedge's edge ran, and an opaque grey block
  over the top half of the brightness slider;
- the Now Playing card is now *present* but empty — content, not compositing.

**The three diagnostics that went in with the fix did not fire**, and `rw_print`
does reach the log (an existing "multisample resolve" message appears 3 times in
the same run). So, for this workload: no pass asks for a clear and records no
draw; no pass clears more than one colour attachment; and **no surface that is
memory of a program's own is ever asked for as a buffer** — `surfaceBuffer()`'s
`kIOReturnUnsupported` path was never taken, which rules out Bergamota's
"client-memory Control Center platters" explanation for this port. (The
"multisample" count was a red herring at first: three of those hits are the GL
extension list `GL_EXT_multisampled_render_to_texture`, not the frontend's own
message. Grep for the sentence, not the word.)

**Open question 2026-10-27 19:00, and everything staged to answer it with one
run.** A per-draw probe in `redwoodmetal.m` has never produced a line: not the
windowed trace, not even an unconditional "the first draw of this process"
line. Either RedwoodMetal draws nothing on this device, or a sandboxed
SpringBoard's output reaches no log at all — and the two have opposite
consequences, because the 51.4 %→21.5 % improvement is otherwise only explicable
by the clear fix inside that same `recordDraw:`. Two independent channels are now
in place and everything is staged: the probe writes to `/dev/console` opened by
name instead of trusting file descriptor 2 (`redwood_metal_say`), and the switch
is read from the environment, then `/redwood/etc/metal.switches`, then the
boot-args (`redwood_metal_switch`). `local/root-daily/etc/metal.switches` holds
`RW_METAL_TRACE=800` and `RW_METAL_TRACE_AFTER=100`, SpringBoard's plist carries
the same two variables, and `local/root-daily-fw.apfs` is repacked with both. One
300 s `--metal` run says which of the two it is; if the first-draw line appears,
its `RW_METAL_TRACE=` field also says whether the environment channel works.

A second traced frame, the same run's neighbour (frame 751, `gltrace_place` +
`gltrace_look`), has a *different* draw mix: 185 listed draws, program 62 only
twice, both small quads over the Focus card leaving white. The two frames either
side of the Control Center transition therefore differ enough that a single
traced frame is only representative of the moment it was taken — pick the
trigger so that the traced frame is a *settled* one (`gltrace_draws=N` with the
settled count, then read the frame number against the `redwood-gl: frame`
lines), not one inside the swipe.

Black coverage of every screenshot so far, for reference (`max(r,g,b) < 24`):
`121100` (Metal, Control Center) 51.3 %, `132016` (Metal, `notile`) 50.6 %,
`142640` (Metal, `gpu_keep=0`) 51.4 %, `122049` (GL, Control Center) 4.3 %,
`115244` (Metal, home screen) 55.2 % but visually correct — the home screen's
wallpaper is genuinely dark, so this number alone says nothing about a frame;
only the *difference* between two frames of the same UI does.

**The display path is not a suspect for the black.** `RedwoodGPUDisplay`
(`iokit/Kernel/RedwoodGPUDisplay.h`) is a single `RM6_BLIT2DSCALE` blit from the
composited IOSurface to the panel, with the source rect and the destination rect
both validated against their surfaces (`displayRectValid`) and the whole panel
covered by `dst`. A frame that is 85.9 % non-black in the framebuffer reads back
85.9 % non-black on the panel; the black is in the *source* surface, i.e. in what
the compositor drew, and the scaler is not making it.

**Measured 2026-10-27 15:23: `gpu_cached=1` changes nothing either.** The Metal
boot with `gpu_cached=1` (cacheable buffers plus an explicit `dc civac` of every
buffer the submission names, instead of the write-combined default that assumes
the CPU's writes are already in memory) is **byte-identical** to the
`gpu_keep=0` and `gpu_keep=1` runs: 0.00 % of pixels differ from
`20261027-142640-screen.png`, and the frame is 51.4 % black in all three. Two
conclusions: the CPU/GPU memory-visibility path (`gGPUCached`, `redwood_gpu_to_memory`,
the write-combined assumption in `submit()`) is exonerated, and **the corruption
is a deterministic function of the draw stream alone** — three different boots
with three different memory knobs produce the same picture to the pixel. The
per-frame wait grew from ~500 to ~1200 µs, so the knob did take effect.

**The two paths fail differently, and the difference is diagnostic.**

- Metal (`--metal`): large pure-black masses — 51 % of the frame. Nothing has
  been written there, and it reads back as zeros.
- GL (no `--metal`): **bright RGB noise** (red/magenta/green/blue patches), a
  solid bright-green block over the home indicator, a narrow dark vertical
  stripe at the top centre, plus the same clipped brightness slider, the same
  empty Now Playing card and the same missing clock
  (`logs/giulia/20261027-151348-screen.png`).

Bright multi-coloured noise is *garbage*, not a colour: the difference between
the two paths is only what the untouched memory happens to hold (zeros for the
Metal path's surfaces, recycled heap for the GL path's). So the shared fault is
**a region of a surface that the renderer never writes and something later
samples anyway** — a pass that does not cover its whole target. In the GL path
the lit-count trace shows exactly that shape: the frame reaches 98.5 % non-black
after the backdrop (draw #83), then program 62 — the backdrop/vibrancy pass —
repeatedly draws a full-screen quad sampling one of the small offscreen blur
targets (455x456, 285x613, 391x392) and *lowers* the count, which is what
sampling an uncovered corner of a blur target looks like.

**Next probes, cheapest first.**

1. `glshow=WxH` is documented but did not fire in this build: `show_texture()`
   is only called from the `frame_end` loop for an image that is both
   `.target` and `.drawn` (`userland/src/eagl.c`), and in the 15:13 run no image
   met both conditions — neither the "the texture N, ..." line nor the
   "there is no texture of 455 x 456" line was printed at all. Either fix that
   gate or make it say why it skipped, before relying on it.
2. Show the blur target directly (`glshow=455x456` after the gate is fixed) and
   look at its corner: if it holds noise, the blur chain is the producer.
3. `glskip=62` to confirm program 62 as the producer of the GL-path noise.
4. The Metal path is the one the user sees and it has no equivalent hook:
   `RedwoodMetal` does not go through `eagl`, so it needs its own instrumentation
   in `userland/src/redwoodmetal.m` (a per-draw log of target, viewport, scissor
   and the pixel the draw leaves at its own centre).

### What other ports hit, and how they fixed it (2026-10-27)

The same *class* — a surface region the renderer never writes and something
later samples — is documented in detail by `ry-diffusion/Bergamota` (a6xx,
Adreno 630/640, iOS 16/18/26; `docs/GPU_EAGL.md`, `docs/METAL_ADRENO630.md`).
Their symptoms, verbatim, and ours:

| Bergamota's symptom | Ours |
| --- | --- |
| "Client-memory (wrapped) IOSurfaces are read-only for the GPU and for backboardd's own mapping, so the render server could not deliver pixels into them (**Control Center platters**, live icons: **black squares and bars**)" | Control Center is the only screen that breaks; its cards stay black |
| "a clear-only pass was lost and the drawable kept the picture from three frames earlier" | stale/garbage content in a region |
| "the shim kept all of them and copied every attachment at every flush, so **older textures overwrote the surface with older pixels**" | the GL path's grey smear over half the frame |
| "One clear colour for every attachment … an attachment-0 clear destroyed a loaded attachment 1" | not yet checked in `RedwoodMetal` |
| GPU "reads past their last row into the guard page after every BO" (wrapped surfaces, mapped read-only with no tail page) | plausible for the 1131x2442 snapshot (6 px larger than the display) |
| GMEM tiling ruled out with `TU_DEBUG=sysmem` | ruled out with `FD_MESA_DEBUG=notile` — same conclusion |

**Their fixes, in the order they matter to us:**

1. **Load actions are one piece of work of their own at the start of the pass**
   (`BG_WORK_CLEAR` → `rwa_render_clear`), each attachment to its own colour,
   loaded ones untouched. A clear must never live inside the first draw.
2. **Only the latest attachment per surface is kept**, plus dirty tracking, so an
   older texture cannot overwrite a newer one.
3. **A writable GPU alias for client-memory surfaces** (`SBO_IN_WRITABLE_ALIAS`,
   the reply's `alias_addr + offset` is pixel (0,0) of a read-write write-back
   mapping). Without it, Control Center's platters cannot be written at all.
4. **Copy wrapped surfaces instead of mapping them** (guard-page slack), because
   the GPU reads past the last row.
5. Cache ordering: clean+invalidate *after* the GPU write, never a write-back of
   dirty CPU lines over GPU pixels.

**Two places in our tree already have the same shape:**

- `userland/src/redwoodmetal.m` takes the clear colour from **attachment 0 only**
  (`if (i == 0 && [attachment loadAction] == MTLLoadActionClear)`) and passes one
  `work->clearColor` for the whole render work — Bergamota's fix 1. The clear
  itself does travel as its own field of the render work, so their "a pass that
  draws nothing never cleared" does not apply; the per-attachment colour does.
- `RedwoodGPUUserClient::surfaceBuffer()` returns `kIOReturnUnsupported` for any
  surface with `s->client != NULL` (`iokit/Kernel/RedwoodGPU.cpp`), i.e. a
  client-memory IOSurface can never become a GPU buffer at all. `MiniIOSurface`
  creates exactly those for memory a program of its own hands over, and maps them
  read-only in the kernel — the same starting point Bergamota had to solve with
  fix 3. Whether Control Center's platters are such surfaces is the first thing to
  log (a line in `surfaceBuffer`'s refusal path would say so).

**Why UniXNU does not show this**: its Metal path is not a hand-rolled frontend
at all — Metal → metal2vulkan → **Turnip** (a mature a7xx Vulkan driver) → IR3,
so binning, tile stores and clears come from upstream Mesa. It also publishes
**per-process** Metal devices (its own docs note that Metal-only layers — wallpaper,
icons, widgets — go black when only backboardd has a device), keeps BOs in a
1 GiB contiguous write-combined pool, invalidates the TLB per unmap rather than
per submission, and forces the render server's framebuffer to plain BGRA when
CoreAnimation takes Metal. `tycykp/UniXNU-OnePlus-Ace5` has no compositor yet (its
A750 is a preparation service only), and `oosexgang/hackios-xnu-kernel` ships only
`hakios.adreno.c` on the GL/freedreno path with no Metal device, so neither has
reached this class of failure.

### What the HackiOS family has verified on hardware, and what transfers (2026-10-27)

Surveyed at the user's request ("the K40 project has GPU and Wi-Fi working"). No
K40 (alioth) port exists; the working claims belong to this repo's own family.
Copies of the two key documents are in `local/refs/` (gitignored).

| project | device / SoC | GPU | Wi-Fi |
| --- | --- | --- | --- |
| `oosexgang/hackios-xnu-kernel` — the upstream this repo forked | POCO X5 Pro (redwood, SM7375, A642L, WCN6750) | CoreAnimation drawn by `eagl` → OSMesa → freedreno GLES (`gpu=1`), photographed on the phone | WCN6750 ath11k (AHB) on the LinuxKPI layer (`wifi/lx/`), joins from Settings, Safari at 55–90 Mbit/s |
| `HeliumStudio-Dev/hackios-xnu-kernel` | raphael / K20 Pro (SM8150, A640) | GLES verified (FD640 triangle draw + readback, GL error 0) | WCN399x ICNSS + MPSS (PAS 4) + ath10k SNOC: WPA2 and WPA3-SAE verified with real page loads; re-scan after a Wi-Fi toggle still broken |
| `iola1999/hackios-redmi-k90-annibale` | K90 (SM8750, A830) | iOS 16.5 desktop drawn on the GPU by `eagl.c` → OSMesa → freedreno; touch usable, page-turn and Control Center animations near full frame | WCN7850 plan only: PCIe RC0 link-up and BHI/PBL readback done 2026-10-27; MHI and ath12k not started |

Neither `ls9875/hackios-umi` (Mi 10: software rendering, no firmware staged) nor
`FFT-infra/UniXNU-alioth` (the actual K40 repos: lock screen on the UEFI GOP, the
A650 probe still a to-do) has GPU or Wi-Fi. **Metal/Turnip works nowhere in the
family**: upstream Metal is 293/456 system shaders in progress, and K90's Turnip
probe dies with SIGILL after enumeration. The GL path is the shipping path
everywhere.

What transfers to giulia:

1. **The GL path is the product path.** Keep the Metal probe experiments, but
   black-region fixes belong to the GL/freedreno path — the same judgement our
   own GL-vs-Metal split already made.
2. **K90's framebuffer-fetch cache fix is the nearest known fix for our symptom
   class.** QuartzCore links OpenGLES and uses framebuffer fetch; Mesa's a6xx/a7xx
   fetch path invalidates CCU and UCHE before the first fetch, and A8xx needed
   CCHE on top (their userland `94e4c97`; ~95 screenshots before and after, the
   known colour blocks went from some to zero). Check what our Mesa does on a750
   before a frame's first fetch, and note our blur-target "uncovered corner"
   sampling is the same read-stale-memory class.
3. **Wi-Fi for WCN785x is chip-matched and already planned**: K90's
   `docs/plan/wcn7850-plan.md` maps redwood's chain (AHB/PIL/GLINK/QRTR/ath11k —
   what our tree carries in `redwood_wifi.c`, `redwood_qmi.c`, `redwood_wlfw.c`,
   `redwood_lx.c`) onto WCN7850: **PCIe RC0 + MHI + ath12k**, with a step table
   (PCIe link-up → minimal MHI → firmware + WLFW-over-MHI → data plane → en0),
   4–8 device weeks estimated to first connect, abort criteria, and firmware from
   linux-firmware `ath12k/WCN7850/hw2.0/` plus the BDF/NV from
   `/vendor/firmware_mnt/image`. Our `giulia.dtb` is the same shape:
   `/soc/qcom,pcie@1c00000` + `pcie0_rp`, `pcie_qtb@16cd000`, `cnss_pci0`,
   `bt_kiwi`, and the stock Android cmdline names the kiwi firmware dirs and
   `pcie_sm_regs=0x1D07000` — the same constants K90 quotes. The
   `redwood_net`/`redwood_apple80211`/`RedwoodWLAN` top layer is chip-independent
   and already in our tree.
4. **Per-SoC discipline transfers**: the raphael Wi-Fi doc forbids reusing
   redwood's WCN6750/WPSS/ath11k config on a different chip; each generation needs
   its own control chain. giulia's is the WCN7850 one, not redwood's.

### Turnip on this port: the build (2026-10-29)

The gate (`tools/vulkan/turnip_test.c`) and the VULKAN=1 build first went through
a compiler today. The path from "mesa configures" to "libvulkan_freedreno.dylib"
was blocked, in this order, and each is now answered:

1. `glslangValidator` — Mesa's top-level meson requires it as soon as a Vulkan
   driver is enabled. Ubuntu: `glslang-tools`.
2. The libdrm shim (`tools/mesa/redwood-drm`) was the GL driver's libdrm. The
   Vulkan runtime needs more of it: syncobjs (the kernel service has none; the
   table lives in the shim, a submit's out-syncobjs are bound to the fence the
   submit was given and waits go through WAIT_FENCE, and an empty submit's fence
   is given out in the shim because the kernel refuses nr_cmds 0), the device
   enumeration (`drmGetDevices2`, one made-up device on /dev/null), and the WSI
   file's `drmGetDevice2`/`drmDevice.businfo`.
3. `pthread_condattr_setclock` does not exist on Darwin; the timeline waits poll
   to their deadline instead (tu_device.cc, patched).
4. **ld64 on this toolchain refuses even a weak undefined symbol in a dylib** —
   the object carries `(undefined) weak external` and the link still says
   `symbol(s) not found`, where ELF resolves it to NULL. Measured with a probe
   built by the same xnu-clang and ld64. Mesa relies on that weak semantics in
   every generated dispatch table (the WSI, vk_common and cmd-enqueue tables
   name entrypoints that nothing defines, expecting NULL). The dylib links with
   `-undefined,dynamic_lookup`: dyld binds a missing **weak** reference to NULL,
   which is the ELF behaviour the tables are written for, and a missing
   **strong** one — the shim's C++-mangled names before the header gained its
   `extern "C"` — fails loudly at dlopen instead (`symbol not found in flat
   namespace __Z14drmSyncobjWaitiPjjxjS_`, which the gate did).
   An earlier attempt generated weak **definitions** (stubs returning
   NOT_PRESENT) for the fallback tables instead: it linked, but it poisoned the
   dispatch — `vk_instance_init` fills empty slots from the WSI table
   (fill=false), a stub took `vkEnumeratePhysicalDevices` before
   `vk_common_instance_entrypoints` could, and the gate answered
   `step 3: VK_ERROR_FEATURE_NOT_PRESENT`. NULL is the correct value; the
   stubs were removed.
5. The gate's Vulkan headers come from the build host (`libvulkan-dev`); they
   are copied out as their own include root (`vulkan/` + `vk_video/`), because
   `-I/usr/include` lets the host's libc answer an iOS compile's `<stdint.h>`.

Measured in CI: `libvulkan_freedreno.dylib` 10,518,148 bytes (the build also
strips it, fixes its install name and checks the ICD exports); it ships into the
root as `/redwood/lib/libvulkan_freedreno.dylib` with the gate as
`/redwood/bin/turnip_test` (`rc.jobs.turnip` runs it after the settle).

The gate on the phone, so far (2026-10-29):

- boot 1, no `--touch`: `turnip_test` ran, `dlopen` failed —
  `symbol not found in flat namespace __Z14drmSyncobjWaitiPjjxjS_` (the
  `extern "C"` answer above).
- boot 2, with `--touch`: **step 1 (the ICD answers
  `vk_icdGetInstanceProcAddr`) and step 2 (`vkCreateInstance`) pass**; step 3
  (`vkEnumeratePhysicalDevices`) answered `VK_ERROR_FEATURE_NOT_PRESENT`, which
  was the stubs' doing (above), not the driver's: the ICD loads and the
  instance is created.
- The next boot runs the library with the stubs removed. Steps 1–7 (ICD,
  instance, physical device, device+queue, host-visible memory, image, empty
  submit→fence) answer "can Turnip talk to RedwoodGPU at all" and each fails in
  a different place.

**Family update 2026-10-28: the PCIe Wi-Fi route now has working in-family
code.** `azzlantern/hackios-thyme` (Mi 10S, SM8250-AC / SD870 — the same SoC and
Adreno 650 as a K40) reports on hardware: GPU `THE GPU STAYS ON`, GMU 587 MHz,
`Mesa 25.0.7; FD650` CoreAnimation hardware rendering, and **Wi-Fi `PCIe →
amss20 SBL → QMI → radio registered (en0) → networks visible in Settings`**
(their `docs/thyme-port-summary.md`, copied to `local/refs/`). Its
acknowledgements credit `DBC1349`'s Mi 10 Ultra as "the first kona phone where
touch, GPU and Wi-Fi worked on this code base". The driver overlay lives in
`devices/thyme/overlays/xnu/files/bsd/dev/`: `umi_pcie.c` + `umi_pcie_phy.h`
(PCIe host), `umi_mhi.c` (MHI), `umi_wifi.c` (the `umi_wifi_step=`
boot-arg bring-up machine), `hakios.qmi.c`, plus the `wifi/lx/` layer and
`tools/stage-wifi.py` — exactly the chain K90's plan marked "to be written"
(its step 0–2), now proven on a PCIe-attached Qualcomm WLAN chip in this tree.

Notes that transfer directly:

- Firmware for the WLAN chip sat in a dedicated firmware partition (51 files:
  `amss20`/`m3`/`bd_*`/`regdb`); on giulia the equivalents are the `modem`
  partition (sde6/sde41, mounted at `/vendor/firmware_mnt`, where the stock
  cmdline already points `firmware_class.path` at `image/kiwi`) and the NV
  halves are `modemst1`/`modemst2`.
- `wifi_mac=` from the runtime interface is mandatory or the scan comes back
  empty; `adb cat` pollutes firmware with CRLF (use `exec-out`).
- Both thyme and the Mi 10 Ultra hit the same wall: after a run the GPU and
  TrustZone keep their state, the next run hangs in the GPU steps, and only a
  cold boot (power 10–15 s) clears it — the same class our loader's return-path
  teardown has to answer for.
- Their display-tearing fix (line-rate × rows frame timing on an autorefresh
  panel) and the diagnostic-mode habit (`umi_wifi_step=2 umi_wifi_return=1`,
  no system image, auto-return) are both worth copying.

Also updated since the 2026-10-27 survey: `FFT-infra/UniXNU-alioth` (the actual
K40) is wiring its A650 probe read-only into the device tree — still bring-up,
no Wi-Fi; `XTOM0706/ios-sweet-k60` (SM8475) is aligning GMU cold start with the
SM8550 reference and counting painted pixels for full-surface verification, but
the desktop is still CPU-rendered; `WuuuWeiii/ios-sweet-ci` (polaris, SDM845) is
working the GPU publish/lazy/reset lifecycle as a patch series; `b1ackm4x`'s
OnePlus 9 imported upstream modem staging and native Bluetooth helpers.

**Family update 2026-10-30: the field changed under this line.** In the ten days
since the 2026-10-27 survey the family went from a handful of repos to roughly
thirty device ports. Three developments matter here:

1. **Metal now runs on Adreno elsewhere.** `superboyyy/hakios-arm64e-dada`
   (Xiaomi 15, SM8750 — the K90's SoC) reports iOS 26 on `xnu-12377.121.6`,
   arm64e userland, 8 cores with PAC and SCMI clocks, the iOS 26 home screen,
   touch and on-demand storage; its GPU path is **Metal → metal2vulkan → Turnip
   → its own kernel driver on the Adreno 830**, with the Liquid Glass core
   shaders checked pixel-by-pixel against Apple's GPU on a Mac. Separately,
   `g11930/HackiOS-Xiaomi11Ultra` (SM8350) claims Metal on an Adreno 660 (FD660,
   `metal_profile=1` plus an injected `metal-enable`). The building blocks are
   public: `UniXNU/AdrenoMetal`, `steelbrain/metal2vulkan`, and even Mali ports
   (`Ken-u/MaliMetal`). This retires the 2026-10-27 line "Metal/Turnip works
   nowhere in the family". What is still true: *this* tree has no Metal path,
   and the GL path is what ships.
2. **Wi-Fi's wall is after M4, not at enumeration.** The 11 Ultra's WCN6855
   (`17cb:1103`) has firmware/BDF/NV extracted and byte-checked, but no PCIe
   LTSSM PIPE clock and no MHI/BHI downloader, so the link never comes up —
   the same class as our M1–M4. dada's WCN7850 gets as far as WMI, but the
   firmware dies in most runs and its current Wi-Fi driver is a stand-in. So
   passing M4 does not mean a working radio; budget a firmware-stability phase.
3. **SM8650 now has four UniXNU ports**: `Asultop/UniXNU-NX769J` (RedMagic 9,
   all images build, not booted), `YX2748198202-lab/UniXNU-shennong` (Xiaomi 14
   Pro, reaches `Failed to mount root device`), `Axiang-dev/UniXNU-caihong`
   (OnePlus Pad Pro) and `peiyuesheng666/UniXNU-PHY120` (Find X7 Ultra). None
   has GPU or Wi-Fi yet, so there is no A750 work to take, but their board
   descriptions are same-SoC cross-checks for UFS/UEFI/4 KiB pages. **No second
   giulia/PKG110 port exists.**

Also worth reading: `yechenan/pudding-ios` (Xiaomi 17, SM8850, iOS 16.7.16,
4 KiB pages, incremental over `ShueiTsuki/data-a-live` + `dipa-studio/ios-sweet`
+ UniXNU); `Scion-Project/scion` (QEMU/Pyre emulated iPhone 11 to an iOS 14 home
screen, and AVF/pKVM running XNU + iOS 16.5 in about 80 s on a Pixel 10 — a
virtualization route, not a bare-metal port); `tanosshi/xnuroid` (a Darwin
Mach-O loader plus graphics translation runtime, not XNU).
`dipa-studio/ios-sweet` (Redmi Note 10 Pro, SM7150) is the 1.1k-star upstream
many of these forks build on, and it renders on the CPU only.

**Measured 2026-10-30: on the GL path the Control Center is correct, and the
51 % frames were `--metal`.** A screenshot series (`--shots 20`) over one boot
is what showed it: home screen 55.2 % black, then Control Center at **0.3 %
black for ten shots and 200 s**
(`logs/giulia/20261030-140009-screen-005.png` — connection card, rotation lock,
Focus, both sliders at full height, the four buttons, the audio pill, the blurred
wallpaper; the status-bar clock and the Now Playing card are still empty, which
are the content-service items below). Nothing is torn, clipped or noisy. Four
things had been hiding this:

- **The compositor draws into the default framebuffer.** The new one-shot counts
  line says `framebuffer 0 colour 0` and `0 drawn` on every frame: `gl_osmesa.c`
  points the default framebuffer at the surface's own memory, so CoreAnimation
  never draws into a named texture of ours. Every `target && drawn` branch in
  `frame_end` — the copy into the surface, and `glshow` — is dead in this
  configuration. `glshow` now pastes into the **widest target surface** at frame
  end regardless of that bookkeeping, and works: it answered "there is no
  texture of 455 x 456" with the inventory `1:1125x2436 2:1125x2436 3:194x237`,
  i.e. the 455x456 blur target only exists while the Control Center animates in.
- **`metal_props on` fails without `metal=1`**:
  `IORegistryEntrySetCFProperties failed 0xe00002c7 (kIOReturnUnsupported: boot
  the kernel with metal=1)`. None of the runs since 2026-10-28 passed
  `--metal`, so RedwoodMetal was never engaged in any of them — and the 51 %
  Control Center is the `--metal` picture, not the GL one.
- **The "screen goes black" was an app.** The 10-28 `rc.jobs` opens Photos at
  ~90 s (`battery_test open photos-redirect://`); Photos draws nothing, so every
  later screenshot was 99.8 % black. That is what the earlier "run-to-run
  variable" readings were measuring: whichever app was in the foreground at the
  moment of the capture. Use a job script that leaves the screen in the state
  being measured (`etc/rc.jobs.cc` for Control Center, `etc/rc.jobs.safari` for
  the panel-cadence page).
- **A local `eagl.c` loop exists**: `local/wsl/build-eagl.sh` builds
  `libredwood-gl.dylib` on the WSL toolchain in seconds (needs the mesa output
  tree from `build-mesa.sh`), so a one-line change no longer waits 20 minutes for
  CI.

So the graphics line's remaining items are the two content services below and
the bounded `--metal` regression, not the GL compositor.

`com.apple.chronoservices` and launchd answers "Could not find job with label
com.apple.chronod", 559 times in one session; `/usr/libexec` has no `chronod`,
but `System/Library/PrivateFrameworks/ChronoCore.framework/Support/chronod` and
`.../MediaRemote.framework/Support/mediaremoted` are both in the system image.
Only their job plists were never staged (the default `IOS_JOBS` list does not
name them, and `/System/Library/LaunchDaemons` is a symlink to
`/redwood/LaunchDaemons`). A hand-written `com.apple.chronod.plist`
(`MachServices com.apple.chronoservices`, `RunAtLoad`) is now in the local root;
whether RenderBox then starts or aborts is the next thing to read. RenderBox's
`default.metallib` is present (655 304 bytes), so the old "chronod dies for want
of a metallib" note no longer applies to this image.

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

The gate on the phone, third answer (2026-10-29 22:12, build `eaeb31c`): step 3
failed at `tu_knl_drm_msm.cc:1028 could not get GPU ID` with the shim's trace
lines right above it — the enumeration reached the shim, the version answered
"msm" 1.7, and the first GET_PARAM failed because **the IOKit user client was
never opened**: only `drmOpenWithType()` opens it and Turnip opens the node path
by itself. Fixed with `ensure_connection()`, committed, not yet on hardware.

### The local (WSL) Turnip loop: set up, one step short (2026-10-29 night)

For small changes, the mesa build now runs on this Windows machine under WSL
(dist `Ace5Build`, Ubuntu 24.04.3 x86_64, rootfs imported from ubuntu-base;
scripts in `local/wsl/`, all paths inside): `build-mesa.sh` de-CRLFs the tree
(git's autocrlf breaks the patches and shebangs), re-syncs `tools/`, stages the
checksummed tarball (archive.mesa3d.org is slow from WSL; download on the host),
and runs `AIR=1 VULKAN=1 tools/mk-mesa.sh`. Toolchain: Ubuntu clang-18 (the
wrapper's compiler), theos **x86_64** toolchain on `/root/theos`, and the
iPhoneOS16.5.sdk — whose Windows clone needs two repairs done in
`fix-sdk-links2.py` (symlinks came out as short text files: libSystem.tbd,
pthread.h, 70 files) and `fix-tbd-crlf.sh` (autocrlf CRLF'd 2702 tbd YAMLs; no
tapi parser accepts those).

What works: meson configures and 600+ targets compile. What remains: **the
linker**. meson probes the linker and, not recognising cctools ld64, emits
GNU-style arguments (`--as-needed`, `--start-group`) that ld64 refuses; the
CI's native aarch64 ld64 tolerates them. The fix in progress: run the CI's own
aarch64 ld64 under `qemu-aarch64-static` (`use-cctools-a64.sh` installs qemu
and the aarch64 toolchain; `mk-wrappers.sh` rebuilds the host/bin wrappers with
the GNU arguments filtered) — written, not yet verified. llvm's ld64.lld is not
an option: it cannot read the SDK's tbd v4 files.

**Verified 2026-10-30: the WSL loop builds.** Two fixes were needed past the
linker itself:

1. **The SDK's symlinks are broken on the Windows checkout.** Git materialised
   every framework's `Headers → Versions/Current/Headers` (and `Current → A`)
   as a short text file, so `#include <IOKit/IOKitLib.h>` failed inside the
   libdrm shim — the real header sits at
   `Versions/A/Headers/IOKitLib.h` and the git tree has it. The tbd and pthread
   repairs of 10-29 did not cover the framework links.
   `local/wsl/fix-framework-links.sh` walks the WSL SDK copy and recreates any
   short file whose whole content is a relative path to an existing target
   (2 links for IOKit; rerun it after every fresh `copy-sdk.sh`).
2. **`cross.ini` carried `-isysroot` on the compile args only.** Every meson
   dependency probe that ends in `-l<name>` therefore linked without a sysroot
   and failed (`library not found for -lz`), meson quietly answered
   `Dependency zlib skipped: feature zlib disabled`, and the last link of
   `libOSMesa.8.dylib` died on undefined `_gzopen` —
   `src/freedreno/common/freedreno_rd_output.c` calls `gzopen` unconditionally,
   with no `HAVE_ZLIB` guard. `c_link_args`/`cpp_link_args` now carry
   `-isysroot` (`b9e85e0`), zlib is back to `auto`, resolves through the SDK's
   `libz.tbd`, and dyld finds `libz.dylib` on the device. The WSL-only
   `-Dzlib=disabled` workaround in `build-mesa.sh` is gone.

The build then completes in about 8 minutes: `libOSMesa.8.dylib` 14.8 MB,
`libredwoodair.dylib` 9.3 MB, `libvulkan_freedreno.dylib` 10.8 MB — the
Turnip library **without the fallback-table stubs**, linked with
`-undefined,dynamic_lookup`. `export-outputs.sh` copies the dylib to
`local/components/`, it is swapped into `local/root-turnip/lib/` and the root
repacked (3176 blocks). Everything is staged for the gate run; only the phone
is needed.

Once a local dylib builds: `export-outputs.sh` copies it to
`local/components/`, the root repack is the same three commands as always, and
the phone cycle is `reboot` → `enter --stage kernel` → `ramdisk --image
local/system-metal.apfs` → `run --seconds 300 --bootcpu 1 --touch --kernel
out/giulia-kernel/mach.ace5-public --root-image local/root-turnip.apfs`. The
gate then answers step 3 for real: with `ensure_connection()` in, the next
expected failure is none — or the first real device-creation data the port has.

**Measured 2026-10-30 12:48: TURNIP GATE PASS (steps 1–7).** Three pieces on top
of the stub removal (commit sequence in `tools/mesa/`):

1. `turnip-darwin.patch` — Turnip's `msm_bo_map`
   (`src/freedreno/vulkan/tu_knl_drm_msm.cc`) mmapped the shim's fake device fd,
   which can never deliver a buffer's memory; on `__APPLE__` it now calls the
   shim's `redwood_drm_map` (the `IOConnectMapMemory64` path freedreno's own
   patch has used all along), and `tu_knl.cc`'s `tu_bo_unmap` calls
   `redwood_drm_unmap` instead of `munmap` (the mapping is owned by the
   kernel's driver, and the reserved-PROT_NONE replacement is not provided).
   Placed allocations (`placed_addr != NULL`, kIOMapAnywhere is what the
   kernel does) are refused rather than silently ignored.
2. `redwood_drm.c`: `redwood_drm_map` opens the user client itself
   (`ensure_connection()`), in line with the shim's own comment that every
   request must be able to open it.
3. `tools/vulkan/turnip_test.c`: step 5 prints each sub-step (allocate, map,
   pointer, write, unmap, map again) — the previous run crashed inside step 5
   with no detail, because the pre-patch `munmap` of an IOConnect mapping left
   the re-map without a readable picture of where it died.

The gate (`logs/giulia/20261030-124816-run.log`, GPU kept alive with
`--gpu-probe 20 --gpu-direct --gpu-keep --touch`): ICD load, `vkCreateInstance`,
**physical device "Turnip Adreno (TM) 750", vendor 0x5143, API 1.4.305**,
`vkCreateDevice` with a queue, host-visible memory written and read back, an
image bound to memory, and an **empty command buffer submitted with the fence
signalled** — the full submission path (command buffer → BOs → RedwoodGPU ring
→ GPU → fence) executed on real hardware. The Vulkan route — Mesa Turnip in
place of the hand-written Metal frontend — is therefore not a plan any more but
a working stack: what remains for a Vulkan-composited desktop is WSI, swapchain
and the CoreAnimation bridge, the same list UniXNU worked through.


**Measured 2026-10-30 14:17: Safari works on loopback, and the device says 60.0 Hz.**
The refresh-rate page (`userland/share/web/refresh.html`) is now opened for real
(`logs/giulia/20261030-141725-*`, recipe `tools/ace5/rc.jobs.safari`), and the
answer is on one screenshot: **60.0 Hz, 16.7 ms a frame, 16.0–17.0 ms, 4000
frames in 67 s, "on cadence"** — no frame over 20 ms, and a one-bar histogram
at the 16 ms bucket. What it took to get there:

1. **`file://` is refused, and not because Safari is missing.** The 14:07 run's
   log says `battery_test: SpringBoard was asked to open
   file:///redwood/share/web/refresh.html: it REFUSES`; `MobileSafari.app` is in
   `system.apfs` (and `local/ios165` is only fetcher scripts — the image, not the
   tree, is the place to look). `SBSOpenSensitiveURLAndUnlock` hands `prefs:` and
   http(s) URLs to their applications, but a file URL from another process is
   not something iOS opens. The upstream POCO project opens `https://yandex.ru`
   the same way and reports Safari working, so the scheme was the variable.
2. **`webserve`** (`userland/src/webserve.c`, built by
   `local/wsl/build-webserve.sh`) serves `/redwood/share/web` on
   `127.0.0.1:8080`; `rc.jobs.safari` starts it with the trailing `&`
   (`init.c`'s run_rc "do not wait"), prints `net_test show` for the record, and
   opens `http://127.0.0.1:8080/`. `net_test show` confirms `lo0: inet
   127.0.0.1`, and the log's `SpringBoard was asked to open
   http://127.0.0.1:8080/: it does` plus `webserve: GET / -> 200 refresh.html
   (4788 bytes)` (twice) is the whole path: SpringBoard → Safari → WebKit →
   CFNetwork → loopback → our server. No radio is involved.
3. **What 60 Hz means here.** The page reports its viewport as
   `375×812 @3.00×` — the display is presenting 1125×2436, i.e. the iPhone X
   class of screen. Those are not ProMotion devices, so CoreAnimation's display
   link runs at 60 Hz by design, and 60.0 with a 16.0–17.0 ms spread is the
   compositor pacing the run at exactly what it asks for. 120 Hz is therefore
   not a bug to fix in the present path: it would need both a ProMotion device
   identity and a 120 Hz mode in `MiniIOMobileFramebuffer`, and nothing in the
   four system-app groups depends on it.
4. **The "10 frames" impression.** `redwood-gl` counts in hundreds during the
   same session (30, 200, …, 1200 frames, ~0.45 ms waiting for the picture and
   3–4 ms a frame) while rAF logged 4000 callbacks: the present path draws when a
   client commits, and the page only changes its text every ten frames, so an
   idle compositor simply looks slow from the host side. It is not capped.
5. Safari's own UI renders too (tab bar, `AA`, URL field, share/bookmark/tab
   buttons) and the status-bar clock advanced 6:18 → 6:19 across the series —
   the first browser screenshot of this port, and the cheapest way to test any
   page from now on: drop it in `userland/share/web/`, open
   `http://127.0.0.1:8080/<name>`.

**Handoff 2026-10-30 (line 1 closed, line 2 planned).** The GPU line is done as
far as the four system-app groups need it: the GL path is correct, the
CoreAnimation display link runs at 60 Hz (rAF-measured, not an optical panel
measurement), and Safari renders. The closure rests on single-boot evidence: the
Control Center 0.3 % figure is one boot's screenshot series, the two-run
stability that `docs/DRIVERS.md` asks for is not done, and
`tools/ace5/app-smoke.py` does not exist yet — `docs/GPU.md` §5 records this
boundary. What remains on that line is written down, with evidence and an
acceptance test for each item, in `docs/GPU.md` — DVFS (the GPU has only ever
been voted the stock minimum clock, though the 13-point DCVS table is uploaded),
runtime power collapse/DPMS, UBWC, the per-frame IOSurface texture cost,
per-client page tables, the Metal frontend (first draw printed once under
`metal_profile=1`; MPS still incomplete, though dada now has
Metal→metal2vulkan→Turnip on an Adreno 830), Vulkan WSI, and perf counters; the
first two to do are DVFS (the RPMh vote mechanism exists in `redwood_adsp.c`)
and the texture cache in `eagl.c`. Backlight and 120 Hz are recorded there as
not needed for the gate.

The next line is Wi-Fi: `docs/WIFI.md` has the milestones, the DTB facts they
stand on, the capacity budget, the abandon criteria, and the pieces already in
the tree (`wifi/lx`, `tools/mk-ath11k.sh`, the QRTR code in `redwood_wifi.c`).
The first host-side step is a diff, not a transcription: annibale's
`redwood_pcie.c` is the same SM8750 and already read RC0 link-up and BAR0 on
2026-10-27, thyme's `umi_pcie.c`/`umi_mhi.c` give the MHI interface; Linux's
`pcie-qcom.c` and `phy-qcom-qmp-pcie.c` are the register-sequence reference.
Only the board-level differences (base addresses, GPIOs, RPMh rails, SMMU
mapping) need to be re-derived. Those facts are now exported from the DTB into
`docs/ACE5-HW.md` (2026-10-30), including the vendor `qcom,phy-sequence` and
`qcom,pcie-sm-seq`; `tools/ace5/pcie-regs.py` generates
`xnu/files/bsd/dev/giulia_pcie_regs.h` from them. `tools/mk-ath12k.sh` also
answered the M5–M6 question: ath12k of Linux 7.0.14 fails on 17 files (the
LinuxKPI 802.11 headers are older than 7.0), 6.16.1 is no better, and 6.12.1
fails on 5 (skb/list helpers, `cfg80211_find_ext_elem`, three mac80211 op
signatures, PM ops, MHI). After the helper shims it is 17 of 19: only
`mac.c` (three `link_id` mac80211 op signatures) and `mhi.c` (the MHI
layer M4 owns) remain. See `docs/WIFI.md` §7. The firmware budget is
measured too: WCN7850's `amss.bin`/`m3.bin`/`board-2.bin` total 8.41 MiB and
the repacked root image is 18.95 MiB, 13.05 MiB under the 32 MiB cap, so the
small image still works; `redwood_wifi_driver_file()` now falls back to
`fw/<basename>` so ath12k can read them.

**Host-side M1 is ready (2026-10-30).** `xnu/files/bsd/dev/giulia_pcie.c`
implements `pcie_step=1` (read-only survey: GDSC/clock/reset state, PARF/PHY/DBI
registers, the PERST/CLKREQ/WAKE pins) and `pcie_step=2` (GDSCs, clock votes,
core and PHY resets, the 115-step vendor PHY sequence, PERST/WLAN_EN, then the
link poll). `phone.py run --pcie 1|2` passes the boot-arg; the tree applies with
the new file. The phone cycle is:

    reboot → enter --stage kernel → ramdisk --image local/system-metal.apfs
    → run --seconds 300 --bootcpu 1 --touch --pcie 1
          --gpu-probe 20 --gpu-direct --gpu-keep
          --kernel out/giulia-kernel/mach.ace5-public
          --root-image local/root-daily-fw.apfs
    (the GPU flags are what publish RedwoodGPU; without them CoreAnimation
    software-renders and every UI judgement is about the CPU, not the port)

Step 1 writes nothing and decides whether step 2 starts from a cold or a warm
RC0.

**Build check 2026-10-30 (WSL x86_64).** `xnu/apply.sh` (75 changed, 125 new
files) + `xnu-deps/setup.sh --stage1/--stage2` + `xnu-build.sh` compile
`giulia_pcie.o` and link it: `_giulia_pcie_init` and `_pcie_thread` are in
`mach.development.bcm2837` (15,100,448 bytes). The first build stops in
`check_all_exports` ("exported name not in import list: IOService::…"), a
host-tool/symbolset mismatch unrelated to this change — the kernel binary is
already linked at that point, and a second `xnu-build.sh` run then exits 0
(2026-10-30, after the `redwood_wifi.o` firmware-lookup change). Three host
fixes were needed for WSL: the six dependency repos and the macOS SDK
prefetched through the Windows proxy (WSL could not reach github.com),
`uuid-dev` installed, and `/usr/bin/env python` pointed at `python3`. The
built kernel is at `out/giulia-kernel/mach.ace5-pcie`.

Also new: `tools/ace5/app-smoke.py`, the 16.5 gate `docs/DRIVERS.md` named
but did not have. On the 2026-10-30 14:21 log it scores network PARTIAL
(loopback and Safari pass, Wi-Fi blocked) and basic FAIL (Settings was not
opened in that run); `--group` judges only the group a run exercised.

**Touch and the renderer, measured on the phone (2026-10-30 evening).**
Two separate findings from a session of drag tests:

- **The S3910 poll loop read reports only every 10th tick.** The attention
  line (162) sits high between reports and its low pulse is shorter than
  the 8 ms sample, so `|| tick%10==0` set the real cadence: 80 ms, about
  12.5 Hz — measured 643/603/629 reports in three 45 s windows. Reading
  every pass costs ~100 µs of bit-banged SPI and the same build measured
  up to 89.8 reports/s, bad=0 (`giulia_s3910.c`).
- **Without the GPU flags, CoreAnimation software-renders.** The
  RedwoodGPU service is published only by `ace5_gpu_keep=1`
  (`giulia_adreno.c`), and `inject.c` falls back silently: "no GPU to draw
  with: CoreAnimation keeps its software renderer". Every run from 18:17
  to 19:00 lacked `--gpu-probe 20 --gpu-direct --gpu-keep`, drew
  1125×2436 on the CPU, and felt like steps; two UI stalls (2 frames/min,
  no crash, 2.8 GB free) appeared ~4 and ~7 min in. The render artifacts
  seen on software (white halo around a home-screen icon, the App Library
  search bar ghosting the home screen) still need a GPU-run comparison
  before they are called GL-path bugs. The daily recipe must carry the
  GPU flags whenever the UI is being evaluated.

**mach.ace5-pcie mount panic, root-caused (2026-10-31).** The pcie+wifi
kernel panicked twice at `vfs_mountroot` (`kmem_alloc(…, 16777216,
0x100021): failed with 3`). The bisect of 2026-10-30 evening (original
sources, wifi-lookup alone, PCIe driver alone all boot; so does
`mach.touchtest` after a fresh entry and staging) was the right
experiment with a wrong conclusion — the panic was never in a committed
change but in the build tree. Symbolizing the backtrace against a
rebuild of the c229d5f source state (`local/bisect/mach.pciewifi.
unstripped`) gave `_pcie_thread+0xd4 → io_map+0x164 →
kmem_alloc_guard_internal+0x690 → __kmem_invalid_object_panic`, and the
disassembly shows the call: `movz w1, #0x100, lsl #16` — 16 MiB. The WSL
tree carried a stale `giulia_pcie.c` whose TLMM mapping asked for
`ml_io_map(0xf000000, 0x1000000)`; every ml_io_map allocation comes from
the 32 MiB io_submap (`VM_MAP_CREATE_DISABLE_HOLELIST`), the guard
allocator takes KMA_NOFAIL, and an unservable 16 MiB panics instead of
returning NULL. The committed repo version has always been `0x100000`
(1 MiB) — the standing build path simply never copied it over. The
build scripts that stand for `mach.autotest` now copy `giulia_pcie.c` +
`giulia_pcie_regs.h` from the repo on every build (`local/wsl/build-auto.
sh`; the forensic `build-sym.sh` keeps the old tree on purpose, that is
how the panic got symbolized). The rebuild is verified host-side:
`_pcie_thread` holds exactly one LSL#16 constant, `movz w1, #0x10,
lsl #16` (1 MiB), and the whole-kernel tally of that instruction class
flips exactly one site 0x100→0x10 with every other immediate unchanged.
`mach.ace5-pcie` needs no retest for this panic; the bisect kernels
(`mach.origtest`, `mach.wifitest`, `mach.pcietest`) and scripts stay in
`local/` (not committed). This is a host check — the corrected kernel
has not booted yet.

**Scripted input, unattended (2026-10-31).** Boot-arg `touch_auto=SECONDS`
starts a kernel thread that replays a fixed gesture set through the
touch queue: ten slow home-screen drags (the content tracks the finger
one to one, so a dropped frame is a long interval between moves in the
log), two Control Center pulls (the blur reference), an App Library
dwell. Finger 9, so the touch controller's own reports never collide
with the script; coordinates span the panel as 0..65535 on both axes.
Its first dry encoding pass also exposed an unsigned-wraparound bug — a
leftward drag computed `(to-from)` in uint32 and teleported the finger
to x=65535; interpolation is int64 now. Each round ends with a
horizontal zigzag (five halves, ~3 s): `tools/latency.py` fits the
page's movement to the finger's along the axis the touch turns in —
auto-detected, and `--finger 9` reads the scripted finger. With
`touch_auto_exit=1` the script ends the run itself (five held fingers,
the loader's designed exit) right after the zigzag, so `phone.py
recording` (new: `oem recording` stages what the kernel left,
`get_staged` fetches it) always gets a `rec_ring` recording whose last
frames are the zigzag. Recording recipe: `rec_secs=1200 rec_ring=1
rec_mb=256 rec_step=4` — 256 MB is the size that did not respring
SpringBoard, and step 4 makes the ring hold ~8.7 s, enough for the 3 s
zigzag plus the exit. The next unattended round, end to end:

    ACE5_CPUS=8 ACE5_CMDLINE="touch_auto=45 touch_auto_exit=1 \
        rec_secs=1200 rec_ring=1 rec_mb=256 rec_step=4" \
      python tools/ace5/phone.py --serial b4cf8812 run --seconds 900 \
        --bootcpu 1 --touch --gpu-probe 20 --gpu-direct --gpu-keep \
        --frame-timing --render-trace \
        --kernel out/giulia-kernel/mach.autotest \
        --root-image local/root-daily-fw.apfs
    python tools/ace5/phone.py --serial b4cf8812 recording   # after the return
    python tools/frames.py logs/giulia/<run>.log
    python tools/latency.py logs/giulia/<run>.log logs/giulia/<...>-recording.raw --finger 9

**Run A, measured (2026-10-31, GPU path, 8 CPUs, `mach.autotest` v4,
log `20261031-133820-run.log` + recording `20261031-134504`).** The
first end-to-end number of the port: **the home screen follows the
finger 30.0 ms behind**, until its pixels are in the panel's memory
(gain 0.99, misfit 2.1 rows — a clean 1:1 fit over 198 frames). The
30 ms: kernel→window server 0.02, →event system 0.08, the application
and the wait for the next frame 15.33, drawing 3.67, SwapEnd 0.48, GPU
1.11, display beat 6.97, copy 2.34; then the panel's scan. Input
dispatch and the GPU are exonerated; the beat is the unavoidable half
frame at 60 Hz. During gestures the same log renders **55–58 fps** on
the home drags and the lag zigzag (2–6 % of frame steps over 16.7 ms,
1 % over 33), and 58.2 fps on the Control Center pull. The felt
stutter lives in the transitions: the CC retract 39 fps (13 % over
16.7 ms, 11 % over 33) and the App Library entry 40 fps (51 % over
16.7). All gestures together: 9 % over 16.7 ms, 2 % over 33. The slow
frames' time goes to **idle before draw, 37–38 ms** — the window server
waiting for commits — while draw, GPU and copy stay small. The
measurement names the transition pipeline (what feeds CA during the CC
dismissal and the App Library snap), not input, not the driver.

Three traps the round documented: the daily root opens Safari on the
refresh-rate page (which is also a panel check: 60.0 Hz, 16.7 ms, on
cadence), and a scripted round without a home swipe "renders nothing"
— v4 starts every round with the home-indicator gesture. A panicked GPU
run skips the cleanup, and the next GPU boot then reads CX
active/HW-controlled and refuses ownership ("CX already active …
refusing", ready=0, silent software rendering at 6 fps) — reboot to
Android between a panicked run and the next GPU run; clean returns are
safe back-to-back. The kernel log's `[seconds]` prefix is not wall time
(one run printed 910 s in ≈270 s wall); the µs intervals of the `tl:`
lines are self-consistent (the zigzag measures 3.40 s, Safari sees
16.7 ms frames) — trust those. And the log ring evicted the early
gestures of the long round (832 of ~1400 samples survive), so frames.py
sees only the tail of the round.

The phone is on Android 15 (`adb devices` shows `b4cf8812`); `local/root-daily`
carries `bin/webserve` and `etc/rc.jobs` = `rc.jobs.safari`, and
`local/root-daily-fw.apfs` is repacked from it.
