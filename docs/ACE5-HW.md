# Ace 5 (giulia) 硬件事实底稿

驱动移植的第一步不是写代码，是把"这台手机的硬件挂在哪"查清楚——每一项都是把别的机型的常量换成这台的。
本文上半部分是结论，下半部分是从 `dtb/giulia.dtb` 自动抽取的节点清单（可重新生成，别手改）。

```bash
python3 tools/ace5/hw-facts.py dtb/giulia.dtb --out docs/ACE5-HW.md   # 重新生成附录，保留上半部分
```

标识性属性（序列号、MAC、board-id）在抽取时被剔除，所以这份文档可以进 Git。

## 结论：五件决定工期的事

### 1. Wi-Fi 是 PCIe 设备，不是 SNOC 挂载的 ath

- 节点：`/soc/qcom,cnss-kiwi@b0000000`，compatible `qcom,cnss-kiwi`（**kiwi 是 WCN7850 的代号**），reg `0xb0000000` 长度 `0x10000`。
- 总线证据：`/soc/apps-smmu@15000000/pcie_qtb@16cd000`（PCIe 的 SMMU 桥，`qcom,stream-id-range: 5120 1024`）、`/soc/crm@1d01000`（`qcom,pcie-crm`）、`/soc/pdc@b350000`（`qcom,pcie-pdc`）、`pcie0`/`pcie1` 的 pinctrl 组、`qps615_intn_wol`（QPS615 前端模块的 WLAN 中断/WoL 引脚）、`cnss_wlan_en_active/sleep`（WLAN 使能）。
- 控制通道：`qcom,qmp: 0x63`（QMP 邮箱 99）——这就是 tycykp 卡住的 "QMP open"。
- 电源域：`qcom,vdd-wlan-*`（aon/dig/io/io12/rfa1/rfa2，每个都成对给出上电/下电的目标电压）、`cnss_wlan_region` 保留内存、`qcom,wlan-ramdump-dynamic: 0x780000`。
- **M1 需要的寄存器与引脚（2026-10-30 从本机 DTB 抽出，附录有全表）**：
  - `reg-names`：parf `0x1c00000+0x3000`、phy `0x1c06000+0x2000`、dm_core（DBI）`0x60000000+0xf1d`、elbi `0x60000f20+0xa8`、iatu（ATU）`0x60001000+0x1000`、conf（ECAM）`0x60100000+0x100000`、pcie_sm `0x1d07000+0x7000`（与 stock cmdline 的 `pcie_sm_regs=0x1D07000` 对上）。
  - `ranges`：io `0x60200000+1 MiB`、mem `0x60300000+0x3d00000`。
  - GPIO（TLMM phandle 197）：PERST `94`、CLKREQ `95`、WAKE `96`；cnss 侧 WLAN_EN `16`、BT_EN `17`、SW_CTRL `18`、WLAN_SW_CTRL `19`。
  - 时钟/复位：只有一路 `gcc_cnoc_pcie_sf_axi_clk`（gcc phandle 44, id 11）；复位 `pcie_0_core_reset`、`pcie_0_phy_reset`；PHY 供电 `gdsc-phy-vdd-supply`。
  - RID→SID：`iommu-map` 把 RID `0` → SID `0x1400`、RID `0x100` → `0x1401`；`qcom,smmu-sid-base = 0x1400`；MSI 用同一对。
  - 端点 IOMMU group：`fastmap`，DMA 池 `0x98000000+0x18000000`，geometry `0x98000000+0x18010000`，`coherent` 页表，MSI 4 KiB。
  - 保留内存：`cnss_wlan_region` = `0x2000000`（32 MiB，4 MiB 对齐）。
  - 供电时序：`qcom,pdc_init_table`（9 条 `{class: wlan_pdc, ss: rf|bb, res: sXc.v, upval|dwnval}`）与 `qcom,pmu_vreg_map`（VDD095/VDD13/VDD19 → sXc 映射）也在 DTB 里，M4 的上电序列直接用。
  - **PHY/SM 序列就在 DTB 里**：`qcom,phy-sequence`（1701 字节）、`qcom,pcie-sm-seq`（13 words）、`qcom,pcie-sm-branch-seq`、`phy-status-offset 0x214`/`bit 6`、`phy-power-down-offset 0x240`。M1 不必从 Linux 抄 PHY 序列，直接用本机的。
  - 设备号：`supported-ids = 0x1107`，QRTR 的 `qcom,dev-id = 0x1107`。
- 结论：**要写 PCIe 主机控制器（枚举 + 链路训练）比预想的重**；`cnss` 只是电源/固件/引脚的外壳，真正的数据面在 PCIe 端点。PCIe 侧参考 annibale 的 `redwood_pcie.c`（同 SM8750），MHI 侧参考 UniXNU `Ath12kWifi`（非上游 ath11k）。

### 2. 背光是 DCS 指令，不是 WLED/PMIC 外设

`qcom,mdss-dsi-bl-pmic-control-type` 的值是字符串 **`bl_ctrl_dcs`**（`0x626c5f63 0x74726c5f 0x64637300`），`bl-min-level: 1`、`bl-max-level: 4095`（另一个面板变体 10/4094）。

也就是说亮度是通过 DSI 的 DCS 写（0x51）设的，**必须能向面板发 DSI 命令**。而我们的移植目前完全不碰 DSI：面板由固件（UEFI/GOP）初始化好，我们只往 panel framebuffer 里写像素（GPU 2D blit 缩放）。

- 可选路径 A：写一个**最小的 mdss DSI 命令通道**（`mdss_dsi_ctrl0@ae94000`），只做 DCS 短包发送，不接管面板时序。风险：动 DSI 寄存器可能把已经点亮的面板弄黑，需要先验证只写 DCS 不影响时序。
- 可选路径 B：不做 DCS，用 GPU 端"调暗"（乘一个系数）——能做但费像素带宽，且不是真背光。

### 3. 音量键是 TLMM GPIO，电源键在 PMIC PON

- `/soc/gpio_keys/vol_up`：`gpios: 1413 6 1`（TLMM GPIO 6）、`linux,code: 114`（KEY_VOLUMEUP）。这是 **gpio_keys 里唯一的键**。
- 没有 `gpio_keys/power`：电源键走 **PMIC PON**（`/soc/pmic-pon-log`、`/soc/qcom,pmic_glink`）。PMIC 是 PM8550 家族（`oplus,plat_pm8550-groups`）。
- 结论：音量键=TLMM GPIO 脉冲，电源键=SPMI/PMIC 的 PON 事件，两套完全不同的机制。

