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
every work item; `rwa_render_draw` then runs `pipe->clear()` before *each* draw.
A pass whose load action is Clear therefore wiped the attachments again before
every draw, so only the **last** draw of a multi-draw pass survived — everything
the earlier ones put down was gone, which reads exactly like those draws never
having been made, in exactly the multi-draw passes (a blur chain, a card made of
several quads) where the picture first goes wrong. The load action now belongs to
the first draw of the pass and to nothing after it. Two diagnostics went in with
it: a pass that asks for a clear and records no draw (the clear is made inside
`rwa_render_draw`, which refuses a draw of no vertices, so it is lost — the same
shape Bergamota calls "a pass that draws nothing never cleared"), and a pass with
more than one colour attachment asking for a clear (this driver has one clear
colour, attachment 0's). `surfaceBuffer()` in `RedwoodGPU.cpp` now also says out
loud when it refuses a surface that is memory of a program's own
(`s->client != NULL`), because that surface can then never be drawn into at all.

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

**The clock and Now Playing are not running at all.** `SpringBoard` asks for
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
