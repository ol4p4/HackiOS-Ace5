# Prebuilt (giulia)

Nothing is committed here. Build outputs stay under `out/`:

| File | Built by |
| --- | --- |
| `out/giulia/hackios-giulia-diagnostic.img` | `tools/ace5/build-diagnostic.sh` |
| `out/giulia-kernel/hackios-giulia-kernel.img` | `build-loader.sh kernel` + `pack-boot.py --stage kernel` |
| `out/giulia-persist/hackios-giulia-persist.img` | `build-loader.sh persist` + `pack-boot.py --stage persist` |
| `out/giulia-kernel/mach.ace5-public` | `xnu-build.sh` |

The CI workflows upload them as `ace5-kernel` / `ace5-system` artifacts;
`tools/ace5/install-artifact.py` places them back into `out/` and `local/`.