### 4. 电量计在 ADSP 后面，不是 I2C 电量计

- `/soc/qcom,pmic_glink/qcom,battery_charger` → compatible **`oplus,hal_sm8350`**，下面挂着 `oplus,adsp_gauge`、`oplus,adsp_pps`、`oplus,pm8550_charger`。
- 结论：**电量百分比、充电状态都来自 ADSP（pmic-glink）**，没有 BQ27xxx 那样的独立 I2C 电量计。所以"电量计"不是一个能独立完成的小驱动——它依赖 ADSP 起来，和音频共用同一套基础设施。这也解释了现在为什么用 `inject_sb` 伪造百分比。

### 5. 音频 = ADSP + WCD939x（I2C）+ SIA91xx 功放（I2C）

- ADSP：`qcom,adsp-loader`、`qcom,adsp-notify`、`adsp-sleepmon`、保留内存 `adspslpi_region@9df00000`（0x5280000 = 82.5 MB）、`q6_adsp_dtb_region`、`adsp_mhi_region`、`lpass_qtb`（SMMU 桥）。
- 编解码器：**`qcom,wcd939x-i2c`**（走 I2C 控制）。
- 功放：**`si,sia91xx-i2c`**（SIA91xx 智能功放，走 I2C）。
- 结论：与 POCO 的音频栈（不同 codec/amp）完全不同，hackios 的 `redwood_sound.c`/`speaker.c` 只能参考框架，常量与器件都要换。

### 6. 相机 = CAMSS（CPAS）+ OnePlus 的 sensor 表

- `/soc/qcom,cam-cpas@ac13000`（CAMSS 的 CPAS 子系统）+ `camera-bus-nodes` 拓扑；传感器清单在 `/soc/oplus_sensor`（`oplus,sensor-devinfo`）。
- CSIPHY/CSID/IFE 的节点在附录里；`camera_region@9b300000` 保留内存 8 MB。

## 对我们 P3 计划的修正

原来把"背光、电量计"当成两个小驱动，实际是：
- **背光**：需要最小 DSI 命令通道（中等，且有一次"可能弄黑屏"的风险）。
- **电量计**：依赖 ADSP（大，与音频同源）。
- **真正的"小驱动"只剩音量键（TLMM GPIO）**；电源键是 PMIC PON（中等，SPMI 下发）。

所以"四个能拿在手上的驱动"这个说法要改：只有音量键是小的；电源键中等；背光中等偏上且有风险；电量计依赖 ADSP。

---

# 附录：从 `dtb/giulia.dtb` 自动抽取的节点

### Wi-Fi / connectivity

