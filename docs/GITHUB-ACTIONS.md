# 用 GitHub Actions 构建 Ace 5（无 Linux ARM64 主机）

`ubuntu-24.04-arm` 对公开仓库免费，4 核 / 16 GB / 约 120 GB 可用空间，足够跑完 UEFI、XNU、Mesa/userland 和两个 APFS 镜像。构建产物只上传一个 artifact：`ace5-kernel`（约 27 MB，含内核、三个 boot image、RAM root 和手机侧工具），保留 7 天。**iOS 系统盘 `local/system-metal.apfs` 只构建和校验，不上传**——它是 Apple 的系统卷，公开 artifact 等于再分发（见 workflow 里 "deliberately NOT uploaded" 的注释）。

工作流只有 `workflow_dispatch`，没有 push/PR 触发；它也不跑 `tools/ace5/host-selftest.sh` 或任何主机测试。

## 1. 公开 fork

工作流必须在一个**公开**仓库里运行（私有仓库的 ARM64 runner 不计入免费额度）。

```bash
gh repo fork YiKongk/HackiOS-Ace5        # 创建 <you>/HackiOS-Ace5
cd HackiOS-Ace5
git remote add fork https://github.com/<you>/HackiOS-Ace5.git
git push -u fork main
```

本仓库已经把 `dtb/giulia.dtb`、`.github/workflows/build-ace5.yml`、`RUNBOOK.md`、`patches/` 提交进去；DTB 已检查过，不含 MAC 地址或真实序列号。

## 2. 触发

```bash
gh workflow run build-ace5.yml --repo <you>/HackiOS-Ace5 --ref main
gh run watch --repo <you>/HackiOS-Ace5
```

也可以在 Actions 页面点 **Build Ace 5 (Linux ARM64) → Run workflow**。

整个 job 约 40 分钟（实测 07:13–07:43 UTC），超时上限 6 小时。步骤：

1. 释放磁盘、把工作区复制到 `/mnt`。
2. 安装 LLVM 18、7zip、ARM64 交叉 binutils；编译手机侧 `f2fs-resize`（aarch64 静态）。
3. Theos 工具链（`test-210562a`）和 `iPhoneOS16.5.sdk`。
4. Mu-Silicium（pin `42fe5637`）+ 本机编译 BaseTools。
5. UEFI + kernel 阶段 boot image，以及 persist 阶段 boot image（`allowed_partition_write: logdump, darwin`）。
6. XNU（`xnu-8796.121.2` + `redwood.patch`）。
7. `ios165/fetch.sh`（从 Apple CDN 取 4.7 GB + 2.7 GB 镜像成员）、Mesa（`AIR=1 VULKAN=1`）、userland、两个 APFS 镜像，`apfsck` 校验。
8. 上传 `ace5-kernel`（系统盘不传）。

## 3. 下载并安装

Phase 1（SMP）只需要内核和 boot image，先下小的：

```bash
gh run download <run-id> --repo <you>/HackiOS-Ace5 -n ace5-kernel -D local/ci-artifact
python3 tools/ace5/install-artifact.py local/ci-artifact
```

iOS 系统盘不在 artifact 里。需要它时在 Linux ARM64 主机上自己构建：`tools/ace5/prepare-gpu-system.py` + `tools/apfs/mkapfs.py`，命令与 workflow 里 prepare-gpu-system / mkapfs 那两步相同。

旧运行可能只有一个 `ace5-build`；先看实际名字：`gh run view <run-id> --json artifacts --jq '.artifacts[].name'`。

脚本会：

- 复制 `out/giulia-kernel/mach.ace5-public`、`hackios-giulia-kernel.img/.json`、`out/giulia-persist/hackios-giulia-persist.img/.json`、`out/giulia/hackios-giulia-diagnostic.img/.json`、`local/root-daily.apfs`、`local/tools/f2fs-resize`；
- 校验 Mach-O、`ANDROID!` 和 APFS `NXSB` magic。

已有文件默认保留，要覆盖加 `--force`。

## 4. 上机（Phase 1）

```bash
adb devices                       # 先确认手机在线
python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" enter --stage kernel
python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" ramdisk --image local/system-metal.apfs
ACE5_CPUS=1 python3 tools/ace5/phone.py --serial "$ACE5_SERIAL" run \
  --seconds 600 --bootcpu 1 --live-log --live-interval 10 \
  --kernel out/giulia-kernel/mach.ace5-public --root-image local/root-daily.apfs
```

`ramdisk --image` 把系统盘作为 `rd=md0`；不传它，内核会把小镜像当根，并在 `exec /sbin/launchd` 时 panic（SMP 的核数日志仍然有效）。

然后依次 `ACE5_CPUS=2`、`4`、`8`，按 `RUNBOOK.md` 的验收项检查 PSCI `RUNNING` 核数、`gic: no redistributor` 和 USB 日志。

## 5. 常见失败

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `could not create work tree dir '/mnt/Mu-Silicium': Permission denied` | `/mnt` 属于 root | 工作流已把 Mu 放到 `/mnt/ace5/Mu-Silicium` |
| `clang-11: error: unsupported option '--gdwarf-5'` | Theos 的 `as` 把 GCC 的 `--gdwarf-5` 转给 clang-11 | 工作流不再把 Theos `bin` 加进 PATH；`prepare-linux-arm64.py` 给 BaseTools 的 `make` 前置 `/usr/bin` |
| `Only NNG free on /mnt` | runner 磁盘不足 | 换更大的 runner；实测 ARM64 有 122 GB |
| Apple CDN 下载超时 | `fetch-from-ipsw.py` 自带重试 | 直接重跑整个 workflow（每次从零开始，约 40 分钟） |

## 6. 产物清单

| 文件 | 大小（实测） | 用途 |
| --- | --- | --- |
| `mach.ace5-public` | 15 MB | XNU 内核，`phone.py --kernel` |
| `hackios-giulia-kernel.img` | 2.0 MB | `fastboot boot` 的 UEFI + loader 镜像 |
| `hackios-giulia-persist.img` | 2.0 MB | persist 阶段（`ufs_write=1`，只允许写 `logdump`/`darwin`） |
| `hackios-giulia-diagnostic.img` | ~2 MB | 只读诊断阶段 |
| `local/tools/f2fs-resize` | 2.7 KB | 手机上收缩 `/data` 的静态 aarch64 工具 |
| `local/root-daily.apfs` | 11 MB | RAM root（`--root-image`） |
| `local/system-metal.apfs` | 2.38 GB | iOS 16.5 系统盘（`ramdisk --image`）；只构建，不上传 |

系统盘不在 artifact 里，本机构建后直接 `ramdisk --image`，不需要分片。
