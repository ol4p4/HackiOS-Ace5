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

**Measured 2026-11-02 19:29: steps 1–8, a triangle drawn through Turnip.** The gate's
own next stage (`tools/vulkan/turnip_test.c`, step 8): two embedded SPIR-V shaders
(`tools/vulkan/triangle.vert|frag`, compiled by glslang to `triangle_spirv.h`), a render
pass clearing to black, a graphics pipeline with no vertex input (`gl_VertexIndex`), a
3-vertex draw into the 256×256 image, a copy back through a host-visible buffer, and a
pixel compare. Run `logs/giulia/20261102-192910-run.log` (`root-turnip.apfs`, `--gpu-probe
20 --gpu-direct --gpu-keep`):

```
turnip_test:   corner (0 0 0 255), middle (255 0 0 255)
turnip_test: step 8: the triangle reached the image and read back
turnip_test: TURNIP GATE: PASS (steps 1-8)
```

`corner` is the render pass's clear (opaque black), `middle` the fragment shader's red —
so ir3 compiled both shaders, the CP ran the draw, and the tiled image round-tripped.
0 PowerOff/panic/SIGSEGV. This is the first real draw through Turnip on this port (step 7
was an empty submit). Next: WSI/swapchain (an IOSurface-backed present for a Vulkan
client), then the CoreAnimation bridge.

**Step 9 prepared (2026-11-02, built and imaged, NOT yet run): the surface half of a
swapchain.** The same triangle is copied into a 256×256 IOSurface in BGRA order (what the
window server, the 2D engine and CoreAnimation read), then the surface is locked for read
and compared (corner black, middle red). Turnip's BOs are not IOSurfaces, so this is the
honest first step: a CPU copy, with a zero-copy surface-as-BO import left for later.
`local/wsl/build-turnip-test.sh` builds only this program; `local/root-turnip.apfs`
repacked (119 entries, apfsck clean, 135/135 read back). The run recipe is the step 8
one: `enter --stage kernel` → `ramdisk --image local/system-metal.apfs` → `run
--gpu-probe 20 --gpu-direct --gpu-keep --root-image local/root-turnip.apfs`. Judge:
`turnip_test: step 9: ...` and `PASS (steps 1-9)`.


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

**The transition stalls are the render clock, not the renderer
(2026-10-31, host-side analysis of the same log).** Inside the CC
retract the slow frames draw in 0.6–2.4 ms, wait 1.0 ms for the GPU,
and the processors are near idle — but the window server *begins* each
frame exactly **100.0 ms** after the one before, thirteen frames in a
row (±0.2 ms), with beat 0.0 (the driver's too-late-resync path every
time). 100.0 ms is six refreshes; other stalls land on four (66.9 ms)
and two (33.4 ms); before the stretch, three frames were drawn
back-to-back (began gaps 0.0) while SwapEnd was held 16/32/49 ms —
commits were queued, the render clock was the limiter. On the iPhone X
QuartzCore does not take the driver's vsync (inject.c: CADeviceUseVBL
is an A7–A10 path there) and runs its own CADisplayTimer, whose phase
it sets from the frame timings of a display driver this one is not — a
copy driver whose beat resyncs whenever a frame is late. Fed those
timings, the timer falls into a lock step of whole refreshes during
transitions: the CC retract at 39 fps, the App Library entry at 40 with
51 % of steps over 16.7 ms, the drags' exact 38.2 ms idles. The
machinery for the other path exists on both sides and has never been
run: boot-arg **ca_vbl=1** sets CA_FORCE_TIMER=1 in backboardd, and the
driver answers IOMobileFramebufferEnableVSyncNotifications with
kFBNotifyVSync — a self-correcting 60 Hz thread_call sending (refresh
time, presented time, count). Plan when next on device: one run with
ca_vbl=1 (plus a one-line driver log when the callback registers, so
"CA never asked" is distinguishable from "the path is broken");
pass/fail from tools/frames.py — the retract at 55+ fps with under 3 %
over 33 ms, no whole-refresh stalls in the retract window
(local/mine-retract.py), the end-to-end lag from 30.0 ms towards
~24–27 ms as the app+frame-wait share shrinks. If the screen freezes,
drop the boot-arg — nothing else changes. Not candidates, with
reasons: fb_queue=1 (measured worse here: 33 % over 16.7 ms against
28 %); fb_beat=0 (the beat is a symptom reporter here, not the cause);
renderer-side throughput work (the CC pull already holds 58 fps with
the blur on).

**ca_vbl=1, run and settled (2026-10-31, runs 141636/142642/143416).**
The boot-arg works: with CA_FORCE_TIMER=1 in backboardd, QuartzCore
registered a notification port — through **IOConnectSetNotificationPort**
("notification port for type 5" at the driver), never through the
notifications selector. The driver's arming had two disconnects (the
port registration dropped the refcon and armed nothing; the sender
demanded a callback only the selector could have set), fixed in
b55eaee — after which the notifications flow, three of them logged at
16.9 ms apart. The result on screen: **worse, not better** — gestures
24.8 fps with 97 % of frame steps over 16.7 ms, idle windows 7.7 fps,
most swaps "a refresh late". The receiver is the unknown left: either
the port queue starves (the sends use kIOUserNotifyOptionCanDrop, and a
slowly drained port drops what the sender offers) or QuartzCore's VBL
receiver parses a different message layout than the generic IOKit async
result we send (callback 0, refcon 0, three 64-bit numbers as six
32-bit halves). Both answers live in QuartzCore in the dyld shared
cache — read its VBL handler before the next on-device attempt. Two
build traps to remember: the boot-arg path is opt-in, so the default is
exactly as before this experiment; and `local/wsl/build-auto.sh` now
copies `MiniIOMobileFramebuffer.cpp` too — the 14:16 and 14:26 runs
silently ran the old driver because the copy list missed it (the
unchanged binary size was the tell).

**ca_vbl=1, the payload fixed and the receiver's crash (runs 143416,
161645).** The sender was wrong: `sendAsyncResult64WithOptions` takes
`io_user_reference_t` args (64-bit each), and the driver packed the
three 64-bit numbers as six 32-bit halves — the receiver of the first
three saw a half timestamp that wraps every 4.3 s. With the args as
three real u64s (now, now, count) the render clock follows the
notifications: **every gesture and transition runs 60.0–62.6 fps with
0 % of frame steps over 16.7 ms and 0 % over 33 ms** — the CC retract
that was 39–44 fps with 9–11 % over 33 ms, the App Library entry that
was 51 % over 16.7 ms, the drags' exact 38.2 ms idles: gone. The stall
mechanism (CA's own timer fed by this copy driver's frame timings) is
confirmed, and the vsync-driven clock is the cure. **But the receiver
crashes on every notification batch**: backboardd takes SIGSEGV (6
crashes, 24 respawns in 161645; 7 and 28 with the old payload; zero in
every run that never delivered a notification), and the screen sits on
the relaunch spinner through the App Library and zigzag gestures — the
recording of that run is the spinner, so the end-to-end lag stays at
the 30.0 ms of the non-ca_vbl run. The crash chain, symbolized from
the cache: QuartzCore's completion dispatcher at unslid 0x1885024c4
(`ldp x20, x19, [x0]; blr x20` — the standard {function, context}
block) is handed a block whose function slot came out as the message's
first 32-bit timestamp sign-extended (pc 0xffffffffd77b8000 =
0xd77b8000 = the mach time of the crash at 19.2 MHz), and the deepest
frame then calls through a vtable slot ([x20+48]) built from message
data. So QuartzCore parses the notification with a layout that is not
the generic IOKit async result we send — the first fields it wants are
a callback and a context, not timestamps. ca_vbl therefore stays opt-in
and experimental; the default path is unchanged and stable, and the
transition stalls remain the known cost until the notification format
is matched.

**ca_vbl=1, closed: Apple never ran this path on A11 (same day, later).**
The protocol question was settled from Apple's own binaries, not guessed:
the iPhone X kernel collection (ipsw member `kernelcache.release.iphone10b`,
IM4P + lzfse, 50.9 MB) holds IOMobileFramebuffer's kext — its code maps
through the kernel's __PLK_TEXT_EXEC segment (vm 0xfffffff005bf5000 →
file 0x179d000) — and its vsync sender at **0xfffffff005c1d854** builds
six arguments by splitting each of three 64-bit values into 32-bit
halves, exactly as this driver originally did, then
`sendAsyncResult64WithOptions(ref, 0, args, 6, CanDrop)`. Five such
senders exist (counts 6, 3, 2, 1, 2 — vsync and other events). The
registration side: the async reference at client+0x18 carries the
client's **callback**, which comes only through the notifications
selector — so the real driver sends only when fCallbacks[5] is set, and
QuartzCore's completion dispatcher calls that callback. On the iPhone X
QuartzCore registers the port and never the callback: **Apple's VBL path
has never run on A11**, and its A11 completion code is not what the
port's messages meet. That is the SIGSEGV — a notification armed from
the port alone hands the dispatcher a bad function to call. The sender
is back to Apple's six-half payload with the callback gate restored
(014701c), the port registration passive, and **ca_vbl is closed as a
path on this phone**: it did prove the mechanism (60 fps transitions)
but the completion protocol cannot be completed from our side. What
remains is the self-timer itself: CADisplayTimer phases from the frame
timings this driver reports, and the beat's too-late resync path is
exactly the input it sees during transitions — so the next experiment
is `fb_beat=0` (show at once, no phase shifts) with the CC retract and
App Library numbers as the verdict, and if the resync path is the
poison, `fb_beat=0` should dissolve the whole-refresh lock steps.

**Family survey on this problem (2026-10-31): nobody has met it.** K90
(annibale) spent 10-30 on rendering correctness (render48–93: stripes,
icons, UBO alignment) and its desktop runs at 10 fps, where a
self-timer's degeneration is invisible; its 120 Hz work is the one
transferable note — CoreAnimation learns the refresh period from
**displayMinRefreshInterval/displayMaxRefreshInterval** properties, and
`fb_hz=120` alone did nothing ("beat 8.33 ms but frames stay 16.6 ms
because CA production ≈9.6 ms > one slot"). K60 (ios-sweet) published
display modes in its 0218 ("let CoreAnimation drive the panel
natively") — this driver is at parity on every piece of it
(TimingElements/ColorElements, GetID=1, GetCurrentAbsoluteTime=77,
GetBlock 0x40) and K60 is still at panel/DSI bring-up; dodge is
migrating to hakios; dada (iOS 26, arm64e) landed on 10-31 and is too
young; the upstream (oosexgang) added `fb_measure=1`, a DSI
frame-counter measurement, not a fix. So the experiment matrix is ours:
**arm A `fb_beat=0`** (no beat phase shifts — the timings CA's
self-timer phases from), **arm C `fb_minint=1`** (publish the refresh
period properties, the one gap the survey found), and in every arm the
5-second stats line now ends with **"N clock reads"** — the count of
GetCurrentAbsoluteTime calls, so the runs say whether CoreAnimation
polls the display clock at all and how its rate changes through a
stall. Verdict per arm: CC retract ≥55 fps with under 3 % over 33 ms,
App Library under 15 % over 16.7 ms, no whole-refresh gaps in
local/mine-retract.py; then the decision: A wins → make it the daily
default and re-measure Run A; A loses and clock reads track the stalls
→ the clock read is the feed, return a beat-aligned time from 77; A
loses and 77 is never read → the timer phases from swap completions,
try fb_queue≥3.

**The arms run (2026-10-31 evening, logs 173351 and 174026): all three
fail, and the mechanism is now pinned.** `fb_beat=0` changed nothing —
the CC retract is 39.0 fps with the same exact **100.0 ms** lock (ten
frames of 99.6–100.2), the App Library 53 % over 16.7 ms; `fb_queue=3`
likewise (36.9 fps, 13 % over 33 ms); and the new stats line says
**"0 clock reads" in every window of every run** — CoreAnimation never
calls GetCurrentAbsoluteTime, so its timer reads no display clock from
this driver. The unimplemented-selector audit is identical across runs
with and without ca_vbl (57, 15, 22, 29, 33, 27, 17, 84 — none of them
a registration), so no hidden callback selector exists either. What
that leaves: the 100 ms cadence is CoreAnimation's **10 Hz fallback for
a missing display link**, reached only by the animation path — the
static screen's commits run at 60 fps without any link, the CC retract
cannot, and the one thing that ever lifted the animations to 60 fps was
the notification stream (ca_vbl). The driver's frame timings are
exonerated: nothing it reports moves the fallback.

The way forward is the one the crash itself pointed at. The receiver's
completion block took its function pointer from the **message payload's
first word** (our timestamp, sign-extended, became the pc), so the real
protocol's payload is three 64-bit values **beginning with the client's
callback and refcon** — which is why the real driver sends only when
the notifications selector has set them. On A11 QuartzCore never
registers a callback, but the callback does not have to be
QuartzCore's: libredwood runs inside backboardd and can register **its
own no-op function** through the notifications selector (72, scalars
{callback, refcon, 5}), and the driver can send (callback, refcon,
time) as the payload — then the dispatcher calls a valid pointer, no
crash, and the display link exists: the 60 fps animations that ca_vbl
already demonstrated, without the crash. Next session: inject.c
registers the no-op (IOConnectCallScalarMethod on the IOMobileFramebuffer
connection, once at startup, gated by ca_vbl=1), the driver's vsync
sends (fCallbacks[5], fRefcons[5], now), one run, the usual thresholds.
`fb_minint=1` (arm C) remains untested and is the fallback if this
fails.

The phone is on Android 15 (`adb devices` shows `b4cf8812`); `local/root-daily`
carries `bin/webserve` and `etc/rc.jobs` = `rc.jobs.safari`, and
`local/root-daily-fw.apfs` is repacked from it.

**REA/IDA field map for the vsync sender (2026-10-31, static).** The iPhone X
kernel collection's IOMFB notification sender starts at
`0xfffffff005c1d838` — the `0x...d854` label used above is +0x1c, the same
raw-text offset as `iomfb-text.bin`. Its entry `sub_FFFFFFF005C1282C` picks
two payloads: with the `+0x1640` period set it sends the beat triple
`(frame count +0x1628, last timestamp +0x1630, delta +0x1638)`; otherwise it
passes the caller's `(+0xB0, +0xB8, +0xC0)`. The raw sender
`sub_FFFFFFF005C1D838` splits whichever three 64-bit values it gets into six
32-bit halves and walks the async-reference list at `+0x1380`, sending each
ref at `+0x18` with `sendAsyncResult64WithOptions(ref, 0, args, 6, CanDrop)`.
That is the map the next run needs: the payload's first word must be the
client's callback, not a timestamp. `vsync()` now sends
`(fCallbacks[5], fRefcons[5], now)` (code changed 2026-10-31, not yet run);
`fb_beat` only paces `show()` (`clock_delay_until`) and never touched this
payload, which is why arm A saw no change. The `+0xB0/+0xB8/+0xC0`
semantics remain an inference until the notifications selector's write point
is read.

### Family survey 2026-11-01: K90's three repos, and what it changes here

Surveyed by `gh api` only (read-only). iola1999 now keeps three repos
(`hackios-xnu-kernel`, `hackios-xnu-userland`, `hackios-redmi-k90-annibale`,
branch `k90-port`; the userland overlays through `.userland.git`, and the
authoritative iOS-26 mesa patch set lives in the kernel tree
`tools/mesa/rebase-26/`).

**Their iOS 26 line reached a usable desktop.** Icons, wallpaper
(MercuryPosterExtension), glass dock/search, lock-screen clock, apps all
display; page-turn median 53.4-53.6 fps (slow lane "today view" about 50, ~410
draws/frame). The wall that owned their 10-30 was **HFI 150 (GMU hang on draws
whose fragment shader reads the framebuffer through bindless descriptors)**,
root-caused to A830's `has_early_preamble` and fixed by
`has_early_preamble = False` in `freedreno_devices.py`. Around it: 16-byte
constant-buffer alignment, fwidth quad helpers, texture-buffer alignment,
IOSurface stride via copy sampling, zero-fill of new render targets.

**Their vsync findings - 26 line only, do not transfer.** "No independent
hardware vsync": the beat is MiniIOMobileFramebuffer's software beat (83
rounds, 258,042 intervals, median 16,640/16,641 us, 90.7 % within
16,667+/-500 us). CA_HEARTBEAT_RATE defaults 1/60 on both 26 and 16.5, with no
direct timer creation anywhere. The 3 s hole / 15 s period got large
cross-round statistics but no declared root cause. Their `ca_vbl=1` control
round silently no-oped because the inject lib never loaded. And their 26 line
runs 50+ fps with **no vsync notification stream at all** - a different
QuartzCore from our 16.5 line, where the 10 Hz fallback is real. Two things
this adds to *our* work only: the payload's third word `now` wants a
timebase/unit audit (they hit three `mach_absolute_time` conversion bugs in
one day), and the injection-load check stays hard (their control failed
exactly there).

**Boot failures - their taxonomy, none matches ours.** Wrong build tool ->
early panic; version string in `__TEXT,__cstring` makes builds never
byte-identical (an 11-byte delta moved 159k segment differences; LC_UUID
written post-link) - **a binary diff is not evidence**; unsigned binaries ->
userspace panic; the loader image tail must be byte-exact ("an image with any
other tail returns to LK about 7 s after the jump"); kernel+root must end
below their 128 MB RESIDENT_RD_OFFSET. Our clean "PMRD: PowerOff at launchd"
is in none of their classes; the control-kernel verdict stands as the main
experiment.

**Next ca_vbl run - additions on top of the standing criteria** (notification
port for type 5 / no-op vsync callback lent / vsync notifications armed; no
SIGSEGV; animations at 60 fps):

- Main verdict unchanged: does the WSL control kernel
  (`local/wsl/build-auto.sh` -> `out/giulia-kernel/mach.autotest`) still power
  off? If it also dies, rebuild the images in WSL too (the standing plan).
- Confirm the injection actually loaded before reading anything else.
- Audit `vsync()`'s `now`: which clock, what unit, versus what the A11
  dispatcher expects as the third word (still an inference).
- CI-kernel provenance check (which tree/script `build-ace5.yml` runs); never
  use a binary diff as evidence, per the version-string lesson above.

**Turnip transplant list (port at the rendering step, not before).** Our gate
passed steps 1-7 on 2026-10-30 ("Turnip Adreno (TM) 750" enumerated, empty
submit fenced); what remains is WSI/swapchain and the CA bridge. When the
first real draws go through Turnip, copy from their `rebase-26`: fd-bo import
size (`freedreno_screen.c` - pass `whandle->size`, not 0, or every texture
laid over an imported buffer fails the size check), linear import pitch
tolerance (accept pitch divisible by `min_pitch_align`, set `pitchalign=6`),
last-level padding `sysmem_only` for linear imports whose final-level rows are
not x4, and submit merging (`should_defer` on `__APPLE__`; backboardd does ~24
submissions/frame). Also check whether a750 wants `has_early_preamble = False`
- A830 needed it, and ours is the a750 the GL path has only exercised through
`eagl`/freedreno, not through Turnip's draw paths.

**120 Hz: no change to our plan.** Their `fb_flip=1` probe read the panel
already refreshing at ~125 Hz (CTL0 command mode drives INTF1,
INTF_FRAME_COUNT +25 per 200 ms); the constraint remains compositor production
(9.39 ms non-beat time exceeds one 8.33 ms slot) - the same conclusion this
runbook reached on 10-30 with the Safari cadence page (60.0 Hz, on cadence).
Their noted borrowable item, "display controller direct scanout of iOS frames"
(~500 lines), stays far-future here.

**Candidates, recorded not started:** (1) a loader watchdog - theirs (v11+) is
fed by host commands, stops feeding after 180 s of host silence, and
`oem hang` measures a 37 s reset; that is the known cure class for our "run
ends, loader USB dead, physical recovery only" state. (2) In-boot
respin/cycle (change environment variables and restart processes within one
boot) to cut ca_vbl variable rounds. Also noted: they rewrote their top-level
git history on 10-30 (session ids removed), so any reference to an old SHA of
theirs is stale.

### 2026-11-01 evening: the system-volume rebuild, its defect, and the ca_vbl device verdict

**The rebuild defect (root-caused).** The 16:42 system volume was built from a
tree extracted by 7z, which cannot read lzfse-compressed DMG bands: the dyld
main cache and `.01` subcache came out as garbage ("CRC Failed" buried in
extract-dyld.log) and the cryptex tree lost 979 items ("Unsupported Method").
Boot signature: `initproc failed to start -- Library not loaded:
/usr/lib/libSystem.B.dylib` (dyld cannot open the corrupt cache, falls back to
on-disk libraries that do not exist in a cryptex-era rootfs). Fix: re-download
the IPSW member from Apple's CDN (`ios165/fetch-from-ipsw.py 096-04666-078.dmg`
→ exactly 2,726,297,600 bytes), extract with `apfs-work/apfs-tool cat` (the
kernel apfs_lib handles lzfse; verify the main cache starts with `dyld_v1`),
relink into the tree, repack with `/root/ios165/system.manifest`, and verify
with `tools/apfs/check-image.sh` — apfsck clean and all 62,423 entries
reconciled. New volume: 2,492,256,256 bytes, sha256 247ae7a9…. Also repacked
root-daily.apfs WITH its regenerated manifest (the 16:09 build had no manifest:
all modes 0777, audio symlink missing; now 135/135 reconciled, sha256
ecf99f46…). Rule: every volume write must tee a log and print sha256 — the
unlogged 16:42 build hid this defect for hours.

**The run (log 20261101-191021, mach.autotest + fixed volume + ca_vbl=1).**
Completed its gesture round: no PowerOff, no panic, no SIGSEGV, 2,960 GL
frames, a 268 MB recording (the failure signature is 32 bytes). ca_vbl
structure confirmed: "notification port for type 5" → selector 72 with
scalars {callback=0x1f268c850, refcon, 5} → "vsync notifications armed at
60 Hz" (231 lines, bursts at 0.10 s = CA's 10 Hz fallback cadence).
**Pacing did NOT improve** — A/B against Run A (20261031-133820), same gesture
script: 89%,70% (CC-retract window) 39.0 fps / 13 % / 11 % → 28.2 fps / 37 % /
28 % (worse); drags 55–58 → 56.5–59.5 (parity); all gestures 9 % / 2 % both.
mine-retract: a steady 33.3 ms (2×refresh) lock, beat 0.0, and "0 clock reads"
persists. So the callback-first payload removes the crash but does not make
CA's render clock follow the stream — the completion's timing is what the
timer needs, and a no-op swallows it. Next: fb_minint=1 (still untested) or
read the completion dispatcher's post-callback use of the payload (REA).

**The morning's PMRD PowerOff did not reproduce** on tonight's config (WSL
kernel + verified volumes + ca_vbl=1). Attribution still open between the CI
kernel build, the payload commit and environment; the CI kernel remains
untested against these volumes.

### Arm C measured (2026-11-01 evening, fb_minint=1, log 20261101-192617)

fb_minint was already implemented (TUNABLE gMinInterval; publishes
displayMinRefreshInterval/displayMaxRefreshInterval at start) and the kernel
carried it — no rebuild. Run: same gesture script + ca_vbl=1 + fb_minint=1.
Health clean (0 PowerOff/panic/SIGSEGV; 235 arm lines). Gestures: drags
56.1–58.2 fps (parity), the 89%,70% CC window still 28.8 fps (35 %/27 % — the
2×refresh lock, unchanged), all gestures 9 %/2 %. **End-to-end lag: 23.5 ms
behind the finger (baseline 30.0 ms)** — the app+frame-wait share fell
15.33 → 9.78 ms, the signature of CA learning the refresh period from the
published properties; this meets the runbook's target band (24–27 ms) and is
the best measured yet. Recording fetched this time (20261101-192623-recording.raw).
Conclusion: arm C is a real win on interaction latency and should become the
daily default candidate; the transition lock (CC retract 28–29 fps, 2×refresh)
survives correct period + armed notifications + no crash, so the remaining
lever is the completion payload semantics — what the dispatcher consumes after
calling the callback (REA task), or a consuming callback in place of the no-op.

### Build routing rule (2026-11-01, user decision)

Estimate the local (WSL) build time first: **over 20 minutes → CI; under 20
minutes → WSL.** Measured baselines: CI `build-ace5.yml` succeeds in
17m48s–20m11s (8 successes across 20 runs, mean ~18.9 min) plus 1–2 min
artifact download, ~19–22 min a round; the WSL mesa/AIR loop is ~8 minutes; the
WSL kernel (`local/wsl/build-auto.sh`) is ~10 minutes. CI also carries friction
(a push needs the 7897 proxy, or the Git Data API), so borderline cases go to
WSL, and CI is kept for two jobs: canonical artifacts (for others / release)
and an independent build when the WSL environment itself is suspect. A full
stack rebuild (kernel + userland + images) is the only common >20 min case;
incremental mesa is seconds, so most changes ride WSL. This rule replaces the
earlier qualitative "small changes local, big changes CI".

### A1 measured: fb_minint alone is baseline; the latency win needs the stream (log 20261101-194032)

Same gestures, `fb_minint=1` WITHOUT ca_vbl: latency 31.5 ms (baseline 30.0),
app+wait 16.29 ms, CC window 38.9 fps / 13 % / 11 % — i.e. fb_minint alone
changes nothing measurable; the 23.5 ms in arm C needs the notification stream.
Arm C vs A1 (the only difference is ca_vbl) isolates the stream's effect: app
frame scheduling tightens (16.29 → 9.78 ms, lag 23.5 ms) but the transition
locks at 2×refresh (38.9 → 28.8 fps, 11 % → 27 % over 33 ms). So the stream IS
consumed (scheduling changes) yet the animation timer still locks — the REA
question narrows to what the completion dispatcher consumes after the callback.
Daily-recipe decision deferred until that is read; the combined config is the
experimental default in the meantime (the tradeoff disappears if the CC window
is fixed).

### B1 read (2026-11-01 night): the notification delivery chain, and where the time goes

Static read of QuartzCore (DSC image load 0x18827b000, text 0x34d000; subcache
.03) around the crash dispatcher:

- **Dispatcher 0x1885024c4** is a trampoline: `ldp x20, x19, [x0]` → helper →
  `mov x0, x19; blr x20; ret` — it calls `{payload[0]}(payload[1])` and returns
  0. It reads nothing else.
- Its only reference (adrp+add at 0x188502470) passes it as the **invoke
  routine** to a wrapper at 0x1885023d4: `wrapper(fn, ctx, flags)` allocates a
  16-byte block, stores `{fn, ctx}`, and dispatches it asynchronously through a
  stub helper (stub island 0x188549e60 → external via [0x1daddc000+0x5a8]).
- Four direct callers of the wrapper exist (0x18839e200, 0x1883c36a8,
  0x1883c634c, 0x188467db4) — the message handler(s) that turn the 6-word
  IOKit payload into `{fn, ctx}`.
- **Conclusion (observed): the delivery chain consumes only payload[0] (the
  callback) and payload[1] (the refcon). The third u64 — the time — is never
  read.** CA's transition timer therefore cannot learn the frame time from the
  notification; only the callback could, and on A11 the callback is ours (the
  no-op). This also re-reads 10-31: the crashing run's "60 fps transitions"
  were measured on the relaunch spinner (backboardd crash loop), and tonight's
  clean A/B shows the stream's real effect: scheduling tightens (lag 31.5 →
  23.5 ms) but transitions stay locked at 2×refresh.