| node | properties |
|---|---|
| `/reserved-memory/cnss_wlan_region` | `shared-dma-pool` |
| `/soc/apps-smmu@15000000/pcie_qtb@16cd000` | `qcom,qsmmuv500-tbu qcom,qtb500`<br>reg: `0x16cd000 0x1000 0x16c2000 0x1000`<br>qcom,iova-width: `36`<br>qcom,num-qtb-ports: `1`<br>qcom,stream-id-range: `5120 1024`<br>reg-names: `base | debugchain-base` |
| `/soc/clocks/pcie_0_pipe_clk` | `fixed-clock`<br>clock-frequency: `1000` |
| `/soc/clocks/pcie_1_phy_aux_clk` | `fixed-clock`<br>clock-frequency: `1000` |
| `/soc/clocks/pcie_1_pipe_clk` | `fixed-clock`<br>clock-frequency: `1000` |
| `/soc/crm@1d01000` | `qcom,pcie-crm`<br>reg: `0x1d01000 0x3000 0x1d04800 0x800 0x1d05000 0x2000 0x1d04000 0x800`<br>interrupts: `0x0 0xf8 0x1`<br>clocks: `61`<br>interrupt-names: `pcie_crm`<br>reg-names: `base | crm_b | crm_c | crm_v` |
| `/soc/pdc@b350000` | `qcom,pineapple-pcie-pdc qcom,pcie-pdc`<br>reg: `0xb350000 0x20000` |
| `/soc/qcom,cnss-kiwi@b0000000` | `qcom,cnss-kiwi`<br>reg: `0xb0000000 0x10000`<br>cnss-enable-self-recovery: ``<br>mpm_wake_set_gpios: `18 19`<br>qcom,bt-en-gpio: `197 17 0`<br>qcom,bus-bw-cfg: `<277 bytes>`<br>qcom,bus-bw-cfg-count: `9`<br>qcom,icc-path-count: `2`<br>qcom,pdc_init_table: `<525 bytes>`<br>qcom,pmu_vreg_map: `VDD095_MX_PMU | s2c | VDD095_PMU | s4i | VDD_PMU_AON_I | s3c | VDD095_PMU_BT | s3c | VDD09_PMU_RFA_I | s3c | VDD13_PMU_PCIE_I | s1c | VDD13_PMU_RFA_I | s1c | VDD19_PMU_PCIE_I | s6c | VDD19_PMU_RFA_I | s6c`<br>qcom,qmp: `63`<br>qcom,same-dt-multi-dev: ``<br>qcom,sw-ctrl-gpio: `197 18 0`<br>qcom,vdd-wlan-aon-config: `0xee480 0xee480 0x0 0x0 0x1`<br>qcom,vdd-wlan-config: `0xe57e0 0xe57e0 0x0 0x0 0x1`<br>qcom,vdd-wlan-dig-config: `0xdfa20 0xdfa20 0x0 0x0 0x1`<br>qcom,vdd-wlan-io-config: `0x1b7740 0x1b7740 0x0 0x0 0x1`<br>qcom,vdd-wlan-io12-config: `0x124f80 0x124f80 0x0 0x0 0x1`<br>qcom,vdd-wlan-rfa1-config: `0x1c7140 0x1c7140 0x0 0x0 0x1`<br>qcom,vdd-wlan-rfa2-config: `0x1414a0 0x1414a0 0x0 0x0 0x1`<br>qcom,vreg_pdc_map: `s3c | rf | l15B | rf | l3c | rf | s1c | rf | s6c | rf | s2c | bb | s4i | bb`<br>qcom,wlan: ``<br>qcom,wlan-cbc-enabled: ``<br>qcom,wlan-ramdump-dynamic: `0x780000`<br>qcom,wlan-rc-num: `0`<br>qcom,wlan-sw-ctrl-gpio: `197 19 0`<br>reg-names: `smmu_iova_ipa`<br>supported-ids: `4359`<br>use-nv-mac: ``<br>use-pm-domain: ``<br>wlan-en-gpio: `197 16 0` |
| `/soc/qcom,ipa@3e00000/ipa_smmu_wlan` | `qcom,ipa-smmu-wlan-cb`<br>qcom,iommu-dma: `atomic` |
| `/soc/qcom,pcie0_msi@0x17110040` | `qcom,pci-msi`<br>reg: `0x17110040 0x0`<br>interrupts: `<447 bytes>` |
| `/soc/qcom,pcie1_msi@0x17110040` | `qcom,pci-msi`<br>reg: `0x17110040 0x0`<br>interrupts: `<671 bytes>` |
| `/soc/qcom,pcie@1c00000` | `qcom,pci-msm`<br>reg: `0x1c00000 0x3000 0x1c06000 0x2000 0x60000000 0xf1d 0x60000f20 0xa8 0x60001000 0x1000 0x60100000 0x100000 0x1d07000 0x7000`<br>interrupts: `0x0 0x8c 0x4 0x0 0x95 0x4 0x0 0x96 0x4 0x0 0x97 0x4 0x0 0x98 0x4`<br>clock-frequency: `0`<br>clock-names: `gcc_cnoc_pcie_sf_axi_clk`<br>clock-suppressible: `1`<br>clocks: `44 11`<br>gdsc-phy-vdd-supply: `740`<br>interrupt-names: `int_global_int | int_a | int_b | int_c | int_d`<br>iommu-map: `0 82 5120 1 256 82 5121 1`<br>msi-map: `0 91 5120 1 256 91 5121 1`<br>perst-gpio: `197 94 0`<br>qcom,aux-clk-freq: `20`<br>qcom,boot-option: `1`<br>qcom,config-recovery: ``<br>qcom,dbi-debug-reg: `260 272 128 500 1840 1844 1848 1852`<br>qcom,drv-l1ss-timeout-us: `5000`<br>qcom,drv-supported: ``<br>qcom,ep-latency: `10`<br>qcom,l1-2-th-scale: `2`<br>qcom,l1-2-th-value: `150`<br>qcom,num-parf-testbus-sel: `185`<br>qcom,parf-debug-reg: `0x1b0 0x24 0x28 0x224 0x500 0x4d0 0x4d4 0x3c0 0x630 0x230 0x0`<br>qcom,pcie-clkreq-gpio: `95`<br>qcom,pcie-phy-ver: `107`<br>qcom,pcie-sm-branch-offset: `4096`<br>qcom,pcie-sm-branch-seq: `4 28 36 44 0 0 0`<br>qcom,pcie-sm-debug: `0x1040 0x1048 0x1050 0x1058 0x1060 0x1068 0x1070 0x1078 0x1080 0x1088 0x1090 0x1094 0x1098 0x109c`<br>qcom,pcie-sm-seq: `0x1c018081 0x70074002 0x50028000 0x28007003 0x80804002 0x70021c01 0x18001802 0x70005000 0x10004000 0x80814002 0x18001c01 0x1c018080 0x100`<br>qcom,pcie-sm-start-offset: `4240`<br>qcom,phy-debug-reg: `<269 bytes>`<br>qcom,phy-power-down-offset: `576`<br>qcom,phy-sequence: `<1701 bytes>`<br>qcom,phy-status-bit: `6`<br>qcom,phy-status-offset: `532`<br>qcom,slv-addr-space-size: `0x4000000`<br>qcom,smmu-sid-base: `5120`<br>ranges: `0x1000000 0x0 0x60200000 0x60200000 0x0 0x100000 0x2000000 0x0 0x60300000 0x60300000 0x0 0x3d00000`<br>reg-names: `parf | phy | dm_core | elbi | iatu | conf | pcie_sm`<br>reset-names: `pcie_0_core_reset | pcie_0_phy_reset`<br>resets: `44 3 44 6`<br>wake-gpio: `197 96 0` |
| `/soc/qcom,pcie@1c00000/pcie0_rp` | reg: `0x0 0x0 0x0 0x0 0x0` |
| `/soc/qcom,pcie@1c00000/pcie0_rp/cnss_pci0` | reg: `0x0 0x0 0x0 0x0 0x0`<br>memory-region: `1592`<br>qcom,iommu-group: `1591` |
| `/soc/qcom,pcie@1c00000/pcie0_rp/cnss_pci0/cnss_pci_iommu_group0` | qcom,iommu-dma: `fastmap`<br>qcom,iommu-dma-addr-pool: `0x98000000 0x18000000`<br>qcom,iommu-faults: `stall-disable | HUPCF | non-fatal`<br>qcom,iommu-geometry: `0x98000000 0x18010000`<br>qcom,iommu-msi-size: `4096`<br>qcom,iommu-pagetable: `coherent` |
| `/soc/qcom,pcie@1c08000` | `qcom,pci-msm`<br>reg: `0x1c08000 0x3000 0x1c0e000 0x2000 0x40000000 0xf1d 0x40000f20 0xa8 0x40001000 0x1000 0x40100000 0x100000`<br>interrupts: `0x0 0x132 0x4 0x0 0x1b2 0x4 0x0 0x1b3 0x4 0x0 0x1b6 0x4 0x0 0x1b7 0x4`<br>clock-frequency: `0x0 0x0 0x124f800 0x0 0x0 0x0 0x0 0x0 0x5f5e100 0x0 0x0 0x0 0x0 0x0 0x0`<br>clock-names: `<311 bytes>`<br>clock-suppressible: `0x0 0x0 0x0 0x0 0x0 0x0 0x1 0x0 0x0 0x0 0x0 0x1 0x0 0x0 0x0`<br>clocks: `<139 bytes>`<br>gdsc-core-vdd-supply: `744`<br>gdsc-phy-vdd-supply: `745`<br>interrupt-names: `int_global_int | int_a | int_b | int_c | int_d`<br>iommu-map: `0 82 5248 1 256 82 5249 1`<br>msi-map: `0 91 5248 1 256 91 5249 1`<br>qcom,aux-clk-freq: `17`<br>qcom,boot-option: `1`<br>qcom,dbi-debug-reg: `260 272 128 500 1840 1844 1848 1852`<br>qcom,drv-l1ss-timeout-us: `5000`<br>qcom,drv-name: `lpass`<br>qcom,ep-latency: `10`<br>qcom,l1-2-th-scale: `2`<br>qcom,l1-2-th-value: `150`<br>qcom,num-parf-testbus-sel: `185`<br>qcom,parf-debug-reg: `0x1b0 0x24 0x28 0x224 0x500 0x4d0 0x4d4 0x3c0 0x630 0x230 0x0`<br>qcom,pcie-phy-ver: `109`<br>qcom,phy-debug-reg: `<292 bytes>`<br>qcom,phy-power-down-offset: `4672`<br>qcom,phy-sequence: `<2524 bytes>`<br>qcom,phy-status-bit: `7`<br>qcom,phy-status-offset: `4628`<br>qcom,slv-addr-space-size: `0x20000000`<br>qcom,smmu-sid-base: `5248`<br>ranges: `0x1000000 0x0 0x40200000 0x40200000 0x0 0x100000 0x2000000 0x0 0x40300000 0x40300000 0x0 0x1fd00000`<br>reg-names: `parf | phy | dm_core | elbi | iatu | conf`<br>reset-names: `pcie_1_core_reset | pcie_1_phy_reset | pcie_1_link_down_reset | pcie_1_phy_nocsr_com_phy_reset`<br>resets: `44 8 44 11 44 9 44 10`<br>wake-gpio: `197 99 0` |
| `/soc/qcom,pcie@1c08000/pcie1_rp` | reg: `0x0 0x0 0x0 0x0 0x0` |
| `/soc/qcom,qrtr-mhi-cnss` | `qcom,qrtr-mhi`<br>qcom,dev-id: `4359` |
| `/soc/qcom,spmi@c42d000/qcom,pmk8550@0/vadc@9000/pm8550_wlan_therm` | reg: `0x146` |
| `/soc/qmi-tmd-devices/modem/wlan` | — |
| `/soc/thermal-zones/wlan-therm` | — |
| `/soc/thermal-zones/wlan-therm/trips` | — |
| `/soc/thermal-zones/wlan-therm/trips/active-config0` | — |
| `/soc/thermal-zones/wlan-therm/trips/active-config1` | — |

