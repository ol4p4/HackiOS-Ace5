# REA 使用规范（逆向辅助）

本文定义本仓库如何使用 REA（`rea-agents`，morluto/rea）做 iOS 二进制的互操作性分析，以及使用配额。目标：每次分析都可复现、可审计，同时不把分析引擎和真机当成默认手段。

## 目的与范围

REA 用于理解 Ace 5 移植所需的闭源接口：kernelcache 里的 kext、IOMobileFramebuffer、QuartzCore 等。分析对象是本项目自己的、已授权的互操作性目标。

不用于未授权第三方目标、绕过保护或恶意软件；也不替代运行时证据——静态结论必须由二进制或真机实验验证。

## 引擎边界（Windows 主机）

- 当前 Windows 分析主机上可用引擎只有 **IDA Pro**（legacy 1.4 attached 协议）。Hopper / Ghidra 未安装。
- REA 自带的 `inspect_macho` 等 Apple 原生命令是 macOS-only，Windows 上不可用。
- `headless` 模式需要上游 database-supervisor 协议，与当前上游不兼容；只能 attached。
- IDA 适配器只暴露 12 个只读操作；没有写库、打补丁、调试器操作。
- FAT Mach-O 会按宿主架构选 slice（win32 x64 → x86_64）；只有 arm64 的 kernelcache 会被拒绝，必须先抽 thin arm64。
- IDA 插件不会自动开服务；需要 `Ctrl-Alt-M` 或启动脚本。
- Hex-Rays 段警告是模态对话框，会阻塞 `execute_sync`；第一次出现时勾 **Don't display this message again**。
- 插件 HTTP 服务是单线程；一个卡住的请求会堵死后续全部请求，只能重启 IDA。

## 成本层级

| 层级 | 手段 | 何时用 | 代价 |
| --- | --- | --- | --- |
| T0 | 宿主静态：Python、hexdump、strings、Mach-O 头解析、差分 | 永远第一步 | 秒级，无进程 |
| T1 | REA 无引擎命令（`inspect-artifact` 等） | 格式受支持且 Windows 可用时 | 秒级 |
| T2 | REA/IDA 单点查询（`function`、`xrefs`、`decompile`、`read_memory_bytes`） | 需要闭源代码语义时 | 0.1–3 s/次，复用已开数据库 |
| T3 | 上机批处理（`idat` 定向分析、建库） | 目标地址段尚未分析 | 10 s–数分钟，一次性 |
| T4 | 设备/运行时实验（`phone.py`、日志、真机） | 静态证据无法判定时 | 分钟级，需真机 |

## 配额与平衡

1. **先写问题，再查询**：一句话写清“要回答什么、预期证据是什么”。写不出来就不查。
2. **禁止全库字符串扫描**：在 50 MB kernelcache 上会触发 2893 个段的字符串表构建（实测 >2 分钟）。改用已知段地址 + `read_memory_bytes`，或只扫目标 kext 的 `__cstring`。
3. **单点优先**：只要反编译就用 `decompile_function`；只有需要完整证据包时才用 `rea function`（它会拉 6 个观测）。`analyze_function` 不是默认选项。
4. **一个 IDA 会话服务一组问题**：相关工作批量做完再关；不要为每个问题重启 IDA。
5. **一个地址段只分析一次**：复用已建数据库；新增范围只做增量分析，不重跑全量 kernelcache。
6. **超时即诊断**：单次查询 >30 s 先 Cancel 或查对话框，不要盲等；单线程 RPC 一个卡死会堵死全部。
7. **上机前说明目的与停止条件**：T3/T4 每次运行前一句话说明“目的、预计耗时、什么结果算结束”。
8. **设备运行一次只改一个变量**，且该变量必须能被这次运行证伪；不能被证伪就不跑。
9. **证据落盘**：每个结论附地址/指令/字节；REA 的 evidence JSON 保存到本地工作目录，不重复查询。
10. **未知就写未知**：`callers`/`callees` 为空不等于没有调用者（间接调用/vtable 不证明直接边）；推论必须标注。

