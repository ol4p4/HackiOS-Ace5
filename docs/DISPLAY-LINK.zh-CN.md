# 显示链路与帧率：transition 的 2x 锁、生态调查与上游解法

> 2026-11-02，分支 `turnip-glslang`。状态：补丁已移植、已构建、镜像已校验；**上机验证尚未进行**（手机未连接）。
> 相关：`RUNBOOK.md` 的 "State of play (2026-11-02, end of session)" 与 `docs/REA.zh-CN.md` 的 worked example。

## 1. 问题

Control Center 收起（89%,70% 窗口）稳定在 33.3 ms 一帧，即 30 fps；同一轮里手指拖动是 55–59 fps。
测量基线（本轮之前，`ACE5_CPUS=8`，`ca_vbl=1 fb_minint=1`）：

| run | 89%,70% 窗口 | 拖拽 | 端到端 |
|---|---|---|---|
| 20261102-151958 | 28.9 fps / 32% / 25% | 55.9–59.2 | 24.5 ms |
| 20261102-164733（新库基线） | 30.4 fps / 33% / 27%（232 armings，1,739 帧） | parity | — |
| arm C（历史） | 28.8 fps / 35% / 27% | parity | 23.5 ms |

通过标准沿用 RUNBOOK:1771：**retract ≥55 fps、超过 16.7 ms 的帧 <3%**。

11-02 当天已经排除的（全部有 run id，见 RUNBOOK）：

- **interval chain 两侧**：窗口服务器的 AppleDisplay period = 0.016666667 s（60.000 Hz）、multiplier = 1
  （vtable probes，run 20261102-172655）；SpringBoard 根本不调用那些槽（同一 probes，零次）；
  两个进程都不调用 `copy_display_timings`（runs 20261102-172403 / 174014），整数比例缩放路径不成立。
- **三个 frame-rate 环境旋钮**：`CA_ENABLE_ALL_FRAME_RATES=1` 在窗口服务器里反而冻结 display link
  （1 arming、2 帧，run 20261102-170049）；`CA_ENABLE_REDUCED_SERVER_VSYNC_RENDER=1` 无害
  （run 20261102-165834）；`CA_ENABLE_FRAME_RATE_ARBITRATION=0` 惰性（run 20261102-151958）。
- **机制限制**：dyld interpose 到不了共享缓存内部的调用（os_variant interpose 从未触发）；
  vtable 补丁与 ObjC method replacement 是这里唯一有效的机制。

结论：33.3 ms 不是"周期算错"，而是 CA 在按某个客户端请求的 30 fps 出帧。

## 2. 生态调查（2026-11-02）

方法见 §8。以下只记录能由 commit 元数据或文档直接证实的内容。

### 2.1 上游 `oosexgang/hackios-xnu-kernel`（我们的 16.5 线，同一个 inject_sb.c 血统）

- **10-31 01:16Z `f6ebf31`**：`fb_ca_hz=N`，单独告诉 CoreAnimation 刷新率（面板 120、CA 按 60 出帧的问题）。
- **10-31 00:52Z `c7b5f9a`**：`mkhighrate.py` 给系统镜像里每个应用写 `CADisableMinimumFrameDurationOnPhone`；
  没有它 CA 把每个进程卡在 60。
- **10-31 01:46Z `cd8fdbb`**：`fb_filter=2` 默认（动效中最近邻、静止 40 ms 后双线性重绘）+ `fb_pri` 面板线程优先级 92；
  缩放 1.9 ms（十帧最慢 4.4），对 3.6/11；Settings 滚动 108 fps。
- **11-01 06:05Z `7caa457`（本轮关键）**：主页翻页后约 0.7 s 的帧间隔正好 33.3 ms（SpringBoard run loop 与窗口服务器都是 30 fps）。
  根因是 **UIKit `UIViewInProcessAnimationManager` 的 display link 向 CADisplayLink 请求 20..30**；
  `inject_sb.c` 给这类 link 打标记并丢掉请求速率后：右滑 78→115 fps、>16.7 ms 17%→1%；
  左滑 85→110、12%→2%；全部手势 13%→6%；Settings 滚动 115 不变。共 126 行，只改 `userland/src/inject_sb.c`。
- **11-02 00:23Z `3639b98`**：`anim_max=1`，让 SpringBoard 交给窗口服务器的 CAAnimation 请求 80..120。
  注：提交说明写明 **OpenGL ES 路径不受影响**（我们走 GL），默认关闭。本轮不移植。
- **11-01 23:20Z ~ 11-02 01:00Z**：CoreAnimation-through-Metal（`METAL_PLUGIN=ca`、`HakiosMetalCA`、metallib writer、
  per-pipeline 变体、`/var/tmp/hakios-metal-cache`）。这是另一个方向，与本轮无关。
