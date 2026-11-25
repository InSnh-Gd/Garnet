# Garnet

[English](README.md) | 简体中文

Garnet 是面向 x86_64 OpenCore macOS 的开源 AMD RDNA 4 显卡与显示驱动栈。它在 [Cobalt64](https://github.com/Almosst-DEV/Cobalt64) 的基础上演进为独立四层架构，去掉了 `Lilu.kext` 与外部显示插件依赖，并吸收了 [RX4Darwin/AMDRadeonRDNA4.kext](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext) 在显示、硬件光标、WSI、VMID、休眠唤醒和 OpenGL 表面桥接方面的非 Lilu 实现。

> [!WARNING]
> `Garnet.kext` 会直接读写 PCIe BAR、PSP、SMU、RLC、SDMA、CP、MES 与 DCN 显示引擎寄存器。版本、显卡或系统构建不匹配都可能导致死机、重启或显示异常。请仅在可恢复的专用测试机上自担风险运行，详见 [LICENSE](LICENSE)。

## 支持的硬件

| 核心代号 | 对应型号 | PCI 设备 ID | LLVM / IP 版本 | 当前状态 |
| --- | --- | --- | --- | --- |
| Navi 44 | Radeon RX 9060 XT **16GB** | `1002:7590` | `gfx1200`、`GC 12.0.0`、`MP0 14.0.2`、`SDMA 7.0.0` | 主要开发与实机已验证目标 |
| Navi 44 | Radeon RX 9060 XT **8GB** | `1002:7590` | `gfx1200`、`GC 12.0.0`、`MP0 14.0.2`、`SDMA 7.0.0` | 代码已就绪（离线与隔离测试通过），待物理机实机验证 |
| Navi 48 | Radeon RX 9070 / RX 9070 XT / AI PRO R9700 | `1002:7550`、`1002:7551` | `gfx1201`、`GC 12.0.1`、`MP0 14.0.3`、`SDMA 7.0.1` | 保留来自 Cobalt64 的分芯片表、固件路径与 Mesa 条目，未在物理机验证 |

共享接口、启动参数和通用文件统一使用 `RDNA4` 与 `Garnet` 命名；仅在存在硬件、固件或 PCI ID 差异处使用 `Navi 44` 与 `Navi 48`。`GarnetAccel.kext` 通过内核集合指纹匹配支持 x86_64 macOS `26.x`（以 `25G332` 为验证基线）；Release 1 二进制包仅面向物理机 Navi 44（`1002:7590`）。

## 架构概览

```text
  Metal / OpenGL / Vulkan 应用
        │                │
        ▼                ▼
  GarnetMetal.bundle   libvulkan_radeon.dylib（Mesa RADV Darwin + VK_EXT_metal_surface）
  （org.garnet.metal，   ▲
   libgarnetxlate.dylib）│
        │                │ IOConnectCallMethod（'GRNT' ABI）
        ▼                │
  GarnetAccel.kext（org.garnet.accel，系统盘 /Library/Extensions AuxKC）
        │  通过 GarnetMetalNub 发布 GarnetAccelerator 与 GarnetFramebuffer
        ▼
  Garnet.kext（org.garnet.driver，系统盘 /Library/Extensions AuxKC）
        ▲
        │  早期 PCI 64 位 Full-BAR（8/16 GiB）分配支持（仅 Full-BAR 模式）
  GarnetPCIBarCompat.kext（org.garnet.pcibarcompat，OpenCore EFI/OC/Kexts/）
```

- `GarnetPCIBarCompat.kext`（`src/garnet-pci-compat/`）：面向 Full-BAR（8GB 卡 8 GiB / 16GB 卡 16 GiB）模式的可选早期 OpenCore 驱动。在跨 `26.x` 对 `com.apple.iokit.IOPCIFamily` 完成 `exact-uuid`（`25G332`）或 `structural` 校验后开启大内存 BAR。
- `Garnet.kext`（`src/garnet/`）：负责 PCIe BAR 映射、片上 IP 发现、固件加载、VMID 与稀疏页表管理、无 Lilu S3 休眠唤醒与 DP DDC/AUX 恢复、`AMDGarnetPrimaryFramebuffer` 模式设置与 128x128 硬件光标，以及 `'GRNT'` 用户客户端（`IOAccelGarnetNativeClient`）。
- `GarnetAccel.kext`（`src/garnet-accel/`）：在 Auxiliary Kernel Collection 中对接 `IOAcceleratorFamily2`，通过版本化布局指纹匹配虚表偏移与实例大小（`tools/ioaccel-layout/`）。
- Mesa RADV Darwin（`mesa-patches/`）：基于 Mesa 提交 `f5cb8ee032adabef599ae892ec4e42d796b84da9` 的补丁系列，编译仅启用 ACO 的 `libvulkan_radeon.dylib`，支持 `'GRNT'` 传输、`VK_EXT_metal_surface` 及 `tools/rdna4/radv-app.sh`。
- `GarnetMetal.bundle`（`src/garnet-metal/`）与 `libgarnetxlate.dylib`（`third-party/metal2vulkan-rdna4/`）：基于 RADV 派生 `MTLIOAccelDevice`，通过 `metal2vulkan` 补丁和 `garnetxlate` 将 Metal AIR 转为 SPIR-V，并在非 `WindowServer` 的 OpenGL 进程内提供 `GarnetGLDrawable` 表面桥接。
- `Garnet.app` 与 `garnetctl`（`src/garnet-app/`）：SwiftUI 安装管理应用与命令行工具，集成预检、OpenCore 自动配置与校验、应用黑名单及脱敏诊断导出。

## 安装指南（RX 9060 XT / Navi 44）

> [!IMPORTANT]
> Release 1 仅支持 **x86_64 macOS 26.x 物理机** 与 **AMD Radeon RX 9060 XT（`1002:7590`，Navi 44）**，虚拟机、Apple Silicon 和 Navi 48 会被预检直接拦截。安装器不会自动挂载或修改你的 EFI 分区。RX 9060 XT **16GB** 已完成实机验证；**8GB** 版本代码已就绪、尚待物理机实机验证（详见 [docs/rdna4/full-bar-aperture.md](docs/rdna4/full-bar-aperture.md)）。

### 第一步：BIOS 设置与启动应用

1. 在主板 BIOS 中开启 `Above 4G Decoding` 和 `Re-Size BAR Support`，关闭 `CSM`。
2. 从 Releases 下载 `Garnet-<version>-Navi44-macOS26.dmg`，把 `Garnet.app` 拖入 `/Applications`。
3. 清除隔离属性（或右键选择 **打开**）后启动 `Garnet.app`：

   ```sh
   xattr -dr com.apple.quarantine /Applications/Garnet.app
   ```

### 第二步：自动配置 OpenCore

1. 手动挂载你的 EFI 分区。
2. 打开 **Garnet.app -> OpenCore Assistant**，按主界面建议选择模式（`Full-BAR (8/16 GiB)` 或 `256 MiB Small-BAR`），点击 **Auto-Configure OpenCore…**，选中已挂载的 `EFI` 目录或 `EFI/OC/config.plist`（命令行等价操作：`/Applications/Garnet.app/Contents/Resources/garnetctl configure-oc --mode full --config /Volumes/EFI/EFI/OC/config.plist`）。程序会自动备份原 `config.plist`、在 Full-BAR 模式下写入 `GarnetPCIBarCompat.kext`、更新所需键值并立即完成校验。
3. 若预检显示 `SIP SETUP NEEDED`，先重启一次让新的 `csr-active-config` 生效。

### 第三步：安装并重启

1. 在 **Garnet.app -> Install / Uninstall** 确认状态为 `READY` 后，点击 **Install / Upgrade Driver…**（或执行 `sudo /Applications/Garnet.app/Contents/Resources/garnetctl install`；需要 Vulkan 时再勾选 Vulkan ICD 或加 `--with-vulkan`）。
2. 重启进入桌面。

### 日常管理与应急恢复

- 应用 Metal 黑名单：将 `.app` 拖入 **Garnet.app -> Status & Denylist**，或执行 `sudo /Applications/Garnet.app/Contents/Resources/garnetctl denylist add-app /Applications/ProblemApp.app`。
- 导出脱敏诊断：在 **Status & Denylist** 导出，或执行 `garnetctl export-diag --out ~/Desktop/garnet-diag.tar.gz`。
- 完整卸载：在 **Garnet.app -> Install / Uninstall** 点击 **Uninstall Garnet…**（或执行 `sudo garnetctl uninstall`），随后在 OpenCore 中禁用 `GarnetPCIBarCompat.kext` 与 `garnet*` 启动参数并重启。
- 应急启动参数（可在 OpenCore 启动菜单临时追加）：`garnet=0` 禁用 `Garnet.kext`，`garnet-aux=0` 禁用 `GarnetAccel.kext`，`garnet-rebar-compat=0` 禁用 Full-BAR 补丁，`garnet-apps-all=0` 关闭普通桌面应用 Metal 加速。

## 目录结构

| 路径 | 内容说明 |
| --- | --- |
| `src/garnet` | 主驱动 `Garnet.kext`、DCN 4.01 模式设置与光标代码、VM/休眠纯逻辑模块、着色器及宿主测试 |
| `src/garnet-accel` | 辅助内核集合驱动 `GarnetAccel.kext` 与布局守卫测试 |
| `src/garnet-pci-compat` | OpenCore 早期 Full-BAR 扩展 `GarnetPCIBarCompat.kext` 与 `IOPCIFamily` 校验测试 |
| `src/garnet-metal` | `GarnetMetal.bundle`、纯 C 不变量模块、`spvcache` 元数据与兼容测试 |
| `src/garnet-app` | Swift `Garnet.app`、`garnetctl` CLI、KC/OpenCore 校验与配置器及 `GarnetCoreTests` |
| `src/dcn41` | DCN 4.01 寄存器定义与生成的写入白名单 |
| `mesa-patches` | Mesa RADV Darwin 补丁系列及离线宿主测试 |
| `third-party/metal2vulkan-rdna4` | `metal2vulkan` 补丁系列与 `garnetxlate` Rust crate（LGPL-3.0-or-later） |
| `tools/rdna4` | 载荷构建脚本（`build-all.sh`）、`.dmg` 打包器（`build-release-dmg.sh`）、安装与恢复脚本及 `radv-app.sh` |
| `tools/native` | 用户态翻译辅助工具与诊断探针 |
| `tools/dcn41` | DCN 4.01 白名单和寄存器代码生成器及变异测试脚本 |
| `tools/ioaccel-layout` | macOS 内核集合解包、虚表偏移校验与布局指纹导出工具 |
| `tools/fetch-firmware.sh` | 从本地 `linux-firmware` 仓库复制 RDNA 4 微码文件 |
| `docs` | [文档索引](docs/README.md)与各子系统技术规格 |

## 依赖准备与构建

仓库不包含第三方二进制组件与上游源码树，从源码构建前需在本地准备以下依赖：

| 依赖项 | 固定版本 / 来源 | 用途 |
| --- | --- | --- |
| `MacKernelSDK` | Acidanthera `MacKernelSDK` `05094e5e88cec7caedbfb35e8449ed0db94bf95b` | 编译 `Garnet.kext` 与 `GarnetAccel.kext` 时通过 `MKSDK` 指定 |
| `linux-firmware` | 上游 Linux 内核固件仓库 | 提供 Navi 44 / Navi 48 微码，复制至 `src/garnet/firmware/` |
| `Mesa` | 提交 `f5cb8ee032adabef599ae892ec4e42d796b84da9` | 应用 `mesa-patches/` 后由 `tools/rdna4/build-radv.sh` 编译（需 `glslangValidator` >= 12.2、`meson`、`ninja`） |
| `metal2vulkan` | 提交 `43c46ac8a24adf1a6e872b8a52c706ec9614fad0` | 应用 `third-party/metal2vulkan-rdna4/` 后由 `tools/rdna4/build-xlate.sh` 编译（需 Rust >= 1.87） |
| 目标 macOS KC 转储 | `BootKernelExtensions.kc` / `SystemKernelExtensions.kc`（25G332） | 供 `tools/ioaccel-layout/` 生成 `GarnetAccel` 布局头文件（`C64_WORK` / `C64_LAYOUT_OUT`） |

获取 Navi 44 固件并编译 `Garnet.kext`（在 macOS 上编译，或在 Linux 上配合 `clang` 与支持 `-kext` 的 cctools `ld64` 交叉编译内核态目标）：

```sh
CHIPS=navi44 bash tools/fetch-firmware.sh /path/to/linux-firmware
make -C src/garnet CHIPS=navi44 MKSDK=/path/to/MacKernelSDK all
```

如需在 macOS 上生成完整的匹配签名载荷，按 [BUILDING.md](BUILDING.md) 和 [tools/rdna4/README.md](tools/rdna4/README.md) 准备好环境变量后运行 `tools/rdna4/build-all.sh OUT_DIR`。

## 文档索引

- [BUILDING.md](BUILDING.md)：构建环境、Linux 交叉编译、固件抽取与成套载荷打包说明
- [docs/README.md](docs/README.md)：子系统设计文档与硬件操作手册索引
- [GLOSSARY.md](GLOSSARY.md)：核心组件命名、芯片命名规则与术语表
- [CHANGES.md](CHANGES.md)：自 Cobalt64 上游基线至独立 Garnet 架构的演进记录
- [tools/rdna4/README.md](tools/rdna4/README.md)：载荷构建、一致性校验与安装/卸载脚本说明

## 致谢与许可证

本仓库主体代码采用 [MIT 许可证](LICENSE)，[third-party/metal2vulkan-rdna4](third-party/metal2vulkan-rdna4/NOTICE) 作为 `metal2vulkan` 的衍生作品适用 GNU Lesser General Public License v3.0 or later（[COPYING](third-party/metal2vulkan-rdna4/COPYING)）。`src/dcn41/` 下的寄存器头文件沿用 AMD MIT 开源寄存器头许可。Git 源码仓库不分发 AMD 二进制固件文件；正式发布的 Release 1 载荷（`RDNA4_RELEASE=1`）与 `.dmg` 安装镜像随同上游 `LICENSE.amdgpu` 打包分发 Navi 44 微码。

本项目基于以下开源工作完成并致谢：

- [Cobalt64](https://github.com/Almosst-DEV/Cobalt64)（MIT）：核心 RDNA 4 启动栈、`'GRNT'` 用户客户端、Mesa RADV Darwin 后端基础、`GarnetMetal` bundle 设计与 `GarnetAccel` 布局工具。
- [RX4Darwin/AMDRadeonRDNA4.kext](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext)（BSD 3-Clause，Copyright (c) 2026 Sunneva N. Mariu，`next` @ `dded456d`）：非 Lilu 相关组件，包括 Navi 44 `ODM_MEM_PWR_CTRL2` 读写危险排除、CoreDisplay `AMD*` 帧缓冲类名前缀识别、DCN 4.01 128x128 硬件光标编程、RADV `VK_EXT_metal_surface` WSI 回退与 `radv-app.sh` 启动封装、VMID 精确 TLB 应答/隔离/稀疏页表回滚、无 Lilu S3 休眠唤醒与 DP DDC-to-AUX 恢复，以及 `GarnetGLDrawable` OpenGL-on-Metal 表面桥接（`supportsUnalignedVertexFetch`、`commitAndHold`、`setLineWidth:`）。
- [RDNA4FB](https://github.com/somestupidgirl/RDNA4FB)（BSD 3-Clause，Copyright (c) 2026 Sunneva N. Mariu）：片上 IP 发现（`src/garnet/src/ipdiscovery.*`）、BAR5 映射与间接显存访问实现参考。
- [mac-amdgpu](https://github.com/lemonade-sdk/mac-amdgpu)（MIT，Copyright (c) 2026 Geramy Loveless）：RDNA 4 IP 模块初始化顺序参考。
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa)（MIT）、[metal2vulkan](https://github.com/steelbrain/metal2vulkan)（LGPL-3.0-or-later）及 [NullMoth Systems](https://github.com/nullmoth/nvidia-macos-driver)（LGPL-3.0-or-later 编译器补丁适配，见 [third-party/metal2vulkan-rdna4/NOTICE](third-party/metal2vulkan-rdna4/NOTICE)）。

完整第三方许可条款及逐文件溯源见 [third-party/THIRD-PARTY-LICENSES.txt](third-party/THIRD-PARTY-LICENSES.txt)。本项目为独立开源驱动实验，与 AMD、Apple、Cobalt64 或 RX4Darwin 均无从属或官方背书关系。
