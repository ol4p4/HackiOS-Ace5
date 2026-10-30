# PCIe0 三机对照：annibale（SM8750）、thyme（SM8250）、giulia（SM8650）

M1 的常量哪些能抄、哪些必须从本机 DTB 重取。annibale 的 `redwood_pcie.c` / `mhi_pcie.c`
**没有公开**（仓库里只有实测记录与计划），所以 annibale 一列来自 `docs/progress/2026-10-26-wifi-pcie-survey.md`
与 `docs/plan/wcn7850-plan.md`；thyme 的 `umi_pcie.c` / `umi_mhi.c` 是公开的完整实现；
giulia 一列来自 `dtb/giulia.dtb`（`tools/ace5/hw-facts.py`、`tools/ace5/pcie-regs.py`）。
本地副本在 `local/refs/pcie/`（`local/` 不入库）。

## 1. 对照表

| 项 | annibale（K90，SM8750） | thyme（Mi 10S，SM8250） | giulia（Ace 5，SM8650） | 结论 |
|---|---|---|---|---|
| RC0 节点 | `pcie@1c00000` | `pcie@1c00000` | `pcie@1c00000` | 同 |
| PARF | `0x1c00000+0x3000` | `0x1c00000` | `0x1c00000+0x3000` | 同 |
| PHY | `0x1c06000` | `0x1c06000` | `0x1c06000+0x2000` | 同 |
| DBI/ELBI/ATU/CONF 窗 | elbi `0x40000000`、conf_apb `0x40100000`、atu `0x40080000` | DBI `0x60000000`、ATU `0x60001000`、CONF `0x60100000` | dm_core `0x60000000`、elbi `0x60000f20`、iatu `0x60001000`、conf `0x60100000` | **不同**：K90 在 `0x4xxxxxxx`，本机与 thyme 在 `0x6xxxxxxx` |
| config 窗 | io `0x40200000+1 MiB`、mem `0x40300000+0x3d00000` | BAR `0x60300000` | io `0x60200000+1 MiB`、mem `0x60300000+0x3d00000` | 本机与 thyme 同基址 |
| pcie_sm | `0x1d07000+0x1000` | 未用 | `0x1d07000+0x7000` | 本机多 `0x6000` |
| PERST | GPIO102 | GPIO79 | GPIO94 | **三机各不相同** |
| CLKREQ / WAKE | 103 / 104 | 80 / 81 | 95 / 96 | 各不相同 |
| WLAN_EN / SW_CTRL / WLAN_SW | 16 / 18 / 19 | 20 / — / — | 16 / 18 / 19 | 本机与 K90 同 |
| GDSC | `0x6b004` / `0x6c000`（读数 `0x0222f801` / `0x02222801`，bit31=0） | `0x6b004` / `0x6c01c`（复位） | `0x6b004` / `0x6c000` | 偏移同，本机值待测 |
| 时钟投票 | 6 路 | `0x52008 = 0x2f` | `0x52008`，6 路 | 同 |
| 复位 | core / phy | `0x6b000` / `0x6c01c` | `0x6b000` / `0x6c01c` | 同 |
| PHY 序列 | 原厂 DT（未公开） | QMP v4.4.14，约 100 步 | DTB 115 步 | **用本机的** |
| RID→SID | RID `0x100` → SID `0x1401`，base `0x1400` | SID base `0x1c00`，CRC8/0x07 | `iommu-map` 同 K90（`0x1400`/`0x1401`） | 本机与 K90 同 |
| IOVA | 视口 `0x98000000+0x80000000`，fastmap `+0x18010000` | DMA 窗 `0xa0000000..0xb0000000` | pool `0x98000000+0x18000000`，geometry `+0x18010000` | 本机与 K90 同 |
| MHI 冷 PBL 签名 | MHIVER `0xdeadbeef`、MHIREGLEN `0x100`、BHI `0x100`、BHIe `0x224`、DB `0x300`/`0x700` | 同签名（`umi_mhi.c` 里逐项校验） | 未测 | 预期同 |
| BAR0 | 2 MiB 64 位 | QCA6390 1 MiB | 未测 | 待 M1 后 |
| 固件 | 模块停在 PBL，host 经 BHI/BHIe 下载 | `amss20`/`m3` 从分区 | `/vendor/firmware_mnt/image/kiwi` 或 linux-firmware | 同族流程 |

## 2. 能直接用的

- **MHI 的寄存器布局与冷 PBL 签名**：thyme 的 `umi_mhi.c` 可整段照搬，只换 BAR 基址与 DMA 窗。
- **GCC 的 GDSC / 复位 / 时钟投票偏移**：`0x6b004`、`0x6c000`、`0x52008`、`0x6b000`、`0x6c01c`。
- **RID→SID**：SID base `0x1400`/`0x1401`，CRC8-MSB poly `0x07`，RID `0x100` 的索引 `0x15`、值 `0x01000100`。
- **IOVA 窗**：`0x98000000+0x18010000`。
- **上电顺序**：PERST 低 → GDSC → 时钟 → 复位 → PHY → PERST 高 → WLAN_EN 高。

## 3. 必须按本机改的

- 所有 DBI/ELBI/ATU/CONF 基址：K90 的 `0x4xxxxxxx` **不能抄**；本机是 `0x60000000` / `0x60000f20` / `0x60001000` / `0x60100000`。
- PERST/CLKREQ/WAKE 三个 GPIO（94/95/96）。
- PHY 序列（115 步，来自 DTB）与 pcie_sm 序列（13 words）。

## 4. 还没验证的

- GDSC 的冷态值（K90 的 `0x0222f801` 只是 K90 的读数）。
- 本机 MHI 签名与 BAR0 大小（要 M1 之后）。
- PARF RID→SID 的 bypass 状态（K90 实测 bypass=1 且链有环；本机未测）。

## 5. 来源

- annibale：GitHub `iola1999/hackios-redmi-k90-annibale` 的 `docs/progress/2026-10-26-wifi-pcie-survey.md`、`docs/plan/wcn7850-plan.md`（代码未公开）。
- thyme：GitHub `azzlantern/hackios-thyme` 的 `devices/thyme/overlays/xnu/files/bsd/dev/umi_pcie.c`、`umi_pcie_phy.h`、`umi_mhi.c`。
- giulia：`dtb/giulia.dtb` → `docs/ACE5-HW.md`、`xnu/files/bsd/dev/giulia_pcie_regs.h`。
