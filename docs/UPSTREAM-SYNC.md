# Upstream sync (targeted cherry-pick)

Our tree is forked from the `YiKongk/HackiOS-Ace5` snapshot of 2026-10-23
(`ab2acff`). Upstream `oosexgang/hackios-xnu-kernel` has moved a long way since
(HEAD `88513176`, 2026-10-26). A wholesale rebase is not worth it yet: the
device layer upstream only holds UEFI, prebuilt images, firmware scripts and
`soc.args`, while the drivers stay in the flat `xnu/files/` tree and carry POCO
X5 Pro constants. This file is the take/do-not-take list for pulling the generic
work in without regressing a tree that boots, has touch and reaches GPU probe
stage 20.

## Ground rules

1. Cherry-pick generic subsystems and fixes; never a device-specific driver.
2. Keep the `redwood_*` names. Upstream renamed them to `hakios.*` and touched
   ~364 files; following that rename is pure churn until a full rebase is chosen.
3. Every pick must pass `tools/ace5/host-selftest.sh` and the CI kernel build
   before it goes near the phone.
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
| WiFi (`hakios.wifi.c`, `lx`, `sta`, `wlfw`, ath11k glue) | POCO is WCN6855/ath11k; the Ace 5 is WCN7850. Use UniXNU `Ath12kWifi` as the reference instead |
| Audio (`adsp`, `audio`, `sound`, `speaker`, `mic`, `guard`) | SM8650 ADSP/PAS layout and the SIA9177 amps are POCO-specific; a port, not a pick |
| Haptics, cameras, torch, buttons, battery, cpufreq | POCO register maps; useful only as templates |
| Cellular / IPA (`mpss`, `ipa`, `nas`, `qmi`, `cell`) | The `soc_*` override support is worth having, but the modem PAS id is not in this phone's tree (`soc_pas_mpss` is missing in `devices/ace5/soc.args`) and the IPA version may differ. Take the framework after the modem work starts |
| `redwood` → `hakios` rename | Churn; see ground rule 2 |
| `devices/poco-x5-pro/uefi/` binaries, `prebuilt/` | Redwood-specific FD, BootShim and DTB |
| Mesa a6xx (`fd6_draw.cc`) fixes | Our GPU is Adreno 750 (a7xx); the freedreno path comes from UniXNU `Adreno740`, not from here |

## Procedure

```sh
git remote add upstream https://github.com/oosexgang/hackios-xnu-kernel.git
git fetch upstream
git log --oneline upstream/main ^HEAD -- xnu/files/bsd/dev/redwood_ufs.c
git cherry-pick -x <sha>
tools/ace5/host-selftest.sh
# then a CI kernel build before any phone run
```

Record each pick below with its upstream id and the local commit, so a later
full rebase can be reconciled against it.

| Upstream | Local | What |
| --- | --- | --- |
| _(none yet)_ | | |
