# Garnet

[English](README.md) | 简体中文

Garnet 是面向 x86_64 OpenCore macOS 的开源 AMD RDNA 4 显卡与显示驱动栈。该项目在 [Cobalt64](https://github.com/Almosst-DEV/Cobalt64) 的基础上演进为独立四层架构，移除了对 `Lilu.kext` 和外部显示插件的依赖，并吸收了 [RX4Darwin/AMDRadeonRDNA4.kext](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext) 在显示、硬件光标、WSI、VMID、睡眠唤醒及 OpenGL 表面桥接方面的非 Lilu 成果。

> [!WARNING]
> `Garnet.kext` 会直接读写 PCIe BAR、PSP、SMU、RLC、SDMA、CP、MES 与 DCN 显示引擎寄存器。版本不匹配、硬件差异或系统构建差异都可能导致系统卡死、重启或显示异常。请仅在可恢复的专用测试机上自担风险运行；详见 [LICENSE](LICENSE)。

## 支持的硬件

| 核心代号 | 对应型号 | PCI 设备 ID | LLVM / IP 版本 | 当前状态 |
| --- | --- | --- | --- | --- |
| Navi 44 | Radeon RX 9060 XT | `1002:7590` | `gfx1200`、`GC 12.0.0`、`MP0 14.0.2`、`SDMA 7.0.0` | 本仓库的主要开发与目标验证硬件 |
| Navi 48 | Radeon RX 9070 / RX 9070 XT / AI PRO R9700 | `1002:7550`、`1002:7551` | `gfx1201`、`GC 12.0.1`、`MP0 14.0.3`、`SDMA 7.0.1` | 保留来自上游 Cobalt64 的分芯片表、固件路径与 Mesa 静态条目；本仓库未在物理机验证 |

共享代码、启动参数与通用接口统一采用 `RDNA4` 和独立 `Garnet` 命名；仅在存在真实硬件、固件或 PCI ID 差异处使用 `Navi 44` 与 `Navi 48`。`GarnetAccel.kext` 通过指纹匹配支持 x86_64 macOS `26.x`（以 `25G332` 为实机验证基线）；Release 1 二进制载荷与安装包严格限定为物理机 Navi 44（Radeon RX 9060 XT，`1002:7590`）。

## 架构概览

Garnet 将 OpenCore 早期 PCI BAR 适配、内核初始化、macOS 加速器接入、Vulkan 后端与用户态 Metal/OpenGL 支持拆分为如下层级：

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
        │  早期 PCI 64 位 16 GiB BAR 分配支持（仅默认 16G 模式）
  GarnetPCIBarCompat.kext（org.garnet.pcibarcompat，OpenCore EFI/OC/Kexts/）
```

- `GarnetPCIBarCompat.kext`（`src/garnet-pci-compat/`，`org.garnet.pcibarcompat`，`1.0.0`）：面向默认 16 GiB Full-BAR 模式的可选早期 OpenCore 驱动（放入 `EFI/OC/Kexts/`）。对 `com.apple.iokit.IOPCIFamily` 执行 `exact-uuid`（`25G332`）及跨 26.x fail-closed `structural` 签名校对后放行大内存 BAR；256 MiB 小 BAR 回退模式下禁用或不放此驱动。
- `Garnet.kext`（`src/garnet/`，`org.garnet.driver`，`0.0.673`）：负责 PCIe BAR 映射、片上 IP 发现、PSP/SMU/GMC/GFX/SDMA/MES 固件加载与引擎初始化、带精确 TLB 应答和隔离区的 VMID 池及稀疏页表回滚、无 Lilu 优先级 S3 休眠/唤醒与 DP DDC-to-AUX 恢复、内置 `AMDGarnetPrimaryFramebuffer` 模式设置与 128x128 硬件光标，以及 `'GRNT'` 原生用户客户端（`IOAccelGarnetNativeClient`）。
- `GarnetAccel.kext`（`src/garnet-accel/`，`org.garnet.accel`，`0.0.8`）：在 Auxiliary Kernel Collection 中对接苹果 `IOAcceleratorFamily2`，通过版本化布局指纹（`layout-fingerprint.json`、`variants/index.json`）跨 `26.x` 内核集合精确匹配虚表偏移与实例大小（`tools/ioaccel-layout/`）。
- Mesa RADV Darwin 后端（`mesa-patches/`）：基于 Mesa 提交 `f5cb8ee032adabef599ae892ec4e42d796b84da9` 的 26 个补丁，编译仅依赖 ACO 编译器的 `libvulkan_radeon.dylib`，支持 `'GRNT'` 传输、`VK_EXT_metal_surface` 窗口 WSI 及 `tools/rdna4/radv-app.sh` 启动封装脚本。
- `GarnetMetal.bundle`（`src/garnet-metal/`，`org.garnet.metal`，`0.1.1` build `43`）与 `libgarnetxlate.dylib`（`third-party/metal2vulkan-rdna4/`）：基于 RADV 派生 `MTLIOAccelDevice`，通过 65 个 `metal2vulkan` 补丁和 `garnetxlate` 将 Metal AIR 着色器转为 SPIR-V，并在非 `WindowServer` 的 OpenGL 进程内提供独立 `GarnetGLDrawable` 表面桥接（不含 `AppleParavirtGPU` 或 `glProgramAir` 编译路径）。
- `Garnet.app` 与 `garnetctl`（`src/garnet-app/`）：纯 Apple Command Line Tools SwiftUI 安装端、命令行工具与只读体检器，内建零第三方依赖的 `KernelCollection` 匹配检验、只读 OpenCore `config.plist` 体检、应用黑名单管理与脱敏诊断导出。

## 安装指南（Release 1 — RX 9060 XT / Navi 44）

> [!IMPORTANT]
> Release 1 仅面向 **x86_64 macOS 26.x 物理机** 及 **AMD Radeon RX 9060 XT（`1002:7590`，Navi 44）**；虚拟机、Apple Silicon 宿主及未验证的 Navi 48 将被安装前预检（Preflight）直接拦截。安装器**绝不会自动挂载、写入或修改你的 EFI 系统分区（ESP）**，所有 OpenCore 修改均由用户手动完成并通过只读体检确认。

### 严格的 Kext 放置位置对照表（OpenCore vs `/Library/Extensions`）

切勿混放 `EFI/OC/Kexts/` 与系统盘 `/Library/Extensions/` AuxKC：

| 组件 | 唯一正确放置位置 | 作用与注意事项 |
| --- | --- | --- |
| `GarnetPCIBarCompat.kext` | **仅放 OpenCore `EFI/OC/Kexts/`** | **16 GiB Full-BAR 默认模式**需在 `Kernel -> Add` 中启用（排在 `Lilu`/`VirtualSMC` 之后）；**256 MiB 小 BAR 回退模式**请禁用或移除。**禁止**放入 `/Library/Extensions/`。 |
| `Garnet.kext` | **仅放系统盘 `/Library/Extensions/`（AuxKC）** | 由 `Garnet.app` / `garnetctl` 安装进系统盘并重建 AuxKC。由于依赖系统 KC 的 `IOGraphicsFamily`，**严禁**放入 OpenCore `EFI/OC/Kexts/`（会导致无法启动）。 |
| `GarnetAccel.kext` | **仅放系统盘 `/Library/Extensions/`（AuxKC）** | 由安装器按宿主 `SystemKernelExtensions.kc` 指纹自动匹配后装入 `/Library/Extensions/` 并构建 AuxKC。**严禁**放入 OpenCore `EFI/OC/Kexts/`。 |

### 第一步：BIOS 设置与 Gatekeeper 放行

1. 在主板 BIOS 中**开启** `Above 4G Decoding` 和 `Re-Size BAR Support`，并**关闭** `CSM`。
2. 下载 `Garnet-<version>-Navi44-macOS26.dmg`，校验 `.sha256` 后打开镜像，将 `Garnet.app` 拖入 `/Applications`（或直接从镜像打开）。
3. 由于 Release 1 使用 ad-hoc 签名（`codesign --sign -`），首次打开需清除隔离属性或右键选择 **打开**：

   ```sh
   xattr -dr com.apple.quarantine /Applications/Garnet.app
   ```

4. 启动 `Garnet.app`（或运行 `/Applications/Garnet.app/Contents/Resources/garnetctl preflight`），查看硬件、macOS `26.x` 内核集合布局匹配结果、`IOPCIFamily` 兼容性推荐（`16 GiB Full-BAR` 默认或 `256 MiB 小 BAR` 回退）以及 SIP 状态。

### 第二步：手动配置 OpenCore 并只读体检

1. 在 **Garnet.app -> OpenCore Assistant** 点击导出（或运行 `garnetctl export-pcibarcompat --out ~/Desktop`），导出 `GarnetPCIBarCompat.kext`。
2. 手动挂载自己的 ESP 分区，按以下要求调整 OpenCore `config.plist`：
   - `Misc -> Security -> SecureBootModel = Disabled`
   - `NVRAM -> Add -> 7C436110-AB2A-4BBB-A880-FE41995C9F82`：设置 `csr-active-config` = `<03080000>`（`0x0803`），并在 `NVRAM -> Delete -> 7C436110-AB2A-4BBB-A880-FE41995C9F82` 中加入 `csr-active-config` 与 `boot-args`。
   - **方案 A — 16 GiB Full-BAR（推荐默认模式）**：
     - 将 `GarnetPCIBarCompat.kext` 放入 `EFI/OC/Kexts/` 并在 `Kernel -> Add` 中启用（顺序在 `Lilu`/`VirtualSMC` 之后，`MinKernel = "25.0.0"`）。
     - `Booter -> Quirks -> ResizeAppleGpuBars = -1`、`UEFI -> Quirks -> ResizeGpuBars = -1`、`Kernel -> Quirks -> IncreasePciBarSize = False`。
     - `boot-args`：`-v keepsyms=1 garnet=1 garnet-interrupts=0 garnet-stage=17 garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 garnet-apps-all=1 amfi=0x80 garnet-full-bar=1`（**不要**加 `npci=0x2000` 或 `-radvesa`）。
   - **方案 B — 256 MiB 小 BAR 回退模式**（仅当预检提示 PFM64 空间不足或 `IOPCIFamily` 未匹配时使用）：
     - 禁用或移除 `GarnetPCIBarCompat.kext`。
     - `ResizeAppleGpuBars = 0`、`ResizeGpuBars = 0`、`IncreasePciBarSize = False`。
     - `boot-args`：`-v keepsyms=1 npci=0x2000 garnet=1 garnet-interrupts=0 garnet-stage=17 garnet-native=1 garnet-metal=1 garnet-metal-disp=1 garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 garnet-apps-all=1 amfi=0x80`（**不要**加 `garnet-full-bar=1` 或 `-radvesa`）。
3. 重启前先做只读核验：将修改后的 `config.plist` 拖入 **Garnet.app -> OpenCore Assistant**，或执行：

   ```sh
   /Applications/Garnet.app/Contents/Resources/garnetctl validate-oc --mode 16g --config /path/to/config.plist
   ```

4. 若预检显示 `SIP SETUP NEEDED`，请先重启一次让 OpenCore 的 `csr-active-config = 03080000` 生效，再继续第三步。

### 第三步：安装或覆盖升级系统盘驱动

1. 在 **Garnet.app -> Install / Uninstall** 确认状态为 `READY` 后点击 **Install / Upgrade Driver…**（或运行 `sudo /Applications/Garnet.app/Contents/Resources/garnetctl install`）。
   - 安装器会自动重验 `SHA256SUMS` 与代码签名，安装 `Garnet.kext` 与匹配的 `GarnetAccel.kext` 至 `/Library/Extensions/`，重建 `/Library/KernelCollections/AuxiliaryKernelExtensions.kc` 并**保留全部已有第三方 Kext**，写入 `GarnetMetal.bundle` 与原生翻译服务；覆盖升级时自动保留 `/Library/Application Support/RDNA4/native/app-denylist.txt`，任意步骤失败均整事务回滚。
   - **可选 Vulkan ICD（默认关闭）**：仅当手动勾选或传入 `--with-vulkan` 时才安装 `/Library/Vulkan/icd.d/radeon_icd.x86_64.json` 与 `/usr/local/bin/radv-app`。
2. 完成后重启进入桌面。

### 第四步：应用黑名单、完整卸载与应急恢复开关

- **应用 Metal 黑名单**：Release 1 默认开启全局应用加速（`garnet-apps-all=1`）并内置 3 次等待超时降级；如需屏蔽特定应用（`WindowServer` 与 `RDNA4M_ALLOW=1` root 工具始终免疫），可将 `.app` 拖入 **Garnet.app -> Status & Denylist** 或运行：

  ```sh
  sudo /Applications/Garnet.app/Contents/Resources/garnetctl denylist add-app /Applications/ProblemApp.app
  ```

- **脱敏诊断导出**：通过 **Garnet.app -> Status & Denylist** 或 `garnetctl export-diag --out ~/Desktop/garnet-diag.tar.gz` 导出已剔除用户名、主目录路径、序列号与 `IOPlatformUUID` 的 `.tar.gz` 排障包。
- **完整卸载**：点击 **Garnet.app -> Install / Uninstall -> Uninstall Garnet…**（或执行 `sudo garnetctl uninstall`，默认保留黑名单，加 `--remove-denylist` 可一并删除），重建 AuxKC 并保留所有第三方驱动；重启前在 OpenCore 中禁用或移除 `GarnetPCIBarCompat.kext` 与 `garnet*` 启动参数。
- **应急启动参数（可在 OpenCore 启动菜单或 `config.plist` 临时追加）**：
  - `garnet=0` — 完全禁用 `Garnet.kext` 与 GPU 初始化
  - `garnet-aux=0` — 禁用 `GarnetAccel.kext` 加速器接入
  - `garnet-rebar-compat=0` — 在不删文件的情况下禁用 `GarnetPCIBarCompat.kext` 16 GiB BAR 补丁
  - `garnet-apps-all=0` — 关闭普通桌面应用 Metal 加速，仅保留系统核心合成器与白名单进程

## 目录结构

| 路径 | 内容说明 |
| --- | --- |
| `src/garnet` | 主驱动 `Garnet.kext`、DCN 4.01 模式设置与光标代码、VM/休眠纯逻辑模块、测试着色器与 ASan/UBSan 宿主测试 |
| `src/garnet-accel` | 辅助内核集合驱动 `GarnetAccel.kext` 与布局守卫测试 |
| `src/garnet-pci-compat` | OpenCore 早期 16 GiB BAR 扩展 `GarnetPCIBarCompat.kext` 与 `IOPCIFamily` 结构化校验测试 |
| `src/garnet-metal` | `GarnetMetal.bundle` Objective-C 实现、纯 C 不变量模块、`spvcache` 元数据与兼容测试 |
| `src/garnet-app` | 纯 CLT Swift `Garnet.app` 安装器、`garnetctl` CLI、只读 KC/OpenCore 体检器与 `GarnetCoreTests` |
| `src/dcn41` | DCN 4.01 寄存器定义与生成的写入白名单（已排除 Navi 44 `ODM_MEM_PWR_CTRL2` 读写锁死地址） |
| `mesa-patches` | 面向 Mesa RADV Darwin 的 26 个补丁及离线运行时/管线/大内存宿主测试 |
| `third-party/metal2vulkan-rdna4` | 65 个 `metal2vulkan` 补丁与 `garnetxlate` Rust crate（LGPL-3.0-or-later） |
| `tools/rdna4` | 成套载荷构建脚本（`build-all.sh`）、`.dmg` 打包器（`build-release-dmg.sh`）、安装与恢复脚本及 `radv-app.sh` |
| `tools/native` | 用户态翻译辅助工具与诊断探针 |
| `tools/dcn41` | DCN 4.01 白名单和寄存器代码生成器及变异测试脚本 |
| `tools/ioaccel-layout` | macOS 内核集合解包、虚表偏移校验与布局指纹导出工具 |
| `tools/fetch-firmware.sh` | 从本地 `linux-firmware` 仓库复制 RDNA 4 所需微码文件 |
| `docs` | [文档索引](docs/README.md)与各子系统技术规格 |

## 依赖准备与构建

仓库不包含第三方二进制组件与上游源码树，构建前需在本地准备好以下依赖：

| 依赖项 | 固定版本 / 来源 | 用途 |
| --- | --- | --- |
| `MacKernelSDK` | Acidanthera `MacKernelSDK` `05094e5e88cec7caedbfb35e8449ed0db94bf95b` | 编译 `Garnet.kext` 与 `GarnetAccel.kext` 时通过 `MKSDK` 指定 |
| `linux-firmware` | 上游 Linux 内核固件仓库 | 提供 Navi 44 / Navi 48 微码，复制至被 git 忽略的 `src/garnet/firmware/` |
| `Mesa` | 提交 `f5cb8ee032adabef599ae892ec4e42d796b84da9` | 应用 `mesa-patches/` 后由 `tools/rdna4/build-radv.sh` 编译（需 `glslangValidator` >= 12.2、`meson`、`ninja`） |
| `metal2vulkan` | 提交 `43c46ac8a24adf1a6e872b8a52c706ec9614fad0` | 应用 `third-party/metal2vulkan-rdna4/` 后由 `tools/rdna4/build-xlate.sh` 编译（需 Rust >= 1.87） |
| 目标 macOS KC 转储 | `BootKernelExtensions.kc` / `SystemKernelExtensions.kc`（25G332） | 供 `tools/ioaccel-layout/` 生成 `GarnetAccel` 布局头文件（`C64_WORK` / `C64_LAYOUT_OUT`） |

获取 Navi 44 固件并编译 `Garnet.kext`（在 macOS 上编译，或在 Linux 上配合 `clang` 与支持 `-kext` 的 cctools `ld64` 交叉编译内核态目标）：

```sh
CHIPS=navi44 bash tools/fetch-firmware.sh /path/to/linux-firmware
make -C src/garnet CHIPS=navi44 MKSDK=/path/to/MacKernelSDK all
```

如需在 macOS 上生成包含 `Garnet.kext`、`GarnetAccel.kext`、`GarnetMetal.bundle`、`libvulkan_radeon.dylib`、`libgarnetxlate.dylib` 与用户态工具的签名载荷，请按 [BUILDING.md](BUILDING.md) 和 [tools/rdna4/README.md](tools/rdna4/README.md) 准备好变量后运行 `tools/rdna4/build-all.sh OUT_DIR`。

## 宿主测试

离线宿主测试在无物理 RDNA 4 显卡的 CPU 环境下验证状态机、ABI 约束、补丁应用与脚本逻辑；宿主测试通过不代表 macOS 真机链接或 GPU 执行已通过。

```sh
make -C src/garnet/tests all
make -C src/garnet-pci-compat test
python3 tools/dcn41/build.py regs
python3 tools/dcn41/build.py unit
python3 tools/dcn41/build.py cxx
python3 tools/dcn41/build.py allowcontrol
make -C src/garnet-accel/tests BRING_ROOT="$(pwd)" all
make -C src/garnet-app test
python3 tools/rdna4/tests/build-identity-test.py -v
bash tools/rdna4/tests/native-activation-test.sh
bash tools/rdna4/tests/radv-app-test.sh
bash tools/rdna4/tests/collect-installed-test.sh
```

关于 Metal、OpenGL 表面桥接以及 Mesa RADV 模拟后端的离线测试说明，见 [src/garnet-metal/TESTING.md](src/garnet-metal/TESTING.md) 与 [mesa-patches/README.txt](mesa-patches/README.txt)。

## 文档索引

- [BUILDING.md](BUILDING.md)：构建环境、Linux 交叉编译、固件抽取与成套载荷打包说明
- [docs/README.md](docs/README.md)：子系统设计文档与硬件操作手册索引
- [GLOSSARY.md](GLOSSARY.md)：核心组件命名、芯片命名规则与术语表
- [CHANGES.md](CHANGES.md)：自 Cobalt64 上游基线至独立 Garnet 架构的演进记录
- [tools/rdna4/README.md](tools/rdna4/README.md)：载荷构建、一致性校验与安装/卸载脚本说明

## 致谢与许可证

本仓库主体代码采用 [MIT 许可证](LICENSE)；[third-party/metal2vulkan-rdna4](third-party/metal2vulkan-rdna4/NOTICE) 作为 `metal2vulkan` 的衍生作品单独适用 GNU Lesser General Public License v3.0 or later（见 [COPYING](third-party/metal2vulkan-rdna4/COPYING)）。`src/dcn41/` 下的寄存器头文件沿用 AMD MIT 开源寄存器头许可。Git 源码仓库本身不重分发任何 AMD 二进制固件文件；正式发布的 Release 1 载荷（`RDNA4_RELEASE=1`）与 `.dmg` 安装镜像随同上游 `LICENSE.amdgpu` 打包分发 Navi 44 微码。

本项目在以下开源工作的基础上完成并致谢：

- [Cobalt64](https://github.com/Almosst-DEV/Cobalt64)（MIT）：提供核心 RDNA 4 启动栈、`'GRNT'` 用户客户端、Mesa RADV Darwin 后端基础、`GarnetMetal` bundle 架构与 `GarnetAccel` 布局工具。
- [RX4Darwin/AMDRadeonRDNA4.kext](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext)（BSD 3-Clause，Copyright (c) 2026 Sunneva N. Mariu，`next` @ `dded456d`）：吸收其非 Lilu 相关成果，包括 Navi 44 `ODM_MEM_PWR_CTRL2` 读写危险排除、CoreDisplay `AMD*` 帧缓冲类名前缀识别、DCN 4.01 128x128 硬件光标编程、RADV `VK_EXT_metal_surface` WSI 回退与 `radv-app.sh` 启动封装、VMID 精确 TLB 应答/隔离/稀疏页表回滚、无 Lilu S3 休眠唤醒与 DP DDC-to-AUX 恢复，以及 `GarnetGLDrawable` OpenGL-on-Metal 表面桥接（`supportsUnalignedVertexFetch`、`commitAndHold`、`setLineWidth:`）。
- [RDNA4FB](https://github.com/somestupidgirl/RDNA4FB)（BSD 3-Clause，Copyright (c) 2026 Sunneva N. Mariu）：提供片上 IP 发现（`src/garnet/src/ipdiscovery.*`）、BAR5 映射与间接显存访问实现参考。
- [mac-amdgpu](https://github.com/lemonade-sdk/mac-amdgpu)（MIT，Copyright (c) 2026 Geramy Loveless）：提供 RDNA 4 IP 模块初始化顺序参考。
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa)（MIT）、[metal2vulkan](https://github.com/steelbrain/metal2vulkan)（LGPL-3.0-or-later）及 [NullMoth Systems](https://github.com/nullmoth/nvidia-macos-driver)（LGPL-3.0-or-later 编译器补丁适配，见 [third-party/metal2vulkan-rdna4/NOTICE](third-party/metal2vulkan-rdna4/NOTICE)）。

完整第三方许可条款及逐文件溯源见 [third-party/THIRD-PARTY-LICENSES.txt](third-party/THIRD-PARTY-LICENSES.txt)。本项目为独立开源驱动实验，与 AMD、Apple、Cobalt64 或 RX4Darwin 均无从属或官方背书关系。