### Display / backlight

| node | properties |
|---|---|
| `/soc/dsi_panel_AA577_P_3_A0020_dsc_cmd` | — |
| `/soc/dsi_panel_pwr_supply` | — |
| `/soc/dsi_panel_pwr_supply/qcom,panel-supply-entry@0` | reg: `0x0` |
| `/soc/dsi_panel_pwr_supply/qcom,panel-supply-entry@1` | reg: `0x1` |
| `/soc/dsi_panel_pwr_supply/qcom,panel-supply-entry@2` | reg: `0x2` |
| `/soc/dsi_panel_pwr_supply_avdd` | — |
| `/soc/dsi_panel_pwr_supply_avdd/qcom,panel-supply-entry@0` | reg: `0x0` |
| `/soc/dsi_panel_pwr_supply_avdd/qcom,panel-supply-entry@1` | reg: `0x1` |
| `/soc/dsi_panel_pwr_supply_sim` | — |
| `/soc/dsi_panel_pwr_supply_sim/qcom,panel-supply-entry@0` | reg: `0x0` |
| `/soc/dsi_pll_codes` | reg: `<511 bytes>` |
| `/soc/oplus,dsi-display-dev` | — |
| `/soc/oplus_dsi_panel_custom_pwr_supply` | — |
| `/soc/oplus_dsi_panel_custom_pwr_supply/qcom,panel-supply-entry@0` | reg: `0x0` |
| `/soc/oplus_dsi_panel_custom_pwr_supply/qcom,panel-supply-entry@1` | reg: `0x1` |
| `/soc/qcom,dsi-display-primary` | `qcom,dsi-display` |
| `/soc/qcom,dsi-display-secondary` | `qcom,dsi-display` |
| `/soc/qcom,mdss_dsi_ctrl0@ae94000` | `qcom,dsi-ctrl-hw-v2.8`<br>reg: `0xae94000 0x1000 0xaf0f000 0x4 0xae36000 0x300`<br>interrupts: `0x4 0x0` |
| `/soc/qcom,mdss_dsi_ctrl0@ae94000/qcom,ctrl-supply-entries` | — |
| `/soc/qcom,mdss_dsi_ctrl0@ae94000/qcom,ctrl-supply-entries/qcom,ctrl-supply-entry@0` | reg: `0x0` |
| `/soc/qcom,mdss_dsi_ctrl1@ae96000` | `qcom,dsi-ctrl-hw-v2.8`<br>reg: `0xae96000 0x1000 0xaf0f000 0x4 0xae37000 0x300`<br>interrupts: `0x5 0x0` |
| `/soc/qcom,mdss_dsi_ctrl1@ae96000/qcom,ctrl-supply-entries` | — |
| `/soc/qcom,mdss_dsi_ctrl1@ae96000/qcom,ctrl-supply-entries/qcom,ctrl-supply-entry@0` | reg: `0x0` |
| `/soc/qcom,mdss_dsi_phy0@ae95500` | `qcom,dsi-phy-v5.2`<br>reg: `0xae95000 0xa00 0xae95500 0x400 0xae94200 0xa0` |
| `/soc/qcom,mdss_dsi_phy0@ae95500/qcom,phy-supply-entries` | — |
| `/soc/qcom,mdss_dsi_phy0@ae95500/qcom,phy-supply-entries/qcom,phy-supply-entry@0` | reg: `0x0` |
| `/soc/qcom,mdss_dsi_phy1@ae97500` | `qcom,dsi-phy-v5.2`<br>reg: `0xae97000 0xa00 0xae97500 0x400 0xae96200 0xa0` |
| `/soc/qcom,mdss_dsi_phy1@ae97500/qcom,phy-supply-entries` | — |
| `/soc/qcom,mdss_dsi_phy1@ae97500/qcom,phy-supply-entries/qcom,phy-supply-entry@0` | reg: `0x0` |
| `/soc/qcom,mdss_mdp@ae00000` | `qcom,sde-kms`<br>reg: `0xae00000 0x84000 0xaeb0000 0x2008 0xaf80000 0x7000 0x400000 0x2000`<br>interrupts: `0x0 0x53 0x4` |
| `/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_panel_AA577_P_3_A0020_dsc_cmd` | qcom,dsi-phy-num: `0`<br>qcom,mdss-dsi-bl-inverted-dbv: ``<br>qcom,mdss-dsi-bl-max-level: `4094`<br>qcom,mdss-dsi-bl-min-level: `1`<br>qcom,mdss-dsi-bl-pmic-control-type: `bl_ctrl_dcs`<br>qcom,mdss-dsi-panel-name: `AA577 P 3 A0020 dsc cmd mode panel`<br>qcom,mdss-dsi-panel-type: `dsi_cmd_mode`<br>qcom,panel-supply-entries: `2021` |
| `/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_panel_AA577_P_3_A0020_dsc_cmd/qcom,mdss-dsi-display-timings` | — |
| `/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_panel_AA577_P_3_A0020_dsc_cmd/qcom,mdss-dsi-display-timings/timing@oplus_fhd_120` | qcom,mdss-dsi-panel-framerate: `120`<br>qcom,mdss-dsi-panel-height: `2780`<br>qcom,mdss-dsi-panel-width: `1264` |
| `/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_panel_AA577_P_3_A0020_dsc_cmd/qcom,mdss-dsi-display-timings/timing@sdc_fhd_120` | qcom,mdss-dsi-panel-framerate: `120`<br>qcom,mdss-dsi-panel-height: `2780`<br>qcom,mdss-dsi-panel-width: `1264` |
| `/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_panel_AA577_P_3_A0020_dsc_cmd/qcom,mdss-dsi-display-timings/timing@sdc_fhd_60` | qcom,mdss-dsi-panel-framerate: `60`<br>qcom,mdss-dsi-panel-height: `2780`<br>qcom,mdss-dsi-panel-width: `1264` |
| `/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_panel_AA577_P_3_A0020_dsc_cmd/qcom,mdss-dsi-display-timings/timing@sdc_fhd_90` | qcom,mdss-dsi-panel-framerate: `90`<br>qcom,mdss-dsi-panel-height: `2780`<br>qcom,mdss-dsi-panel-width: `1264` |
| `/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_vtdr6130_fhd_plus_cmd` | qcom,dsi-phy-num: `0`<br>qcom,mdss-dsi-bl-inverted-dbv: ``<br>qcom,mdss-dsi-bl-max-level: `4095`<br>qcom,mdss-dsi-bl-min-level: `10`<br>qcom,mdss-dsi-bl-pmic-control-type: `bl_ctrl_dcs`<br>qcom,mdss-dsi-panel-name: `vtdr6130 amoled cmd mode dsi visionox panel with DSC`<br>qcom,mdss-dsi-panel-type: `dsi_cmd_mode`<br>qcom,panel-supply-entries: `2019` |
| `/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_vtdr6130_fhd_plus_cmd/qcom,mdss-dsi-display-timings` | — |
| `/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_vtdr6130_fhd_plus_cmd/qcom,mdss-dsi-display-timings/timing@0` | qcom,mdss-dsi-panel-framerate: `144`<br>qcom,mdss-dsi-panel-height: `2400`<br>qcom,mdss-dsi-panel-width: `1080` |
| `/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_vtdr6130_fhd_plus_cmd/qcom,mdss-dsi-display-timings/timing@1` | qcom,mdss-dsi-panel-framerate: `120`<br>qcom,mdss-dsi-panel-height: `2400`<br>qcom,mdss-dsi-panel-width: `1080` |
| _21 more_ | |

