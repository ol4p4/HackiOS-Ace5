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

## 6. K90 的实测与源码（2026-11-06，源码已公开）

`docs/WIFI.md` 写"annibale 的 `redwood_pcie.c` 没有公开"已过时：2026-11-06 时
`iola1999/hackios-xnu-kernel` 的 `xnu/files/bsd/dev/` 下有完整的
`mhi_pcie.c`、`mhi_pcie_power.c`、`mhi_pcie_fw.c`、`mhi_pcie_wlfw.c`、
`mhi_pcie_phy_seq.h`、`mhi_pcie_private.h`，可直接读。

他们踩过与本次相同的坑，并把结论写进了 `mhi_pcie.c` 的注释：

- **step 1 只读 GCC/TLMM**：两个 PCIe GDSC、根复合体的时钟、DT 接的六个引脚
  （wlan-en 16、sw-ctrl 18/19、perst 102、clkreq 103、wake 104）。
- **step 2 才读根复合体**，而且有两个前提：① 隔 30 秒，"so that the host has step 1's
  report if these reads stall the SoC"；② 两个 GDSC 都 `PWR_ON` 且 `cfg_ahb`、`slv_axi`、
  `mstr_axi`、`aux` 时钟都在跑（`show_power()` 的 `readable`）。读的是 LTSSM、
  LINKCTRLSTATUS、PHYSTATUS、DBI 里的 RC 头、整个 iATU 窗。
- **他们的第一次把根复合体读取放在 step 1（内核 `474d6e9`），第 8 秒就不回 USB 了**
  ——和我们在 2026-11-06 遇到的两次完全同型。
- 映射本身是安全的：他们在 survey 一开始就把 parf/phy/dm_core/conf/pcie_sm/gcc/tlmm
  全映射了；**杀 USB 的是读，不是 map**。

对本板的含义：`giulia_pcie.c` 现在的 step 1（不映射、不读 PCIe 窗，只 GCC/TLMM）就是
K90 的 step 1；窗读取必须另行成步，并满足他们的两个前提。`windows_clocked()` 也要按
K90 的规则收紧：两个 GDSC + 四个时钟，而不是只看 core GDSC 一个位。

## 7. step 2 上电顺序对照（2026-11-06，读 `mhi_pcie_power.c`）

| 阶段 | K90（`mhi_pcie_power_up`） | 我们（`giulia_pcie.c`） | 差异 |
| --- | --- | --- | --- |
| PERST / WLAN_EN 拉低 | B：PERST 2 mA、WLAN_EN 8 mA 输出低 | `gpio_out` 两脚拉低 | 同 |
| RPMh 供电 | C：从 cmd-db 找齐七路 `vdd-wlan-*` 再投票 | **没有** | 缺；没有供电 PHY/卡不会起 |
| 电源域与时钟 | D：两个 GDSC + `clocks_on()` | GDSC core/phy + vote `0x2f` | 同 |
| 复位与控制器 glue | E/F：`resets_pulse()` 后 **`parf_setup()`**（DBI_BASE_ADDR_V2、RO_WR_EN、LNKCTL2 目标 Gen1） | 只有复位脉冲，**没有 parf_setup** | 缺 |
| PHY | G：厂商序列 | DTB 115 步序列 | 同 |
| 卡使能 | H：WLAN_EN 高 → 50 ms → CLKREQ/WAKE 引脚 → `root_port_setup` → 50 ms（使能到 PERST 100 ms）→ **PERST 高** | **先 PERST 高，再 WLAN_EN 高** | 顺序相反 |
| 链路训练 | `PARF_LTSSM |= LTSSM_EN`，10 × 90 ms 轮询状态机与 LINKCTRLSTATUS | 只轮询 DBI 的 LINKCTRLSTATUS，**没有写 LTSSM_EN** | 缺 |

step 2 真正上机前，这四处（RPMh 七路、parf_setup、使能→PERST 顺序、LTSSM_EN）要补齐；
否则链路不会按 PCIe 规范起来。
