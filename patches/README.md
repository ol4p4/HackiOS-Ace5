# Ace 5 patches

Two patches, applied in this order:

1. **`ace5-smp.patch`** — `common/devices/giulia.conf` and `tools/ace5/phone.py`.
   - Adds `ACE5_CPUS=1..8` and `ACE5_CPUMASK=<number>`.
   - Emits exactly one of `cpus=` / `cpumask=` (XNU panics if both are present).
   - `cpumask` bits are positions in the loader's CPU list, not physical core
     numbers; the loader omits its own USB core, so `cpus=8` is the normal path.
   - Removes the duplicate `cpus=1`.
   - Also carries the inert `--persist` / `--stage persist` flags used by patch 2.

2. **`ace5-varstore.patch`** — `tools/ace5/build-loader.sh` and `tools/ace5/pack-boot.py`.
   - Adds the `persist` loader stage (`HACKIOS_STORAGE_WRITES=1`).
   - Lets `pack-boot.py --stage persist` accept `storage_writes=true` and emit
     `allowed_partition_write: ["logdump", "darwin"]`.

Patch 2 is inert until you build and enter the `persist` stage. The diagnostic and
kernel loaders stay write-disabled.

Apply:

```bash
git apply patches/ace5-smp.patch
git apply patches/ace5-varstore.patch
```

After the destructive carve, fill the constants printed by
`tools/ace5/storage-constants.py` into `xnuloader/loader.c` and
`xnu/files/bsd/dev/redwood_ufs.c` (or let `tools/ace5/apply-storage-constants.py`
do the five replacements). The runbook (`RUNBOOK.md`) has the full order,
including the backup and rollback gates.

Host-only tooling, not part of the two patches: `tools/ace5/extract-hw.sh`,
`storage-constants.py`, `apply-storage-constants.py`, `check-host.py`,
`host-selftest.sh`.