**Implication:** the "consuming callback" must bring its own time (e.g.
mach_absolute_time) and drive CA's display link itself — i.e. the next REA
target is the function the real (A12+) VBL callback would call: find it by the
registration path (who would pass a function through selector 72), not by the
delivery path. Alternative: keep the timer's phase input on the driver side
(beat/swap timing) — the 100 ms (baseline) vs 33.3 ms (with stream) locks show
the timer consumes different inputs either way.

### B1 continued: the display-config path, and why selector 27/17 are not the differentiator

The IOMFB client library (`/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework`,
load 0x1af1bf000, text 0xd000) maps the 3084-byte display-config blob:
- get: 0x1af1c58e4 — `malloc(3084)`, `IOConnectCallStructMethod(conn, 27, NULL, 0, buf, &3084)`,
  cached at this+0x668, and the caller checks the returned size == 0xC0C;
- set: 0x1af1c5838 — `IOConnectCallStructMethod(conn, 17, buf, 3084, NULL, NULL)`;
- a 0xAA-filled template at 0x1af1c6628 (the blob a caller uses when the get failed).
The runtime sequence (20261101-192617): `27 (3084 out) → 17 (3084 in)` at backboardd
startup — get fails with kIOReturnUnsupported, CA proceeds with the template and ignores
the set. Also `GetBlock(0x4d)` returns 1580 bytes zeroed (our driver answers any block with
zeros unless it is 0x40).

**But the family has the same gap**: `iola1999/hackios-xnu-kernel` (branch k90-port) carries
the SAME `MiniIOMobileFramebuffer.cpp` with the same selector enum — no 27/17, no 0x4d — and
its iOS 26 desktop runs at 50+ fps. So the display-config gap is not what separates 50 fps
from our locked transitions, and implementing it is an experiment, not a known fix.

Selector 77's shape in the library (0x1af1c6284) is scalar-out × 1 — exactly what our
driver implements — so "0 clock reads" is CA's own gate on A11, not a shape mismatch.

**Arm prepared (`fb_cfg27=1`, driver change in this commit):** answer selector 27 with a
zero-filled 3084-byte blob (size exact) and accept selector 17, behind the boot-arg. The
question it asks on device: does a SUCCESSFUL display-config read change the render clock
(clock reads, transition lock) at all? The layout is unknown; zeros are the honest minimum.

### fb_cfg27 arm measured (2026-11-01 night, log 20261101-205121): hypothesis closed

With selector 27/17 answered (zero-filled 3084-byte blob, exact size) on top of
arm C: run clean (no panic from the zero fields), the "selector 27 not
implemented" line is gone, but **nothing else moved** — clock reads still 0,
the 89%,70% window 29.8 fps / 35 % / 25 % (arm C: 28.8 / 35 / 27), latency
26.5 ms (arm C 23.5, within noise). The display-config read is not the lever,
consistent with K90 running 50+ fps on the same driver gaps. fb_cfg27 stays as
an opt-in, documented dead end. With this, every low-cost arm in the decision
matrix is measured: fb_beat=0, fb_queue>=3, fb_minint, the notification
stream, the no-op callback and the display config — the 2x-refresh transition
lock survives them all. The one path left is a consuming callback that drives
CA's display link itself, which needs the display-link tick in QuartzCore —
more static work, not a device run.

### The timer probe (2026-11-01 night): what CADisplayTimer consumes

Read from QuartzCore (DSC, same tooling): the period function at 0x1882d3930
computes the heartbeat period in the documented order — `CA_HEARTBEAT_RATE`
getenv (0x188571eb0, 1/hz) → `[this+0xAF8]` displayRefreshStepMachTime →
`[this+0xAF0]` displayMinRefreshInterval (Q32 seconds, a flag bit chooses
2^-32 vs 2^-33) → the 1/60 constant at 0x188553000+2912 — identical to the
chain K90 documented for iOS 26. fb_minint feeds the third step; that is the
latency win's mechanism.

The presentation path gives CA no timing at all: SwapEnd returns only
kIOReturnSuccess, SwapWait only success, and selector 77 is never called
(0 clock reads). The transition lock moved only with the notification stream
(100 ms without it, 33.3 ms with it), so the timer's phase signal during
transitions is the notification arrival, not the presented frames.

**Consequence: fb_flip / display-controller direct scanout cannot fix the 2x
lock** — it changes presentation performance (the 2.4 ms copy, K90 parity), not
any signal CA consumes. It stays a performance item. The one lever for the lock
is a callback that participates in CA's display-link bookkeeping; the no-op
already shrank the lock from 6x to 2x, which says CA still wants the callback
to confirm/complete something. That internal tick remains the unknown.

### The IDA pipeline for QuartzCore, and the timer class (2026-11-01 late)

Built the decompilation path: `local/wsl/dsc-macho.py` extracts one image's
__TEXT from the fixed cache and wraps it in a minimal Mach-O (QuartzCore:
3,461,296 bytes, __TEXT at 0x18827b000, saved as
`E:\emu-data\rea\work\quartzcore.text.bin`); `analyze_range.py` (REA_RANGE_*
env) analyzed the full text into `quartzcore.bin.i64` (14,916 functions) and
`rea function` decompiles work (~3 s/query, attached mode, IDA must be open;
the "unpacked base" prompt wants Proceed with unpacked base).

Findings:
- `sub_1882D3930` (0xc8 bytes) is the period function, decompiled: it caches
  at this+0xB18; CA_HEARTBEAT_RATE getenv (1/hz) → this+0xAF8
  (displayRefreshStepMachTime) → this+0xB10 (displayMinRefreshInterval; the
  2^-33/2^-32 constants 1.16415322e-10 / 2.32830644e-10 chosen by this+0x220
  bit 4) → qword_188553B60 (1/60). This is the same chain K90 documented.
- The function is **vtable-dispatched** (get_callers/get_xrefs are empty; no
  direct references anywhere): its chained-fixup pointer lives at
  **0x1d5b44808** in the 0x1d5b44xxx vtable region (same region as the
  AppleCLCDDisplay vtable the 10-27 work found). The vtable spans at least
  0x1d5b447c8-0x1d5b448e0; constructors referencing the region: 0x1883a6380 →
  0x1d5b44530, 0x1883a64dc → 0x1d5b444e0, 0x1883ac034 → 0x1d5b44f30, …
- The Itanium typeinfo walk (vtable[-1] → name) did not resolve — the cache's
  RTTI is not laid out the usual way; the class must be identified through the
  constructors above.

Next (next session, all host-side): decompile the constructors at 0x1883a6380 /
0x1883a64dc to name the class, then its timer methods around the vtable slot
0x1d5b44808 — looking for the per-frame "completed/presented" bookkeeping the
no-op callback fails to do (the 6x→2x evidence).

### The CA env-knob table, and the CA_VSYNC_OFF experiment (2026-11-01 night)

QuartzCore initializes 153 environment knobs into a global byte table
(0x1E1F6B790; sub_188370A28 reads them). The relevant ones and their table
indices: CA_VSYNC_OFF=111 (global 0x1E1F6B7FF), CA_ENABLE_ALL_FRAME_RATES=113,
CA_ENABLE_FRAME_RATE_ARBITRATION=114,
CA_ENABLE_REDUCED_SERVER_VSYNC_RENDER=116, CA_VELOCITY_FRAME_RATE_UNCONSTRAINED=124,
plus CA_FORCE_120HZ, CA_FORCE_DOUBLE_BUFFERING, CA_USE_PRESENT_ON_FINISH,
CA_DISABLE_MODE_SWITCHING, CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION. These are
Apple's own switches, settable through the job environment (the FD_MESA_DEBUG
channel).

The one reader of CA_VSYNC_OFF is sub_188523160 (0x1344 bytes) — CA's display
render/swap loop ("display %u swap_end returned error %x", the skip-spinner and
detached-src paths). `v9 = MEMORY[0x1E1F6B7FF]` feeds the branch that skips the
source attach (v30 = (flags&4)|v9 → v55 → the detached-src skip).

Experiment prepared (not yet run — the phone was not connected):
- `CA_VSYNC_OFF=1` added to local/root-daily/LaunchDaemons/com.apple.backboardd.plist
  EnvironmentVariables;
- root-daily.apfs repacked WITH manifest, sha256 61d47ed9…, check-image 135/135 clean;
- run recipe unchanged: enter → ramdisk system-metal.apfs → run mach.autotest +
  root-daily.apfs + ACE5_CMDLINE="ca_vbl=1 fb_minint=1 touch_lat=1 touch_auto=45
  touch_auto_exit=1 rec_secs=1200 rec_ring=1 rec_mb=256 rec_step=4";
- judge: the 89%,70% window (arm C baseline 28.8 fps), clock reads, health. One
  variable at a time: CA_VSYNC_OFF alone on top of arm C.

### The CA knobs measured (2026-11-01 late): both inert

