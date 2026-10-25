# Ace 5 云构建主机指南（Ubuntu 24.04 ARM64）

本指南在一台 **Linux ARM64** 云主机上从零构建 Ace 5 的 XNU、loader、UEFI 和 userland，然后把四个产物传回 Windows 刷机。手机不需要交给云主机；`fastboot` 仍在 Windows 上执行。

已验证的构建环境是 Apple Silicon Mac 上的 Colima Linux ARM64 / LLVM 18；本指南把同一套步骤落到 Ubuntu 24.04 ARM64 云主机。`tools/ace5/prepare-linux-arm64.py` 会拒绝非 Linux ARM64 主机，x86-64 的 WSL2 不适用。

## 0. 机器规格

| 项 | 建议 |
|---|---|
| CPU | 4 vCPU 以上（ARM64） |
| 内存 | 16 GB 起，24 GB 更稳；8 GB 只能用 `-j2` |
| 磁盘 | **100 GB**（IPSW 约 7 GB、解包 rootfs 约 15 GB、Mu 约 3 GB、Theos 约 1 GB、XNU/Mesa 构建 15–20 GB） |
| 系统 | Ubuntu 24.04 ARM64 |
| 参考机型 | Oracle Ampere A1 免费额度（4 OCPU/24 GB/200 GB）、Hetzner CAX21、AWS t4g.xlarge |

首次完整构建约 **1–1.5 小时**；之后只重编 XNU 约 5–10 分钟。

## 1. 登录与基础依赖

在 Windows 上生成密钥（如果还没有）：

```powershell
ssh-keygen -t ed25519 -C ace5-build
type $env:USERPROFILE\.ssh\id_ed25519.pub
```

把公钥加到云主机，然后：

```bash
ssh ubuntu@<云主机IP>
```

安装依赖：

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential git curl xz-utils rsync tmux \
  clang-18 lld-18 llvm-18 \
  flex bison m4 perl autoconf automake libtool pkg-config libncurses-dev \
  python3 python3-venv python3-pip meson ninja-build nasm uuid-dev \
  gcc-aarch64-linux-gnu device-tree-compiler acpica-tools 7zip
export PATH="/usr/lib/llvm-18/bin:$PATH"
```

### 7-Zip 23+

`ios165/fetch.sh` 需要能读 APFS 的 7-Zip 23+，命令名必须是 `7z`。Ubuntu 24.04 的 `7zip` 包提供 `7zz`，做一次软链：

```bash
sudo ln -sf /usr/bin/7zz /usr/local/bin/7z
7z | head -3
```

如果版本低于 23，去 7-zip.org 下载 Linux ARM64 的 `7zz` 放到 `/usr/local/bin/7zz`，再执行上面的软链。`p7zip` 16.02 不行。

### Python 图像依赖

```bash
python3 -m venv local/image-venv
. local/image-venv/bin/activate
python3 -m pip install fonttools Pillow lzfse
```

## 2. Theos 工具链与 SDK

```bash
export THEOS="$PWD/local/theos"
mkdir -p "$THEOS/toolchain"
curl -fL --retry 3 -o local/ios-toolchain-arm64.tar.xz \
  https://github.com/L1ghtmann/llvm-project/releases/download/test-210562a/iOSToolchain-aarch64.tar.xz
tar -xJf local/ios-toolchain-arm64.tar.xz -C "$THEOS/toolchain"
git clone --no-checkout https://github.com/theos/sdks.git "$THEOS/sdks"
git -C "$THEOS/sdks" checkout --detach 0222fd5413cf4b9af096f37b4621afa2688572f7
test -x "$THEOS/toolchain/linux/iphone/bin/ld"
test -x "$THEOS/toolchain/linux/iphone/bin/ldid"
test -d "$THEOS/sdks/iPhoneOS16.5.sdk/usr/include"
```

只把 `export THEOS=...` 写进 `~/.bashrc`。**不要把 Theos 的 `bin` 加进 PATH**：里面的 `clang-11` 和 `as` 会盖住系统的 LLVM 18，导致 Mu 的 UEFI 构建和 GCC 的汇编调用失败。`xnu-env.sh` 会把需要的 cctools 链接到 `xnu-deps/host/bin`，不需要 PATH。

## 3. 源码和 DTB

```bash
git clone https://github.com/YiKongk/HackiOS-Ace5.git
cd HackiOS-Ace5
mkdir -p local/device
```

从 Windows 传设备树（这是云主机唯一需要的手机文件）：

```powershell
scp D:\\.zcode\workspace\c5551999-ba0b-46c7-81d5-e7ff5990553c\HackiOS-Ace5\local\device\giulia.dtb `
  ubuntu@<云主机IP>:~/HackiOS-Ace5/local/device/giulia.dtb
```

