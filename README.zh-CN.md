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

共享代码、启动参数与通用接口统一采用 `RDNA4` 和独立 `Garnet` 命名；仅在存在真实硬件、固件或 PCI ID 差异处使用 `Navi 44` 与 `Navi 48`。`GarnetAccel.kext` 的内核集合布局目前面向 x86_64 macOS 26.7.2（`25G332`）。

## 架构概览

Garnet 将内核初始化、macOS 加速器接口接入、Vulkan 后端与用户态 Metal/OpenGL 支持拆分为四个层级：

```text
  Metal / OpenGL / Vulkan 应用
        │                │
        ▼                ▼
  GarnetMetal.bundle   libvulkan_radeon.dylib（Mesa RADV Darwin + VK_EXT_metal_surface）
  （org.garnet.metal，   ▲
   libgarnetxlate.dylib）│
        │                │ IOConnectCallMethod（'GRNT' ABI）
        ▼                │
  GarnetAccel.kext（org.garnet.accel，Auxiliary Kernel Collection）
        │  通过 GarnetMetalNub 发布 GarnetAccelerator 与 GarnetFramebuffer
        ▼
  Garnet.kext（org.garnet.driver，内置 DCN 4.01 AMDGarnetPrimaryFramebuffer）
```

- `Garnet.kext`（`src/garnet/`，`org.garnet.driver`，`0.0.673`）：负责 PCIe BAR 映射、片上 IP 发现、PSP/SMU/GMC/GFX/SDMA/MES 固件加载与引擎初始化、带精确 TLB 应答和隔离区的 VMID 池及稀疏页表回滚、无 Lilu 优先级 S3 休眠/唤醒与 DP DDC-to-AUX 恢复、内置 `AMDGarnetPrimaryFramebuffer` 模式设置与 128x128 硬件光标，以及 `'GRNT'` 原生用户客户端（`IOAccelGarnetNativeClient`）。
- `GarnetAccel.kext`（`src/garnet-accel/`，`org.garnet.accel`，`0.0.8`）：在 Auxiliary Kernel Collection 中对接苹果 `IOAcceleratorFamily2`，虚表和结构体偏移通过 `tools/ioaccel-layout/` 从目标 macOS 内核集合自动抽取生成。
- Mesa RADV Darwin 后端（`mesa-patches/`）：基于 Mesa 提交 `f5cb8ee032adabef599ae892ec4e42d796b84da9` 的 26 个补丁，编译仅依赖 ACO 编译器的 `libvulkan_radeon.dylib`，支持 `'GRNT'` 传输、`VK_EXT_metal_surface` 窗口 WSI 及 `tools/rdna4/radv-app.sh` 启动封装脚本。
- `GarnetMetal.bundle`（`src/garnet-metal/`，`org.garnet.metal`，`0.1.1` build `43`）与 `libgarnetxlate.dylib`（`third-party/metal2vulkan-rdna4/`）：基于 RADV 派生 `MTLIOAccelDevice`，通过 65 个 `metal2vulkan` 补丁和 `garnetxlate` 将 Metal AIR 着色器转为 SPIR-V，并在非 `WindowServer` 的 OpenGL 进程内提供独立 `GarnetGLDrawable` 表面桥接（不含 `AppleParavirtGPU` 或 `glProgramAir` 编译路径）。

## 目录结构