### PMIC / GPIO / keys

| node | properties |
|---|---|
| `/soc/gpio_keys` | `gpio-keys` |
| `/soc/gpio_keys/vol_up` | gpios: `1413 6 1`<br>linux,code: `114` |
| `/soc/mem_dump/pmic` | — |
| `/soc/msm_cdc_pinctrl@32` | `qcom,msm-cdc-pinctrl` |
| `/soc/oplus-gpio` | `oplus,oplus-gpio` |
| `/soc/oplus-ocp-state-log` | `oplus,pmic-ocp-log` |
| `/soc/oplus-pmic-monitor-log` | `oplus,pmic-monitor-log` |
| `/soc/oplus_chg_core/oplus-charger/oplus,plat_pm8550-groups` | — |
| `/soc/pinctrl@f000000` | `qcom,pineapple-pinctrl`<br>reg: `0xf000000 0x1000000`<br>interrupts: `0x0 0xd0 0x4` |
| `/soc/pmic-pon-log` | `qcom,pmic-pon-log` |
| `/soc/qcom,pmic_glink` | `qcom,pmic-glink` |
| `/soc/qcom,pmic_glink/qcom,altmode` | `qcom,altmode-glink` |
| `/soc/qcom,pmic_glink/qcom,battery_charger` | `oplus,hal_sm8350` |
| `/soc/qcom,pmic_glink/qcom,battery_charger/oplus,adsp_gauge` | — |
| `/soc/qcom,pmic_glink/qcom,battery_charger/oplus,adsp_pps` | — |
| `/soc/qcom,pmic_glink/qcom,battery_charger/oplus,pm8550_charger` | — |
| `/soc/qcom,pmic_glink/qcom,ucsi` | `qcom,ucsi-glink` |
| `/soc/qcom,pmic_glink/qcom,ucsi/connector` | — |
| `/soc/qcom,pmic_glink/qcom,ucsi/connector/port` | — |
| `/soc/qcom,pmic_glink/qcom,ucsi/connector/port/endpoint` | — |
| `/soc/qcom,pmic_glink/qcom,wireless_pen_hboost` | `qcom,wireless-pen-hboost` |
| `/soc/qcom,pmic_glink_log` | `qcom,pmic-glink` |
| `/soc/qcom,pmic_glink_log/qcom,battery_debug` | `qcom,battery-debug` |
| `/soc/qcom,pmic_glink_log/qcom,charger_ulog_glink` | `qcom,charger-ulog-glink` |
| `/soc/qcom,pmic_glink_log/qcom,glink-adc` | `qcom,glink-adc` |
| `/soc/qcom,pmic_glink_log/qcom,glink-adc/smb1502_1_die_temp` | reg: `0x1076803` |
| `/soc/qcom,pmic_glink_log/qcom,glink-adc/smb1502_1_ichg` | reg: `0x1076802` |
| `/soc/qcom,pmic_glink_log/qcom,glink-adc/smb1502_1_iin` | reg: `0x1076801` |
| `/soc/qcom,pmic_glink_log/qcom,glink-adc/smb1502_2_die_temp` | reg: `0x1076903` |
| `/soc/qcom,pmic_glink_log/qcom,glink-adc/smb1502_2_ichg` | reg: `0x1076902` |
| `/soc/qcom,pmic_glink_log/qcom,glink-adc/smb1502_2_iin` | reg: `0x1076901` |
| `/soc/qcom,pmic_glink_log/qcom,pmic_glink_debug` | `qcom,pmic-glink-debug` |
| `/soc/qcom,pmic_glink_log/qcom,pmic_glink_debug/i2c@107` | reg: `0x107` |
| `/soc/qcom,pmic_glink_log/qcom,pmic_glink_debug/i2c@107/qcom,idt9418@3b` | `qcom,i2c-pmic`<br>reg: `0x3b` |
| `/soc/qcom,pmic_glink_log/qcom,pmic_glink_debug/i2c@107/qcom,smb1502@68` | `qcom,i2c-pmic`<br>reg: `0x68` |
| `/soc/qcom,pmic_glink_log/qcom,pmic_glink_debug/i2c@107/qcom,smb1502@69` | `qcom,i2c-pmic`<br>reg: `0x69` |
| `/soc/qcom,pmic_glink_log/qcom,pmic_glink_debug/spmi@0` | reg: `0x0` |
| `/soc/qcom,pmic_glink_log/qcom,pmic_glink_debug/spmi@0/qcom,pm8550b-debug@7` | `qcom,spmi-pmic`<br>reg: `0x7 0x0` |
| `/soc/qcom,spmi-debug@10b14000` | `qcom,spmi-pmic-arb-debug`<br>reg: `0x10b14000 0x60 0x221c8784 0x4` |
| `/soc/qcom,spmi-debug@10b14000/qcom,pm8010-debug@c` | `qcom,spmi-pmic`<br>reg: `0xc 0x0` |
| _556 more_ | |