CA_VSYNC_OFF=1 (run 20261101-222657) and CA_VELOCITY_FRAME_RATE_UNCONSTRAINED=1
(run 20261101-223346), each on top of arm C via backboardd's plist
EnvironmentVariables: both runs clean, the 89%,70% window 28.5 / 28.3 fps
(arm C 28.8), latency 26.0 / 24.5 ms, all gestures 9-10 %/2 %. Neither knob
touches the animation-timer backoff — consistent with the mechanism (the lock
lives in the timer's own scheduling, not the frame-attach or velocity paths).
Caveat, recorded honestly: the env channel itself is proven (FD_MESA_DEBUG,
10-28) but CA does not print its env, so "the knob reached CA" has no direct
evidence; a CA_PRINT_* run would witness it if a future experiment depends on
these knobs. Root volume for the last run: sha256 9fe65f9d… (plist carries the
velocity knob; revert to no CA_* key for the daily recipe).

### The AppleDisplay class named; the refresh-multiplier cluster; the immutable-display check (2026-11-02)

The QuartzCore database was rebuilt cleanly (`run-quartzcore-full.cmd`, idat -A,
input `quartzcore.text.bin`, 14,916 functions, imagebase 0) after the old .i64
was found not-closed; IDA 9.3 attached via rea works again.

**Confirmed facts (addresses, code, bytes)**

- **Vtable 0x1D5B44530 = CA::WindowServer::AppleDisplay.** The factory
  `sub_1883A618C` (0x1883a618c; the old 0x1883a6380 is inside its tail) walks
  IOKit display services (`IOServiceMatching`/`GetMatchingServices`/`IteratorNext`
  thunks at 0x18DBB81E4/0x18DBB62D0/0x18DBB62F4), matches the transport type via
  "external" (type 1) / "wireless" (type 2), allocates 2848 bytes
  (`MEMORY[0x18DBB90E4]`), calls the constructor `sub_1883A3EB0`, stores
  `*result = 0x1D5B44530`, and zeroes `result[355]` = **+0xB18** — the exact cache
  the period function reads. The constructor references
  "windowserver-apple-display.cpp", the kIOMFB* keys
  (kIOMFBSourceSurfaceWidth/HeightLimitKey, kIOMFBSourceRectangleLimit*,
  kIOMFBMaxCompressedSizeInBytes, kIOMFBSourceLimitsKey), CA_RESIZE_YUV_SCALE and
  CA_MAIN_DISPLAY_ORIENTATION. The second constructor `sub_1883A6468` (0x1883a6468;
  old 0x1883a64dc inside it) asserts "index <= count ()" at
  windowserver-apple-display.cpp:2622, dispatch_once-installs vtable **0x1D5B444E0**,
  and calls the same factory with type 2 — a sibling AppleDisplay variant.
- **Vtable decode.** Slots in the dylddata subcache (dyld_shared_cache_arm64.27)
  are chained fixups: raw slot & 0x1FFFFFFFF = target address (each raw is
  `0x2_00000000 | addr`; e.g. 0x1d5b44808 → 0x201882d3930 → sub_1882D3930, which
  confirms the RUNBOOK's period-function claim). Slot N = 0x1d5b44530 + 8N.
- **Neighbour slots around the period function are the refresh-rate multiplier
  cluster** (all decompiled, all vtable-dispatched):
  - slot 88 = 0x188527514, slot 89 = 0x1882b5518, slot 90 = 0x1885275a4,
    slot 91 = 0x1882d3930 (period), slot 92 = 0x1882b3ae0,
    slot 93 = 0x1883a3d7c, slot 94 = 0x18836dbe0.
  - slot 89: `if (byte[556]==1 && qword[560]==-1) return period() * slot92();`
    else slot88() — a derived period.
  - slot 90: `if (dword[544]&0x10 && sub_18839345C()) { if (!slot15()) slot94();
    period(); } else slot89();`
  - slot 92: `if (dword[544]&0x10 && qword[560] bit63) n = dword[2168] ? 4 : 2;
    return max(n, MEMORY[0x1E1F6B720]);` else slot93().
  - slot 93: `n = 1; if (dword[544]&0x10) n = (qword[560] >= 0) ? 1 : 2;
    return max(n, MEMORY[0x1E1F6B720]);`
  - Inference: this is CA's frame-rate / refresh multiplier (1x/2x/4x) capped by
    the global word 0x1E1F6B720; +544 bit 0x10 is the same flag the period
    function uses to pick the 2^-33 vs 2^-32 constant. The "2x" language matches
    our 33.3 ms lock — the lock may be arbitration, not a missing stream.
- **The immutable-display check in the CADisplayLink creation path**
  (`sub_18829E6D0`, 0x18829e6d0):
  `if ( (*(_WORD *)(*(_QWORD *)(v53 + 8) + 620LL) & 0x400) != 0 )` logs
  "CADisplayLink is created from an immutable display (display_id = %u) and will
  not receive vsync timings." (two channels, 0x188572b4c / 0x18859a206).
  Related strings: "cannot change properties on an immutable CADisplay",
  "cannot update immutable CADisplay", "immutableCopy",
  "CADisplayLink: reload_item, item does not exist",
  "CADisplayLink: new link for external display id %u",
  "AGX vsync hints will track display %u with %u Hz refresh rate".
  Callers: `sub_188351F10` (refcount wrapper — releases the link when the result's
  byte+12 != 1, i.e. immutable links are dropped) and `sub_188363D1C` (display
  enumeration). Inference: a display flagged at +0x26C bit 0x400 yields a display
  link with no vsync timings — the shape of our fallback. Whether our AppleDisplay
  gets the bit is unknown; the device logs do not contain the string (userspace
  os_log is probably not captured by our runs).

**Unknown / next**

- Who sets +0x26C bit 0x400: not in the AppleDisplay constructor (no write to
  620 found in 1581 decompiled lines); likely an update/snapshot path or the ObjC
  CADisplay immutable copy. The ObjC method-name strings have no code xrefs
  (metadata only), so the setter needs the display-update functions.
- The per-frame "completed/presented" bookkeeping is NOT in the AppleDisplay
  vtable neighbours — it must live in the display-link class whose timer locks.
  Next targets: the two link-creation callers (0x188351f18 done — a refcount
  wrapper; 0x188363fa8 = sub_188363D1C, 0x2dc, display enumeration) and the
  timer object they attach.
- The global 0x1E1F6B720 (cap?) and +2168 (0x878) bool semantics.
- The device side of the immutable check: a CA_PRINT_* style run, or an os_log
  capture, would tell whether the log fires on giulia at all.

### The vtable mapped; the multiplier cluster; the A11 identity gate (2026-11-02, continued)

All 119 slots of the AppleDisplay vtable were decompiled and the region decoded
from the dylddata subcache.

**Confirmed facts**

- The vtable at 0x1D5B44530 is one class and runs to ~0x1D5B44CE0 (≈246 function
  slots; slot N = 0x1d5b44530 + 8N). The header at vptr-16 is [0, 0] (RTTI
  stripped), so the boundary is the long TEXT run, not a typeinfo walk.
- Slot map (function pointers decoded from the cache, `raw & 0xFFFFFFFFF`):
  - 31 / 33 / 35 / 45 (0x774 each): windowserver-iomfb-display.cpp,
    `set_dmrr_enabled`, `CADeviceDMRRCapable()` — the base-class DMRR path.
  - 49 = sub_188523160 (0x1344): the display swap loop that reads CA_VSYNC_OFF
    (the RUNBOOK's earlier find) — a virtual method of this class.
  - 83 / 84 = sub_188526E1C / sub_1885270C8 (0x788 each): display mode switch
    ("Cannot switch display modes while powered off."); line 330 of both writes
    `*(_QWORD *)(display + 560) = mode_word` — **+560 is the display mode/refresh
    word**, written here.
  - 89–94: the refresh multiplier cluster (slot 89 = 0x1882b5518, 90 =
    0x1885275a4, 91 = 0x1882d3930 period, 92 = 0x1882b3ae0, 93 = 0x1883a3d7c,
    94 = 0x18836dbe0). Slots 92/93 return max(n, MEMORY[0x1E1F6B720]) with
    n = 1/2/4: n = 2 when +560 bit63 is set (or +2168 = 0), gated by +544 bit 4
    (the same flag the period function uses to pick 2^-33 vs 2^-32). The global
    0x1E1F6B720 is 0 in the shipped cache (all zeros 0x1e1f6b700-0x1e1f6b740).
  - 98 = sub_18838DE64 references the notification name
    com.apple.CoreAnimation.CAWindowServer.DisplayPower.
- **No vtable slot writes +620 (the immutable bit 0x400)** — the setter is not a
  virtual method of AppleDisplay; it is elsewhere (update/snapshot path).
- `now` audit (source, MiniIOMobileFramebuffer.cpp:1256-1343): selector 72 =
  kFBMethodNotifications stores fCallbacks/fRefcons; vsync() sends six 32-bit
  halves = (callback, refcon, now) with now = fVSyncTime in mach_absolute_time
  units (armVSync: mach_absolute_time + nanoseconds_to_absolutetime). The
  completion dispatcher reads the first word as the function pointer; the third
  word (time) is never consumed (B1), so the callback must call
  mach_absolute_time itself — the "which clock/unit" question is answered for the
  payload: mach_absolute_time, and it does not reach CA.

**Inference — the A11 identity gate (important for the callback plan)**

The driver comment (vsync(), 1313-1318) records that on the iPhone X identity
QuartzCore registers the notification port and never the callback: Apple never
ran its VBL callback path on A11. Our kernel is a real iPhone10,3 16.5 kernel, so
CA's capability checks take the A11 branch. **The "real A12+ VBL callback"
(RUNBOOK 2147-2150) may be unreachable under our device identity even if named** —
the registration call site would only run on a newer-model capability. Two ways
out: (a) find whether the gate is a model string or a capability bit we can flip
for the CA process only; (b) keep the no-op stream and instead attack the
multiplier/scheduling side.

### CA_ENABLE_FRAME_RATE_ARBITRATION=0 measured (2026-11-02): inert

- Preparation: the velocity knob removed, CA_ENABLE_FRAME_RATE_ARBITRATION=0 added
  to backboardd's EnvironmentVariables; root-daily.apfs rebuilt
  (`mkroot-manifest.py` + `mkapfs.py -z --plain fw`), sha256 feb73630…,
  check-image apfsck clean and 135/135 entries.
- **ACE5_CPUS lesson**: phone.py defaults ACE5_CPUS to 1. The first run
  (20261102-151720) went out as `cpus=1`: drags 23.8 fps, copy 36–53 ms, one
  backboardd SIGSEGV (respawned). Not comparable to any arm-C number. Arm
  comparisons must set `ACE5_CPUS=8` explicitly; the omission here was the
  session's, not the tool's.
- Valid run 20261102-151958 (cpus=8, ca_vbl=1 fb_minint=1, 900 s): health clean
  (0 PowerOff/panic/SIGSEGV; vsync armed), **17 "0 clock reads" still**.
  - CC window (89%,70%): 2 times, 60 frames at **28.9 fps / 32 % / 25 %**
    (arm C 28.8 / 35 / 27).
  - Drags: 55.9 / 57.9 / 59.2 / 57.7 fps — parity.
  - All gestures: 1450 steps, 9 % / 2 % (arm C 1459, 9 / 2).
  - End-to-end: **24.5 ms** (arm C 23.5) — parity. Recording
    20261102-152018-recording.raw.
- Verdict: **inert**, like CA_VSYNC_OFF and CA_VELOCITY_FRAME_RATE_UNCONSTRAINED.
  The transition 2x lock survives arbitration off. Caveat unchanged: no direct
  evidence the knob reached CA; a CA_PRINT_* run would witness it.
- Consequence: the multiplier cluster is not driven by this knob (or not reached);
  the display-link class that consumes slots 92/93 becomes the next static target.

### The display-link interval chain, and why the multiplier cancels (2026-11-02, continued)

Static read of the CADisplayTimer/display-link code (QuartzCore, same DB).

**Confirmed facts**

- **Interval chain** `sub_1883C6390` (display-link state update): it locks
  display+952, reads the link state at display+960 → {+40 phase, +56 timestamp,
  +4 atomic interval}, calls CADisplayTimerAlignPhase, then computes
  `interval = display->period() × display->multiplier()` (vtable+728 = slot 91,
  vtable+736 = slot 92) and stores it at link+520 (deadline at link+504,
  timestamp at link+512); a signpost (835321856) traces the value.
- **CADisplayTimer is a dispatch_source timer** (source at this+120).
  `sub_1882AB034` = CADisplayTimerAlignPhase: heartbeat=this+232, last=this+240,
  duration=this+208, deadline=this+224; it computes the phase index
  `((ts-last)/heartbeat) % 240` vs the incoming `*a2 % 240`, re-bases when the
  phase mismatches (or first run): `heartbeat = interval`,
  `last = ts - phase*interval`, `duration = interval*(duration/interval)`;
  logs "CADisplayTimerAlignPhase: unexpected state (interval:%llx, heartbeat:%llx)"
  when duration/interval == 0; re-arms with dispatch_source_set_timer.
  Callers: the timer fire (0x1882f682c), 0x1883315b4, 0x1883480a4, the timer
  creation (0x18837cf30), and the state update (0x1883c6390).
- **The multiplier's gate**: slot 92 returns 2/4 only when (+544 bit 4) AND
  (+560 bit 63); otherwise slot 93 returns max(1, MEMORY[0x1E1F6B720]) (the
  global is 0 in the shipped cache). **+544 bit 4 = MobileGestalt
  "SupportsPerseus"** (obfuscated key GdXjx1ixZYvN9Gg8iSf68A, iOS 10.3+;
  writer sub_1884C1F48 = the "System UI Framebuffer Compression Enabled"
  initializer; `CA_DISABLE_APT` clears it when sub_188549344() is true).
  The ctor seeds +544 and +2168 with the NaN pair 0x7FC000007FC00000
  (0x7FC00000 at +544 and +2168; bit 4 clear; +2168 truthy).
- **+560 is never written by the AppleDisplay ctor** — only the mode switch
  (slots 83/84) writes it, from the IOMFB display-config path (the same
  selector 27/17 that fb_cfg27 answered with zeros).
- **The multiplier cancels**: the period function halves its result when
  (+544 bit 4) && (+560 bit 63) (`return result * 0.5`), and the multiplier is
  then 2 — net interval = (period/2) × 2 = period. With bit 63 clear the net is
  period × 1 = period. **So the multiplier cluster does not change the effective
  frame interval**; it is bookkeeping for "2x" display modes (ProMotion/VRR), not
  the source of our 2x lock.

**Inference — where the 2x lock is NOT, and where it must be**

- Not in the display timer's period: it is 1/60 in both multiplier cases.
- Not in the display-config read: fb_cfg27 (zeros) left the window at 29.8 fps.
- The 33.3 ms cadence with "idle before draw 35.9 ms" (frames.py, the valid run)
  means the compositor waits ~2 beats for a commit — the app-side CADisplayLink
  (SpringBoard's render loop) commits every other beat. The next target is the
  client-side display link: where it gets its vsync phase from the server and
  how CADisplayTimerAlignPhase re-bases on our stream.

**Next static targets**: the timer fire (0x1882f682c) and the link creation
(0x18837cf30) — the fire path is where the per-frame bookkeeping lives; the
client display link (SpringBoard) is the cadence that actually locks.

### The frame-rate knobs are pinned on release builds; CADisplayTimingsControl (2026-11-02, continued)

**Confirmed facts**

- The three frame-rate getters all have the same shape:
  - CA_ENABLE_ALL_FRAME_RATES (env index 113, byte 0x1E1F6B801):
    `sub_1882CE9E0 = (MEMORY[0x1E1F6A1E8] != 0) | (byte & 1)`;
  - CA_ENABLE_FRAME_RATE_ARBITRATION (114, 0x1E1F6B802): `sub_18839345C`, same;
  - CA_ENABLE_REDUCED_SERVER_VSYNC_RENDER (116, 0x1E1F6B804): same shape
    (reader near 0x18832a244).
- **The global 0x1E1F6A1E8 = 1 on a release build**: `sub_188340DCC` writes
  `os_variant_has_internal_content("com.apple.coreanimation") ^ 1`. So the three
  getters return true whatever the env byte says — **these knobs are dead on our
  IPSW**. This is why CA_ENABLE_FRAME_RATE_ARBITRATION=0 was inert.
- The arbitration getter has 34 call sites. Two matter: AppleDisplay slot 90
  (the multiplier cluster, which cancels) and the **CADisplayTimingsControl**
  machinery: `sub_18832778C` (add_reason, list at this+160..176, max 8,
  "CADisplayTimingsControl too many reasons.") and `sub_1883AC178` (register a
  **CAFrameIntervalRange** request, sorted list at this+112..120, max 16,
  "CAFrameIntervalRange.minimum = 0 should not be registered.",
  "CADisplayTimingsControl too many requests.").
  Strings: overrideDisplayTimings:, copyDisplayTimingsFromDisplay:,
  setDigitalModes:withTimings:, copy_display_timings: unexpected state,
  timingsControl, "CADisplayTimingsControl too many reasons/requests".
- **CAFrameRateClient** feeds it: sub_1883421EC registers
  ("CAFrameRateClient: register %u %u %u") and sub_188306088 updates
  ("CAFrameRateClient: update %u %u %u to %u %u %u") a client's
  {min, max, preferred} range (client+44/+48/+52) with the timings control at
  display+408. This is the ProMotion panel-rate request system; a fixed-60 Hz
  panel has nothing to switch.
- The display-link interval chain is unaffected: period × multiplier cancels
  (previous section).

**Inference**

- The transition 2x is not in the period/multiplier and not in the env knobs as
  shipped. The frame-rate request system (CAFrameIntervalRange) is the plausible
  place a 30 Hz interval could be requested, but the static read shows no
  registration from our port; this needs runtime evidence.
- Unlocking the knobs would need `os_variant_has_internal_content` to return
  true for the CA domain: an interpose in backboardd (userland/src/inject.c) and
  SpringBoard (userland/src/inject_sb.c), both of which already carry a dyld
  `__DATA,__interpose` table.

**Device-needed (the static thread has reached its end)**

- (a) Interpose os_variant_has_internal_content (domain-scoped) → re-test
  CA_ENABLE_FRAME_RATE_ARBITRATION=0 / ALL_FRAME_RATES / REDUCED_SERVER_VSYNC_RENDER,
  each alone on arm C.
- (b) Or trace the real cadence: the timer signposts (835321856, 835322052) via
  kdebug, or instrument the driver to timestamp the server's tick.
Both need the phone; no further static step can separate them.

### Correction (2026-11-02, measured): the frame-rate knobs are NOT pinned — they are live

The "release pin" reading above is wrong. Measured on the phone:

- `sub_188340DCC` (the writer of the global 0x1E1F6A1E8) has **no direct callers**
  (xrefs 0), and the global stays 0, so the three getters return the environment
  bytes. The interpose of os_variant_has_internal_content in
  inject.c/inject_sb.c never fires (os_variant is never called); it stays in the
  libraries, inert, and the constructor prints `REDWOOD_CA_UNLOCK=...` (the
  plist environment channel is proven this way).
- **Evidence**: adding CA_ENABLE_ALL_FRAME_RATES=1 and
  CA_ENABLE_REDUCED_SERVER_VSYNC_RENDER=1 (backboardd + SpringBoard) **froze the
  compositor** in two consecutive runs — one vsync arming, 2 GL frames, the
  Apple logo left on the screen (not a panic: 0 real PowerOff/panic/SIGSEGV).
  The same libraries with those two knobs removed render normally: 232 armings,
  1,739 frames, CC window 30.4 fps / 33 % / 27 %, all gestures 1,467 steps 9 % / 2 %
  (run 20261102-164733 — the working baseline with the new libraries).
- Consequence: the earlier inert results (CA_VSYNC_OFF, velocity,
  CA_ENABLE_FRAME_RATE_ARBITRATION=0) are real — those knobs reached CA and did
  nothing. The knobs default to 0 (unset) and that is what every working run has
  used.
- **New clue**: REDUCED_SERVER_VSYNC_RENDER / ALL_FRAME_RATES = 1 stop the
  display link outright. The server's vsync render path is load-bearing for the
  frame clock; which of the two does it, and what the freeze leaves running, is
  the next one-knob-per-run question.
- **Isolation (2026-11-02, one knob per run, backboardd only, working config otherwise)**:
  - CA_ENABLE_REDUCED_SERVER_VSYNC_RENDER=1: **no freeze** — 235 armings,
    1,724 frames, health clean, gestures 1,449 steps 9 % / 3 % (run 20261102-165834).
  - CA_ENABLE_ALL_FRAME_RATES=1: **freeze** — 1 arming, 2 frames
    (run 20261102-170049). So the freeze is ALL_FRAME_RATES=1 in the window
    server alone; REDUCED_SERVER_VSYNC_RENDER is harmless alone, and the
    SpringBoard side was not needed for the freeze.
  - Reading: the frame-rate selection machinery (CADisplayTimingsControl /
    CAFrameRateClient) is load-bearing — allowed to pick freely, CA stops the
    display link altogether on this port (no frames), rather than picking a
    different rate. The 2x transition lock is a *constrained* choice of that
    same machinery; the unconstrained one is worse (zero).

### copy_display_timings: the link interval is an integer ratio between displays (2026-11-02, static)

- `sub_1883C5ADC` ("copy_display_timings: unexpected state (source: %x, this: %x)")
  builds and updates the display link's 64-byte timing state at display+960:
  - First time: `state+4 (interval) = sub_1882AAFC0(*(double *)(display+824))` —
    the display's period (seconds) converted to timer units; display+956 marks it
    initialized.
  - Then it compares with a **source display** (a2+96): `v12 = source interval`,
    `v13 = this interval`; if they are nearly an integer multiple
    (`max % min` within `min>>5`), it sets `v16 = round(v13/v12)` (or
    `1/round(v12/v13)`), stores it at display+972, and
    `state+4 = (u32)(v16 * v12)`.
  - **So the link's heartbeat is an integer multiple of the source's interval: a
    2x period mismatch between the two displays produces exactly the 2x lock.**
    The AlignPhase (previous section) consumes this state+4 interval.
- The writer of display+824 is not the AppleDisplay ctor and not among the 15
  direct stores at offset 824 in the scanned text (none in the ctor or the link
  code); likely a struct copy or another image. `sub_1883C5ADC` has no direct
  callers (vtable/function pointer).
- Next: name the source display (a2) and find where display+824 is set — the two
  operands of the ratio are the whole story of the 2x.
- **The +824 period is never initialized by the ctor chain**: the AppleDisplay
  ctor writes +808 (a dword) but nothing at +824, and the base ctor
  `sub_188413AFC` (vtable 0x1D5B38230) does not either. So it is 0 (fresh pages)
  or set by a later update path we have not decompiled. With +824 = 0 the
  first-time block of copy_display_timings is skipped and the interval depends
  on the source's interval and the ratio path — runtime values decide.
- The class owning copy_display_timings: **vtable 0x1D5B37000** (26 slots,
  header [0,0]; slot 4 = 0x1883c5adc; slots 0/18 are nulls; the methods are the
  contiguous 0x1883c5ab0-0x1883c60fc region). No constructor in the text stores
  that vptr (ADRP+ADD scan and 8-byte literal scan both empty) — the object is
  built by a path we have not named (another image, or a template copy).
- Static reading is done for this chain; the next evidence must be runtime: the
  two intervals (this +824 and the source's state+4) on a live run.

### The interval arithmetic measured live (2026-11-02): 60 Hz x 1, not 2x

- vtable probes in inject.c (backboardd): AppleDisplay slot 0x2d8 (period) and
  slot 0x2e0 (multiplier), logging once per change. On the working config:
  **`display period 0.016666667 s (60.000 Hz)`, `display multiplier 1`** — the
  display link's interval is 16.66 ms. The 2x is NOT in the period/multiplier
  arithmetic.
- The same probe in inject_sb.c (SpringBoard) fired **zero** times (and the
  patch reported no mismatch): the app process never calls those AppleDisplay
  slots — its display proxy is a different class, so the app-side cadence does
  not come from this vtable.
- The copy_display_timings probe (slot 4 of vtable 0x1D5B37000) also fired zero
  times in a healthy run — that function is not on our boot path either.
- Consequence: the 2x lock is downstream of the display interval — the app-side
  commit cadence (SpringBoard's render loop / its display proxy) or the
  AlignPhase scheduling, not the display period, multiplier, or the
  copy_display_timings ratio.
- Mechanism note (inject_sb.c's own comment, now confirmed twice): dyld
  interposing does not reach calls made inside the shared cache (one library
  branches straight into the other) — that is why the os_variant interpose never
  fired. Vtable patches and ObjC method replacement are the working mechanisms.

### The SpringBoard timings path is not used either (2026-11-02, measured)

- copy_display_timings patched in SpringBoard (inject_sb.c, slot 4 of
  0x1D5B37000) with a wrapper that logs the three values and forces the interval
  to the source's (ratio 1): the wrapper fired **zero** times in a healthy run
  (1,735 frames). The app never calls it.
- So the app's display-link cadence does not come from copy_display_timings
  either. With the server-side measurement (period 0.016666667 s, multiplier 1),
  the interval chain is exonerated on both sides.
- The forced-ratio fix is therefore inert (kept in the libraries, documented; it
  would act if the path ever runs).
- Remaining candidates for the transition cadence: the AlignPhase scheduling
  (sub_1882AB034 — direct calls, no vtable slot; needs inline code patching) and
  the app's actual display-link class (not AppleDisplay, not the 0x1D5B37000
  class).

## State of play (2026-11-02, end of session)

**Goal**: the transition 2x lock (Control Center retract ~30 fps; drags 60).

**Measured and ruled out today**

- **The interval chain, both sides**: the window server's display period is
  0.016666667 s (60.000 Hz) and the multiplier 1 (vtable probes in inject.c;
  run 20261102-172655) — the link interval is 16.66 ms. SpringBoard does not use
  the AppleDisplay period/multiplier slots at all (same probes; zero calls), and
  neither process calls copy_display_timings (probes; runs 20261102-172403 and
  20261102-174014). The integer-ratio scaling path is therefore not the 2x.
- **The frame-rate env knobs are live, not pinned**: the writer of the global
  0x1E1F6A1E8 has no callers; the plist environment reaches the processes
  (constructor log). CA_ENABLE_ALL_FRAME_RATES=1 in the window server alone
  freezes the display link (1 arming, 2 GL frames; run 20261102-170049);
  CA_ENABLE_REDUCED_SERVER_VSYNC_RENDER=1 is harmless (run 20261102-165834);
  CA_ENABLE_FRAME_RATE_ARBITRATION=0 is inert (run 20261102-151958). The
  working baseline with the new libraries is run 20261102-164733 (232 armings,
  1,739 frames, CC window 30.4 fps / 33 % / 27 %).
- **Mechanism notes**: dyld interposing does not reach calls made inside the
  shared cache — the os_variant interpose never fired and is inert; vtable
  patches and ObjC method replacement are the working mechanisms. phone.py's
  ACE5_CPUS defaults to 1 — arm comparisons must set `ACE5_CPUS=8`.

**Probes now in the tree (all log once per change, silent otherwise)**

- inject.c: AppleDisplay slots 0x2d8 (period) and 0x2e0 (multiplier), and slot
  0x20 of vtable 0x1D5B37000 (copy_display_timings).
- inject_sb.c: the same three for SpringBoard; the copy_display_timings wrapper
  also forces the interval to the source's (ratio 1) — inert while that path is
  not run.
- The CA-unlock interpose (os_variant_has_internal_content) stays in both
  libraries, inert (see above), with a constructor log of REDWOOD_CA_UNLOCK.

**What remains**

1. The app-side display-link class (not AppleDisplay, not the 0x1D5B37000
   class) — find what paces SpringBoard's commits.
2. The AlignPhase scheduling (sub_1882AB034 — direct calls, no vtable slot;
   needs inline code patching).

**Session commits**: 43b64cf, b64dfa4, 9e892ee, ec1b2dd, 9257e72, 4bd8413,
03fd573, 8e92854, 3ae4ea2, ea950a0, 4f7e1e2.

**Phone**: back in Android; loader workflow unchanged (enter → ramdisk → run;
`oem stop` returns the kernel early; the resident system disk survives runs).

### The transition lock: the app asks for 30 a second (2026-11-02 evening)

The ecosystem survey (full write-up: [docs/DISPLAY-LINK.zh-CN.md](docs/DISPLAY-LINK.zh-CN.md))
found the answer upstream, on the same iOS 16.5 line: `oosexgang/hackios-xnu-kernel`
`7caa457` (2026-11-01 06:05Z) traced the 0.7 s of 33.3 ms frames after a home-screen
swipe to **UIKit's `UIViewInProcessAnimationManager` display link asking CADisplayLink
for 20..30**; dropping the rates asked of that one link took the swipe from 78 to 115
fps and the frames over 16.7 ms from 17% to 1% (+126 lines, `userland/src/inject_sb.c`
only). Our 33.3 ms retract window is the same signature: 30 fps × the 60 Hz panel is
the multiplier-2 the AppleDisplay vtable cluster returns.

Ported into `userland/src/inject_sb.c` with attribution (new block after
`control_center_fades()`, called from `redwood_springboard_init()` right after
`setvbuf()`):

- `+[CADisplayLink displayLinkWithTarget:selector:]` marks links whose target is
  `UIViewInProcessAnimationManager`; `-setPreferredFramesPerSecond:` and
  `-setPreferredFrameRateRange:` pass 0 / {0,0,0} for a marked link and the original
  value for every other (the clock's 10 and 60, the motion effects' 32).
- `touch_lat=1` prints every link made and every rate asked, with the caller as
  `image+offset` (300 lines max). `dl_max=0` is the control; the default drops.
- `anim_max` (upstream `3639b98`) is NOT ported: its commit says OpenGL ES is
  unaffected, and it would contend with `cc_fade` for `addAnimation:forKey:`.

Built with the new `local/wsl/build-sb.sh` (only this dylib; the compile line is
`userland/build.sh:293`'s): `obj/libredwood-sb.dylib` 90,904 bytes. `local/root-daily.apfs`
repacked with the manifest (135 entries), sha256 `07d2f747…`, check-image apfsck clean
and 135/135 read back.

**Run (2026-11-02 18:32, log 20261102-183210).** The mechanism is confirmed verbatim:
`display links: made ok, fps ok, range ok; UIViewInProcessAnimationManager's at the
panel's rate (dl_max=1)`, links made for `-[UIViewInProcessAnimationManager
_displayLinkFire:]` by `UIKitCore+0x42fe34`, then `asked for 20..30, 0 a second
(dropped) by UIKitCore+0x415d78`; the clock's 10..120 and the 60s keep their rates.
**But the frame rate did not move.** Control run `dl_max=0` (log 20261102-183813),
same image: CC window 50.4 fps / 15 % / 6 % against 46.1 / 21 % / 9 % with the drop;
the pre-patch baseline 20261102-172655 was 51.9 / 9 % / 5 %. All within the day's
run-to-run spread (40.6–52.5 fps). Both runs clean (0 PowerOff/panic, vsync 233/214).

**Why the window never showed it — the metric.** `tools/frames.py` counts every
frame gap under 100 ms as movement. SpringBoard's idle commits after an animation are
~82 ms apart with 7–13 draws, so a 0.4 s animation at 60 fps plus 1.5 s of idling
averages to ~30 fps. With frames filtered to those that drew ≥40 things (`frames.py
--min-draws 40`, added this session): the CC animation is 16.0–16.7 ms in every run
since 11-01 (arm C 16.2, A1 with no ca_vbl 16.0, the 172655 baseline 16.7, both new
runs 16.2/16.4). `local/mine-retract.py` on the 10-31 "stall" window agrees (16 ms, one
32.7 ms hiccup). The only genuine 33.4 ms animation is 20261031-143416 — the run
before the injection callback fix. **So the 2x transition lock, as measured since
11-01, was the tool; the animation has been 60 fps.** What is left is the occasional
30–50 ms hiccup inside an animation (4–25 % of frames, run-dependent), not a rate
lock; AlignPhase is not needed for it.

**The interrupt case (user's report).** `redwood_input.c` gained boot-arg
`touch_apps=N` (this session): from the home screen, tap the four dock icons and
swipe home 120–180 ms later (mid-animation) or 400–500 ms later, N times, then the
five-finger exit. Log 20261102-190938 (120–180 ms): tap to first screen change 5–14 ms
(first cycle 166 ms, cold), launch segments 57–61 fps with 0 % over 33 ms, home swipes
59–61 fps with 0 % over 33 ms. The only slow one is the first cold launch (9 frames at
38.3 fps). At 120 ms the app window has not appeared yet (recording: icon grows, home
returns); at 400 ms it has (Messages' "Shared with You" ~320 ms after the tap).
**Music crashed with SIGSEGV on all three launches** (`UIKitApplication:com.apple.Music
… exited due to SIGSEGV`) — an app fault, not display.

### GPU clock ladder and the IOSurface hash's cost (2026-11-06)

Four runs, one script (`touch_auto=45 touch_auto_exit=1 glframes=1 touch_lat=1`,
`ca_vbl=1 fb_minint=1`), same kernel (`out/giulia-kernel/mach.autotest`, built from
the full `xnu/files` overlay) and root (`root-daily.apfs` with `libredwood-gl.dylib`).
The stage-15 line names the point:

| run | `ace5_gpu_speed` | stage 15 | frame | hashing | GPU wait |
| --- | --- | --- | --- | --- | --- |
| 20261106-210254 | 1 | point 1, 231000 kHz | 2812 µs | 330 µs | 445 µs |
| 20261106-210615 | 6 | point 6, 578000 kHz | 2861 µs | 332 µs | 508 µs |
| 20261106-210931 | 12 | point 12, 903000 kHz | 2814 µs | 330 µs | 516 µs |
| 20261106-211230 | 1, 4-lane hash | point 1, 231000 kHz | 2691 µs | 162 µs | 445 µs |

Gesture fps and the >16.7 ms share do not move across the ladder (58–59 fps on the
swipes, 4 % / 2 % all gestures); the clock is not this path's limit at 231 MHz.
903 MHz ran clean (0 errors, no hang). Per-frame attribution: `hashing` 330 µs,
`mirroring` 1 µs, `uploading` 5–6 µs, `attach` ~10 µs — the old "792 µs making
textures" is mostly the whole-surface hash, and uploads of 5 µs say almost every
hash concludes "unchanged". Four independent lanes (full coverage, same mix) halve
it to 162 µs; frame time −121 µs, no stale textures, the same copy counts
(50 copied / 4 shared). End screenshot 20261106-211230-screen.png: home screen
correct.

### PCIe M1 step 1, first run: the loader's USB went away (2026-11-06 evening)

`run --seconds 180 --bootcpu 1 --pcie 1 --kernel out/giulia-kernel/mach.autotest
--root-image local/root-daily.apfs` (no `--touch`, no GPU flags — a deviation from
the 10-30 recipe). The payload uploaded; `fastboot getvar result` timed out 20 s
later and the loader's USB disappeared. The phone then enumerated as "Qualcomm
HS-USB Diagnostics 900E (COM11)"; neither adb nor fastboot sees it. **Physical
power cycle needed**; no run log exists to read back.

Hypothesis (unverified): step 1 read the PARF/PHY/DBI/`pcie_sm` windows while the
PCIe core's clocks/GDSC may have been off, and a read of an unclocked window can
hang the AHB. Window reads are now **opt-in** (`pcie_windows=1`) and, when asked
for, guarded by `windows_clocked()` (core GDSC bit 31 set and `SLV_HALT` bit 31
clear, both from always-on GCC). Without the flag step 1 reads only GCC/TLMM and
says so. Next run: the validated recipe WITH `--touch --gpu-probe 20 --gpu-direct
--gpu-keep`.

### PCIe M1, 2026-11-06: `pcie_step` in the boot path kills the loader's USB (paused)

Four runs carried `pcie_step=1`; every one lost the loader's USB inside ~20 s and
ended in Qualcomm diag 900E (physical power cycle each time):

| run | config | outcome |
| --- | --- | --- |
| ~21:40 | `--seconds 180`, no GPU flags | USB gone; no log |
| ~21:55 | `--seconds 240`, GPU recipe | USB gone; logdump later showed the kernel returned, 1.44 MB log, **no `pcie:` line at all** |
| ~22:0x | `--seconds 300`, map-gated (GCC/TLMM only) | same: kernel returned, log complete, **no `pcie:` line** (this build had no init prints) |
| ~22:5x | `--seconds 120`, init-print build | USB gone; logdump stayed on the previous run (stale), no evidence |

The same recipe **without** `pcie_step` runs and returns normally (six runs the
same evening). So the boot-path PCIe probe is the variable, and the one kernel log
we have shows the pcie thread printed nothing — either `giulia_pcie_init` never
starts it or the boot-arg parse fails; a run that dies cannot decide which.

**M1 is paused.** The next design must not run PCIe MMIO from the boot path:
trigger the survey at runtime (a device ioctl from userland) or from the
loader/UEFI, so a hang leaves the boot log readable. No more blind pcie runs.

### PCIe M1 root cause: the TLMM GPIO bank, and K90's structure adopted (2026-11-07)

The family survey (K90/thyme/dada/main line; the table in the session log,
details in docs/PCIe-DIFF.md §6–§8) settled how PCIe bring-up is triggered
everywhere: a boot arg selects staged steps, a dedicated kernel worker thread
does every MMIO access, root-complex reads are gated on both GDSCs and four
clocks, and `pcie_pause=N` sleeps before each stage so the host keeps the
previous stage's log. Nobody triggers from userland or the loader — so the
"runtime ioctl / loader-side" redesign is withdrawn; what was missing is the
gating and one address.

K90's `mhi_pcie_power.c` also records our exact failure shape: their kernel
`76c1ad6` stopped the SoC reading a pin at a wrong TLMM base. Ours was wrong
the same way: the device tree's pinctrl is `/soc/pinctrl@f000000` (0x1000000
long), but the per-pin CFG/IO blocks start at **+0x100000** — the address
`giulia_s3910_hw.h` calls `S39_TLMM_PA` (0xf100000). `giulia_pcie.c` was
reading GPIO 94's config at 0xf05e000, inside other tiles' registers, in every
"safe" run too.

`giulia_pcie.c` now follows K90's structure with this board's numbers:

- **step 1** gcc/tlmm only, TLMM mapped at the GPIO bank (`GIULIA_TLMM_GPIO
  0x0f100000`, new in giulia_pcie_regs.h).
- **step 2** the root complex, 30 s later (`pcie_pause`) and only when
  `windows_clocked()` — both GDSCs on, `cfg_ahb/slv_axi/mstr_axi/aux` running.
- **step 3** the power-up: the seven supplies the cnss-kiwi node names
  (smpc2/smpc3/ldob15/ldoc3/smpc6/smpc1/smpi4 — cmd-db names straight from the
  `-supply` phandles' target nodes), voted through the apps sequencer at
  0x18220000 DRV 2, TCS 1 (the same sequencer redwood_adsp.c uses; TCS 0 is
  its); PERST/WLAN_EN low, GDSCs, clocks, resets, `parf_setup` (pcie-qcom
  init_2_7_0 with our DBI 0x60000000 and iATU 0x60001000), the vendor PHY
  sequence, WLAN_EN high → 100 ms → `root_port_setup` → PERST high →
  `LTSSM_EN` → link poll with the LTSSM state printed; on a live link the
  step-2 survey runs.

`pcie_pause=N` (seconds, default 30 before the step-2 reads) is the
escape hatch that keeps a stalled run diagnosable: the host pulls the log

### PCIe M1, 2026-11-07 continued: the supplies vote confirmed end to end (runs 13:12-14:47)

With the TLMM fix in, one run per finding, each visible live through pcie_log:

- **step 1 green** (run 20261107-122230): cold registers read, loader alive the
  whole run — the GPIO-bank fix retired the 900E class.
- **step 2 green** (run 20261107-123736): the gate refused to read the
  powered-off windows (gdsc 0x22f801, halts all set) — exactly the design.
- **step 3's supply stage green** (run 20261107-140902 after the TCS rewrite
  `8d93e37`): all seven cnss-kiwi rails plus rfclka1 found in cmd-db at
  0x81c60000, and each rail voted `RPMh HAS DONE IT after 100 us` through the
  apps RSC DRV 2 slot the s39 discipline picked. The card's power is real.
- Three of our own bugs fixed on the way: the vote overwrote the armed slot's
  enable register (must OR, per __tcs_buffer_write), the timeout path skipped
  the reclaim (a dead transfer left armed wedged the DRV2 sequencer — the
  14:29 run's `s3910 power rc=-13` and the GPU ZAP auth timeout behind it),
  and the IRQ registers are inside the TCS block at 0xd00, not at the RSC base.
- **Where it stops**: after the supplies, the GDSC stage fails — the DTB shows
  both PCIe GDSCs are VOTABLE (`qcom,collapse-vote` = syscon@15214c bit 0 / bit
  3, the same GCC+0x5214c K90 clears with bits 0/2 on SM8750), the collapse
  clear is now in, but GDSCR still reads 0x22f801 after the clear — the
  CFG-GDSCR power-up bit position on SM8650 is unknown (K90's 1<<0 / 1<<2 are
  a guess). The diagnostic prints gdscr+4 on every timeout.
- **Cross-boot state discovered**: the GPU CX GDSC and the PCIe supply rails
  keep their state across a loader return — the 14:47 run's GPU probe refused
  ("CX already active") because the 14:29 kernel panicked with CX on. The A/B
  decomposition (`pcie_supplies=0/1`, `pcie_gdsc=0/1`, one kernel covers all
  cells) must run each cell after a full reboot to Android.
- A 200 ms timeout is also too short for a first vote; next round polls 400 ms
  and additionally accepts the command status word's own complete echo as a
  secondary completion signal.

### PCIe M1, 2026-11-07 evening: the wedge isolated to the window mappings

The A/B decomposition went one cell deeper and changed the picture again. Cell B
(`pcie_step=3 pcie_supplies=0 pcie_gdsc=0`: PERST/WLAN_EN driven low, the four
PCIe windows mapped, no reads at all) wedged the loader exactly like every other
`pcie_step=3` run -- while step 1, which maps only GCC/TLMM, always survived.
The remaining difference is the **Device mappings themselves**: mapping an
unclocked DBI/PHY window invites speculative reads that external-abort the bus
(seven wedged runs, all mapping the windows; every surviving run did not).

Fix (`09386ad`): power_up creates the window mappings right after clocks_on
confirms the four PCIe clocks are running; survey_step2 maps behind its
windows_clocked() gate; the boot path never maps a PCIe window. Two more
findings from the same round: the seven supplies voted HAS DONE IT again on a
fresh boot (reproduced), and the SM sequences (branch 7 words at pcie_sm+0x1000,
start 13 words at pcie_sm+0, START=1 at pcie_sm+0x1090) are now wired into
power_up ahead of the link poll -- the vendor driver's state-machine start that
the mainline pcie-qcom port lacks.

Next round: one run of `--pcie 3` on a fresh boot. Expected path: supplies ->
GDSC (collapse-vote clear + CFG BIT(16) poll) -> clocks -> window maps -> SM
sequences -> PHY -> WLAN_EN -> root port -> PERST -> SM start + LTSSM_EN ->
link poll. Every stage names itself in the log.
before the risky stage.

### PCIe M1, 2026-11-08 morning (host-side): the ladder reached the link poll clean, LTSSM stuck at 0; the refclk gate prepared

The 21:55 run (build e66f28, the `09386ad` window-map fix in) is the round the
"next round" paragraph asked for, and it changes the picture: the whole ladder
ran without a wedge on a fresh boot — supplies HAS DONE IT, both GDSCs on,
clocks voted, resets pulsed, parf set up, the PHY sequence, WLAN_EN high
(`sw-ctrl pins read 0 0`), root port, PERST, SM start + LTSSM_EN — and the
link poll reports `link training, state machine 0`, then
`link still down (state machine 0, link status 0x10110008, speed=1 width=1)`,
`step 3 finished rc=60`. The window-map fix held. The card simply never
answers: the LTSSM never leaves state 0.

The 22:00 run (build e66f40) carries no verdict: the kernel hung during the
GPU probe (stackshot) before the pcie thread printed anything. A re-enter
right after the 21:55 run, so the cross-boot CX/GDSC state is the prime
suspect again — the next `--pcie 3` round must start from a full reboot to
Android. The phone was not enumerable on USB at all on 11-08 morning (adb,
fastboot and the Windows device list all empty); physical recovery pending,
so the round has not run.

The refclk hypothesis, built and address-verified but not yet measured: with
the supplies and GDSCs on and the controller glue up, the remaining reason
PERST does nothing is that the card has no reference clock — the TCSR gate
`tcsr_pcie_0_clkref_en` (absolute 0x1ff1100, enable BIT(0)) is off. Two
sources agree on the absolute address: the OnePlus sm8650 kernel
(clock-controller@1f40000, +0xb1100) and mainline tcsrcc-sm8650.c (enable_reg
0x31100 on the qcom,sm8650-tcsr syscon at 0x1fc0000). Mainline's halt_check
is BRANCH_HALT_DELAY — write the bit and move on, no poll. `power_up` now
maps TCSRCC and sets the gate right after the card's-enable pause, before
WLAN_EN goes high, and prints the register value it read — a run can tell
"Android already had it on" (the cross-boot state lesson) from our write.

Build note: the bootstrap config's `check_all_exports` now fails
deterministically (BUILD_EXIT=2, `exported name not in import list:
IOService::…`), but the kernel proper links and the mach.autotest artifact
carries the new code (verified by strings). Recorded, not yet chased.

### PCIe M1, 2026-11-08 11:03: the refclk gate moves the LTSSM off zero — Detect.Active, not a link yet

One fresh-boot run of `--pcie 3` (cmdline identical to the 21:55 recipe,
mach.autotest with the refclk build; healthy return, no wedge, the GPU probe
ran all 20 stages green and the desktop came up). The fresh ladder: supplies
HAS DONE IT → both GDSCs → clocks → resets + parf → `phy ready (0x41)` →
**`the card's refclk gate is on (0x1)`** → wlan_en high → root port → PERST →
SM start + LTSSM_EN → poll. The verdict line:

- yesterday (no refclk code, fresh boot): `state machine 0`, dead — the
  LTSSM never left Detect.Quiet;
- today: `state machine 0` → **`state machine 0x1` after 90 ms**, and it sat
  there for the whole 10×90 ms poll (`link still down (state machine 0x1,
  link status 0x10110008, speed=1 width=1)`, rc=60).

State 0x1 is Detect.Active: the host PHY is cycling receiver detection and
never finds the card's termination — the card is electrically alive at the
wires but its receiver never shows up. The refclk gate is the one variable
that moved the state machine, so the gate write landed. Attribution caveat:
the line prints only the post-write value, so "Android had it on already" is
not excluded by this run alone — the next build prints the pre-write value
(0 or 1) to settle it.

Next round's probes (logging only, no behavior change, already in the tree):
the refclk gate's pre-write value, and the PERST/WLAN_EN pin readback right
after PERST is released (`perst in=N wlan_en in=N` — GPIO 94 / GPIO 16, the
IN register bit 0; the step-3 path never printed pin states before). If the
gate was 0 and the pins read back driven, the remaining suspects for a
Detect.Active stall are, cheapest first: the PERST source itself (is TLMM
GPIO 94 what the card's slot actually sees — confirm against the cnss-kiwi
node on host before touching code), and the card-side refclk delivery (the
gate routes TCXO, but the connector's 100 MHz needs the pad/clkreq path
too).

### PCIe M1, 2026-11-08 11:18: both readbacks answer — the gate write is ours, and the pins never drove

One more run of the same recipe (`20261108-111854`, back-to-back, healthy
return again). The two readbacks settle both open questions at once:

- **`the card's refclk gate pre=0 post=0x1`** — Android boots with the gate
  off; the write in power_up is what turns it on. The refclk hypothesis is
  now fully attributed: gate off → LTSSM Detect.Quiet, gate on → the card
  shows up at Detect.Active. Reproduced across two runs.
- **`perst released; perst in=0 wlan_en in=0`** — driving PERST/WLAN_EN high
  leaves both pads reading 0. The card has never been enabled or released
  from reset in any run so far; Detect.Active is exactly what a reset card
  looks like to the host PHY.

The pad readback led to the root cause: `gpio_out` assumed the old TLMM
GPIO_CFG layout (function bits [1:0], OE bit 2, drive [8:6], pull [10:9]).
Mainline pinctrl-sm8650.c says this generation is different: pull [1:0],
**function [5:2]** (gpio = 0), drive [8:6], **output enable bit 9**. The old
write `(cfg & ~0x7ff) | 0x4 | (6<<6)` cleared the real OE bit, set bit 2
which is now the function field's LSB — function 1, not GPIO — and never
enabled the output. The IN/OUT register bits (IN 0, OUT 1) were right all
along, which is why the readbacks looked self-consistent while the pads
stayed inputs.

`gpio_out` is fixed to the sm8650 layout (`(cfg & ~0x3ff) | (6<<6) | BIT(9)`:
gpio function, no pull, drive 6, OE on). The phone's own DTB independently
carries `clock-controller@1f40000 / qcom,pineapple-tcsrcc` and
`pcie_clkref_en`, so the TCSRCC address now has three agreeing sources.

Next round: one fresh-boot `--pcie 3` with the fixed gpio_out. Expect
`perst in=1 wlan_en in=1`; the interesting question is whether the link
trains past Detect.Active once the card is actually out of reset with its
refclk on.

### PCIe M1, 2026-11-08 11:37: the pads drive for the first time — still Detect.Active, the software side is done

One fresh-boot run of `--pcie 3` with the fixed `gpio_out`
(`20261108-113747`, healthy return, GPU all green, desktop up). A note on
method: the first attempt at this round (`20261108-112818`) was a re-enter on
the round-2 loader session — `enter` failed, the run was launched anyway —
and it hung at GPU stage 17 with a stackshot and zero pcie lines, the same
shape as the 11-07 22:00 run. Not a verdict; the step-3 rounds really do
need the full reboot to Android first, and the fresh re-run confirms it.

The verdict lines:

- **`perst released; perst in=1 wlan_en in=1`** — the fixed `gpio_out`
  works; for the first time in this whole effort PERST is actually
  deasserted and WLAN_EN actually high at the pads.
- `refclk gate pre=0 post=0x1` — reproduced a third time.
- The link poll: `state machine 0` → `0x1 after 90 ms`, stuck there through
  the whole 10×90 ms window, `link status 0x10110008, speed=1 width=1`,
  rc=60 — **identical to the run with the pads dead**.

So the complete software ladder — supplies voted, GDSCs on, clocks on,
resets + parf, PHY ready (0x41), refclk gate on, WLAN_EN high, PERST
deasserted, SM start, LTSSM_EN — lands the machine in Detect.Active and no
further. The card's termination never appears. The state is fully
attributed now: yesterday's Detect.Quiet was the refclk gate; today's
Detect.Active stall is the first state that requires the card itself to
answer, and it does not.

What is left is the electrical layer, cheapest probe first: (1) the seven
rails' actual voltages — the log confirms smpc2 at 980 mV but says nothing
about the rest; the vote path may have enabled rails without setting their
voltages; (2) the refclk routing — the TCSR gate turns the clock on at the
controller, but the pad/clkreq path to the chip (and whether the WCN's own
xtal/ln_bb_clk needs a second gate) is unverified; (3) whether the soldered
WCN7850's PCIe really hangs off pcie0's lanes as the DTB says — a host-side
re-read of the cnss-kiwi node's ranges/phandles against what power_up
drives. None of these is testable from the current prints; the next build
should log every rail's requested voltage next to its confirmation.

### PCIe M1, 2026-11-08 afternoon (host-side): the rail-voltage suspect dies, the vendor driver's sequence is diffed

The rail-voltage suspect is closed by the logs we already had: all seven
rails confirmed with voltages (`smpc2 on at 980 mV: confirmed` …
`smpi4 on at 938 mV: confirmed`) — the "only smpc2" reading was a truncated
tail. The phone's own DTB independently agrees with every value
(`qcom,vdd-wlan-*-config` = the same microvolts).

The DTB re-read (host, authoritative for this board):

- `qcom,wlan-rc-num = 0` — the WLAN is on pcie0; the root complex is right.
- cnss-kiwi drives four GPIOs: wlan-en 16 (we drive), bt-en 17, sw-ctrl 18,
  wlan-sw-ctrl 19 (we only read the last two). The vendor cnss2 driver
  (`power.c`) drives **only wlan_en** at power-on; bt_en is touched only for
  QCA6490 and sw_ctrl is read-only there — so our GPIO set now matches the
  kernel behavior.
- `reg` pair 7: `pcie_sm @ 0x1d07000` — our SM base matches the DT.
- No `qcom,tcsr` table in the pcie node, so the vendor's `pcie_tcsr_init`
  writes nothing on this board either.

The vendor `pci-msm.c` (the driver this board's `qcom,pci-msm` compatible
selects) diffed against our ladder: our parf_setup already covers
DEVICE_TYPE=4, PHY_CTRL BIT(0) clear, DBI/ATU/SLV bases, MHI BYPASS bit,
PM_CTRL BIT(5), WR_HALT BIT(31); the SM seq→branch→START matches the
vendor's state-manager load. What we lacked, all before PERST release:

1. **PARF_CFG_BITS_3 (0x2c4): clear BIT(0), set BIT(8)** — "Disable override
   for fal10_veto logic to de-assert Qactive signal" + dynamic width
   upscaling;
2. **DBI 0x80c (GEN3_GEN2_CTRL) bits [12:8] = 1** — the controller's own
   Gen1 target (we only wrote LNKCTL2 TLS);
3. **DBI 0x714 (LANE_SKEW_OFF) BIT(5)** — width-upscale support.

Plus a blind spot: the vendor judges the link on **ELBI_SYS_STTS** (ELBI at
DBI+0xf20 per the DT, SYS_STTS at +0x08; LTSSM state bits [17:12],
XMLH_LINK_UP BIT(10)), not on the PARF_LTSSM readback our poll uses. All
four are in this round's build: the three writes (each logged with the
readback) and the poll/final line now print the ELBI value next to the PARF
state. Deliberately NOT this round: the vendor's full
`PARF_SYS_CTRL = 0x365E` overwrite (our bit-29 clear from upstream
pcie-qcom stays — one bundle per round; SYS_CTRL is the next candidate if
this one fails).

### PCIe M1, 2026-11-08 12:14: the vendor's pre-training writes land — still Detect.Active

One fresh-boot run of `--pcie 3` (`20261108-121442`, healthy return, GPU all
green). All three vendor writes landed and read back:

- `vendor pre-train: cfg_bits_3 0x168 gen3_gen2 0x130190 lane_skew 0x8000020`
  — BIT(0) cleared and BIT(8) set in CFG_BITS_3, the controller's Gen1
  target in GEN3_GEN2_CTRL, the lane-skew override bit on;
- `wlan_en high; sw-ctrl pins read 1 1` — the sw-ctrl pads now read high
  (0 0 in every earlier round), consistent with the card side actually
  being alive for the first time;
- the ELBI double-read works: `parf state 0 elbi 0x1` → `parf 0x1 elbi
  0x1001` after 90 ms, `link still down (parf 0x1 elbi 0x1001 …)`, rc=60.

The vendor's own LTSSM name table (`msm_pcie_ltssm_str`) says 0x1 is
LTSSM_DETECT_ACT on this exact hardware — the receiver detect never finds
the card's termination even with the full pre-training write set in place.
XMLH_LINK_UP (ELSI bit 10) never lights.

The software-diff well against the vendor driver is nearly dry: the one
structural write left is the full `PARF_SYS_CTRL = 0x365E` (our upstream
port only clears MAC_PHY_POWERDOWN_IN_P2 bit 29; the vendor replaces the
whole register on every enable — and 0x365E keeps bit 29 clear, so the two
agree on the one bit we knew about). Round 5 adds that write plus a finer
poll (10 ms × 90 = the same 900 ms window at 9× the temporal resolution, so
a transient Polling excursion cannot hide between samples). If that round
also sits in Detect.Active, the software register diff is exhausted and the
remaining suspects are PHY-domain: the aux-clock mux switch (xo → ext
source, vendor does it after phy_init) and the pipe-clock gating.

### PCIe M1, 2026-11-08 12:23: SYS_CTRL lands, the register diff is dry — the PHY clock domain is the next suspect

Round 5 (`20261108-122323`, healthy return): `sys_ctrl 0x361c -> 0x365e`
landed — Android leaves 0x361c, the vendor value additionally sets bits 1
and 6 — and the finer 10 ms poll shows no hidden transient: `parf 0` /
`elbi 0x1` at 0 ms, `0x1`/`0x1001` from 40 ms on, no Polling excursion, no
XMLH_LINK_UP. Every register write the vendor driver performs before
training is now in our ladder and the LTSSM still sits in
LTSSM_DETECT_ACT (the vendor's own name table confirms the encoding).

The register diff against `pci-msm.c` is exhausted. What the vendor never
writes but the *kernel clock tree* does — via the QMP PHY driver — is the
PHY's own clock domain. Mainline sm8650's `pcie0_phy` node demands five
clocks: aux, cfg_ahb, **ref (`<&tcsr TCSR_PCIE_0_CLKREF_EN>` — our refclk
gate, already equivalent)**, **rchng at 100 MHz**, and **pipe**. Two of
those five were never in our ladder:

- **GCC_PCIE_0_PHY_RCHNG_CLK** — the recharge domain that runs the receiver
  detect circuit: RCG at 0x6b058 (`GPLL0_EVEN / 3 = 100 MHz`, cfg 0x602),
  branch vote 0x52000 BIT(22);
- **GCC_PCIE_0_PIPE_CLK** — the PHY→controller PCLK gate, our own vote
  register's bit 4 at 0x52008, deliberately excluded so far ("all but
  PIPE"; mainline marks the branch BRANCH_HALT_SKIP — it only ticks once
  the PHY is out of reset, and the kernel's bulk-enable order turns it on
  before calibration).

The mechanism story fits every observation so far: the refclk gate turns
the PHY's PLL on, the pipe clock starts ticking, and the LTSSM leaves
Detect.Quiet for Detect.Active at exactly that moment (rounds 1-3) — but
without the recharge clock the receiver detect can never complete. Round 6
enables both: the RCG config + branch vote with logged readbacks in
clocks_on, and the pipe bit added to the vote word.

### PCIe M1, 2026-11-08 12:37: the PHY clock domain is on — Detect.Active stands, the register playbook is exhausted

Round 6 (`20261108-123753`, fresh full cycle, healthy return, GPU all
green). The clock word now reads `0x423c023f` — the pipe bit (0x52008
BIT(4)) is in the vote — and the recharge clock:

- `rchng vote pre=0xe0a57f post=0xe0a57f, halt 0` — the branch was **already
  voted** (Android left it on), so this was at most a rate problem; the RCG
  is now configured for GPLL0_EVEN/3 = 100 MHz and the halt status reads 0
  (running). The pipe gate is voted with the same bulk-enable order the QMP
  driver uses (before calibration).

The verdict is unchanged: `parf 0x1 / elbi 0x1001` from 40 ms on, stuck in
LTSSM_DETECT_ACT through the whole window, rc=60.

At this point every item on the software checklist is in place and carries a
readback: seven supplies with DT-matching voltages, both GDSCs, the five RC
clocks plus pipe plus a running 100 MHz rchng, the TCSR refclk gate (pre=0 →
post=1, three runs), a ready PHY, the CESTA/SM sequence with START, the
complete vendor PARF write set (SYS_CTRL 0x365e included), PERST deasserted
and WLAN_EN high at the pads. The register-level diff against the vendor
driver is exhausted; the 0 → 0x1 transition at refclk time remains the only
state the software ever moved.

What is left, cheapest first, is diagnostic rather than corrective:

1. **The PARF test bus** — the DT gives `qcom,num-parf-testbus-sel = 0xb9`
   and `qcom,parf-debug-reg`, and the vendor's `pcie_parf_dump` sweeps
   PARF_SYS_CTRL's testbus selector (bits [23:16]) reading PARF_TEST_BUS:
   the only window that can SHOW what the detect circuit and the PHY think
   is happening instead of guessing from state registers.
2. The aux-clock mux switch (xo → ext source) the vendor does after
   phy_init.
3. Re-examining the one thing nobody can verify from software: whether
   GPIO 94's pad is actually wired to the WCN's PERST pin on this board,
   and whether the TCSR gate's 100 MHz actually reaches the chip — both
   board truths the DT asserts but nothing has measured.

### PCIe M1, 2026-11-08 12:48: the test bus is silent, but the card shows signs of life

Round 7 (`20261108-124836`, fresh cycle, healthy return) ran the first
instrument: the full PARF test-bus sweep (185 selectors, SYS_CTRL [23:16] →
0xe4) plus the vendor's debug-register snapshots. Two findings:

- **Every one of the 185 test-bus selectors reads 0** in the Detect.Active
  state — the bus carries nothing. Either the detect circuitry genuinely
  has no activity to show, or the bus needs a condition we don't meet; a
  before/after delta sweep (power_up entry vs the failed poll) is the
  cheap way to tell those apart next round.
- The debug snapshots are on the record for future comparison:
  `parf[0x24]=0x80000400` (PHY_CTRL: our bit-0 clear took, bit 10 and 31
  set), `parf[0x28]=0x4802` (PHY_STATUS), `parf[0x230]=0xb00`,
  `parf[0x4d0]=0x1a`, `parf[0x4d4]=0x2f20`, `parf[0x630]=0x3030`, and the
  PHY window `0x1d0..0x1f8 = a3 38 57 73 01 82 01`.
- **`wlan_en high; sw-ctrl pins read 1 1`** — GPIO 18/19
  (`mpm_wake_set_gpios` per the cnss-kiwi node: card-side lines we never
  drive) read high for the first time. They read 0 0 in every round before
  the pads actually drove. The flip correlates with the card being enabled
  and released from reset: the card is alive, its rails are on, and it is
  driving something.

So the picture sharpens: enabled card, alive card, host side fully
configured per the vendor's own sequence — and the lanes still never
complete receiver detection. The remaining unknowns are exactly the two
board truths plus one pin nobody reads: **CLKREQ (GPIO 95)** — the card's
own open-drain clock-request line, whose level after PERST release would
say whether the chip's PCIe interface is powered enough to drive it. Next
round adds one log line (perst/wlan_en/clkreq/wake/sw-ctrl pin levels in
the same readback) before any further theory.

### PCIe M1, 2026-11-08 12:56 / 13:07: CLKREQ is low — the card is asking for its clock

Round 8 (`20261108-125633`) added the pin nobody read: **`clkreq in=0`**.
GPIO 95 (pcie0_clk_req_n, open-drain, pull-up per the DT's active pinctrl)
sits LOW after PERST release — the card is asserting its clock request.
With wake high and sw-ctrl 1 1, the chip is powered, out of reset and
asking for its 100 MHz; the TCSR gate is on; and the receiver detect still
never completes.

Round 9 (`20261108-130757`) closed the routing hole the DT exposed: the
active pinctrl muxes gpio95 to the `pcie0_clk_req_n` function (function 1,
pull-up) while Android's sleep state leaves it a GPIO, and our ladder never
touched the pin's function field. The mux write landed
(`clkreq pin muxed to pcie0_clk_req_n (cfg 0x7)`), CLKREQ still reads low,
the link still sits in Detect.Active — the pad now reaches the controller
and that alone doesn't unstick it either.

With the two remaining structural writes in sight from the family's only
working ladder (thyme's `umi_pcie.c`, the complete SM8250/QCA6390
implementation), round 10 fixes a deviation our own DT convicts:
`qcom,slv-addr-space-size = 0x4000000` — the vendor writes it to
PARF+0x358 and thyme writes 0x04000000 to the same register, but our
parf_setup wrote 0. Round 10 writes the DT's value. (Thyme's other deltas
— the kona-specific GCC 0x8c004 refclk branch and the DBI 0x154 = 0x55555555
VSEC write — are board data with no evidence on this SoC and stay out.)

### PCIe M1, 2026-11-08 13:21: round 10 — the SLV size lands, the day's ten rounds close

Round 10 (`20261108-132112`, fresh cycle, healthy return): `SLV
ADDR_SPACE_SIZE` now carries the DT's 0x4000000 (was 0), and the verdict is
unchanged — `parf 0x1 / elbi 0x1001`, Detect.Active, rc=60. Expected: the
SLV window configures AXI address decode, not the LTSSM; it was a
correctness fix against our own DT, not a candidate blocker.

Where ten rounds leave the day: every software action any evidence source
can justify is in the ladder with a readback — the seven WLAN rails with
DT-matching voltages, both GDSCs, five RC clocks plus the pipe gate plus a
running 100 MHz recharge clock, the TCSR refclk gate (0→1, four runs), a
ready PHY, the complete vendor PARF set (SYS_CTRL 0x365e included), the
CESTA/SM sequence, the DT's SLV size, the CLKREQ pad mux, PERST deasserted
and WLAN_EN high at the pads. The card is alive (sw-ctrl 1 1) and asking
for its clock (CLKREQ low). The LTSSM leaves Detect.Quiet exactly when the
refclk gate turns on and goes no further.

The two remaining unknowns are the ones no register write has addressed:
whether the gated 100 MHz actually reaches the chip (the card's persistent
CLKREQ request is the classic signature of a chip holding its interface
down for a clock that never arrives), and the board's PERST trace. The next
software probe, before any hardware conclusions: sweep the TCSR window for
further PCIe0 clock-control words beyond 0xb1100 bit 0 (pad/generator
enables the one gate doesn't cover), and check GCC for a
reference-generator branch analogous to kona's 0x8c004.

### PCIe M1, 2026-11-08 13:36-13:53: rounds 11-13 — the A/B settles the gate's role, and the PHY's status migrates when its ref arrives

Round 11 (`20261108-133648`, `pcie_clkref=0`, first run of the new A/B arg)
settled the standing question cleanly:

- **Without the gate write the LTSSM never leaves Detect.Quiet** — `parf 0
  / elbi 0x1` for the whole poll. The 0 → 0x1 move is the gate's, not a
  confound of the gpio fix; the TCSR gate feeds the host PHY's reference
  path. (The only gate-less run, yesterday 21:55, also had the broken
  gpio_out — the two changes had always ridden together until now.)
- The entry pin survey shows Android's handoff state: `wlan_en in=0`,
  `clkreq in=1` — the radio is powered down at handoff (WLAN_EN low,
  CLKREQ released) and the card only starts requesting its clock after our
  enable sequence pulls it up. Both TCSR tables (OnePlus and mainline)
  carry exactly one gate for pcie0 (no pad variant — only UFS has one), and
  the phone's DTB wires the gate to no consumer at all: Android never
  enables it across boots (pre=0, four runs) yet its Wi-Fi works.

Rounds 12-13 (`20261108-134554`, `20261108-135331`) instrumented the clock
domain and the PHY's status:

- `clock halts: pipe 0 rchng 0` — the pipe branch's CLK_OFF is clear: PCLK
  is running, the LTSSM is clocked. The dead-PCLK theory is out.
- **The PHY status migrates: `phy ready (0x41)` right after the sequence,
  then `0x86` by the PERST-release point and constant through the poll.**
  The flip window contains only the TCSR gate write, wlan_en high, the
  root-port setup and the pre-train writes — the natural reading is the
  PHY's reference clock arriving (gate on) and the PHY moving to its
  running state, i.e. healthy, not a drop.

The model after thirteen rounds: gate on → the host PHY runs and the
LTSSM genuinely cycles Detect.Active; the card is powered, PERST-released
at the pad and holds CLKREQ low asking for its clock; pipe and recharge
clocks run; every vendor write is in place with readbacks; and the
receiver detect still never finds the card's termination. The suspicion
now has exactly two leaves: the 100 MHz between the gate and the card's
lanes (one gate, no further software-visible control on this SoC — and
Android's own Wi-Fi works without the gate ever being set, which hints
the card's refclk may not run through the host path at all), or the
board's lane/PERST traces. Both are past what the register file can
answer; the next probe is board-level or a fresh hypothesis about the
chip's own power sequencing (thyme clears the WCN's
WLAON_QFPROM_PWR_CTRL SHUTDOWN/VDD4BLOW bits — but only after its link is
up).

### PCIe M1, 2026-11-10: round 14 — the two source probes are dead ends, and the A/B settles 0x86

Three results, the last two from the device:

1. **The TCSR sweep and the GCC refgen hunt are dead ends.** Linux's
   `tcsrcc-sm8650` table has exactly **one** PCIe0 clock gate, `0xb1100`
   (bit 0); the neighbouring words are other blocks' gates — `0xb1104`
   UFS pad, `0xb1108` USB3, `0xb1110` UFS, `0xb1114` PCIe1, `0xb1118`
   USB2. `gcc-sm8650.c` has no PCIe0 reference-generator branch at all: its
   PCIe0 blocks live at `0x6b000` with votes at `0x52000`/`0x52008`, and
   `0x8c0xx` is not a PCIe0 range on this SoC (PCIe1 is `0x8d0xx`). Sweeping
   the TCSR window would only re-read other blocks' gates. (Local copies:
   `tmp/survey-1105/tcsrcc-pineapple.c`, `gcc-sm8650.c`.)
2. **The A/B settles what `0x86` is — and round 13's reading stands.** The
   device tree's `qcom,phy-status-bit = 6` is a *pre-clock* phase indicator,
   not a health check that stays valid after the PHY switches to the card's
   reference clock. The staged runs (14A gate on, `20261110-203749`; 14B gate
   off on a fresh loader, `20261110-204655`) show that:

   | stage | 14A gate on | 14B gate off |
   | --- | --- | --- |
   | `sequence` | `0x41 ready=1` | `0x41 ready=1` |
   | `refclk` (gate write) | `0x41 ready=1` | `0x41 ready=1` (skipped) |
   | `wlan_en` + 100 ms | **`0x86 ready=0`** | `0x41 ready=1` |
   | `root_port` / `pretrain` | `0x86` | `0x41` |
   | PERST release | `0x86` | `0x41` |
   | LTSSM | Detect.Active (`0x1`) | `0` |

   `0x86` needs **both** the TCSR gate and the card enabled; with either one
   missing the PHY stays `0x41`. It appears exactly in the state where the
   LTSSM starts moving, so it is the clocked/running state — the earlier
   host-side note that "the PHY stops reporting ready" is retracted. The gate
   is still the only write that moves the LTSSM off zero; the card's enable
   is what moves the PHY's status into its clocked state.

3. **Back-to-back step-3 runs are not independent.** The first 14B attempt
   (`20261110-204100`, gate off without a fresh loader) never reached the PHY:
   the first RPMh supply vote timed out (`RPMh HAS NOT SAID DONE after
   400000 us`, slot 1, `rc=22`) because the previous run's slot/RSC state was
   still there. `reboot → enter --stage kernel → ramdisk` made the rerun
   clean. Every step-3 round must start from a fresh loader.

The remaining picture is unchanged: host PHY clocked, card powered, PERST
released, CLKREQ held low, every vendor write in place — and receiver detect
still never completes. The two leaves are the same two board truths (the
100 MHz between the gate and the card's lanes, and whether GPIO94 reaches the
card's PERST pin). One cheap in-software test of the second is left for a next
round: after the card is enabled and CLKREQ reads low, pulse PERST low again
and watch whether CLKREQ releases — a card that sees PERST will let go of its
clock request. That needs one more small probe, not a new theory.

```
# gate on (round 14A): where bit 6 falls
python3 tools/ace5/phone.py run --pcie 3 \
  --kernel out/giulia-kernel/mach.ace5-pcie-phy \
  --root-image local/root-daily.apfs
# gate off (round 14B; boot args ride in ACE5_CMDLINE)
ACE5_CMDLINE="pcie_clkref=0" python3 tools/ace5/phone.py run --pcie 3 \
  --kernel out/giulia-kernel/mach.ace5-pcie-phy \
  --root-image local/root-daily.apfs
```

### PCIe M1, 2026-11-10: the PERST probe — CLKREQ does not follow PERST, so this observable cannot judge the wiring

Two probe runs after the 14A/14B pair, both healthy returns:

- `20261110-205710` (`pcie_perst_probe=1`): after the normal enable and
  PERST release, PERST was asserted again for 100 ms. `clkreq` stayed 0 and
  the PHY stayed `0x86` through assert, hold and release.
- `20261110-210348` (same, plus a sample taken **before** the first PERST
  release): `perst probe: still low with the card enabled, clkreq=0
  phy=0x86`.

So CLKREQ is already asserted while the card is still held in reset; it does
not follow PERST, and no PERST transition moves either CLKREQ or the PHY
status. The probe therefore cannot tell "GPIO94 reaches the card" from "the
card holds CLKREQ low whenever it is powered" — a negative here is not
evidence of a broken trace. It does add one fact to the model: the `0x86`
state needs the gate and the card's power, not PERST.

The probe is behind `pcie_perst_probe=1` and is inert otherwise
(`mach.ace5-pcie-perst` is the first version, `mach.ace5-pcie-perst2` the
one with the pre-release sample; both built with WSL `xnu-build.sh`, exit 0).
The PERST/GPIO94 question is now a physical measurement at the card, not a
software probe; do not spend another round on CLKREQ for it.

### The Turnip gate: step 9 passed — the triangle reaches an IOSurface in BGRA (2026-11-08 14:29)

Back on the GPU line after the PCIe rounds. The step-9 image had been
packed and waiting since 2026-11-02 (`local/root-turnip.apfs`, the 11-02
recipe: `--seconds 300 --root-image local/root-turnip.apfs --touch
--gpu-probe 20 --gpu-direct --gpu-keep`, no extra boot args, today's
kernel — the pcie driver is inert without `pcie_step`). One fresh cycle:

```
turnip_test: step 8:   corner (0 0 0 255), middle (255 0 0 255)
turnip_test: step 9: the same pixels in an IOSurface (BGRA), read back from it
turnip_test:   surface 256x256, stride 1024, id 36
turnip_test:   surface corner (0 0 0 255), middle (0 0 255 255)
turnip_test: TURNIP GATE: PASS (steps 1-9)
```

The RGBA red pixel lands in the surface as BGRA `(0 0 255 255)` — exactly
the byte order the window server, the 2D engine and CoreAnimation read.
The Vulkan real-driver gate is now green end to end: ICD → instance →
device → memory → image → submit → draw → IOSurface. Zero PowerOff,
panic or SIGSEGV in the run (`20261108-142924`).

Next on the GPU line: **WSI/swapchain** — an IOSurface-backed present for
a real Vulkan client (vkCreateSwapchainEXT-style through our shim), then
the CoreAnimation bridge so SpringBoard's layers render through Turnip.
The K90 transplant list (fd-bo import size, linear import pitch
tolerance, last-level padding, submit merging) ports on first symptom at
the rendering step. Fluency work from there follows the user's method
note: fast-drag lanes for dropped frames, slow-drag lanes for frame
generation errors.

### The Turnip gate, step 10 (two-image swapchain): built, bisected, one driver-level crash isolated — the dynamic-path run is staged (2026-11-08 afternoon, host-side)

Step 10 extends `turnip_test.c` to the WSI contract in miniature: a second
render target and a second IOSurface, frame 1 re-rendering the same pipeline
over a grey clear, presented to the compositor's second surface, both
surfaces verified (a stale or swapped present fails). During the bring-up,
five device runs bisected one SIGSEGV (`turnip_test[35] Corpse`, then
`[killed by signal 11]`):

- `vkCreateImageView` on the second image crashed — **our bug**: the
  viewInfo struct captured image2's NULL handle at declaration time; fixed
  by assigning `viewInfo.image = image2` before the call. Creation chain
  then ran clean.
- A stray unconditional FAIL (an editing leftover) killed the run after the
  present copy — removed.
- **With the step-8 framebuffer the whole second frame works**: record,
  submit, fence, read-back, surface copy — everything. The crash is exactly
  `CmdBeginRenderPass` **with framebuffer2** — the second framebuffer
  object. bisect: same pass/pipeline/commands2/grey clear with the step-8
  framebuffer = full pass; swap in framebuffer2 = SIGSEGV.

The source was read end to end for the difference
(`tu_CmdBeginRenderPass2` → `tu_choose_gmem_layout` →
`framebuffer->tiling[]` / `tu_emit_subpass_begin` → gmem load/clear blits
through the attachment iviews): framebuffer creation fills all
`TU_GMEM_LAYOUT_COUNT` tiling entries successfully, and the same code path
serves framebuffer1's begin. The native-path bug is real but not found by
reading; the empirically staged answer is `TU_DEBUG=dynamic`, which routes
CmdBeginRenderPass through mesa's generic dynamic-rendering implementation
— also the strategically right path for the CA bridge. The build with
`TU_DEBUG=startup,dynamic` and the restored two-image design is packed and
waiting for a free phone; steps 8-9 now ride the dynamic path too, so the
run doubles as its validation.

Method note from these runs: the bisect was done with per-call marker
prints (the failure was silent SIGSEGV, corpse capture failed on this
XNU). Three of the five runs were burned to heredoc/patch mangling
(`\n` eaten in python heredocs) — the Edit tool, not scripted patching,
against this CRLF worktree.

### The Turnip gate, step 10 PASSED: a two-image swapchain with in-order presents (2026-11-08 19:13)

The staged runs landed it, and the "second framebuffer driver bug" from the
earlier bisect turned out to be **our own C initialization trap, twice**:
`begin.framebuffer`, like `viewInfo.image` before it, captured
`framebuffer2`'s NULL handle at declaration time — every begin with
framebuffer2 dereferenced NULL+0x58 (`FAULT sig=11 addr=0x58`, caught by
the SIGSEGV handler the test now installs; the XNU here fails corpse
capture, so the handler is the only crash reporter). The bisect that
"passed" with the step-8 framebuffer was really passing because THAT handle
was already valid. The driver was never at fault for the crash. The fix is
one line after `vkCreateFramebuffer`: `begin.framebuffer = framebuffer2;`.

The dump also exposed a real driver-level finding worth its own chase: **the
a7xx generic-clear path loses the clear's RGB while writing alpha
correctly** — frame 1 cleared with green `(0,1,0,1)` read back
`(0,0,0,255)`, grey `(0.25)` likewise black. Every earlier gate step only
ever cleared black, so this was invisible until now (chain:
`tu7_emit_subpass_clear` → `tu7_generic_clear_attachment` →
`tu7_generic_layer_clear`; the value rides
`pack_blit_event_clear_value`-style packing into the blit clear registers —
the RGB lanes die somewhere in there). The gate sidesteps it: frame 1's
differentiator is a **second pipeline with a green fragment shader**
(`triangle-green.frag` → `triangle_green_frag_spirv`, generated through the
extended `mk-spirv-header.py`), the draw path being fully proven.

Final verdict (log `20261108-191353`, fresh cycle, zero
PowerOff/panic/SIGSEGV):

```
turnip_test: step 10: a two-image swapchain: acquire, render, present, the compositor's surfaces verified
turnip_test: step 10: frame 1 rendered, presented as surface id 37
turnip_test: step 10: both presents landed, in order, and the compositor read them
turnip_test: TURNIP GATE: PASS (steps 1-10)
```

Two stale-frame runs mid-development taught a hygiene lesson: the marker
cleanup `sed` deleted whole command lines that carried prints
(`pCmdBeginRenderPass` and friends vanished, frame B rendered nothing, the
copy read step-8's stale buffer — the verification "failure" was the stale
frame), so a verification failure after an edit pass means re-read the
whole region, not just the failing check.

Next on the GPU line: the a7xx generic-clear RGB chase (it will bite every
real client), then the CA bridge — SpringBoard's layers through Turnip —
with the fluency lanes (fast drags for drops, slow drags for errors) as the
acceptance method.

### The wallpaper: assets staged, PosterBoard running, the home-screen shot still owed (2026-11-08 evening)

The home screen has always been black because the wallpaper pipeline was
empty end to end. Staged in this session, entirely from the /root/ios165
extraction (no Mac, no sips):

1. **The assets**: the stock WWDC_2022 wallpaper for this canvas
   (375w-812h@3x) lives in
   `/root/ios165/rootfs/Library/Wallpaper/Collections/iOS_16~iphone.wallpaperCollection/Wallpapers/…`
   — three `.ca` bundles (Background/Floating/Foreground), each with
   `main.caml` + `assetManifest.caml` + HEIC layers (Head/Head Mask/Leg/
   Arm/Body), 2.5 MB all told. The local/system-metal tree carried only the
   empty directory skeleton.
2. **HEIC → PNG**: libheif-examples alone refuses ("Unsupported codec" —
   no HEVC decoder in Ubuntu's build); `pip3 install
   --break-system-packages pillow-heif` + `Image.frombytes(mode, size,
   heif.data)` converts all five layers (scripts: local/wsl/
   stage-wallpaper.sh, convert-heic.sh).
3. **CAML rewrite**: the `src="…HEIC"` references → `.png`, including the
   percent-encoded `Head%20Mask.HEIC` (decode the URI before looking for
   the file; re-quote for the attribute) — local/wsl/rewrite-wallpaper-caml.sh.
4. **The renderer**: iOS 16's wallpaper renderer is **PosterBoard**
   (Applications/PosterBoard.app + LaunchAngels/com.apple.PosterBoard.plist;
   `prepare-gpu-root.py --wallpaper` exists for exactly this). Two boot
   plumbing changes were needed: init.c's LaunchAngels bind was an empty
   stand-in by design — now it binds the image's own LaunchAngels when the
   image carries one (committed); and the plist moved into **LaunchDaemons**
   with `RunAtLoad`+`KeepAlive` (the Angel flavour never ran on this port;
   the Daemon flavour does). The staged plist copy also drops
   `_ExperimentalNonLaunching` and registers the
   `com.apple.posterboardservices.services` Mach service.
5. **The verdict so far** (log 20261108-200724): launchd spawned PosterBoard
   as pid 12 under user/501, spawned `WallpaperKit.CollectionsPoster`
   (the collection renderer) on demand, and **SpringBoard's
   `posterboardservices` lookups stopped failing** (zero for the whole run,
   after failing twice per run before). The wallpaper render path is live.
   What is still owed: a screenshot of the home screen WITH the wallpaper —
   the touch-scripted session navigated to the Safari refresh page (60.0 Hz,
   15490 frames, on cadence — the fluency baseline incidentally confirmed)
   before any shot. Next run: omit `--touch`, or pass `--shots`, and the
   early screenshot should show the WWDC swirl; if SpringBoard still draws
   black, the remaining suspect is PosterBoard's /var state (the store a
   real restore creates), which an empty ramfs var does not have.

**CONFIRMED on device by the user** (2026-11-08 evening): the WWDC 2022
wallpaper renders on the home screen. The wallpaper pipeline is complete —
assets, PNG layers, CAML references, PosterBoard as a LaunchDaemon, the
SpringBoard handshake — and the desktop shows the stock iOS 16 wallpaper
where it showed black since the port began. The screenshot chase is moot;
what remains for wallpaper work is only choice/polish (picking a different
image means swapping the staged PNG layers and re-running
rewrite-wallpaper-caml.sh).

### The a7xx generic-clear RGB chase, round 1 (2026-11-08 night, host-side — paused for handoff)

The bug: `TU_DEBUG=startup` native path, renderpass clear of green `(0,1,0,1)`
on RGBA8_UNORM renders `(0,0,0,255)` — **alpha survives, RGB forced to 0**
(grey 0.25 likewise). Every earlier gate step cleared black, so it was
invisible until step 10's green frame.

What was read (mesa 25.0.7 tree, /root/HackiOS-Ace5/xnu-deps/build/mesa/
mesa-25.0.7, all host-side):

- Chain: `tu_emit_subpass_begin_gmem` (a7xx has_generic_clear → skips the
  gmem clears) → `tu7_emit_subpass_clear` (tu_cmd_buffer.cc:4670) →
  `tu7_generic_clear_attachment` (tu_clear_blit.cc:4515) →
  `tu7_generic_layer_clear` (tu_clear_blit.cc:2012): packs the value, writes
  **4 dwords to `REG_A6XX_RB_BLIT_CLEAR_COLOR_DW0..3`**, then
  `event_blit_setup(BLIT_EVENT_CLEAR)` + `event_blit_run<A7XX>`.
- `pack_blit_event_clear_value` for RGBA8_UNORM: `PACK_F(r8g8b8a8_unorm)` —
  all four channels byte-packed into DW0 (green → `0xFF00FF00`), DW1..3 = 0.
- Register DB (a6xx.xml): the block is 0x88df..0x88e2, dense and by design
  (0x88dc BLIT_FLAG_DST reg64, 0x88de FLAG_DST_PITCH, 0x88e3 RB_BLIT_INFO,
  0x88e4 RB_BLIT_CLEAR_MODE A7XX-only, 0x88e5 RB_CCU_CNTL2). Not a
  transcription error.

**The upstream comparison (gitlab.freedesktop.org raw fetches WORK from this
host — WebFetch):** mesa main's `tu7_generic_layer_clear` writes the clear
color to **`REG_A6XX_RB_RESOLVE_CLEAR_COLOR_DW0..3`** — a register that does
not exist in 25.0.7's register DB at all (added later), with a new packing
helper `tu_pack_float32_for_color` (per-channel by format size into the low
64 bits — for RGBA8 the byte layout matches ours, so the packing is
equivalent; **the register is the difference**). Upstream also changed
`event_blit_run<A7XX>`'s signature (fdl_view via `tu_image_view_fdl_view`).
`r2d_clear_value` uses yet another block (`RB_A2D_CLEAR_COLOR_DW0`).

Hypothesis: on A7XX the generic/BLIT-event clear reads its color from the
RESOLVE_CLEAR block (moved/renamed), and 25.0.7 writes the old a6xx
BLIT_CLEAR block — the hardware clears with whatever stale value sits there
(the observed alpha=255 needs one more datum: check what the
RESOLVE_CLEAR/BLIT_CLEAR overlap holds at clear time, or just try the fix).

Next session's plan (cheapest first):
1. Fetch the CURRENT registers DB for the A7XX clear-color block: the
   registers moved out of the mesa repo (a6xx.xml 404s at
   src/freedreno/registers/adreno/ in main) into the separate
   `mesa/registers` repo — the repo ROOT hits a login wall, but raw file
   URLs may still work: try
   `https://gitlab.freedesktop.org/mesa/registers/-/raw/main/adreno/a7xx.xml`
   and compare `RB_RESOLVE_CLEAR_COLOR_DW0..3` offsets against our
   0x88df..0x88e2.
2. Patch our 25.0.7 `tu7_generic_layer_clear` to write the A7XX block (and
   adopt `tu_pack_float32_for_color` if the layout differs per-DW), rebuild
   mesa (local/wsl/build-mesa.sh), repack the ICD into
   local/root-turnip/lib + local/root-daily/lib, run the step-10 gate.
3. If green lands: the bug is closed; also re-test the wallpaper-path
   greys. If not: instrument `event_blit_run<A7XX>` / compare with freedreno
   (GL) a7xx clear handling in the same tree.

Scripts left behind: local/wsl/read-clear.sh, read-clear2.sh,
check-regs.sh, find-regs.sh, regdb.sh, regdb2.sh (read-only source dumps);
the wallpaper staging trio (stage-wallpaper.sh, convert-heic.sh,
rewrite-wallpaper-caml.sh) and build-init.sh remain the canonical tools.

### The a7xx generic-clear RGB chase, round 2: the clear was never broken (2026-11-08 21:40)

Round 1 was wrong on both counts, and the device says so.

**The register never moved.** The current DB is the msm driver's copy
(`drivers/gpu/drm/msm/registers/adreno/a6xx.xml` in mainline Linux — reached
through the GitHub contents API after `gitlab.freedesktop.org/mesa/registers`
turned out to be login-walled for WebFetch *and* for `git clone`). There,
0x88df..0x88e2 is `RB_RESOLVE_CLEAR_COLOR_DW0..3` — exactly where 25.0.7 writes
`RB_BLIT_CLEAR_COLOR_DW0..3`. Every "new" register in the upstream clear path is
the same offset under a new name: 0x88d7 `DST_INFO`→`SYSTEM_BUFFER_INFO`
(bitfields identical), 0x88e3 `BLIT_INFO`→`RESOLVE_OPERATION` (identical),
0x88d0 `UNKNOWN_88D0`→`RESOLVE_CNTL_0`. The only behavioural delta left in
`tu7_generic_layer_clear` is the trailing event: `FD_BLIT` (25.0.7) vs
`FD_CCU_RESOLVE` (main).

**The measurement.** A probe in `turnip_test` clears image 2 with channels that
cannot be confused and prints the bytes (`local/root-turnip-probe.apfs`, packed
from `local/root-turnip` with `mkroot-manifest.py` + `mkapfs.py -z --plain fw`).
Run `20261108-213834`: `TURNIP GATE: PASS (steps 1-10)`, zero
PowerOff/panic/SIGSEGV —

```
turnip_test: probe 0: clear (0.25 0.50 0.75 0.00) reads back RGBA 64 128 191 0
turnip_test: probe 1: clear (0.00 0.00 0.00 1.00) reads back RGBA 0 0 0 255
turnip_test: probe 2: clear (1.00 0.00 0.00 0.00) reads back RGBA 255 0 0 0
```

RGB exactly, alpha exactly, alpha 0 included. **There is no a7xx
generic-clear RGB bug on this driver**, and nothing in `tu7_generic_layer_clear`
needs to change. Round 1's green→(0,0,0,255) was a bisect-era measurement
artifact — the same session already documented one stale-frame read producing a
false verification failure, and the committed step-10 code clears black in
`clear2` (green only names `clear`, used by the unused `begin`). The green
shader in step 10 stays as the gate's stale-present differentiator, not as a
workaround for the clear.

Kept: the probe (~70 lines, prints `turnip_test: probe N:`) as the standing
check for any future clear/format change. New read-only tools:
`local/wsl/probe-regs.sh`, `find-regs-api.sh`, `read-upstream-clear.sh`,
`diff-clear-path.sh`, `diff-clear-dst.sh`, `check-clear-mask.sh`, `cmp-88e3.sh`,
`check-fdl-view.sh`; plus `phone-wsl.sh` + `setup-wsl-adb.sh` to run phone.py
from WSL against the Windows adb (phone.py itself must run on Windows python:
it hands fastboot Windows paths).

Next on the GPU line: WSI — an IOSurface-backed present through the shim — then
the CA bridge, with the fluency lanes as the acceptance method.

### The Turnip gate, step 11: an IOSurface imported as a Vulkan image — the draw lands in the compositor's memory, no copy (2026-11-08 22:14)

The zero-copy milestone. Step 11 creates a **linear** `VkImage` with
`VkExternalMemoryImageCreateInfo` (`DMA_BUF` handle type), imports the surface's
memory with `VkImportMemoryFdInfoKHR` whose `fd` is the **IOSurface id**, binds
it, draws the step-8 triangle, and then reads the IOSurface directly — no
`vkCmdCopyImageToBuffer`, no frame copy:

```
turnip_test:   the imported image needs 262144 bytes, alignment 64, type bits 0x3
turnip_test:   the surface the GPU wrote: corner BGRA 0 0 0 255, middle BGRA 0 0 255 255
turnip_test: step 11: the draw is in the compositor's surface, imported by its id, no copy
turnip_test: TURNIP GATE: PASS (steps 1-11)
```

Run `20261108-221456`, zero PowerOff/panic/SIGSEGV. The kernel side already had
the primitive (`kRedwoodSurface`, 0x7e — an IOSurface's pages as a GPU buffer),
and the shim already exported it (`drmPrimeFDToHandle` with the surface id as the
"fd"). Two port-specific walls came out on the way:

1. **`msm_bo_init_dmabuf` lseek()s the prime_fd for its real size** — with a
   surface id that fails, and the import returns `VK_ERROR_INVALID_EXTERNAL_HANDLE`
   before anything else runs. `tools/mesa/turnip-darwin-surface-import.patch`
   patches it under `__APPLE__` to ask the shim's `redwood_drm_surface()` for the
   buffer's `size/offset/row_bytes/height` (the kernel answers for the surface's
   pixels) and sanity-checks them; the same patch skips the `close(fd)` on import
   success, because this "fd" is an id and the close would hit an unrelated
   descriptor.
2. **The surface is BGRA in memory.** The CPU-copy present swaps R/B by hand, so
   the zero-copy import must declare `VK_FORMAT_B8G8R8A8_UNORM`; with
   `R8G8B8A8_UNORM` the first run read middle BGRA `255 0 0 255` (red in B) and
   failed the check — the pixels were right, the declaration was not.

Memory-type selection needed a fallback too: every heap on this driver is
host-visible system memory, so the usual "prefer device-local" loop finds
nothing (`type bits 0x3`).

What it unlocks: the compositor's surface IS the render target. Present is no
longer a copy, and a client can render straight into the drawable. Next: put a
real client's drawable on this path (the Metal/CA layer), then the CA bridge with
the fluency lanes as acceptance.

### Step 12: the present loop — 0.10 ms/frame pipelined, and the seed rule (2026-11-08 22:49)

32 frames into the same imported surface, each timed around submit → fence, with
the surface read back every frame (each frame's clear carries its index in the
red channel, so a dropped or reordered present fails); then 32 more submitted
back to back with only the last fence waited, and the index in green:

```
turnip_test:   32 frames, wait each: min 1.36 ms, median 1.37, max 2.55, mean 1.41
turnip_test:   32 frames pipelined: 3.09 ms total, 0.10 ms/frame
turnip_test: TURNIP GATE: PASS (steps 1-12)
```

Run `20261108-224906`. 0.10 ms/frame is the cost of *presenting* once there is
queue depth; 1.41 ms is the full round trip including the fence wait, the seed
bump and the CPU read. Against 16.7 ms at 60 Hz the present path is free.

**The seed rule, and why the first run failed.** Frame 1 read back frame 0's
colour (`surface red 0, expected 8`). The pixels were right; the *cache* was not.
`RedwoodGPU.cpp` (the submit path's comment) invalidates this processor's cached
lines of an IOSurface when the surface's **seed** moves, and otherwise only once
in `GPU_PUSH_AGAIN_MS` — so a loop that reads within one frame of the write sees
the stale line. The way to say "the GPU has drawn into this surface" is a write
lock/unlock pair (`IOSurfaceLock(surface, 0, NULL)` + unlock), exactly what
`userland/src/eagl.c` does after its draws. Step 12 now bumps the seed before
each read.

This is a port-wide contract, not a test detail: **every client that has the GPU
draw into an IOSurface must bump its seed**, or whoever reads (the display, the
compositor, another process) can see the previous frame. Turnip's present path
needs it too — the natural home is the WSI/import layer, since a client that
imports a surface and submits into it is exactly "somebody who wrote one".
Metal's path already gets it from `MiniIOSurfaceDrawn()`; GL's from eagl.c.

### Step 13: a Vulkan frame in a CoreAnimation drawable (built 2026-11-09; the run is owed)

Steps 1–12 never touched the window server: every surface was one the gate made
itself and every frame was read back in the same process. The one link nobody
has measured is the real one — a drawable CoreAnimation owns, drawn into by
Turnip, taken to the panel by the image queue. Step 13 is that link, in
`tools/vulkan/vk_triangle_app.m`, and it reuses only proven halves:

| link | already measured by |
|---|---|
| `CAMetalLayer nextDrawable` | `triangle_app --layer`, on this port |
| the drawable's IOSurface | `triangle_app`: `_CAMetalDrawablePrivate` +0x40, validated against +0x48/+0x50 |
| surface id → `VkImage` import | `turnip_test` step 11 (`turnip-darwin-surface-import.patch`) |
| the draw | `turnip_test` steps 8/11 (ir3, the CP) |
| the seed bump | `turnip_test` step 12's rule — `IOSurfaceLock(surface,0,NULL)` + unlock |
| presentation | `-[CAMetalDrawable present]`: the surface is registered in the layer's CAImageQueue, no device in the path |

The picture is **green (or cyan) with a red triangle**, in BGRA. The Metal
application draws blue with an orange triangle, so a screenshot cannot confuse
the two: this program's colours appear nowhere else on this port.

Two modes in one binary, because a boot is expensive and they fail separately:
`--layer` (no UIKit: a bare CAMetalLayer of the panel's size, three frames,
every check except the panel; output is certainly in the log because the job
list runs it) and the application (UIApplicationMain, a window whose view's
layer is a CAMetalLayer, a frame every half second, so a screenshot can be taken
whenever).

**What was built and packed (2026-11-09, host side only — no phone run yet):**

- `tools/vulkan/vk_triangle_app.m` + `vk_triangle_app.plist` / `.entitlements`;
  built by `local/wsl/build-vk-app.sh` into `local/components/vk_triangle_app`
  and `local/root-turnip/bin/vk_triangle_app` (90,744 bytes, arm64 Mach-O).
- `tools/ace5/rc.jobs.vkapp`: the gate first (`turnip_test`, so a regression in
  steps 1–12 is visible in the same log), then `vk_triangle_app --layer`, then
  unlock and `battery_test open redwoodvk://`, then wait.
- Small root `local/root-turnip-vk.apfs` (13,037,568 bytes, sha256
  `b7aaa7d3…`, apfsck clean, 120/120 entries read back) — the job above and the
  binary in `/redwood/bin`.
- System image `local/system-metal-vk.apfs`: `local/system-metal` is the empty
  directory skeleton (its files are symlinks into the iOS extraction and it is
  not a packable tree), so the tree is built by
  `tools/ace5/prepare-gpu-system.py --ios /root/ios165 --stage0
  userland/obj/stage0 --triangle local/root-turnip/bin/triangle_app --output
  /root/system-vk`, then `Applications/RedwoodVK.app` (binary + Info.plist +
  PkgInfo, unsigned) is copied in, and it is packed with
  `python3 tools/apfs/mkapfs.py -z -o local/system-metal-vk.apfs
  System=/root/system-vk,manifest=/root/ios165/system.manifest` and checked with
  `tools/apfs/check-image.sh local/system-metal-vk.apfs
  System=/root/system-vk,manifest=/root/ios165/system.manifest`.
  Result: 2,492,272,640 bytes, sha256 `d0672a60…`, apfsck clean, 62,427/62,427
  entries read back (the port's 62,423 plus RedwoodVK.app's three files and its
  directory).

**The run (owed):**

```
reboot → enter --stage kernel → ramdisk --image local/system-metal-vk.apfs
→ run --seconds 600 --bootcpu 1 --live-log --root-image local/root-turnip-vk.apfs
      --gpu-probe 20 --gpu-direct --gpu-keep
```

**Acceptance, all of it in one log and two screenshots:**

- `turnip_test: TURNIP GATE: PASS (steps 1-12)` still there;
- `vktriangle: surface <id> imported: …` once per drawable in rotation;
- `vktriangle: frame N: corner BGRA 0 255 0 255, middle BGRA 0 0 255 255` (or
  cyan for the corner when the clear alternates every eight frames);
- `vktriangle: LAYER MODE: PASS`;
- the applications' frames continue with `failures 0`;
- two screenshots taken ≥1 s apart: green/cyan background with a red triangle
  in the window, and the background colour differs between them (the clear
  alternates every eight frames) — a still frame is told from a live present.

**First device run, 2026-11-09 19:36 (log 20261109-193626-run): half of step 13
is now measured on hardware.** The gate stayed `PASS (steps 1-12)`, and
`--layer` **passed**: a real `CAMetalDrawable` handed surface ids 39 and 40
(1125×2436, stride 4608, `0x42475241`), Turnip imported each by its id, drew,
the seed bumped, the present was called, and the readback was exact —
`corner BGRA 0 255 0 255, middle BGRA 0 0 255 255`, `LAYER MODE: PASS`, zero
failures. The drawable rotation (39 → 40 → 39) also shows the layer recycles
two surfaces exactly as CA's image queue does.

The application died on its first frame, and the reason is a one-line Apple
habit: `-[CALayer setDevice:]` **unrecognized selector**. A UIViewController's
default view is a plain UIView whose layer is a CALayer; `RWVKView`'s
`+layerClass` was never asked because `loadView` was not overridden, so
`self.view.layer` was never a CAMetalLayer. `loadView` now makes the RWVKView
(commit `ddc2c90`), the binary was rebuilt, and both images were repacked and
re-verified against it. **The on-panel frame is still owed**: the picture the
screenshot of that run shows is the home screen, not the triangle.

**Third run, 2026-11-09 20:48 (log 20261109-204858): the panel shows the Vulkan
frame. Step 13 is complete.** Gate `PASS (steps 1-12)`, `LAYER MODE: PASS`, then
the application: frames 1–91 and on, `0 failure(s)`, each frame's readback exact.
Two screenshots four seconds apart are the acceptance the log cannot give:
`20261109-205051-screen.png` is **cyan with a red triangle**, and
`20261109-205111-screen.png` is **green with a red triangle** — the clear
alternates every eight frames, so the second picture is a second present, not a
still. The whole chain is now measured on hardware end to end: CAMetalLayer
drawable → IOSurface by id → Turnip import → ir3 draw → seed bump →
`-[MTLCommandBuffer presentDrawable:]` → CAImageQueue → window server → panel.

Two things had to change after the black-window runs, and both are now in
`vk_triangle_app.m` (commit `3f11fec`):

1. **Present rides a command buffer.** `-[CAMetalDrawable present]` alone left
   the window black even though the surface held the right pixels; Apple's
   `-[MTLCommandBuffer presentDrawable:]` followed by `-commit` is the path
   `src/triangle_app.m` uses, and the present is a scheduled handler that
   `-commit` delivers.
2. **The drawable's size is CA's to choose.** The app had forced
   `layer.drawableSize = 1125x2436` at `contentsScale` 3; the Metal application
   that reached the panel never touches either. Left alone, CA hands the first
   frames a 1125×2436 drawable and, once UIKit has laid the window out,
   **375×812** (stride 1536) — exactly the size docs/ACE5.md records for the
   Metal app. Both sizes import and draw; the one the compositor takes is
   CA's.

Final images for this step: small root `local/root-turnip-vk.apfs`
(sha256 `09e01854…`) and system `local/system-metal-vk.apfs`
(sha256 `5a10bb1f…`), both apfsck clean with every entry read back.

**If it fails, the failure names the link:** `nextDrawable gave nil` (the Metal
device or the drawable pool), `the drawable's IOSurface could not be reached`
(the private layout moved), `vkAllocateMemory importing surface N` (the import
path), `the fence never signalled` (the GPU), or every readback wrong but the
present called (the image queue / CA).

### Step 14: the port's WSI layer, and the first pacing numbers (2026-11-09 21:07)

The drawable half of a swapchain is no longer app code. `tools/vulkan/
wsi_redwood.h`/`.m` owns the four things that belong to the window system —
`nextDrawable`, the IOSurface by id (validated against the texture and the
device), **the seed bump**, and the present (`presentDrawable:` + `commit`) —
and `vk_triangle_app` is its first client; the Vulkan import stays with the
client because it needs that client's VkDevice.

The app now presents **240 frames back to back at startup** (no timer) and
reports the frame cost and the interval between presents. One boot
(log 20261109-210755, images root `4d9790e7…` / system `11021803…`) gave both
sides of the answer:

```
vktriangle: BENCH layer: 120 of 120 frames, 0 failure(s)
vktriangle: BENCH present interval (ms): min 1.445, median 1.649, mean 1.624, max 2.418
vktriangle: BENCH layer: 615.8 fps over the window (1.6 ms/frame)

vktriangle: BENCH app: 240 of 240 frames, 0 failure(s)
vktriangle: BENCH frame (draw+readback, us): min 553.750, median 16675.625,
            mean 18024.162, max 418595.417
vktriangle: BENCH present interval (ms): min 0.554, median 16.676, mean 18.093,
            max 418.596
vktriangle: BENCH app: 55.3 fps over the window (18.1 ms/frame)
```

Read it this way:

- **With a window, the loop is display-locked.** The median interval between
  presents is **16.676 ms** — 59.97 Hz, the panel's 60 Hz — and the median
  frame cost equals it (16.675 ms): `waitUntilCompleted` is where the display
  wait lands. The path does not outrun the display, which is what a compositor
  needs.
- **Without a window, nothing paces it.** 1.6 ms/frame, 615.8 fps: the present
  itself costs about a millisecond, and the 16.7 ms of the app is the display
  link, not this port's CPU or GPU.
- **One 418.6 ms stall at startup** (max interval) drags the mean to 18.1 ms /
  55.3 fps; the median is untouched, so it is the launch hitch, not a
  steady-state loss. Worth one look later; not chased now.
- 240 presents, zero failures: the drawable pool never ran dry, which is the
  proof that presents complete and are recycled.

The screenshot after the bench (`20261109-211002`) still shows cyan with the
red triangle: the picture stays up while the loop paces.

**What this sets up.** The cadence a CA bridge must inherit is this 16.676 ms
median, and the cost it has to fit inside is the 1.6 ms present. The next step
on this line is a `VkSwapchainKHR` in the ICD over the same import (the loader
has no WSI today: the instance was created with no extensions), or the CA
bridge with the fluency lanes as acceptance.

### Step 15: sampling an IOSurface through Turnip — the bridge's read side (2026-11-09 21:50)

Writing into a drawable is half a compositor; the other half is reading someone
else's surface. The app now fills a 256×256 BGRA IOSurface with an 8×8
blue/yellow checker (opposite in every channel), imports it **by its id** as a
linear `VkImage`, copies it once into a tiled image, and draws a full-screen
quad sampling it into the CA drawable. The shaders are
`tools/vulkan/composite.vert`/`.frag` (embedded by `mk-spirv-arrays.py`), and
the readback counts every sampled pixel as blue, yellow or other.

**First run crashed in `vkAllocateDescriptorSets` — SIGSEGV at 0x58.** The
fault handler added for exactly this reason printed the address, and the code
had one mistake: `VkDescriptorSetAllocateInfo.descriptorPool` was never set (the
initializer ran before the pool existed and the field was left NULL). The
driver read the pool's field at +0x58. Fixed by naming the pool after
`vkCreateDescriptorPool`; the crash, the address and the cause are all in the
log of that boot (`20261109-213839`).

**Second run (`20261109-215002`): the read side works.**

```
vktriangle: source: surface id 39, 256x256 0x42475241, imported linear,
            copied once into a tiled image, sampled through a nearest sampler
vktriangle: frame 1: sampled grid blue 28016, yellow 28012, other 0
...
vktriangle: LAYER MODE: PASS
vktriangle: BENCH app: 55.3 fps over the window (18.1 ms/frame), 0 failures
```

`other 0` over the whole drawable is the strong statement: every sampled pixel
is one of the two checker colours, so the pixels came out of the surface's
memory with the right stride and channel order — nothing was a clear colour, a
staging copy that never arrived, or a channel swap. `20261109-215210-screen.png`
shows the blue/yellow checker filling the panel, and the app's 240-frame bench
still presents at the display's cadence with zero failures.

**What it leaves open, named so it is not mistaken for done.** The copy from
the imported linear image into the driver's tiling is deliberate: this step
proves the import and the sampler, not linear sampling. Whether a LINEAR image
can be sampled directly is the next question on this road, and it is the one
that decides how close a zero-copy CA bridge can get.

**The answer, same evening (2026-11-09 22:05): yes, LINEAR is sampled where it
lies.** `vkGetPhysicalDeviceFormatProperties(B8G8R8A8_UNORM)` answers
`linearTilingFeatures 0x1fd83` with `SAMPLED_IMAGE` set, and with
`RWVK_SAMPLE_LINEAR=1` the app drops the tiled copy and puts the imported
surface straight into `SHADER_READ_ONLY_OPTIMAL`. Run `20261109-220543`:

```
vktriangle: source: B8G8R8A8 linear tiling 0x1fd83 (sampled yes), optimal tiling 0x1fd83 (sampled yes)
vktriangle: source: surface id 39, 256x256 0x42475241, imported linear,
            sampled where it lies (no copy), sampled through a nearest sampler
vktriangle: frame 1: sampled grid blue 28016, yellow 28012, other 0
vktriangle: LAYER MODE: PASS
vktriangle: BENCH app: 240 of 240 frames, 0 failure(s), 58.7 fps (17.0 ms/frame)
```

The counts are identical to the tiled road's, so the copy bought nothing:
**reading an IOSurface zero-copy through Turnip is a measured fact on this
driver**, and that is the read half of a CA bridge settled. The tiled road stays
compiled in (`RWVK_SAMPLE_LINEAR=0`) as the comparison. The screenshot
(`20261109-220747`) shows the checker on the panel.

Images: small root `1984d6e7…`, system `bbdc449f…`.

### Step 16: two runs with the 34-register dump — the frame-4 crash did not reproduce (2026-11-10)

The stale step-16 image (built 11-09 22:32–22:35; the binary carries both the
34-register dump and the entry steps) was run twice on 11-10
(`20261110-212010`, `20261110-212456`) with the step-13 recipe.

- Both runs: `LAYER MODE: PASS` 123/123, the app ran ~291 frames,
  **zero `FAULT sig`**.
- Run 1 used five distinct app drawables (44–48); run 2 used 43–49 —
  including **49**, the fourth distinct drawable whose arrival preceded the
  11-09 crash — and survived, reusing 49 for 31 frames.

So the frame-4 SIGSEGV (old dump: fault addr `0xc6`, `x0=0`, stopped at
`uctx[15]`, no pc) is intermittent, and the entry-step logging added in
`34ddc2e` likely perturbs the race window enough to mask it. Two runs and
~580 app frames are not proof the bug is gone — only that this build did not
show it.

That was done (`17224ba`): the steps are opt-in (`--steps`), the app rebuilt
(109,544 bytes), and both images repacked (`root-turnip-vk-steps.apfs`,
`system-metal-vk-steps.apfs`). Two more runs (`20261110-222214`, `-222628`)
kept the original timing and still showed **zero `FAULT sig`** — surfaces
44–48 / 42–48, ~291 app frames each.

Four runs, ~1,160 app frames, no crash. The 11-09 fault was ~250 ms after
the app spawned, at frame 4; that window has now been crossed four times
without it. Treat it as a rare startup race, not a blocker: the 34-register
dump stays in the binary, so when it recurs the PC is one run away. Do not
spend more boots on it before the CA bridge; a stress run after the bridge
is the better reproducer.

### CA bridge stage 1: a SpringBoard-side Turnip overlay probe (2026-11-12; built, run pending)

The client half is proven (steps 13–16), but only from a standalone app. This
stage moves it inside CoreAnimation's own client to answer the one question that
app cannot: can SpringBoard -- the process that owns the window server -- acquire
a CA drawable, import its IOSurface into Turnip and put pixels on the panel?

**Design (new files).**

- `tools/vulkan/redwood_vk_bridge.{h,m}`: `rwb_open(device)` opens the ICD
  (`/redwood/lib/libvulkan_freedreno.dylib`, no Vulkan loader), creates an
  instance/device/queue and one BGRA8 render pass. `rwb_present_clear(bridge,
  wsi, bgra)` acquires a drawable through `rw_wsi_acquire`, imports it as a
  linear BGRA `VkImage` (`DMA_BUF_BIT_EXT`, fd = IOSurface id -- the step-11
  sequence), clears it, submits and waits, then `rw_wsi_present` (seed bump +
  `presentDrawable:` + `commit`). The import is duplicated from
  `vk_triangle_app.m` on purpose: the proven app path is not edited for a probe.
- `userland/src/redwood_vk_overlay.m`: a 240 pt `CAMetalLayer` band added as a
  sublayer of SpringBoard's key window, driven by `CADisplayLink` at 60 Hz with
  a colour that cycles. Enabled by `vkbridge=1` or `/redwood/etc/vkbridge`.
  stderr is pointed at `/dev/console` before the first line; a real start
  failure stops instead of retrying (so a failed device/layer is not recreated
  every 0.5 s); a failed surface import destroys its partial entry.
- `userland/build.sh` grows a `VKBRIDGE=1` branch (default off: CI needs no
  Vulkan headers); `inject_sb.c` calls the overlay only under
  `REDWOOD_VKBRIDGE`.

**Build (host, WSL).**

```
cd userland && THEOS=/root/theos /root/HackiOS-Ace5/xnu-deps/host/bin/xnu-clang \
  -arch arm64 -isysroot /root/theos/sdks/iPhoneOS16.5.sdk -O1 -dynamiclib \
  -install_name /redwood/lib/libredwood-sb.dylib \
  -framework Foundation -framework CoreFoundation -framework IOKit -framework UIKit \
  -framework QuartzCore -framework Metal -framework IOSurface -lobjc \
  -DREDWOOD_VKBRIDGE=1 -I../tools/vulkan -I/root/vulkan-include \
  -o obj/libredwood-sb.dylib src/inject_sb.c src/redwood_vk_overlay.m \
  ../tools/vulkan/redwood_vk_bridge.m ../tools/vulkan/wsi_redwood.m
```

Result: 113,360 B dylib. `otool -L` shows Foundation/CoreFoundation/IOKit/UIKit/
QuartzCore/Metal/IOSurface; `_OBJC_CLASS_$_RWVKBOverlay`, selectors `start`,
`tick:`, `fail:` and minos 16.5 are present; `/redwood/etc/vkbridge` and the
`vkbridge:` log strings are in the binary.

**Images.** Small root `local/root-turnip-vk-bridge.apfs` (13,058,048 B, 122
manifest entries including `etc/vkbridge`); system image unchanged from step 16
(`local/system-metal-vk-steps.apfs`).

**Runs on 2026-11-12 (the phone came back).** Five boots, each one fixed a
different layer:

1. `20261112-203139`: overlay armed, `RedwoodMetalDevice` built, layer added;
   `FAILED: the ICD did not give the instance entry points`.
   `vkEnumeratePhysicalDevices` and friends are instance commands and this ICD
   returns NULL for them when asked with a NULL instance; resolve them after
   `vkCreateInstance`.
2. `20261112-203726`: `FAILED: the ICD has no get_device_queue` -- RWB_LOAD
   passed the snaked field name to `vkGetDeviceProcAddr`; a name table now maps
   every field to `vkGetDeviceQueue` etc.
3. `20261112-204051` (sublayer of SpringBoard's first key window): the bridge
   opened, imported surfaces 6/7/8, presented 1,300+ frames (first 120 at
   median **16.666 ms**), gate PASS, zero faults -- but **no band on the
   panel**: the first key window is a boot/transition window and is gone by the
   time the home screen arrives.
4. `20261112-204723` (our own 375x240 `UIWindow`, level 1001): geometry is
   right (window/view/layer 375x240, drawable 1125x720), first 120 frames median
   **16.667 ms**, gate PASS, zero faults; the panel shows only the **bottom
   ~57 pt** of the band. A 240 pt drawable is not fully rendered -- the
   non-panel-height surface class to investigate.
5. `20261112-205649` (full-screen 375x812, drawable 1125x2436): **the whole
   screen turns the cycling colour** -- Turnip output is composited by
   SpringBoard and the screenshots prove it; gate PASS, zero faults. But the
   present interval is **~50 ms (20 Hz)**, so the full-screen path does not
   hold 60 Hz yet.

Settled: a process inside SpringBoard can acquire a CA drawable, import its
IOSurface into Turnip, clear it and have CA display it. Not settled: pacing (the
same overlay measured 16.667 ms on one boot and 50-66 ms on another), and what
the partial bands in the screenshots really are.

**Geometry, 2026-11-12 21:12 (`20261112-211213`).** The 240 pt window's drawable
describes cleanly: kernel `kRedwoodSurface` answers size 3,325,952, **offset 0**,
rowBytes 4608, 1125x720, BGRA, 0 planes; the IOSurface agrees; the VkImage needs
3,317,760 bytes at alignment 64 (the surface is 8 KiB larger). The import is
therefore neither shifted nor truncated. The screenshots that showed "only the
bottom strip" differ from shot to shot (one shows two red bands with the home
screen between them), and the user watching the panel saw the band flashing
continuously -- the partial bands are **framebuffer-capture tearing**, not a
stable render defect. Do not patch the import for them.

What is left is pacing: this boot's 240 pt window ran at 50-66 ms medians while
an earlier boot held 16.667 ms for its first 120 frames.

**Pacing, three isolations (2026-11-12 21:23-21:30).**

- `20261112-212346`, overlay-only job list (no `turnip_test`, no app):
  **65.0 ms** median over 120 frames -- not contention with the gate/app.
- `20261112-212736`, draw loop moved from SpringBoard's main-thread
  CADisplayLink to a dispatch timer on its own queue: **64.2 ms** -- not
  main-thread scheduling.
- `20261112-213051`, window level dropped from status+1 to normal+1:
  **65.5 ms** -- not the window level.

So the ~65 ms sits in the present path itself: `rw_wsi_present` waits for the
Metal command buffer's present on a window **owned by SpringBoard**, while the
same WSI in a normal app process measured 16.676 ms (step 14). Next: separate
the wait from the render (drop `waitUntilCompleted` and measure the timer's
rate), and compare a normal app-process window on the same boot. The working
hypothesis is that CA schedules an extra SpringBoard window at ~15 Hz (60/4);
if so, 60 Hz for the bridge needs the client to be a normal app/frontmost
window, not a SpringBoard overlay.

**Risks carried from K90's 11-11 Metal work.** Process-owned IOSurfaces are
refused as GPU buffers by our kernel (`s->client != NULL`); K90 added a
copy-backed path for that class (`018a9e0`). The probe's drawable is CA-owned,
so it should not hit this, but a real compositor reading client-backed layers
will. CA's compositor also asks half-float formats: K90 maps R16Float to
`L00h`/`1C0h` and RG16Float to `2C0h` (`cd6555e`, `84371e3`, `e09aed9`). This
bridge declares BGRA only and will refuse those -- expected at this stage.