## 数据与仓库卫生

- Apple 系统文件、kernelcache、`.i64` 数据库、DMG、SDK、DTB、分区备份和日志留在本机，不入 Git。
- 分析产物放在本地工作目录（建议 `local/rea/`，已被 `.gitignore` 忽略）；文档里用 `%REA_WORK%` 之类的占位符，不写本机绝对路径。
- 只有文本证据和结论进入 Git；需要入库的辅助脚本放在 `tools/` 下，并与分析产物分开。
- 提交前检查 `git diff --check` 和变更内容；不要用 `git add -f` 绕过忽略规则。

## 证据格式

每条结论必须区分：

- **已确认事实**：地址、指令、字节或反编译片段。
- **合理推论**：由代码结构推出但未运行时验证的判断。
- **未知疑点**：需要下一次查询或真机实验才能确认的部分。

函数地址以数据库中的函数起点为准；`0x...` 若来自裸段反汇编，先确认偏移。`ios165/iomfb-text.bin` 有 **0x1c 前导零**，从该文件推导的地址可能整体 +0x1c；引用前先与 kernelcache 数据库核对。

## 已知坑

1. **FAT Mach-O 被拒**：`FAT binary has no host-compatible x64 architecture`。先抽 arm64 slice。
2. **裸段地址偏移**：`iomfb-text.bin` 的 0x1c 前导零；旧 asm 地址可能整体 +0x1c。
3. **Hex-Rays 模态警告**：段权限/只读推断警告会阻塞 RPC；勾选“不再显示”后继续。
4. **单线程 RPC**：卡死请求会堵住后续全部请求；重启 IDA（数据库已保存）。
5. **首次打开数据库**：可能弹 “unpacked database”，选 **Proceed with unpacked base**。
6. **插件手动启动**：IDA 的 `init()` 只注册，必须 `Ctrl-Alt-M` 或用 `-S` 脚本自动调用。

## 示例：一次完整的单点追踪（2026-10-31）

问题：IOMobileGraphicsFamily 的 vsync 发送函数、回调来源、参数与调用约定。

已确认事实（地址以数据库函数起点为准）：

- 入口 `sub_FFFFFFF005C1282C`（`0x5c1282c`）按 `+0x1640` 周期选择 beat 或通知路径。
- beat 路径 `sub_FFFFFFF005C0FAEC`（`0x5c0faec`）以 `(帧计数 +0x1628, 时间戳 +0x1630, 差值 +0x1638)` 调用 raw sender。
- raw sender `sub_FFFFFFF005C1D838`（`0x5c1d838`）把三个 64 位值拆成六个 32 位半字，遍历 `+0x1380` 的 async ref 链表，经 `+0x18` 的 ref 发送。
- 通知路径由 `sub_FFFFFFF005C11BE8`（`0x5c11be8`）传入 `(+0xB0, +0xB8, +0xC0)` 三个字段。

合理推论：beat 路径的第一字是帧计数而非回调，与“payload 第一字被当成 pc”的崩溃一致；`fb_beat=0`（周期清零）走通知路径。

未知：`+0xB0/+0xB8/+0xC0` 的语义（callback/refcon/time 还是时序参数），需要 notifications selector 的写入点。

这个示例说明本文的格式：先给地址和代码，再给推论，最后列出未知。具体发现写入 `RUNBOOK.md`，本文只保留方法与示例。

## 相关文档

- [`RUNBOOK.md`](../RUNBOOK.md)：设备发现与实验记录（证据账本）。
- [`docs/checks.md`](checks.md)：检查如何撒谎，以及如何记录例外。
- [`docs/DEVELOPMENT.zh-CN.md`](DEVELOPMENT.zh-CN.md)：源码分层、验证顺序与提交规范。