### Battery / charger

| node | properties |
|---|---|
| `/soc/oplus,battery_log` | `oplus,battery_log` |
| `/soc/oplus,mms_gauge` | `oplus,mms_gauge` |
| `/soc/oplus,mms_gauge/oplus,virtual_gauge` | `oplus,virtual_gauge` |
| `/soc/oplus,mms_gauge/oplus,virtual_gauge/silicon_x24` | — |
| `/soc/oplus_chg_core/oplus,bq27541_groups` | — |
| `/soc/oplus_chg_core/oplus-charger` | — |
| `/soc/oplus_chg_core/oplus-charger/oplus,plat_pm8550-groups` | — |
| `/soc/oplus_chg_core/oplus-gauge` | — |
| `/soc/oplus_chg_core/oplus-gauge/oplus,gauge_groups` | — |
| `/soc/qcom,pmic_glink/qcom,battery_charger` | `oplus,hal_sm8350`<br>qcom,thermal-mitigation: `0x2dc6c0 0x16e360 0xf4240 0x7a120` |
| `/soc/qcom,pmic_glink/qcom,battery_charger/oplus,adsp_gauge` | — |
| `/soc/qcom,pmic_glink/qcom,battery_charger/oplus,adsp_pps` | — |
| `/soc/qcom,pmic_glink/qcom,battery_charger/oplus,pm8550_charger` | — |
| `/soc/qcom,pmic_glink_log/qcom,battery_debug` | `qcom,battery-debug` |
| `/soc/qcom,pmic_glink_log/qcom,charger_ulog_glink` | `qcom,charger-ulog-glink` |
| `/soc/qcom,pmic_glink_log/qcom,glink-adc/smb1502_1_die_temp` | reg: `0x1076803` |
| `/soc/qcom,pmic_glink_log/qcom,glink-adc/smb1502_1_ichg` | reg: `0x1076802` |
| `/soc/qcom,pmic_glink_log/qcom,glink-adc/smb1502_1_iin` | reg: `0x1076801` |
| `/soc/qcom,pmic_glink_log/qcom,glink-adc/smb1502_2_die_temp` | reg: `0x1076903` |
| `/soc/qcom,pmic_glink_log/qcom,glink-adc/smb1502_2_ichg` | reg: `0x1076902` |
| `/soc/qcom,pmic_glink_log/qcom,glink-adc/smb1502_2_iin` | reg: `0x1076901` |
| `/soc/qcom,pmic_glink_log/qcom,pmic_glink_debug/i2c@107/qcom,smb1502@68` | `qcom,i2c-pmic`<br>reg: `0x68` |
| `/soc/qcom,pmic_glink_log/qcom,pmic_glink_debug/i2c@107/qcom,smb1502@69` | `qcom,i2c-pmic`<br>reg: `0x69` |
| `/soc/qcom,qupv3_2_geni_se@8c0000/i2c@880000/bq27541-battery@55` | `oplus,bq27541-battery`<br>reg: `0x55` |
| `/soc/qcom,qupv3_i2c_geni_se@9c0000/i2c@990000/sc8547-charger-slave@6A` | `oplus,sc8547d`<br>reg: `0x6a` |
| `/soc/qcom,qupv3_i2c_geni_se@9c0000/i2c@990000/sc8547-charger-slave@6A/oplus,sc8547_slave_cp` | — |
| `/soc/qcom,spmi@c42d000/qcom,pmk8550@0/sdam@7000/smb1510_present@5d` | reg: `0x5d 0x1` |

### Buses