- 120 Hz 链路的单位约定（K90 的 11-01 借鉴文档转述上游 `b6c1f82`/`0dedaec`）：`displayMinRefreshInterval`、
  `displayMaxRefreshInterval`、`displayRefreshStep` 用 2^-32 秒定点（120 Hz = 2^32/120），
  只有 `displayRefreshStepMachTime` 用 mach tick；属性在 `publishModes()` 里设置，必须在 `registerService()` 之前。
  我们 11-02 的 period probe 得到 60.000 Hz ×1，与此一致。

### 2.2 其它项目

| 项目 | 最近进度 |
|---|---|
| `iola1999` K90（iOS 26） | 10-31~11-01 Wi-Fi 全链路（WCN7850 固件、ath12k、QRTR/WLFW、Settings 加入 WPA2 并上网）；此后只有文档提交；11-02 无新内核提交 |
| `shuaifengji-sudo/k90-hackios-build` | 11-02 01:49~08:56Z：在 macOS runner 上从 IPSW 原生构建 iOS 26 镜像（arm64e 动态程序、16.5 helpers、系统镜像拆分 artifact、强制 Mesa REBASE） |
| `wuxianlin/hackios-os_devices_oneplus-dodge` | 11-01 18:55Z 修好 blur 渲染；11-02 00:28Z 把补丁收敛为单个 `kernel-common.patch`，命名统一为 hakios |
| `kusesad-1122/hackios-dodge` | 11-02 加入 OnePlus 13 的 S3910 触摸驱动与 Goodix gate；11-01 完成 dada Wi-Fi 工具链与 arm64e 链接 |
| `Jrorz/umi-mi10`（唯一的另一条 16.5 线） | 10-31 触摸完全可用（"ten bugs gone"）；其 `docs-umi/PROGRESS.md` 记录 10-26 已到主屏，触摸/按键是当时的缺口；没有帧率/display link 相关工作 |
| `superboyyy/hakios-arm64e-dada` | 10-31 后静默，101★ |
| `WuuuWeiii/ios-sweet-ci` | 11-02 仍在 polaris GPU 2D probe 的 CI 上迭代 |

**判读**：我们的 ca_vbl 通知流调查在生态里是独有的；但"transition 锁在 33.3 ms"这个问题，上游已经用
`7caa457` 在**同一条 16.5 线**上定位并修掉。继续自己推 AlignPhase 内联补丁的边际收益很低。

## 3. 为什么这解释我们的 33.3 ms

把已有证据串起来：

1. CA 的周期函数（`0x1882d3930`）给出 1/60 s；multiplier 槽 92/93 在 `+560` bit63 置位时返回 2
   （RUNBOOK 的 multiplier cluster）——2×16.66 = 33.3 ms，正好是 30 fps。
2. 我们的 timer probe：presentation 路径不给 CA 任何时间（SwapEnd/SwapWait 只回 success，selector 77 从未被调用），
   **phase 信号是通知到达**；no-op 回调把 fallback 从 100 ms 压到 33.3 ms，说明 CA 一旦拿到 link，就按请求的速率走。
3. 上游在同一 OS 上直接读到请求值：`UIViewInProcessAnimationManager` 的 link 请求 20..30。
   30 fps 的请求 → multiplier 2 → 33.3 ms。这与我们的测量完全吻合。
4. 所以 `dl_max` 丢掉这个请求后，CA 应回到面板速率（我们 60 Hz → 16.6 ms）。

**不确定性（按重要性）**：

- 上游的验证在 120 Hz 面板上（他们的 120 Hz 链路 10-31 就通了）。我们的面板当前按 60 Hz 发布；
  若 UIKit 在 60 Hz 面板上请求的仍是 20..30，结论不变；若它按面板比例请求（例如 10..15），`dl_max` 同样丢掉。
- 类名必须精确匹配 `UIViewInProcessAnimationManager`。我们的 16.5 与上游同版本（20F66），预期一致；
  若日志显示是别的类，按日志里的 `image+offset` 扩展标记即可。
- A11 身份门（RUNBOOK 2404）影响的是"真正的 VBL 回调"能否注册；本修法不依赖它。

## 4. 移植

文件：`userland/src/inject_sb.c`（唯一改动）。来源：`oosexgang/hackios-xnu-kernel 7caa457`，按 AGENTS.md 保留归属。

- 新块位于 `control_center_fades()` 之后：
  - `frame_rate_range`（三个 float，与 CAFrameRateRange 同布局）；
  - `caller()`：`dladdr(__builtin_return_address(0))` → `image+offset`；
  - `display_link_with()`：`+[CADisplayLink displayLinkWithTarget:selector:]` 的替换；target 是
    `UIViewInProcessAnimationManager` 时给 link 挂 associated object 标记；
  - `set_fps()` / `set_range()`：`-setPreferredFramesPerSecond:` 与 `-setPreferredFrameRateRange:` 的替换；
    被标记的 link 传 0 / {0,0,0}，其余原样；
  - `display_link_rates()`：读 boot-args，安装三个替换，打印一行安装结果。