| 路径 | 内容说明 |
| --- | --- |
| `src/garnet` | 主驱动 `Garnet.kext`、DCN 4.01 模式设置与光标代码、VM/休眠纯逻辑模块、测试着色器与 ASan/UBSan 宿主测试 |
| `src/garnet-accel` | 辅助内核集合驱动 `GarnetAccel.kext` 与布局守卫测试 |
| `src/garnet-metal` | `GarnetMetal.bundle` Objective-C 实现、纯 C 不变量模块、`spvcache` 元数据与兼容测试 |
| `src/dcn41` | DCN 4.01 寄存器定义与生成的写入白名单（已排除 Navi 44 `ODM_MEM_PWR_CTRL2` 读写锁死地址） |
| `mesa-patches` | 面向 Mesa RADV Darwin 的 26 个补丁及离线运行时/管线/大内存宿主测试 |
| `third-party/metal2vulkan-rdna4` | 65 个 `metal2vulkan` 补丁与 `garnetxlate` Rust crate（LGPL-3.0-or-later） |
| `tools/rdna4` | 成套载荷构建脚本（`build-all.sh`）、安装与恢复脚本、RADV/翻译器构建脚本及 `radv-app.sh` |
| `tools/native` | 用户态翻译辅助工具与诊断探针 |
| `tools/dcn41` | DCN 4.01 白名单和寄存器代码生成器及变异测试脚本 |
| `tools/ioaccel-layout` | macOS 内核集合解包与虚表偏移校验工具 |
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
python3 tools/dcn41/build.py regs
python3 tools/dcn41/build.py unit
python3 tools/dcn41/build.py cxx
python3 tools/dcn41/build.py allowcontrol
make -C src/garnet-accel/tests BRING_ROOT="$(pwd)" all
python3 tools/rdna4/tests/build-identity-test.py -v
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

本仓库主体代码采用 [MIT 许可证](LICENSE)；[third-party/metal2vulkan-rdna4](third-party/metal2vulkan-rdna4/NOTICE) 作为 `metal2vulkan` 的衍生作品单独适用 GNU Lesser General Public License v3.0 or later（见 [COPYING](third-party/metal2vulkan-rdna4/COPYING)）。`src/dcn41/` 下的寄存器头文件沿用 AMD MIT 开源寄存器头许可。仓库不重新分发任何 AMD 二进制固件文件。

本项目在以下开源工作的基础上完成并致谢：

- [Cobalt64](https://github.com/Almosst-DEV/Cobalt64)（MIT）：提供核心 RDNA 4 启动栈、`'GRNT'` 用户客户端、Mesa RADV Darwin 后端基础、`GarnetMetal` bundle 架构与 `GarnetAccel` 布局工具。
- [RX4Darwin/AMDRadeonRDNA4.kext](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext)（BSD 3-Clause，Copyright (c) 2026 Sunneva N. Mariu，`next` @ `dded456d`）：吸收其非 Lilu 相关成果，包括 Navi 44 `ODM_MEM_PWR_CTRL2` 读写危险排除、CoreDisplay `AMD*` 帧缓冲类名前缀识别、DCN 4.01 128x128 硬件光标编程、RADV `VK_EXT_metal_surface` WSI 回退与 `radv-app.sh` 启动封装、VMID 精确 TLB 应答/隔离/稀疏页表回滚、无 Lilu S3 休眠唤醒与 DP DDC-to-AUX 恢复，以及 `GarnetGLDrawable` OpenGL-on-Metal 表面桥接（`supportsUnalignedVertexFetch`、`commitAndHold`、`setLineWidth:`）。
- [RDNA4FB](https://github.com/somestupidgirl/RDNA4FB)（BSD 3-Clause，Copyright (c) 2026 Sunneva N. Mariu）：提供片上 IP 发现（`src/garnet/src/ipdiscovery.*`）、BAR5 映射与间接显存访问实现参考。
- [mac-amdgpu](https://github.com/lemonade-sdk/mac-amdgpu)（MIT，Copyright (c) 2026 Geramy Loveless）：提供 RDNA 4 IP 模块初始化顺序参考。
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa)（MIT）、[metal2vulkan](https://github.com/steelbrain/metal2vulkan)（LGPL-3.0-or-later）及 [NullMoth Systems](https://github.com/nullmoth/nvidia-macos-driver)（LGPL-3.0-or-later 编译器补丁适配，见 [third-party/metal2vulkan-rdna4/NOTICE](third-party/metal2vulkan-rdna4/NOTICE)）。

完整第三方许可条款及逐文件溯源见 [third-party/THIRD-PARTY-LICENSES.txt](third-party/THIRD-PARTY-LICENSES.txt)。本项目为独立开源驱动实验，与 AMD、Apple、Cobalt64 或 RX4Darwin 均无从属或官方背书关系。
