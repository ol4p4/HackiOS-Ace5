# 驱动矩阵：来源、代际匹配、缺口与验收

这份表回答三个问题：**这个驱动从哪来、能不能直接用、还差什么、怎么算做完**。
硬件地址一律以 [ACE5-HW.md](ACE5-HW.md)（由 `dtb/giulia.dtb` 自动抽取）为准，不在本文里抄第二份。

## 为什么"驱动"要重写而不是移植

XNU 没有 Linux 的驱动模型：没有 `platform_driver`/`probe`/设备树绑定，没有 `clk`/`regulator`/`genpd` 框架，没有 dma-buf、workqueue、`devm_*`。能带走的是**寄存器时序与固件协议**，代码要按 XNU 的接口（`ml_io_map`、XNU 中断、`IOBufferMemoryDescriptor`、我们的 `RedwoodGPU`/`MiniIOSurface`）重新表达。

合规：取自 GPL-2.0 的寄存器表留在独立文件并保留原头（如 `giulia_gpu_regs.h`），不混进 APSL 部分。

## 矩阵

| 子系统 | 实现 / 参考出处 | 与 Ace 5 匹配 | 状态 | 缺口 | 验收 |
|---|---|---|---|---|---|
| UFS + 分区 | 上游 UFS 框架 + 我们的 `redwood_ufs.c`/`var_store` | 可用 | ✅ | — | 两次启动 `var_store` 双槽往返 |
| 显示（帧缓冲 + 缩放） | 我们写的 `RedwoodGPUDisplay`（RM6_BLIT2DSCALE，1 次 2D blit 1125×2436→1264×2780） | 自研 | ✅ 4.6 ms/帧 | 面板由固件初始化，我们只写像素 | 主屏像素对齐 |
| GPU（GMU/ZAP/SMMU/CP） | **同机** OnePlus SM8650 KGSL（`giulia_gpu_regs.h`，GPL-2.0）+ Linux a7xx + UniXNU NativeAdreno 的队列/fence 顺序 | **完全匹配** | ✅ | — | GPU 探针 stage 1–20、`display_test` 9/9 |
| Vulkan（Turnip） | Mesa Turnip + `tools/mesa/turnip-darwin.patch` + libdrm shim | 自研适配 | ✅ 空提交 + fence（gate 1–7） | 三角形/WSI/呈现（未做） | `turnip_test` PASS |
| GL 合成（CoreAnimation） | `userland/src/eagl.c` + Mesa freedreno/OSMesa | 自研适配 | ✅ 单启动正确（2026-10-30，控制中心 0.3% 黑）；跨启动两轮稳定未做 | 见 `docs/GPU.md` 第 5 节证据边界 | 同配置两轮黑/饱和分布稳定（第二轮未做） |
| 触摸 | 我们写的 `giulia_s3910.c`（S3910，轮询 SPI） | 匹配 | ✅ | 多点/长时精度 | `s3910: ready` + 报告计数 |
| 音量键 | `gpio_keys/vol_up` → TLMM **GPIO 6**，`linux,code 114` | 需新写（hackios 的 `redwood_buttons.c` 是 POCO PMIC 通道） | ❌ | 只需 GPIO 中断 + HID 上报 | 按下→音量变化（`buttons` 测试工具） |
| 电源键 / 锁屏 | 不经 gpio_keys，走 **PMIC PON**（`pmic_pon`，PM8550 家族，SPMI） | 需新写 | ❌ | SPMI 读 PON 事件 + 长按语义 | 短按锁屏/唤醒、长按关机面板 |
| 背光 / 亮度 | `bl_ctrl_dcs`：**DSI DCS 0x51**，`mdss_dsi_ctrl0@ae94000`；min 1 / max 4095 | 需新写最小 DSI 命令通道 | ❌ | 只发 DCS 短包、不接管时序（有弄黑风险，先验证） | 设置里拖亮度→屏幕实时变化 |
| 电量 / 充电 | **ADSP**：`pmic_glink/qcom,battery_charger`（`oplus,hal_sm8350`）+ `oplus,adsp_gauge` | 需新写，依赖 ADSP | ❌（现由 `inject_sb` 伪造） | ADSP 起来后的 gauge 通道 | 设置/状态栏百分比随放电变化 |
| ADSP / PAS | `qcom,adsp-loader`、`adspslpi` 82.5 MB、`q6_adsp_dtb`、`lpass_qtb`；hackios 的 `redwood_adsp.c` 可参考 PAS 框架 | 框架可参考，固件/布局不同 | ❌ | PAS 认证 + SMEM/GLINK + MHI | ADSP `running` 且能应答 |
| 音频 | ADSP + **WCD939x（I2C）** + **SIA91xx 功放（I2C）** | POCO 的 codec/amp 完全不同 | ❌ | 编解码器初始化 + 功放 + MI2S/TDM 数据面 | 点击音/铃声/Music/闹钟/录音回放 |
| Wi-Fi | **`qcom,cnss-kiwi`（WCN7850）走 PCIe**；`qcom,qmp=0x63`、`pcie_qtb`、`qps615` FEM；参考 UniXNU `Ath12kWifi` | 需新写（POCO 是 WCN6750/ath11k，错代） | ❌ | **PCIe 主机控制器** + cnss 电源/固件 + QMI | 扫到 AP→加入→Safari 打开页面 |
| 相机 | `cam-cpas@ac13000`（CAMSS）+ `oplus,sensor-devinfo` | 寄存器可抄 camss/csiphy，3A 在 Android 用户态 | ❌ | CSIPHY/CSID/IFE + 传感器 + 3A | 相机 App 出预览帧 |
| 振动 / 手电 | hackios `redwood_haptic.c` / `redwood_torch.c`（POCO） | 需换常量 | ❌ | 执行器/闪光灯通道 | `torch_test`、震动开关 |
| 调频 | hackios `redwood_cpufreq.c` + DT 的 OPP | 需换 OPP 表 | ❌（`cpufreq=0`） | OPP 表 + 频率域 | 频率随负载变化、空闲降频 |
| 时钟 / 正在播放 | `chronod` plist 已铺；`mediaremoted` 未铺 | 内容服务，非驱动 | ⚠️ | 铺 job plist + RenderBox | 状态栏时钟、媒体卡有内容 |

## 顺序（用户定：一次只推一条线）

1. **GPU/图形收口**（含设备自测刷新率、`glshow`/`glskip` 定位、控制中心损坏、Metal 能否启用）
2. **Wi-Fi（WCN7850）** —— 长杆，先 PCIe 主机控制器
3. 填充（等 CI/等上机的空隙）：音量键 → 电源键 → 电源/调频
4. **音频**（ADSP + WCD939x + SIA91xx）
5. **相机**（月级）

第 1 条线已按"单启动通过"收口（2026-10-30）：控制中心 0.3% 黑来自一次启动的一组截图；本表 GL 行要求的两轮稳定、以及下面的 `app-smoke` 都还没做（见 `docs/GPU.md` 第 5 节）。

## 16.5 的出门闸门

出门闸门是 `tools/ace5/app-smoke.py` 覆盖四组系统应用全绿（基础/联网/音频/相机）才谈 iOS 26。脚本已实现：读一次 `phone.py run` 的日志，逐条给 PASS / FAIL / BLOCKED（未移植的子系统），`--group` 只判本次跑过的那组。第三方 App 安装与适配不在本轮。