1.03 GiB 的 boot 链备份留在 Windows，不要上传。

## 4. Mu-Silicium 和 UEFI

```bash
export ACE5_WORK="$PWD"
git clone --no-checkout https://github.com/Project-Silicium/Mu-Silicium.git ../Mu-Silicium
git -C ../Mu-Silicium checkout --detach 42fe563791e51bf5500c2e470ed294ca0c94b97c
git -C ../Mu-Silicium submodule update --init --recursive
export ACE5_MU="$(cd ../Mu-Silicium && pwd)"

python3 -m venv local/mu-venv
. local/mu-venv/bin/activate
python3 -m pip install -r "$ACE5_MU/pip-requirements.txt" pyyaml
python3 tools/ace5/prepare-linux-arm64.py "$ACE5_MU"
export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
```

构建 UEFI FD 和诊断镜像：

```bash
tools/ace5/build-diagnostic.sh "$ACE5_MU" "$ACE5_WORK/local/device/giulia.dtb"
```

产物：

```text
out/giulia/hackios-giulia-diagnostic.img
out/giulia/hackios-giulia-diagnostic.json
$ACE5_MU/Build/giuliaPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
```

然后构建可执行 XNU 的 loader 和 boot image（复用上面的 FD）：

```bash
tools/ace5/build-loader.sh kernel
python3 tools/ace5/pack-boot.py --mu "$ACE5_MU" \
  --dtb "$ACE5_WORK/local/device/giulia.dtb" --stage kernel \
  --output out/giulia-kernel/hackios-giulia-kernel.img
```

## 5. XNU

```bash
xnu-deps/setup.sh --stage1
xnu/apply.sh
xnu-deps/setup.sh --stage2
./xnu-build.sh -j"$(nproc)" build
mkdir -p out/giulia-kernel
cp xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  out/giulia-kernel/mach.ace5-public
```

如果 8 GB 内存的机器 OOM，把 `-j"$(nproc)"` 改成 `-j2`。`xnu-build.sh` 使用 `make`，不要用 `-k` 掩盖错误。

## 6. iOS 用户空间、Mesa 和 APFS

```bash
bash ios165/fetch.sh
AIR=1 DRIVERS=softpipe,freedreno tools/mk-mesa.sh
GL=1 PREBUILT= bash userland/build.sh
tools/ace5/build-gpu-userland.sh local/components --metal
```

`ios165/fetch.sh` 从 Apple CDN 下载约 7 GB，5–15 分钟。Mesa 固定 25.0.7，脚本会校验 SHA-256。

生成小 root 和系统目录：

```bash
python3 tools/ace5/prepare-gpu-root.py \
  --base userland/root --components local/components --ios-root ios165/rootfs \
  --output local/root-daily --metal --app --display-test \
  --compositor-threading inline --jobs tools/ace5/rc.jobs.daily
python3 tools/ace5/prepare-gpu-system.py --ios ios165 \
  --stage0 userland/obj/stage0 --triangle local/components/triangle_app \
  --output local/system-metal
```

**壁纸步骤跳过。** `prepare-wallpaper-system.py` 需要 macOS 的 `sips`；Linux 上直接使用 `local/system-metal`，桌面能启动，默认壁纸可能不完整。需要壁纸时在 Mac 上跑那一步，再重新生成 `system-wallpaper.apfs`。

生成并检查 APFS：

