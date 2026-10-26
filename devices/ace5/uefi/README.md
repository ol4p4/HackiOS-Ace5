# UEFI (giulia)

The Mu-Silicium port for this phone is built from the top-level `uefi/` patch
tree by `tools/ace5/prepare-uefi.py`, pinned in
`.github/workflows/build-ace5.yml` (`Project-Silicium/Mu-Silicium`,
commit `42fe5637`). `tools/ace5/build-diagnostic.sh` produces the FD and
`tools/ace5/pack-boot.py` packs it with the DTB and payload.

Upstream's device layer keeps the same inputs here (`BASE`, `apply.sh`,
`export.sh`, a `Platforms/<vendor>/<pkg>` package and the prebuilt FD). Moving
the existing port into that shape is part of the targeted cherry-pick, not a
separate rewrite; until then this directory is a marker, not a build input.
