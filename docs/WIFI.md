# Ace 5 Wi-Fi（WCN7850 / ath12k over PCIe）bring-up 计划

第 2 条线的落地文档。目标不是"写一个 Wi-Fi 驱动"，而是**把 Linux 的 ath12k 编译到本仓已有的 LinuxKPI 层上**，前面补它需要的两段本机硬件：PCIe 主机控制器与 MHI。硬件底稿见 `docs/ACE5-HW.md` 第 1 节，驱动矩阵见 `docs/DRIVERS.md`。

## 1. 为什么不是"从零写"

本仓已经有三块现成资产，POCO X5 Pro（WCN6750/ath11k）就是靠它们跑出 55–90 Mbit/s 的：

| 资产 | 位置 | 作用 |
|---|---|---|
| FreeBSD LinuxKPI 补丁 + 我们补的头 | `wifi/lx/`（227 KB） | 让 Linux 无线驱动以"FreeBSD 内核代码"的方式编进 XNU |
| 内核侧 glue | `xnu/files/bsd/dev/redwood_lx.c` + `bsd/sys/redwood_lx.h` | 锁、睡眠、work、内存、固件、总线这一小把调用 |
| 把驱动编成内核对象 | `tools/mk-ath11k.sh` | 取 Linux 7.0.14 的 `drivers/net/wireless/ath/ath11k`，配 FreeBSD 15.1 的 LinuxKPI，按 `DRIVER` 列表编译，缺的符号变成日志里报名字的桩（`out/stubs.txt`） |

WCN6750 与 WCN7850 的差别只在**总线与固件协议**：6750 走 WPSS（AP 侧 DSP，`redwood_wifi.c` 的 SMEM/SMP2P/IPCC/GLINK/QRTR 那 16 步就是它），7850 是**独立 PCIe 模块，固件跑在模块内部，AP 通过 MHI 与它通信**。所以本线的增量是：

1. **PCIe0 主机控制器——移植，不是新写**。同 SoC（SM8750）的 annibale/K90 已经在 2026-10-27 跑通 RC0 上电、Gen1 x2 链路、WCN7850 BAR0 与 BHI PBL 读取（同族仓库 annibale；本地摘要 `local/refs/k90-wcn7850-plan.md:3,107`，`local/` 不入库）；thyme 的 `umi_pcie.c`/`umi_mhi.c` 是另一块 SoC 上的完整联网路径（本地摘要 `local/refs/thyme-port-summary.md`）。本线的做法是 **diff 这两份代码与本板 DTB，只搬 SM8650 的寄存器序列**；Linux 的 `pcie-qcom.c`/`phy-qcom-qmp-pcie.c` 只作寄存器语义参照。三机的常量对照（哪些能抄、哪些必须重取）见 `docs/PCIe-DIFF.md`。
2. **MHI host**：接口面照 thyme `umi_mhi.c`，状态机照 Linux `drivers/bus/mhi`。
3. **ath12k** 接到 `mk-ath12k.sh`（PCI + MHI 路径），以及 board data 的 QMI（QRTR over MHI）。
4. cnss 外壳：RPMh 供电（`vdd-wlan-*` 七路）、`wlan_en`/`sw_ctrl`/`bt_en` 引脚、保留内存。

已有可复用的部分：`redwood_wifi.c` 的 QRTR 实现（`sys/redwood_qrtr.h`）与 QMI 报文格式；`wifi/lx` 的 802.11 与 wpa 层。

## 2. 硬件事实（来自本机 DTB，`tools/ace5/hw-facts.py`）