| node | properties |
|---|---|
| `/soc/fingerprint_dev` | `oplus,fp_spi` |
| `/soc/qcom,cci0@ac15000/qcom,i2c_custom_mode` | — |
| `/soc/qcom,cci0@ac15000/qcom,i2c_fast_mode` | — |
| `/soc/qcom,cci0@ac15000/qcom,i2c_fast_plus_mode` | — |
| `/soc/qcom,cci0@ac15000/qcom,i2c_standard_mode` | — |
| `/soc/qcom,cci1@ac16000/qcom,i2c_custom_mode` | — |
| `/soc/qcom,cci1@ac16000/qcom,i2c_fast_mode` | — |
| `/soc/qcom,cci1@ac16000/qcom,i2c_fast_plus_mode` | — |
| `/soc/qcom,cci1@ac16000/qcom,i2c_standard_mode` | — |
| `/soc/qcom,cci2@ac17000/qcom,i2c_custom_mode` | — |
| `/soc/qcom,cci2@ac17000/qcom,i2c_fast_mode` | — |
| `/soc/qcom,cci2@ac17000/qcom,i2c_fast_plus_mode` | — |
| `/soc/qcom,cci2@ac17000/qcom,i2c_standard_mode` | — |
| `/soc/qcom,pmic_glink_log/qcom,pmic_glink_debug/i2c@107` | reg: `0x107` |
| `/soc/qcom,pmic_glink_log/qcom,pmic_glink_debug/i2c@107/qcom,idt9418@3b` | `qcom,i2c-pmic`<br>reg: `0x3b` |
| `/soc/qcom,pmic_glink_log/qcom,pmic_glink_debug/i2c@107/qcom,smb1502@68` | `qcom,i2c-pmic`<br>reg: `0x68` |
| `/soc/qcom,pmic_glink_log/qcom,pmic_glink_debug/i2c@107/qcom,smb1502@69` | `qcom,i2c-pmic`<br>reg: `0x69` |
| `/soc/qcom,qupv3_1_geni_se@ac0000` | `qcom,geni-se-qup`<br>reg: `0xac0000 0x2000` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a80000` | `qcom,i2c-geni`<br>reg: `0xa80000 0x4000`<br>interrupts: `0x0 0x161 0x4`<br>qcom,clk-freq-out: `0x61a80` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a80000/nq@28` | `qcom,sn-nci`<br>reg: `0x28`<br>interrupts: `0x4b 0x0` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a84000` | `qcom,i2c-geni`<br>reg: `0xa84000 0x4000`<br>interrupts: `0x0 0x162 0x4` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a88000` | `qcom,i2c-geni`<br>reg: `0xa88000 0x4000`<br>interrupts: `0x0 0x163 0x4` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a8c000` | `qcom,i2c-geni`<br>reg: `0xa8c000 0x4000`<br>interrupts: `0x0 0x164 0x4` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a8c000/wcd939x_i2c@e` | `qcom,wcd939x-i2c`<br>reg: `0xe`<br>interrupts: `0x0 0xb6 0x1 0x3` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a90000` | `qcom,i2c-geni`<br>reg: `0xa90000 0x4000`<br>interrupts: `0x0 0x165 0x4` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a94000` | `qcom,i2c-geni`<br>reg: `0xa94000 0x4000`<br>interrupts: `0x0 0x166 0x4` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a98000` | `qcom,i2c-geni`<br>reg: `0xa98000 0x4000`<br>interrupts: `0x0 0x16b 0x4` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a98000/nq@64` | `rtc6226`<br>reg: `0x64` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a9c000` | `qcom,i2c-geni`<br>reg: `0xa9c000 0x4000`<br>interrupts: `0x0 0x243 0x4` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i3c-master@a80000` | `qcom,geni-i3c`<br>reg: `0xa80000 0x4000 0xec90000 0x10000` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i3c-master@a90000` | `qcom,geni-i3c`<br>reg: `0xa90000 0x4000 0xecb0000 0x10000` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/spi@a80000` | `qcom,spi-geni`<br>reg: `0xa80000 0x4000`<br>interrupts: `0x0 0x161 0x4` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/spi@a84000` | `qcom,spi-geni`<br>reg: `0xa84000 0x4000`<br>interrupts: `0x0 0x162 0x4` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/spi@a88000` | `qcom,spi-geni`<br>reg: `0xa88000 0x4000`<br>interrupts: `0x0 0x163 0x4` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/spi@a8c000` | `qcom,spi-geni`<br>reg: `0xa8c000 0x4000`<br>interrupts: `0x0 0x164 0x4` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/spi@a90000` | `qcom,spi-geni`<br>reg: `0xa90000 0x4000`<br>interrupts: `0x0 0x165 0x4` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/spi@a90000/goodix-berlin@0` | `goodix,gt9916S`<br>reg: `0x0`<br>interrupts: `0xa2 0x2008` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/spi@a90000/synaptics_tcm@0` | `synaptics-s3910`<br>reg: `0x0`<br>interrupts: `0xa2 0x2008` |
| `/soc/qcom,qupv3_1_geni_se@ac0000/spi@a90000/synaptics_tcm@0/S3910` | — |
| `/soc/qcom,qupv3_1_geni_se@ac0000/spi@a90000/synaptics_tcm_hbp@0` | `synaptics,tcm-spi-hbp`<br>reg: `0x0`<br>interrupts: `0xa2 0x2008`<br>qcom,rt: `` |
| _73 more_ | |

### Audio

| node | properties |
|---|---|
| `/reserved-memory/adsp_heap_region` | `shared-dma-pool` |
| `/reserved-memory/adsp_mhi_region@81f00000` | reg: `0x0 0x81f00000 0x0 0x20000` |
| `/reserved-memory/adspslpi_region@9df00000` | reg: `0x0 0x9df00000 0x0 0x5280000` |
| `/reserved-memory/q6_adsp_dtb_region@9de80000` | reg: `0x0 0x9de80000 0x0 0x80000` |
| `/soc/adsp-sleepmon` | `qcom,adsp-sleepmon` |
| `/soc/apps-smmu@15000000/lpass_qtb@7b3000` | `qcom,qsmmuv500-tbu qcom,qtb500`<br>reg: `0x7b3000 0x1000 0x7be000 0x1000` |
| `/soc/audio_etm0` | `qcom,coresight-remote-etm` |
| `/soc/audio_etm0/out-ports` | — |
| `/soc/audio_etm0/out-ports/port` | — |
| `/soc/audio_etm0/out-ports/port/endpoint` | — |
| `/soc/ipcc-self-ping-adsp` | `qcom,ipcc-self-ping` |
| `/soc/lpass_stm` | `qcom,coresight-dummy` |
| `/soc/lpass_stm/out-ports` | — |
| `/soc/lpass_stm/out-ports/port` | — |
| `/soc/lpass_stm/out-ports/port/endpoint` | — |
| `/soc/mem_dump/etf_lpass` | — |
| `/soc/mem_dump/etflpass_reg` | — |
| `/soc/oplus,adsp-voocphy` | `oplus,adsp-voocphy` |
| `/soc/oplus,audio-drv` | `oplus,asoc-audio` |
| `/soc/qcom,audio-pkt-core-platform` | `qcom,audio-pkt-core-platform` |
| `/soc/qcom,msm-adsp-loader` | `qcom,adsp-loader`<br>qcom,adsp-state: `0` |
| `/soc/qcom,msm-adsp-notify` | `qcom,adsp-notify` |
| `/soc/qcom,msm-adsprpc-mem` | `qcom,msm-adsprpc-mem-region` |
| `/soc/qcom,msm-ext-disp/qcom,msm-ext-disp-audio-codec-rx` | `qcom,msm-ext-disp-audio-codec-rx` |
| `/soc/qcom,msm-stub-codec` | `qcom,msm-stub-codec` |
| `/soc/qcom,pmic_glink/qcom,battery_charger/oplus,adsp_gauge` | — |
| `/soc/qcom,pmic_glink/qcom,battery_charger/oplus,adsp_pps` | — |
| `/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a8c000/wcd939x_i2c@e` | `qcom,wcd939x-i2c`<br>reg: `0xe`<br>interrupts: `0x0 0xb6 0x1 0x3` |
| `/soc/qcom,qupv3_i2c_geni_se@9c0000/i2c@99c000/sipa91xx_i2c@6c` | `si,sia91xx-i2c`<br>reg: `0x6c` |
| `/soc/qcom,qupv3_i2c_geni_se@9c0000/i2c@99c000/sipa91xx_i2c@6d` | `si,sia91xx-i2c`<br>reg: `0x6d` |
| `/soc/qcom,smp2p-adsp` | `qcom,smp2p`<br>interrupts: `0x3 0x2 0x1` |
| `/soc/qcom,smp2p-adsp/master-kernel` | — |
| `/soc/qcom,smp2p-adsp/qcom,sleepstate-in` | — |
| `/soc/qcom,smp2p-adsp/qcom,smp2p-rdbg2-in` | — |
| `/soc/qcom,smp2p-adsp/qcom,smp2p-rdbg2-out` | — |
| `/soc/qcom,smp2p-adsp/slave-kernel` | — |
| `/soc/qcom,smp2p-adsp/sleepstate-out` | — |
| `/soc/qcom,spmi@c42d000/qcom,pm8550b@7/qcom,hv-haptics@f000/qcom,hap-swr-slave-reg` | — |
| `/soc/remoteproc-adsp@03000000` | `qcom,pineapple-adsp-pas`<br>reg: `0x3000000 0x10000` |
| `/soc/remoteproc-adsp@03000000/glink-edge` | interrupts: `0x3 0x0 0x1` |
| _78 more_ | |

