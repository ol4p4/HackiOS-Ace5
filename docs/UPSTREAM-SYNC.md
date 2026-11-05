# Upstream sync (targeted cherry-pick)

Our tree is forked from the `YiKongk/HackiOS-Ace5` snapshot of 2026-10-23
(`ab2acff`). Upstream `oosexgang/hackios-xnu-kernel` has moved a long way since
— the repository is now named `hakios-xnu-kernel` (old URLs redirect), HEAD
`87bf4851`, 2026-11-04, and Metal-on-Vulkan landed on it on 11-03. A wholesale
rebase is not worth it yet: the
device layer upstream only holds UEFI, prebuilt images, firmware scripts and
`soc.args`, while the drivers stay in the flat `xnu/files/` tree and carry POCO
X5 Pro constants. This file is the take/do-not-take list for pulling the generic
work in without regressing a tree that boots, has touch and reaches GPU probe
stage 20.

## Ground rules

1. Cherry-pick generic subsystems and fixes; never a device-specific driver.
2. Keep the `redwood_*` names. Upstream renamed them to `hakios.*` and touched
   ~364 files; following that rename is pure churn until a full rebase is chosen.
3. Every pick must pass `tools/ace5/host-selftest.sh` (run it yourself — CI does
   not run it) and a CI kernel build (dispatch `build-kernel.yml` by hand; there
   is no push/PR trigger) before it goes near the phone.
4. Use `git cherry-pick -x` so the upstream commit id is recorded, and resolve
   conflicts in favour of our giulia constants.

## Take

| Area | Upstream change | Why |
| --- | --- | --- |
| UFS / var_store | `hakios.ufs.c`, `hakios.ufshc.c`, `userland/src/var_store.c` | Compare against our `redwood_ufs.c` / `redwood_ufshc.c`; take the write-path, claim and save-on-return fixes, keep the giulia `darwin` geometry |
| Loader | generic fixes in `xnuloader/loader.c` (timeout handling, USB, block bounds) | Our `platform_giulia` and S3910 probe stay; only shared logic moves |
| APFS / minicrypto | read-only APFS fixes | Both trees read the same iOS 16.5 volume |
| Scheduler / locks | generic kernel fixes | Same XNU base (`xnu-8796.121.2`) |
| USB | CDC/log transport fixes | Our debug path is USB |
| Device layer shape | `devices/<phone>/` layout, `pull-soc.sh`, `soc.args` | Already mirrored in `devices/ace5/`; no POCO binaries |
| Legal docs | `LEGAL.md`, `COMPLIANCE.md`, `THIRD_PARTY_LICENSES.md` | Adapt, do not copy verbatim |

## Do not take (yet)

| Area | Reason |
| --- | --- |
| WiFi (`hakios.wifi.c`, `lx`, `sta`, `wlfw`, ath11k glue) | POCO is WCN6750/ath11k; the Ace 5 is WCN7850. Use UniXNU `Ath12kWifi` as the reference instead |
| Audio (`adsp`, `audio`, `sound`, `speaker`, `mic`, `guard`) | SM8650 ADSP/PAS layout and the SIA9177 amps are POCO-specific; a port, not a pick |
| Haptics, cameras, torch, buttons, battery, cpufreq | POCO register maps; useful only as templates |
| Cellular / IPA (`mpss`, `ipa`, `nas`, `qmi`, `cell`) | The `soc_*` override support is worth having, but the modem PAS id is not in this phone's tree (`soc_pas_mpss` is missing in `devices/ace5/soc.args`) and the IPA version may differ. Take the framework after the modem work starts |
| `redwood` → `hakios` rename | Churn; see ground rule 2 |
| `devices/poco-x5-pro/uefi/` binaries, `prebuilt/` | Redwood-specific FD, BootShim and DTB |
| Mesa a6xx (`fd6_draw.cc`) fixes | Our GPU is Adreno 750 (a7xx); the freedreno path comes from UniXNU `Adreno740`, not from here |

## K90 line (`iola1999`): loader v17 and dsu_store — comparison, 2026-11-05

Why this line: our worst loader failure class is "a run ends, USB is dead,
recovery is physical" (see the `loader-usb-dead-after-run` memory and RUNBOOK),
and our `/private/var` persistence (`userland/src/var_store.c`) solves the same
problem as their `dsu_store`, from the kernel side. Their tree is a sibling of
the same oosexgang base, but their loader is split into `xnuloader/loader_*.inc`
fragments while ours is one `loader.c` (4090 lines), so this is a comparison and
a re-expression, not a cherry-pick.

Source commits (`iola1999/hackios-xnu-kernel`, branch `k90-port`):