- 端点外壳：`qcom,cnss-kiwi@b0000000`（kiwi = WCN7850），`qcom,wlan-rc-num = 0` → **pcie0**。
- 控制器：`/soc/qcom,pcie@1c00000`（`qcom,pci-msm`，5 个中断 0x8c/0x95/0x96/0x97/0x98，SPI 4）；pcie1 在 `0x1c08000`（本机 Wi-Fi 不用）。
- MSI：`/soc/qcom,pcie0_msi@0x17110040`（`qcom,pci-msi`）。
- PHY/时钟：`crm@1d01000`（`qcom,pcie-crm`）、`pdc@b350000`（`qcom,pcie-pdc`）、`pcie_0_pipe_clk`；`qmp` 句柄 `0x63`（QMP 邮箱 99）。
- SMMU：`pcie_qtb@16cd000`，`qcom,stream-id-range: 5120 1024`，`qcom,iova-width: 36`。
- 引脚：`cnss_wlan_en_active/sleep`、`qps615_intn_wol`（前端模块中断/WoL）。
- 供电（RPMh，七路成对给上下电电压）：`vdd-wlan-aon/dig/io/io12/rfa1/rfa2/vdd-wlan`（0xee480/0xdfa20/0x1b7740/0x124f80/0x1c7140/0x1414a0/0xe57e0，单位 µV）。
- 保留内存：`cnss_wlan_region`；ramdump 动态区 `0x780000`。
- QRTR：`/soc/qcom,qrtr-mhi-cnss` 在 DT 里就写着 **MHI**——这条总线的协议栈是确定的。
- **`pcie_sm_regs=0x1D07000`**：stock Android cmdline 里就有（`RUNBOOK.md:1016`），K90 的 RC0 上电序列也用它。本机 DTB 的 `reg` 块里 `pcie_sm` 就是它（`0x1d07000+0x7000`），不需要外部常量。
- **设备号 `0x17cb:0x1107`**：本机 DTB 的 `supported-ids` 与 QRTR 的 `qcom,dev-id` 都写 `0x1107`（vendor `0x17cb`）；枚举时核对，再去 ath12k 的 `pci_device_id` 表。
- **M1 的输入已导出（2026-10-30）**：`hw-facts.py` 现在抽 clocks/clock-names、resets/reset-names、`perst-gpio`/`qcom,pcie-clkreq-gpio`/`wake-gpio`、`reg-names`、`ranges`、`iommu-map`/`msi-map`、`qcom,phy-*`、`qcom,pcie-sm-*`，`docs/ACE5-HW.md` 附录是完整表。关键值：parf `0x1c00000`、phy `0x1c06000`、dbi `0x60000000`、elbi `0x60000f20`、atu `0x60001000`、conf `0x60100000`、pcie_sm `0x1d07000`；PERST/CLKREQ/WAKE = GPIO 94/95/96；RID `0x100` → SID `0x1401`；`cnss_wlan_region` 32 MiB；`qcom,phy-sequence`（115 步）就在 DTB 里。
- **已知卡点——QMP**：`qcom,qmp = 0x63`（邮箱 99）是 cnss 侧电源握手的 "QMP open"，也是 tycykp 的 WCN7850 尝试卡住的地方（`docs/ACE5-HW.md:18`）。它不能只当背景事实，M1/M3 各要一条独立的验收行。
- **同族的两道墙**：11 Ultra 的 WCN6855（`17cb:1103`）固件/BDF/NV 已抽取校验，但缺 PCIe LTSSM PIPE 时钟与 MHI/BHI 下载器，链路未起（对应我们的 M1–M4）；dada 的 WCN7850 到了 WMI，但固件大多数运行会死，其 Wi-Fi 驱动目前是临时替身。前者说明 M1 不是走过场，后者说明 M4 之后还有一道固件稳定性墙。

## 3. 里程碑与验收（每步一次上机，boot-arg 控制，同 `redwood_wifi.c` 的风格）

| # | 内容 | 验收（日志/现象） | 风险 |
|---|---|---|---|
| M1 | PCIe0 控制器复位、时钟、PHY 上电与 LTSSM 训练 | `link up, speed gen2/gen3, width x1/x2`；`pcie0: link training done` | 时钟树与 PHY 组合序列最杂；**代码已就位**：`giulia_pcie.c` 的 `pcie_step=1`（只读勘察）/`2`（上电+链路），PHY 序列来自 DTB |
| M2 | 配置空间访问 + 枚举 + **PARF RID→SID 读回** | 读到 vendor `0x17cb`（期望 device id `0x1107`）与 BAR0 大小；`cnss_pci0` 出现；RID `0x100` → SID `0x1401` 可读回 | ECAM 窗口/ATu 配置；K90 实测 PARF bypass=1、RID 链出现环、SID 未验证（`local/refs/k90-wcn7850-plan.md:73,135-137`） |
| M3 | BAR0 映射、MHI 寄存器可读、MSI 落点、**QMP open** | `MHI_READY`/`MHIVER` 读出，写 MSI 后收到一次中断；QMP 握手返回 | MSI 控制器 0x17110040 的编程；QMP 邮箱 99 |
| M3.5 | **apps SMMU/QTB context、IOVA 窗、端点 DMA 映射** | 一次 host→device 的 DMA 往返（host 写一块 buffer，模块读回同一个值） | SID `0x1401` 的路由与 context；与 DisplayDxe/保留区撞窗（K90 放弃准则第 4 条） |
| M4 | MHI 主机状态机 + **BHI/BHIe 固件下载** | `mhi: device in M0`；DB 通道可写；`EE` 同步完成 | 模块停在 PBL（K90 实测），必须由 host 下载：先 BHI 下 `amss.bin` 前 512 KiB，再 BHIe 下全量。固件来源 `/vendor/firmware_mnt/image/kiwi`（modem 分区）或 linux-firmware `ath12k/WCN7850/hw2.0/`；版本/长度/SHA-256 必须固定 |
| M5 | QRTR/QMI：board data 下发，ath12k 起 | `ath12k: firmware ready`；`wlan0` 出现在 `net_test show` | board-2.bin/regdb 的来源与国家码要写死；MAC 地址来源（thyme 不给 `wifi_mac=` 就扫不到网）；board file 版本与固件 API 对齐；**ath12k 版本要钉**（见 §7：7.0.14 的 802.11 头对不上 LinuxKPI，6.12.1 只差 MHI 与几个宏） |
| M6 | mac80211/扫描/加入 | 设置里扫到 AP、加入、Safari 打开 `https://example.com` | 走既有 `wifi/lx` 的 802.11/wpa 层 |