### Camera

| node | properties |
|---|---|
| `/reserved-memory/camera_region@9b300000` | reg: `0x0 0x9b300000 0x0 0x800000` |
| `/soc/oplus_sensor` | `oplus,sensor-devinfo` |
| `/soc/oplus_sensor/mag_fusion` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/bps0-all-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/bps0-all-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/cre0-all-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/cre0-all-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/custom0-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/custom0-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/custom1-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/custom1-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/icp0-all-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife0-pdaf-linear-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife0-rdi-pixel-raw-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife0-stats-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife0-ubwc-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife1-pdaf-linear-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife1-rdi-pixel-raw-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife1-stats-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife1-ubwc-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife2-pdaf-linear-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife2-rdi-pixel-raw-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife2-stats-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife2-ubwc-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife3-rdi-stats-pixel-raw-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife4-rdi-stats-pixel-raw-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ipe0-all-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ipe0-in-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ipe0-ref-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/jpeg-dma0-all-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/jpeg-dma1-all-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/jpeg-enc0-all-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/jpeg-enc1-all-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/jpeg0-dma0-all-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/jpeg0-enc0-all-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/jpeg1-dma1-all-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/jpeg1-enc1-all-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/rt-cdm0-all-rd` | — |
| _93 more_ | |

### Other SoC services

| node | properties |
|---|---|
| `/reserved-memory/aop_cmd_db_region@81c60000` | `qcom,cmd-db`<br>reg: `0x0 0x81c60000 0x0 0x20000` |
| `/reserved-memory/ipa_fw_region@9b080000` | reg: `0x0 0x9b080000 0x0 0x10000` |
| `/reserved-memory/ipa_gsi_region@9b090000` | reg: `0x0 0x9b090000 0x0 0xa000` |
| `/reserved-memory/mpss_dsm_region@86b00000` | reg: `0x0 0x86b00000 0x0 0x4900000` |
| `/reserved-memory/mpss_dsm_region_2@8b400000` | reg: `0x0 0x8b400000 0x0 0x800000` |
| `/reserved-memory/mpss_region@8bc00000` | reg: `0x0 0x8bc00000 0x0 0xf400000` |
| `/reserved-memory/q6_mpss_dtb_region@9b000000` | reg: `0x0 0x9b000000 0x0 0x80000` |
| `/reserved-memory/smem_region@81d00000` | `qcom,smem`<br>reg: `0x0 0x81d00000 0x0 0x200000` |
| `/soc/apps-smmu@15000000/lpass_qtb@7b3000` | `qcom,qsmmuv500-tbu qcom,qtb500`<br>reg: `0x7b3000 0x1000 0x7be000 0x1000` |
| `/soc/ipcc-self-ping-adsp` | `qcom,ipcc-self-ping` |
| `/soc/ipcc-self-ping-apss` | `qcom,ipcc-self-ping` |
| `/soc/ipcc-self-ping-cdsp` | `qcom,ipcc-self-ping` |
| `/soc/ipcc-self-ping-slpi` | `qcom,ipcc-self-ping` |
| `/soc/lpass_stm` | `qcom,coresight-dummy` |
| `/soc/lpass_stm/out-ports` | — |
| `/soc/lpass_stm/out-ports/port` | — |
| `/soc/lpass_stm/out-ports/port/endpoint` | — |
| `/soc/mem_dump/etf_lpass` | — |
| `/soc/mem_dump/etflpass_reg` | — |
| `/soc/power-controller@c300000` | `qcom,aoss-qmp`<br>reg: `0xc300000 0x400`<br>interrupts: `0x0 0x0 0x1` |
| `/soc/qcom,cam-cpas@ac13000` | `qcom,cam-cpas`<br>reg: `0xac13000 0x1000 0xac19000 0xac80 0xbbf0000 0x1f00 0xadd7000 0x5000`<br>interrupts: `0x0 0x1cb 0x1` |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/bps0-all-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/bps0-all-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/cre0-all-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/cre0-all-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/custom0-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/custom0-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/custom1-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/custom1-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/icp0-all-rd` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife0-pdaf-linear-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife0-rdi-pixel-raw-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife0-stats-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife0-ubwc-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife1-pdaf-linear-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife1-rdi-pixel-raw-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife1-stats-wr` | — |
| `/soc/qcom,cam-cpas@ac13000/camera-bus-nodes/level0-nodes/ife1-ubwc-wr` | — |
| _347 more_ | |
