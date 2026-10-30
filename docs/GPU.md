# Ace 5 GPU（Adreno 750 / RedwoodGPU）现状与缺口

第 1 条线（图形）的收口文档。**结论先说：GPU 不挡四组系统应用。** GL 路径下桌面、控制中心、Safari 都正确，CoreAnimation 的 display link 跑在 60 Hz；差的是性能、功耗、健壮性，以及一条只跑通过一次探针的 Metal 路。硬件事实见 `docs/ACE5-HW.md`，驱动矩阵见 `docs/DRIVERS.md`。

## 1. 已经硬件验证的部分

- 提交链路：GMU + HFI、ZAP/PAS13、SMMU 4K、CP ring、PM4 提交、fence 回传（`--gpu-probe 20 --gpu-direct`，stage 1–20 全过）。
- 显示：`RM6_BLIT2DSCALE` 2D blit，1125×2436 → 1264×2780 缩放后扫描出图。
- 正确性（GL/Mesa 路径，`eagl` → OSMesa → freedreno）：控制中心 0.3% 黑、桌面图标/状态栏正确、Safari 页面渲染正常。刷新率的证据是 `userland/share/web/refresh.html` 的 `requestAnimationFrame` 间隔：**60.0 Hz / 16.7 ms / 16.0–17.0 ms / 4000 帧 / 0% 超 20 ms**（`logs/giulia/20261030-141725-*`）。它证明的是 display link 的节拍，不是面板的光学刷新；present 路径按客户端提交出图，空闲时 `MiniIOMobileFramebuffer` 计数是 6–10 fps（`logs/giulia/20261030-142126-run.log`）。
- 挂死检测与可选恢复：`gpu_recover=1` 复位整核、放弃挂住的那条提交，后续继续（`xnu/files/iokit/Kernel/RedwoodGPU.cpp:247`）。

## 2. 缺口（按对体验的影响排序）

| # | 缺什么 | 证据 | 影响 | 验收方式 |
|---|---|---|---|---|
| 1 | **GPU 调频/调压（DVFS）** | stage 15 只投 "stock minimum clock vote"（`xnu/files/bsd/dev/giulia_gmu_hfi.h:280`）；驱动已上传 13 点 DCVS 表，231–903 MHz（同文件 `:178-187`），但没有投票策略/消费者，也没有 GPU rail 的电压表 | 全程最低档；重负载靠 CPU 等 fence 顶；功耗非最优 | 重负载下帧时间下降、`redwood-gl` 的 fence 等待变小 |
| 2 | **运行时掉电/空闲省电** | RSC slumber 只在 probe 收尾用过；无 idle collapse；无屏幕关/开（DPMS）。DPMS 要 DSI 命令（§4 明确不碰），且 GPU 掉电后 scanout 归谁还没分析 | 待机功耗高；锁屏/熄屏路径未走通 | 空闲 N 秒后 GPU 掉电、触摸后 100 ms 内恢复出帧 |
| 3 | **UBWC / 表面压缩** | `RedwoodGPU.cpp` 无 UBWC/macroTile 代码，表面全线性 | 1125×2436×4@60 读写带宽翻倍，模糊/大图层吃带宽 | `MSM_PARAM` 报 UBWC 后画面不变、带宽计数下降（依赖第 8 项） |
| 4 | **每帧纹理开销** | 帧统计：`4120 us a frame`，其中 `792 us` 花在"为 IOSurface 建纹理"；1200 帧里 1441 个 IOSurface 当纹理、仅 32 次真拷贝 | 每帧 ~0.8 ms 纯开销（16.7 ms 预算的 5%） | 缓存命中后 `making textures` 时间趋零、帧时间下降 |
| 5 | **每客户端页表 / 每表面 fence** | 方法面只有 GEM/submit/fence（`RedwoodGPU.cpp:1257`）；所有客户端共用一个地址空间 | 一个客户端的坏命令影响全局；恢复只能整核复位 | 两个客户端各自故障互不影响 |
| 6 | **Metal 路径（RedwoodMetal）** | 最近运行未启用；2026-10-28 用 `metal_profile=1` 打印过 first draw（SpringBoard 5 次 draw，`RUNBOOK.md:426-436`）；`metal_props on` 无 `metal=1` 仍会失败（`kIOReturnUnsupported`） | 现在全靠 GL/Mesa；系统 App 不强制 Metal 所以未暴雷，第三方 App/新系统组件要 Metal 就空 | 任意 Metal 小程序画出第一帧并打出探针行（探针已打印过，缺的是任意程序与完整 MPS/platter） |
| 7 | **Vulkan 呈现（WSI/swapchain + CA 桥）** | Turnip gate 1–7 全过（设备/队列/内存/镜像/**空提交+fence**），但无三角形、无 swapchain、无 VkImage→CoreAnimation 的桥 | 只有改走"Vulkan 合成桌面"才需要；当前不是瓶颈 | 一个清屏的 Vulkan 帧出现在屏幕上 |
| 8 | **性能计数器/时间戳** | 只有墙钟；无 GPU perfcntr、无 timestamp query | 调优靠猜，无法归因 | 能读出每条提交的 GPU 起止时间 |

## 3. 如果继续这条线，先做两件

1. **DVFS**（第 1 项）。RPMh 投票的**机制**可照 `xnu/files/bsd/dev/redwood_adsp.c:768`（ADSP 的 LCX/LMX 两路 rail 电平），但那是 LPASS 轨，不是 GPUCC；GPU 侧要补的是 OPP 策略/消费者和电压表，比"接线"多。
2. **IOSurface 纹理缓存**（第 4 项）。只改 `userland/src/eagl.c`，每帧省 ~0.8 ms，风险低。

其余（UBWC、每客户端页表、Metal、WSI）是"要更好"而非"要能用"，等 16.5 四组系统应用全绿之后再说。

## 4. 明确不需要的

- **120 Hz**：设备以 `375×812 @3.00×`（1125×2436，非 ProMotion 的 iPhone X 一代身份）呈现，CoreAnimation 的 display link 就是 60 Hz。要 120 需要同时改 ProMotion 身份与 `MiniIOMobileFramebuffer` 的 120 Hz mode，四组应用都不依赖。
- **背光**：`bl_ctrl_dcs`，要走 DSI DCS 0x51；本移植完全不碰 DSI（面板由固件点亮，我们只写像素），不在关键路径上，已挂起。
- **`--metal` 的 51% 黑块**：只在 `--metal` 启动出现；GL 路径 0.3% 黑。`metal_props on` 在没有 `metal=1` 时根本不会生效，所以近期运行其实从未真正启用 Metal。这条回归留到第 6 项一起看。

## 5. 证据边界

这一节的每一条都是"线 1 收口"成立的前提，写在这里避免下一个人把结论读强：

- **单启动**：控制中心 0.3% 黑来自一次启动的 10 张截图（`RUNBOOK.md:1121-1158`）；`docs/DRIVERS.md` 自己写的验收是"同配置两轮黑/饱和分布稳定"，第二轮还没做。
- **60 Hz 是 display link**：rAF 间隔，不是面板光学测量，也不是 present 延迟；present 计数在空闲时只有 6–10 fps（这是正常的，合成器按提交出图）。
- **四组闸门未实现**：`docs/DRIVERS.md` 的"16.5 的出门闸门"一节写的 `tools/ace5/app-smoke.py` 不存在，四组用例也没写；"GPU 不挡四组"是当前证据下的判断，不是闸门跑绿的结果。
- **Metal 只到探针**：first draw 打印过一次，`MPS`/platter 仍不完整；第三方 App 要 Metal 仍会空。