M1–M3.5 是"移植 + 板级差异"：annibale 给出同 SoC 的寄存器与流程，thyme 给出 MHI 的接口面，本板要改的是基址、GPIO、RPMh 轨和 SMMU 映射。M4 的 `mhi_*` 接口正好是 ath12k 要的那一小把（`mhi_alloc_controller/mhi_register_controller/mhi_prepare_for_power_up/mhi_sync_power_up/mhi_prepare_for_transfer/mhi_queue_*`），FreeBSD LinuxKPI 没有 MHI 框架，这一层要由我们提供。M5–M6 是"接上 ath12k"，理论上是 `mk-ath11k.sh` 的兄弟脚本加 `DRIVER` 列表。

**容量预算**：`phone.py` 的小根镜像上限 32 MiB（`tools/ace5/phone.py:272-273`）。WCN7850 的 `amss.bin`、`m3.bin`、`board-2.bin`、regdb 都要进 `fw/`，加上已有的 GPU 四件（`local/root-daily-fw.apfs` 已经 10.7 MB）。M4 之前必须做一次预算；超了就先把固件换成分区读取或第二个 ramdisk，不能到 M4 才发现。

## 4. 立即要做的下一步（主机侧，不需要手机）

0. **上机（M1）**：`phone.py run --pcie 1 --kernel out/giulia-kernel/mach.ace5-public --root-image local/root-daily-fw.apfs` 先跑只读勘察，确认基址、GDSC 与时钟状态；再 `--pcie 2` 上电并等链路。
1. **先 diff，再抄**：取 annibale 的 `redwood_pcie.c`（同 SM8750）与 thyme 的 `umi_pcie.c`/`umi_mhi.c`，对着本板 DTB 逐项列出"同/异"；Linux `pcie-qcom.c`、`phy-qcom-qmp-pcie.c` 里的对应段只作寄存器语义对照，不整段照抄。
2. **把 DTB 里的序列变成头文件**：从 `qcom,phy-sequence`（offset/value/delay 三元组）、`qcom,pcie-sm-seq`、`qcom,pcie-sm-branch-seq` 生成 `giulia_pcie_regs.h` 的初稿。这些是厂商数据，不是 GPL；和从 Linux 抄来的寄存器语义分开放，并在文件头注明来源。基址已核对：parf/phy/dbi/elbi/atu/conf/pcie_sm 见 `docs/ACE5-HW.md`。
3. 写 `xnu/files/bsd/dev/giulia_pcie.c` 的 M1 步骤（只读+复位+PHY，绝不写端点），boot-arg `pcie_step=N`；同时给 `phone.py` 加 `--pcie`（或让 `ACE5_CMDLINE` 能覆盖默认的 `wifi=0`，避免命令行里出现两个 `wifi=`）。

## 5. 明确不做

- 不做 Android `cnss` 的完整电源/热管理外壳（热区、ramdump、IPA offload）；只做 M1–M6 需要的最小集。固件由 host 经 BHI/BHIe 下载（见 M4），不是"模块自己起"。
- 不碰 pcie1（dsu/其它用途）。
- 不在 WiFi 线上做 120 Hz、相机或音频。

## 6. 风险与放弃准则

从 K90 的 `local/refs/k90-wcn7850-plan.md:118-126`（本地，未入库）平移，按本板改写：

