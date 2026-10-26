# OnePlus Ace 5 (giulia, PKG110)

Everything in HakiOS that belongs to this one phone lives here. The kernel, loader
and user space above know nothing about it: a port to another Qualcomm phone is
another directory like this one.

| Item | Value |
| --- | --- |
| SoC | Qualcomm SM8650 ("pineapple"), Snapdragon 8 Gen 3 |
| GPU | Adreno 750v2, chip id `0x43051401`, `qcom,adreno-gpu-gen7-9-1` |
| Display | 1264x2780 |
| Touch | Synaptics S3910 over SPI |
| Storage | UFS `ufshc@1d84000`, LUN 0 = `sda` |
| Port stage | diagnostic / kernel / persist (see `RUNBOOK.md`) |

## What is here and what is not

This directory is the upstream-shaped device layer for the targeted cherry-pick
path. The current tree still builds through `tools/ace5/` and the top-level
`uefi/`; nothing in the build reads this directory yet.

| Path | State |
| --- | --- |
| `soc.args` | generated from this phone's own device tree; see below |
| `pull-soc.sh` | regenerates `soc.args` from `/sys/firmware/fdt` over adb |
| `pull-firmware.sh` | pulls the GPU firmware into `fw-gpu/` (root shell; `adb pull` is denied) |
| `uefi/` | the Mu-Silicium port is built by `tools/ace5/prepare-uefi.py` for now |
| `prebuilt/` | build outputs land in `out/giulia*/`; nothing is committed here |

Not in the repository, never: iOS images, the phone's firmware, keys, or anything
from Apple's servers. `pull-firmware.sh` writes into `fw-gpu/`, which git ignores.

## soc.args

`tools/ace5/soc-args.py` reads the phone's flattened device tree and prints the
`modem`/`ipa` addresses as boot arguments. It is adapted from the MIT-licensed
`tools/soc-args.py` in `oosexgang/hackios-xnu-kernel`; the upstream matcher
assumes POCO X5 Pro node names, while SM8650 differs:

- `aoss` is `qcom,aoss-qmp` on `power-controller@c300000` (no SoC prefix);
- `cmd_db` is the `aop_cmd_db_region` reservation (compatible `qcom,cmd-db`);
- `smem` carries `qcom,smem` on the reservation itself;
- the modem's memory is the reservation its `remoteproc-mss` node points at
  (`mpss_region@8bc00000`), not the first node named `mpss` (`mpss_dsm_*` come first).

Regenerate from a phone:

```sh
ACE5_SERIAL=<serial> bash devices/ace5/pull-soc.sh
```

Or from a saved tree:

```sh
python3 tools/ace5/soc-args.py local/device/giulia.dtb --check
python3 tools/ace5/soc-args.py local/device/giulia.dtb > devices/ace5/soc.args
```

Verified against `local/device/giulia.dtb` (2026-10-26). One value is not in the
tree and is left out on purpose: `soc_pas_mpss` (the modem node has no
`qcom,pas-id`), so the kernel's built-in default stays until the modem work
proves the right id.

The kernel only honours these arguments after the `soc_*` override support is
cherry-picked from upstream; until then they are inert and safe to pass.

## UEFI and boot images

`tools/ace5/build-diagnostic.sh` builds the Mu-Silicium FD, `tools/ace5/pack-boot.py`
packs the boot image, and `phone.py` runs the guarded stages. Upstream's
`devices/<phone>/uefi/` layout (BASE pin, patch, platform package, prebuilt FD)
is the shape this directory should take after the cherry-pick; the Mu-Silicium
pin used today is in `.github/workflows/build-ace5.yml`.

## Storage

`darwin` is the port's own partition at the end of LUN 0: 20 GiB carved off
`userdata` (entry 15, `sda16`), holding the var_store slots. The carve tooling is
`tools/userdata-carve.py` plus `tools/ace5/storage-constants.py`; the phone-side
superblock is read from the mapper device because `/data` is metadata-encrypted.
See `RUNBOOK.md` Phase 2.
