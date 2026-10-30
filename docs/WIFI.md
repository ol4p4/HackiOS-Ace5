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

1. **PCIe0 主机控制器**（枚举、PHY/时钟/复位、链路训练、BAR、MSI）；
2. **MHI host**（模块固件的 MHI 状态机、通道、doorbell、事件环）；
3. **ath12k** 接到 `mk-ath12k.sh`（PCI + MHI 路径），以及 board data 的 QMI（QRTR over MHI）；
4. cnss 外壳：RPMh 供电（`vdd-wlan-*` 七路）、`wlan_en`/`sw_ctrl`/`bt_en` 引脚、保留内存。

已有可复用的部分：`redwood_wifi.c` 的 QRTR 实现（`sys/redwood_qrtr.h`）与 QMI 报文格式；`wifi/lx` 的 802.11 与 wpa 层。

## 2. 硬件事实（全部来自本机 DTB，`tools/ace5/hw-facts.py`）

- 端点外壳：`qcom,cnss-kiwi@b0000000`（kiwi = WCN7850），`qcom,wlan-rc-num = 0` → **pcie0**。
- 控制器：`/soc/qcom,pcie@1c00000`（`qcom,pci-msm`，5 个中断 0x8c/0x95/0x96/0x97/0x98，SPI 4）；pcie1 在 `0x1c08000`（本机 Wi-Fi 不用）。
- MSI：`/soc/qcom,pcie0_msi@0x17110040`（`qcom,pci-msi`）。
- PHY/时钟：`crm@1d01000`（`qcom,pcie-crm`）、`pdc@b350000`（`qcom,pcie-pdc`）、`pcie_0_pipe_clk`；`qmp` 句柄 `0x63`（QMP 邮箱 99，cnss 侧电源握手的那个"QMP open"）。
- SMMU：`pcie_qtb@16cd000`，`qcom,stream-id-range: 5120 1024`，`qcom,iova-width: 36`。
- 引脚：`cnss_wlan_en_active/sleep`、`qps615_intn_wol`（前端模块中断/WoL）。
- 供电（RPMh，七路成对给上下电电压）：`vdd-wlan-aon/dig/io/io12/rfa1/rfa2/vdd-wlan`（0xee480/0xdfa20/0x1b7740/0x124f80/0x1c7140/0x1414a0/0xe57e0，单位 µV）。
- 保留内存：`cnss_wlan_region`；ramdump 动态区 `0x780000`。
- QRTR：`/soc/qcom,qrtr-mhi-cnss` 在 DT 里就写着 **MHI**——这条总线的协议栈是确定的。

## 3. 里程碑与验收（每步一次上机，boot-arg 控制，同 `redwood_wifi.c` 的风格）

| # | 内容 | 验收（日志/现象） | 风险 |
|---|---|---|---|
| M1 | PCIe0 控制器复位、时钟、PHY 上电与 LTSSM 训练 | `link up, speed gen2/gen3, width x1/x2`；`pcie0: link training done` | 时钟树（crm）与 PHY 组合序列最杂；参考 Linux `pcie-qcom.c` + `phy-qcom-qmp-pcie.c` |
| M2 | 配置空间访问 + 枚举 | 读到 vendor `0x17cb` 的 Qualcomm 端点与 BAR0 大小（device id 以枚举为准，DT 里没有 `qcom,device-id`；拿到的号去 ath12k 的 `pci_device_id` 表核对）；`cnss_pci0` 出现 | ECAM 窗口/ATu 配置 |
| M3 | BAR0 映射、MHI 寄存器可读、MSI 落点 | `MHI_READY`/`MHIVER` 读出，写 MSI 后收到一次中断 | MSI 控制器 0x17110040 的编程 |
| M4 | MHI 主机状态机 + 固件引导 | `mhi: device in M0`；DB 通道可写；`EE` 同步完成 | 模块固件来自 Android `/vendor/firmware`（`devices/ace5/pull-firmware.sh`） |
| M5 | QRTR/QMI：board data 下发，ath12k 起 | `ath12k: firmware ready`；`wlan0` 出现在 `net_test show` | board file 版本与固件 API 对齐 |
| M6 | mac80211/扫描/加入 | 设置里扫到 AP、加入、Safari 打开 `https://example.com` | 走既有 `wifi/lx` 的 802.11/wpa 层 |

M1–M3 是全新代码（本仓没有 PCIe）；M4 可照 UniXNU `Ath12kWifi` 的 `nw_mhi.c` 的接口面（ath12k 只用到 `mhi_alloc_controller/mhi_register_controller/mhi_prepare_for_power_up/mhi_sync_power_up/mhi_prepare_for_transfer/mhi_queue_*` 这一小把）；M5–M6 是"接上 ath12k"，理论上是 `mk-ath11k.sh` 的兄弟脚本加 `DRIVER` 列表。

## 4. 立即要做的下一步（主机侧，不需要手机）

1. 取 Linux 7.0.14（`tools/mk-ath11k.sh fetch` 已经下载同一份 tarball）里的 `drivers/pci/controller/dwc/pcie-qcom.c`、`phy/qcom/phy-qcom-qmp-pcie.c`，把 SM8650 对应的寄存器序列抄成 `giulia_pcie_regs.h`（GPL 隔离，同 `giulia_gpu_regs.h` 的做法）。
2. 对照 `qcom,pcie@1c00000` 的 reg 列表确认 DBI/ATu/ELBI/PARF 基址（`reg` 是 121 字节的属性块，需要按 4 字节解）。
3. 写 `xnu/files/bsd/dev/giulia_pcie.c` 的 M1 步骤（只读+复位+PHY，绝不写端点），boot-arg `pcie_step=N`。

## 5. 明确不做

- 不做 `cnss` 固件加载器（模块自己起固件；我们只供 MHI 通道）。
- 不碰 pcie1（dsu/其它用途）。
- 不在 WiFi 线上做 120 Hz、相机或音频。