1. **PCIe 起不来**：连续 3 个真机窗口 link-up 失败（PERST/时钟/上电序排查完仍无 LTSSM detect）→ 放弃本轮。嫌疑点：LK/UEFI 是否已初始化 RC0 并被安全态锁住；`pcie_sm` 序列与 bootargs 常量对不上。
2. **MHI 下载成功但固件不 boot**（SBL 无 state change / QMI 服务不出）≥2 周 → 降级为"能枚举到设备 + 读卡 ID"存档。
3. **M4 过了但固件不稳**：连续多次启动里固件在 WMI/校准阶段死掉（dada 的 WCN7850 现状）≥2 周 → 只保留"能起、能读卡 ID"的结论，不追数据面。
4. **SMMU/IOVA 冲突**：端点 SID `0x1401` 的路由或 context 配置失败，或与 DisplayDxe/保留区撞窗 → DMA 不可用 → 放弃数据面，保留结论。
5. **数据面 timebox**：M6 开工 2 周无第一笔 TX 完成 → 砍到"QMI 握手通"为止。
6. 板级校准/BDF 缺失可能只有 2.4G 保守速率——可接受，不算失败。
7. **工期**：真机窗口累计 4–8 周；从开工到"可用联网"保守 1.5–3 个月（K90 的估计，`local/refs/k90-wcn7850-plan.md:96,114`）。本计划不承诺更短。
8. **待验证**：WLAN 模块是否和 GPU/TrustZone 一样有冷启动状态残留（`RUNBOOK.md:1105-1108`）；第一次 M1 上机顺带记录。

## 7. ath12k 首次编译（2026-10-30）

`tools/mk-ath12k.sh` 把 Linux 的 ath12k（7.0.14 含 `wifi7/hal_wcn7850.c`）按 ath11k 的方式编到 FreeBSD 15.1 的 LinuxKPI 上；`VERSION=` 可换 Linux 版本。**结论：M5–M6 不是"兄弟脚本加一个 DRIVER 列表"，而且版本选择决定代价。**

| Linux | 编过的 ath12k 文件 | 失败 | 主要缺口 |
|---|---|---|---|
| 7.0.14 | 20 / 37 | 17：core、dp、dp_htt、dp_peer、dp_rx、dp_tx、mac、mhi、p2p、pci、qmi、reg、wifi7/{core,dp_mon,dp_tx,hw}、wmi | LinuxKPI 的 802.11 头比 7.0 旧：`rate_info.eht_ru_alloc`、`ieee80211_mgmt.addba_resp`、`ieee80211_ops.link_sta_statistics`、`WLAN_EXT_CAPA11_BCN_PROTECT`、`WLAN_EID_NON_TX_BSSID_CAP`、radiotap EHT 枚举；通用设施缺 `linux/clk.h`、`mdelay`、`skb_cow_data`、`rcu_read_lock_any_held`、`cleanup_spinlock_bh_t`、`kmalloc_objs` 的参数个数、`struct reserved_mem`、`KHZ_TO_MHZ` |
| 6.12.1 | 12 / 19 | 7：ce、dp、dp_rx、dp_tx、mac、mhi、pci | 只剩 `mdelay`、`from_timer`、`WLAN_EID_NON_TX_BSSID_CAP`、`IEEE80211_EHT_PHY_CAP4_PSR_SR_SUPP`、`SET_SYSTEM_SLEEP_PM_OPS`；MHI 的 `mhi_*`。芯片支持在（`hw.c` 有 `ATH12K_HW_WCN7850_HW20`） |

已补的编译层 shim（`wifi/lx/include/`，只解决编译，不代表功能）：`linux/panic_notifier.h`（+ `wifi/lx/src/lx_panic.c`）、`acmacosx.h`（ACPICA 在 `__APPLE__` 下找不到的平台头）、`linux/of_graph.h`、`linux/rcupdate.h`（`RCU_LOCKDEP_WARN`）。`lx_bus.c` 是 ath11k 的 AHB 总线（include 了 ath11k 的 `pcic.h`），不编进 ath12k 对象。

三条路线：

1. **钉 6.12.1 的 ath12k**（推荐先做）：12/19 已过，补上面五个小项后重测，预期只剩 MHI。
2. **补 LinuxKPI 到 7.0**：7.0 的 17 个失败文件对应几十处 802.11/cleanup/slab 差异，还要跟 FreeBSD 的补丁集合并；代价最大。
3. **K90 的 Plan A**：不用 ath12k 的 mac80211 层，手写最小 WMI；绕开 802.11 头，但 MHI 仍要自己写。

MHI 是三条路都绕不开的：`mhi_power_down_keep_dev`、`mhi_download_rddm_image` 等，LinuxKPI 没有 MHI 总线——正是 M4 要提供的那个 `mhi_*` 层。