- 调用点：`redwood_springboard_init()` 里 `setvbuf()` 之后、任何 SpringBoard 的 link 创建之前。
- Boot args：
  - `touch_lat=1`：每次 link 创建、每次速率请求都打印（上限 300 行），带调用者 `image+offset`；
  - `dl_max=0`：关闭丢弃（对照组）；默认 `dl_max=1`。
- 不移植 `anim_max`：上游注明 GL 路径无效，且会与 `cc_fade` 争同一个 `addAnimation:forKey:`。

构建（新增 `local/wsl/build-sb.sh`，只编这一个 dylib，编译行与 `userland/build.sh:293` 一致）：

```
obj/libredwood-sb.dylib   90,904 bytes   (旧 90,336)
strings: "hakios: display links" / "UIViewInProcessAnimationManager" 均在
```

镜像：`local/root-daily.apfs` 重新打包（manifest 135 条，`--plain fw`），
sha256 `07d2f747fa2703a5c98ffb82496edd7ce890b4cd07ed4c9c0aafff8be61f22f3`，
`check-image`：apfsck clean，apfs-tool 读回 135/135。

## 5. 上机验证计划（待执行）

手机接上后：

```bash
ACE5_CPUS=8 python3 tools/ace5/phone.py --serial b4cf8812 run ... \
  --kernel out/giulia-kernel/mach.autotest --root-image local/root-daily.apfs \
  ACE5_CMDLINE="ca_vbl=1 fb_minint=1 touch_lat=1 touch_auto=45 touch_auto_exit=1 \
                rec_secs=1200 rec_ring=1 rec_mb=256 rec_step=4"
```

（`enter --stage kernel` → `ramdisk --image local/system-metal.apfs` 照常。backboardd 的 plist 里仍留着
`CA_ENABLE_FRAME_RATE_ARBITRATION=0`——已测惰性，保留是为了与 17:26 基线单变量可比。）

判据：

1. 日志：`hakios: display links: made ok, fps ok, range ok`；至少一条
   `display link ... made for -[UIViewInProcessAnimationManager ...]`；对应 `asked for 20..30 ... (dropped)`。
2. `python tools/frames.py logs/giulia/<run>.log`：89%,70% 窗口 **≥55 fps、>16.7 ms <3%**。
3. 无回归：拖拽 55–59 fps；无 PowerOff/panic/SIGSEGV；`vsync notifications armed` 仍在。
4. 若 30 fps 请求来自别的类：按日志的 class + `image+offset` 扩展标记，再跑一轮。

## 6. 成功之后的顺序

1. **A/B `ca_vbl=0` + `dl_max=1`**：确认 no-op 通知流是否还需要。若不需要，可以去掉 ca_vbl 的注入面。
2. **120 Hz 链路**：上游 10-31 的三件套（`publishModes` 的 2^-32 属性、`CADisableMinimumFrameDurationOnPhone`、
   `fb_ca_hz`）。Ace 5 面板支持 120 Hz；这是独立的性能项，不与本轮混跑。
3. **AlignPhase（`sub_1882AB034`）**：只在日志证明锁与 link 请求无关时再回到内联补丁。
4. **Turnip WSI/CA bridge 与 Wi-Fi**：两个更大的方向；Wi-Fi 可照 K90 的 cnss2→ath12k→QRTR 链路。

## 7. 参考

- 上游提交：`7caa457`（display link 20..30）、`3639b98`（anim_max）、`f6ebf31`（fb_ca_hz）、
  `c7b5f9a`（CADisableMinimumFrameDurationOnPhone）、`cd8fdbb`（fb_filter=2/fb_pri）。
- K90 文档：`docs/analysis/2026-11-01-hakios-upstream-lessons.md`、`docs/plan/2026-10-29-120hz-refresh-interval.md`
  （经 GitHub contents API 读取，见 §8）。
- 本仓库：`RUNBOOK.md` 2026-11-02 各节；`docs/REA.zh-CN.md`；`userland/src/inject_sb.c`；`local/wsl/build-sb.sh`。

## 8. 附录：这台机器上读 GitHub 的方法

- `api.github.com` 可用；`raw.githubusercontent.com`、`github.com`、`cdn.jsdelivr.net`、`r.jina.ai` 全部
  `ECONNRESET`/超时（2026-11-02 实测）。
- 文件内容走 `GET /repos/{owner}/{repo}/contents/{path}`：返回 JSON 的 `content` 是 base64，
  WebFetch 的小模型可以直接解码中等长度的 markdown（`docs-umi/PROGRESS.md` 整篇成功）。
- 提交细节走 `GET /repos/{owner}/{repo}/commits/{sha}`：`files[].patch` 是明文 diff，
  可以要求只取某个文件的 `+` 行；超过截断长度的文档（120Hz 方案）只能拿到摘要。
- 提交列表：`?since=...&per_page=...`；仓库搜索：`/search/repositories?q=hackios&sort=updated`。
- 本机 `gh`/`curl` 在 plan mode 下被拦；实际可用的通道是 WebFetch + api.github.com。