```bash
python3 tools/apfs/mkapfs.py -z --plain fw -o local/root-daily.apfs redwood=local/root-daily
tools/apfs/check-image.sh local/root-daily.apfs redwood=local/root-daily
python3 tools/apfs/mkapfs.py -z -o local/system-metal.apfs \
  System=local/system-metal,manifest=ios165/system.manifest
tools/apfs/check-image.sh local/system-metal.apfs \
  System=local/system-metal,manifest=ios165/system.manifest
```

小 root 必须 ≤ 32 MiB；系统镜像约 2.5 GB。检查失败不要启动。

## 7. 产物回传 Windows

云主机上确认四个文件：

```bash
ls -lh out/giulia-kernel/mach.ace5-public \
  out/giulia-kernel/hackios-giulia-kernel.img \
  out/giulia-kernel/hackios-giulia-kernel.json \
  local/root-daily.apfs local/system-metal.apfs
```

在 Windows 上拉回（路径按你的工作区调整）：

```powershell
$R = "D:\.zcode\workspace\c5551999-ba0b-46c7-81d5-e7ff5990553c\HackiOS-Ace5"
scp ubuntu@<云主机IP>:~/HackiOS-Ace5/out/giulia-kernel/mach.ace5-public "$R\out\giulia-kernel\"
scp ubuntu@<云主机IP>:~/HackiOS-Ace5/out/giulia-kernel/hackios-giulia-kernel.img "$R\out\giulia-kernel\"
scp ubuntu@<云主机IP>:~/HackiOS-Ace5/out/giulia-kernel/hackios-giulia-kernel.json "$R\out\giulia-kernel\"
scp ubuntu@<云主机IP>:~/HackiOS-Ace5/local/root-daily.apfs "$R\local\"
scp ubuntu@<云主机IP>:~/HackiOS-Ace5/local/system-metal.apfs "$R\local\"
```

系统镜像 2.5 GB，`scp` 需要几分钟；也可以先 `tar -cf` 再传，避免压缩浪费时间。

## 8. Windows 上刷机

```bash
cd "$R"
python tools/ace5/phone.py --serial b4cf8812 enter --stage kernel
python tools/ace5/phone.py --serial b4cf8812 ramdisk --image local/system-metal.apfs
ACE5_CPUS=2 python tools/ace5/phone.py --serial b4cf8812 run \
  --seconds 120 --bootcpu 1 --live-log --live-interval 10 \
  --kernel out/giulia-kernel/mach.ace5-public --root-image local/root-daily.apfs
```

之后把 `ACE5_CPUS` 改成 `4`、`8`。日志在 `logs/giulia/`。

## 9. 常见问题

| 现象 | 处理 |
|---|---|
| `7z: command not found` 或版本 < 23 | 安装 `7zip`，软链 `7zz`；不要用 p7zip |
| `This setup is specifically for Linux ARM64` | 主机不是 ARM64；换云机型，不要在 x86-64 WSL 上硬套 |
| `Mu revision mismatch` | 重新 checkout 到 `42fe563791e51bf5500c2e470ed294ca0c94b97c` |
| `build_uefi.py` 找不到依赖 | 激活 `local/mu-venv` 后再跑 `build-diagnostic.sh` |
| XNU 编译 OOM | `-j2`，或换 16/24 GB 内存的机型 |
| `prepare-wallpaper-system.py` 报 `requires macOS sips` | Linux 上跳过；用 `system-metal.apfs`，壁纸在 Mac 上补 |
| `pack-boot.py` 报 FD/DTB 错误 | 确认 `SILICIUM_UEFI.fd` 已生成，DTB 是 giulia 的 `qcom,pineapple` |
| 磁盘不够 | 至少 100 GB；`ios165/rootfs`、Mu、构建缓存最占空间 |

## 10. 安全与清理

- 云主机上有 Apple IPSW 解包出的文件；不要公开共享、不要提交到 git。
- 构建完成后可以删除 `ios165/rootfs`、`local/ios-toolchain-arm64.tar.xz`、Mesa 源码包，保留产物即可。
- 不再需要云主机时直接销毁实例；所有 Apple 文件都在云盘上，随实例删除。