| Commit | Date (UTC) | What |
| --- | --- | --- |
| `cced4247` | 11-05 02:50 | loader v17: UEFI failure → PSCI reset; no bootable payload → cold restart; BO rejects size 0; host tests updated |
| `ba6f0d08` | 11-05 05:16 | dsu_store: loader exposes the DSU slot's last 4 GiB; the kernel reads/writes the `/private/var` archive through it (`xnuloader/loader_dsu.inc` +165, `tools/test-loader-dsu-store.py` +166) |
| `45d97a8b` | 11-05 05:39 | DSU file extent cap 64 → 1024, with a reason when exceeded |
| `779c08d2` | 11-05 05:44 | `dsu.py`: keep the old image when shared storage is not unlocked |

Compare points (each must end in take / skip / unknown, with a local anchor):

| # | Their side | Our side | Question |
| --- | --- | --- | --- |
| 1 | loader v17 failure paths: PSCI reset, cold restart, size-0 rejection | `xnuloader/loader.c` failure paths: `oem stop`, run end, USB command timeout, UEFI chain return | Does our loader ever sit in a state where USB is dead and nothing resets? Which of their three guards closes it? |
| 2 | store medium: last 4 GiB of the DSU slot, extents | `darwin` partition, two fixed 2 GiB slots (`userland/src/var_store.c:52`) | Do we gain anything from extents? We own the partition; our cap is fixed. |
| 3 | archive atomicity: header last over a zeroed one, checksum, newest good slot | same idea already in `var_store.c` (header last, double read, two slots, claim) | Anything they do that we do not (locked storage, partial write, reason logging)? |
| 4 | UFS write path: `redwood_ufs.c` +41/−68, `loader_03_ufs_block_watch.inc` +32/−72 | our `redwood_ufs.c` / `redwood_ufshc.c` | Are their write-path/claim changes generic, or DSU-specific? |
| 5 | host test `tools/test-loader-dsu-store.py` (+166) | `tools/ace5/host-selftest.sh` | Is there a loader-store test we can run on the host before a phone run? |

Read method: `git fetch` to github.com times out from this host; read files with
`gh api repos/iola1999/hackios-xnu-kernel/contents/<path>?ref=<sha>` (base64
decode) and diffs with the commits API. Desk comparison — no phone run.

Exit: the take/skip list recorded in this file; only then an implementation plan
for the take items (host selftest + CI kernel build + one phone run).

### Result (2026-11-05, read-only)

| # | Verdict | Evidence |
| --- | --- | --- |
| 1 | **take (candidate — measure on giulia first)** | Their v17 replaces `gRT->ResetSystem` with a PSCI `SYSTEM_RESET` via `ArmMonitorCall`. Measured on annibale: the firmware path brought the phone back after **5 min 26 s**, PSCI after **34 s**. All four of our reset sites use EFI ResetSystem: `uefi/files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.c:303` (payload returned/missing), `xnuloader/loader.c:589` (`reboot()`), `:2492` (UEFI exception), `:3882` (`reboot`/`oem reboot`). Our loader already has `psci_call` with HVC/SMC per platform, so the loader side is a small change; the UEFI side needs `ArmMonitorLib` + `ArmPkg/ArmPkg.dec` in `BootManagerLib.inf` (their `mu-silicium.patch`). Gate: on giulia, time payload-return → Android screen with both paths; take only if PSCI is not slower and still cold-boots. |
| 2 | skip | dsu_store is the tail of a 16 GiB DSU file in `userdata`; extents exist because that file fragments. We own a contiguous GPT partition (`darwin`), fixed by construction. The 64→1024 extent cap is theirs alone. |
| 3 | skip | Atomicity is already equivalent: header last over a zeroed one, double read, two slots, claim (`var_store.c`). Their extra guard (each header block is zeros or `RDWVAR\0`) protects a stale DSU whose extents became Android data; our equivalent is the GPT identity check plus the fixed layout. |
| 4 | skip | The `redwood_ufs.c` + block-watch diff is the darwin→dsu_store migration itself (bounds, name/GUID checks, extent I/O). Our write → flush → read-back → CRC path is the same shape (`xnuloader/loader.c:2154`); nothing generic to take. |
| 5 | skip (pattern noted) | Their host test checks DSU listing predicates; our `host-selftest.sh` already checks GPT → constants → apply → carve preflight. Revisit the pattern only if we ever add a store. |

Only #1 goes forward. Its own gate is a device measurement (one UEFI build, one
loader build, two timed runs), not a code review; nothing is picked until the
giulia numbers exist.

## Procedure

```sh
git remote add upstream https://github.com/oosexgang/hackios-xnu-kernel.git
git fetch upstream
git log --oneline upstream/main ^HEAD -- xnu/files/bsd/dev/redwood_ufs.c
git cherry-pick -x <sha>
tools/ace5/host-selftest.sh
# then dispatch build-kernel.yml by hand before any phone run (no push trigger)
```

Record each pick below with its upstream id and the local commit, so a later
full rebase can be reconciled against it.

| Upstream | Local | What |
| --- | --- | --- |
| _(none yet)_ | | |
