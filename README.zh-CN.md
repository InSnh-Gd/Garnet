# Garnet

[English](README.md) | 简体中文

Garnet 是面向 x86_64 OpenCore macOS 的开源 AMD RDNA 4 显卡与显示驱动栈。它在 [Cobalt64](https://github.com/Almosst-DEV/Cobalt64) 的基础上重构为独立四层架构，移除了 `Lilu.kext` 与外部显示插件依赖，并吸收了 [RX4Darwin/AMDRadeonRDNA4.kext](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext) 在显示、硬件光标、WSI、VMID、休眠唤醒与 OpenGL 表面桥接方面的非 Lilu 实现。

> [!WARNING]
> `Garnet.kext` 会直接读写 PCIe BAR、PSP、SMU、RLC、SDMA、CP、MES 与 DCN 显示引擎寄存器。版本、显卡或系统构建不匹配都可能导致死机、重启或显示异常。请仅在可恢复的专用测试机上自担风险运行，详见 [LICENSE](LICENSE)。

## 支持的硬件

| 核心代号 | 对应型号 | PCI 设备 ID | LLVM / IP 版本 | 当前状态 |
| --- | --- | --- | --- | --- |
| Navi 44 | Radeon RX 9060 XT **16GB** | `1002:7590` | `gfx1200`、`GC 12.0.0`、`MP0 14.0.2`、`SDMA 7.0.0` | 主要开发与实机已验证目标 |
| Navi 44 | Radeon RX 9060 XT **8GB** | `1002:7590` | `gfx1200`、`GC 12.0.0`、`MP0 14.0.2`、`SDMA 7.0.0` | 代码已就绪，待物理机实机验证 |
| Navi 48 | Radeon RX 9070 / RX 9070 XT / AI PRO R9700 | `1002:7550`、`1002:7551` | `gfx1201`、`GC 12.0.1`、`MP0 14.0.3`、`SDMA 7.0.1` | 保留来自 Cobalt64 的分芯片表、固件路径与 Mesa 条目，未在物理机验证 |

驱动包名、启动参数与命令行工具统一使用 `Garnet`（`garnet-*`）命名，跨芯片共享的 GPU 族代码使用 `RDNA4`；仅在硬件规格、微码或 PCI ID 存在差异处区分 `Navi 44` 与 `Navi 48`。`GarnetAccel.kext` 通过布局指纹匹配 x86_64 macOS `26.x` 内核集合（以 `25G332` 为验证基线）；v0.1.1 发布版安装包仅面向物理机 Navi 44（`1002:7590`）。

## v0.1.1 变更与 v0.2.0 路线图

### v0.1.1 变更亮点

- **中断、休眠与 4K@160Hz 桌面**：默认开启 MSI EOP 中断（`garnet-interrupts=1`）、DCN 4.01 OTG `VSTARTUP` 主帧 VBLANK 与 `VUPDATE_NO_LOCK` 自愈、强制 DP/DSC 重训练的硬件 DPMS 休眠唤醒、跨 macOS 26.x 动态 AGDC 解析、4K@160Hz WindowServer 提交流水线，以及 GPL 快速链接与冷预热。
- **8 GB Navi 44、异步计算与显存管理**：补齐 RX 9060 XT 8 GB 显存几何与无 Full-BAR 时的自动 Small-BAR 回退、专用 MEC 异步计算队列（`ctx.kcq`）、带事务回滚的 2 MiB `PDB0` 大页，以及默认开启的 GFX12 `PTE.DCC`（`garnet-dcc=0` 关闭开关），配合 GPU `CP_DMA` 清零与 GL2 失效保护。
- **原生 Vulkan 光追**（RX 9060 XT 实测通过）：`4,788.0 MRays/s`（10 万面）、`1,710.6 MRays/s`（100 万面）；在开启默认 GFX12 DCC 的 4K@160Hz 桌面上，Sascha `raytracingbasic` 跑出 `398.881 FPS`、`rayquery` 跑出 `374.329 FPS`；支持 `VK_KHR_ray_tracing_position_fetch`。
- **Metal 3 光追与 Bindless**：实现 `intersection_query`、`@air.intersect`、IFT ABI v2、无状态嵌套 VFT、无编码器 Bindless、`primitiveDataBuffer`、`MTLHeap` 加速结构与异步计算队列 AS 构建。实机通过 `1280x720` 交互路径追踪器（默认开启 DCC 下平均 `103.93 FPS`、峰值 `136.8 FPS`，约 `630.4 MRays/s`）、未修改的 Apple 官方 Sample 1 与 Sample 2，以及 4 路并发 RT/Metal/Vulkan + 2 GiB 显存 / 4x4K DCC 压测（`145,148` 条提交序列零故障、零卡死）。
- **离线 Blender Cycles 编译**：`63/63` 个 Blender 4.2.23 Cycles（`CYCLES_METALRT=1`）内核通过 SPIR-V 校验，RADV 补丁 `0032` 将 ACO spill 内存压低约 7 到 14 倍以避免 OOM。完整场景渲染计划在 v0.2.0 完成。
- **日志、打包与升级清理**：发布版 `os_log` 日志分级、修复 `rdna4_sb_*` 沙盒预检、扩展 `Garnet.app` 诊断导出，并在升级时自动清理 `0.1.0` 旧残留。完整记录见 [CHANGES.md](CHANGES.md)。

### v0.2.0 计划

- **VCN 5.0.0 硬件视频编解码**：内核初始化 VCN 5.0（含动态功耗门控与 MMHUB 每会话 VMID），开启 Mesa RADV 原生 Vulkan Video（`H.264`、`HEVC`、`AV1`、`VP9` 解码，以及 `H.264`、`HEVC`、`AV1` 编码与 EFC RGB 转换），并提供独立 MIT 实现的 VideoToolbox 桥接（`libgarnetvt.dylib`，零 CPU 拷贝 `IOSurface` 导入与按探测门控的 `MediaExtension` 解码，不修改 Seatbelt `.sb` 文件）。
- **沙盒浏览器与桌面应用兼容性**：通过 WebKit 安全选择器别名、延迟入沙盒处理、两级进程内着色器缓存回退与可选 APFS 根快照部署，解决 Safari（`com.apple.WebKit.GPU`）、Chrome 与 Electron 窗口空白问题；修复冷缓存下 WindowServer 回退崩溃、IORegistry `"model"` 属性格式（修复 Geekbench 7 启动）、非 heap IOSurface 并发队列共享及启动阶段剩余的 MES 初始化故障。
- **MetalFX 超分辨率与插帧**：基于 AMD FidelityFX FSR 3.1 计算着色器实现 `MTLFXSpatialScaler` 与 `MTLFXTemporalScaler`（空间缩放保留原生回退路径），让调用 MetalFX 的游戏与基准测试可在 RDNA 4 上运行。
- **高级 Metal 3 光追与 Blender Cycles**：扩展曲线图元、运动模糊、多级实例化与间接加速结构构建，并完成 Blender 4.2 LTS Cycles（`CYCLES_METALRT=1`）完整场景实机渲染。
- **Metal 4 核心 API 基础**：在现有 RADV 后端之上引入 Metal 4 核心接口（`MTL4CommandQueue`、`MTL4CommandBuffer`、`MTL4CommandAllocator`、统一参数表及编译/归档入口）。
- **CPU 开销优化、DisplayPort 热插拔、功耗管理与更多显卡验证**：用增量脏区间追踪或 BDA 间接表替代逐帧全量参数缓冲区扫描，引入磁盘持久化 ACO 管线缓存并消除冷编译卡顿，实现 DisplayPort 热插拔（HPD）链路重训练而无需重启，协同 VCN 功耗门控完成 SMU DPM / `GFXOFF` 待机功耗调优，并补齐 RX 9060 XT 8 GB 与 Navi 48（`RX 9070` / `RX 9070 XT`）的物理机验证。

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

- `GarnetPCIBarCompat.kext`（`src/garnet-pci-compat/`）：面向 Full-BAR（8GB 卡 8 GiB / 16GB 卡 16 GiB）模式的可选早期 OpenCore 驱动。在跨 `26.x` 对 `com.apple.iokit.IOPCIFamily` 完成 `exact-uuid`（`25G332`）或结构校验后开启大内存 BAR。
- `GarnetRecover.efi`（`src/garnet-recover-efi/`）：独立 OpenCore 启动菜单恢复工具（`EFI/OC/Tools/GarnetRecover.efi`），不写 ESP 磁盘，仅通过 UEFI NVRAM 与系统内自包含守护进程 `com.garnet.recovery` 通信（支持 `[1]` 单次安全启动与 `[2]` 安全启动并自动卸载）。
- 可选 `AMFIAnchor` 信任锚路径（`GarnetAnchorInjector.kext`）：构建时提供 `GARNET_SIGN_CA_DIR` 会使用专用代码签名 CA 签发用户态 Mach-O 并生成无代码 Schema-1 注入 kext（`org.garnet.code-signing`），配合上游 BSD-3-Clause 的 `b00t0x/AMFIAnchor`（加载顺序 `Lilu.kext -> AMFIAnchor.kext -> GarnetAnchorInjector.kext`）即可去掉 `amfi=0x80` 保持 AMFI 开启；`Garnet.kext` 与 `GarnetAccel.kext` 仍保持零 `Lilu.kext` 依赖。
- `Garnet.kext`（`src/garnet/`）：负责 PCIe BAR 映射、片上 IP 发现、固件加载、VMID 与稀疏页表管理、S3 休眠唤醒与 DP DDC/AUX 恢复、`AMDGarnetPrimaryFramebuffer` 模式设置与 128x128 硬件光标，以及 `'GRNT'` 用户客户端（`IOAccelGarnetNativeClient`）。
- `GarnetAccel.kext`（`src/garnet-accel/`）：在 Auxiliary Kernel Collection 中对接 `IOAcceleratorFamily2`，通过版本化布局指纹匹配虚表偏移与实例大小（`tools/ioaccel-layout/`）。
- Mesa RADV Darwin（`mesa-patches/`）：基于 Mesa 提交 `f5cb8ee032adabef599ae892ec4e42d796b84da9` 的补丁系列，编译仅启用 ACO 的 `libvulkan_radeon.dylib`，支持 `'GRNT'` 传输、`VK_EXT_metal_surface` 及 `tools/rdna4/radv-app.sh`。
- `GarnetMetal.bundle`（`src/garnet-metal/`）与 `libgarnetxlate.dylib`（`third-party/metal2vulkan-rdna4/`）：基于 RADV 派生 `MTLIOAccelDevice`，通过 `metal2vulkan` 补丁和 `garnetxlate` 将 Metal AIR 转为 SPIR-V，并在非 `WindowServer` 的 OpenGL 进程内提供 `GarnetGLDrawable` 表面桥接。
- `Garnet.app` 与 `garnetctl`（`src/garnet-app/`）：SwiftUI 安装管理应用与命令行工具，包含预检、OpenCore 自动配置与校验、应用黑名单及脱敏诊断导出。

## 安装指南（RX 9060 XT / Navi 44）

> [!IMPORTANT]
> v0.1.1 仅支持 **x86_64 macOS 26.x 物理机** 与 **AMD Radeon RX 9060 XT（`1002:7590`，Navi 44）**，虚拟机、Apple Silicon 和 Navi 48 会被预检直接拦截。安装器不会自动挂载或修改你的 EFI 分区。RX 9060 XT **16GB** 已完成实机验证；**8GB** 版本代码已就绪、尚待物理机实机验证（详见 [docs/rdna4/full-bar-aperture.md](docs/rdna4/full-bar-aperture.md)）。

### 第一步：BIOS 设置与启动应用

1. 在主板 BIOS 中开启 `Above 4G Decoding` 和 `Re-Size BAR Support`，关闭 `CSM`。
2. 从 Releases 下载 `Garnet-<version>-Navi44-macOS26.dmg`，把 `Garnet.app` 拖入 `/Applications`。
3. 清除隔离属性（或右键选择 **打开**）后启动 `Garnet.app`：

   ```sh
   xattr -dr com.apple.quarantine /Applications/Garnet.app
   ```

### 第二步：自动配置 OpenCore

1. 手动挂载你的 EFI 分区。
2. 打开 **Garnet.app -> OpenCore Assistant**，按主界面建议选择 BAR 模式（`Full-BAR (8/16 GiB)` 或 `256 MiB Small-BAR`）与 AMFI 模式（载荷含 `GARNET_SIGN_CA_DIR` 签名且 `EFI/OC/Kexts/` 已有 `Lilu.kext` 与 `AMFIAnchor.kext` 时可选 `AMFIAnchor (AMFI Enabled)`，否则使用 `Legacy (amfi=0x80)`）。点击 **Configure OpenCore…** 并选中已挂载的 `EFI` 目录或 `EFI/OC/config.plist`（命令行等价操作：`/Applications/Garnet.app/Contents/Resources/garnetctl configure-oc --mode full --amfi legacy --config /Volumes/EFI/EFI/OC/config.plist`，启用信任锚时改用 `--amfi anchor`）。程序会保存时间戳备份及一次性安装前基线 `config.plist.garnet-pre-garnet`，写入 `GarnetPCIBarCompat.kext`（Full-BAR）与 `EFI/OC/Tools/GarnetRecover.efi`，在 `anchor` 模式下自动按 `Lilu.kext -> AMFIAnchor.kext -> GarnetAnchorInjector.kext`（并位于 `AMFIPass.kext` 之前）排序且移除 `amfi=0x80`，完成后立即校验。
3. 若预检显示 `SIP SETUP NEEDED`，先重启一次让新的 `csr-active-config` 生效。

### 第三步：安装并重启

1. 在 **Garnet.app -> Install / Uninstall** 确认状态为 `READY` 后，点击 **Install / Upgrade Driver…**（或执行 `sudo /Applications/Garnet.app/Contents/Resources/garnetctl install`；需要 Vulkan 时再勾选 Vulkan ICD 或加 `--with-vulkan`）。从 `v0.1.0` 升级会自动迁移 `app-denylist.txt` 到 `/Library/Application Support/Garnet/native` 并清理旧版运行时目录。
2. 重启进入桌面。

### 日常管理与应急恢复

- **应用 Metal 黑名单**：将 `.app` 拖入 **Garnet.app -> Status & Denylist**，或执行 `sudo /Applications/Garnet.app/Contents/Resources/garnetctl denylist add-app /Applications/ProblemApp.app`。
- **导出脱敏诊断**：在 **Status & Denylist** 导出，或执行 `garnetctl export-diag --out ~/Desktop/garnet-diag.tar.gz`。
- **OpenCore 启动菜单一键恢复（`GarnetRecover.efi`）**：在 OpenCore 引导菜单选择 **Garnet: Safe Boot & Recovery**（全程不写 ESP 磁盘）：
  - `[1] Safe Boot Once`：仅向 UEFI NVRAM 追加 `garnet=0 garnet-aux=0 garnet-rebar-compat=0`，保留已安装驱动以便临时进入安全桌面排查；
  - `[2] Safe Boot & Uninstall Garnet`：在安全启动参数基础上写入 NVRAM 卸载标记，随后选入 macOS 后由系统盘自包含的 `com.garnet.recovery` 先清除标记、执行 `garnetctl uninstall`、写入 `/Library/Logs/Garnet/recovery-complete.txt`，并停留在安全桌面（不自动重启）。
- **一键恢复 OpenCore 配置（`restore-oc`）**：正常卸载或自动恢复卸载后，手动挂载 EFI 分区，在 **Garnet.app -> OpenCore Assistant** 点击 **Restore OpenCore…**，或执行 `/Applications/Garnet.app/Contents/Resources/garnetctl restore-oc --config /Volumes/EFI/EFI/OC/config.plist`（无基线备份时可加 `--strip-only`）。该操作会恢复 `config.plist.garnet-pre-garnet`、删除已校验的 Garnet EFI 文件，且绝不删除 `Lilu.kext` 或 `AMFIAnchor.kext`。
- **手动完整卸载**：在 **Garnet.app -> Install / Uninstall** 点击 **Uninstall Garnet…**（或执行 `sudo garnetctl uninstall`），再挂载 EFI 执行 `restore-oc` 后重启。
- **应急启动参数**（可在 OpenCore 启动菜单临时追加）：`garnet=0` 禁用 `Garnet.kext`，`garnet-aux=0` 禁用 `GarnetAccel.kext`，`garnet-rebar-compat=0` 禁用 Full-BAR 补丁，`garnet-apps-all=0` 关闭普通桌面应用 Metal 加速。

## 目录结构

| 路径 | 内容说明 |
| --- | --- |
| `src/garnet` | 主驱动 `Garnet.kext`、DCN 4.01 模式设置与光标代码、VM/休眠纯逻辑模块、着色器及宿主测试 |
| `src/garnet-accel` | 辅助内核集合驱动 `GarnetAccel.kext` 与布局守卫测试 |
| `src/garnet-pci-compat` | OpenCore 早期 Full-BAR 扩展 `GarnetPCIBarCompat.kext` 与 `IOPCIFamily` 校验测试 |
| `src/garnet-recover-efi` | 独立 x86_64 UEFI OpenCore 引导恢复工具 `GarnetRecover.efi` 与纯 C 宿主测试 |
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
| 目标 macOS KC 转储 | `BootKernelExtensions.kc` / `SystemKernelExtensions.kc`（macOS 26.x） | 供 `tools/ioaccel-layout/` 生成 `GarnetAccel` 布局头文件（`GARNET_KC_WORK` / `GARNET_LAYOUT_OUT`） |

获取 Navi 44 固件并编译 `Garnet.kext`（在 macOS 上编译，或在 Linux 上配合 `clang` 与支持 `-kext` 的 cctools `ld64` 交叉编译内核态目标）：

```sh
CHIPS=navi44 bash tools/fetch-firmware.sh /path/to/linux-firmware
make -C src/garnet CHIPS=navi44 MKSDK=/path/to/MacKernelSDK all
```

如需在 macOS 上生成完整的匹配签名载荷，按 [BUILDING.md](BUILDING.md) 和 [tools/rdna4/README.md](tools/rdna4/README.md) 准备好环境变量后运行 `tools/rdna4/build-all.sh OUT_DIR`。如需启用 `AMFIAnchor` 信任锚签名（免 `amfi=0x80`），先在仓库外通过 `bash tools/rdna4/make-signing-ca.sh ~/garnet-signing-ca` 生成本地代码签名 CA，并在运行 `tools/rdna4/build-all.sh` 时传入 `GARNET_SIGN_CA_DIR=~/garnet-signing-ca`（以及可选的 `GARNET_AMFIANCHOR_KEXT=/path/to/AMFIAnchor.kext`，需要 `rcodesign`）；未设置 `GARNET_SIGN_CA_DIR` 时保持默认 ad-hoc 签名（`amfi=0x80`）。

## 文档索引

- [BUILDING.md](BUILDING.md)：构建环境、Linux 交叉编译、固件抽取与成套载荷打包说明
- [docs/README.md](docs/README.md)：子系统技术规格与 Metal 能力矩阵索引
- [GLOSSARY.md](GLOSSARY.md)：核心组件命名、芯片命名规则与术语表
- [CHANGES.md](CHANGES.md)：自 Cobalt64 上游基线至独立 Garnet 架构的演进记录
- [tools/rdna4/README.md](tools/rdna4/README.md)：载荷构建、一致性校验与安装/卸载脚本说明

## 致谢与许可证

本仓库主体代码采用 [MIT 许可证](LICENSE)，[third-party/metal2vulkan-rdna4](third-party/metal2vulkan-rdna4/NOTICE) 作为 `metal2vulkan` 的衍生作品适用 GNU Lesser General Public License v3.0 or later（[COPYING](third-party/metal2vulkan-rdna4/COPYING)）。`src/dcn41/` 下的寄存器头文件沿用 AMD MIT 开源寄存器头许可。Git 源码仓库不分发 AMD 二进制固件文件；正式发布载荷（`RDNA4_RELEASE=1`）与 `.dmg` 安装镜像随同上游 `LICENSE.amdgpu` 打包分发 Navi 44 微码。

本项目基于以下开源工作完成并致谢：

- [Cobalt64](https://github.com/Almosst-DEV/Cobalt64)（MIT）：核心 RDNA 4 启动栈、`'GRNT'` 用户客户端、Mesa RADV Darwin 后端基础、`GarnetMetal` bundle 设计与 `GarnetAccel` 布局工具。
- [RX4Darwin/AMDRadeonRDNA4.kext](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext)（BSD 3-Clause，Copyright (c) 2026 Sunneva N. Mariu，`next` @ `dded456d`）：非 Lilu 相关组件，包括 Navi 44 `ODM_MEM_PWR_CTRL2` 读写危险排除、CoreDisplay `AMD*` 帧缓冲类名前缀识别、DCN 4.01 128x128 硬件光标编程、RADV `VK_EXT_metal_surface` WSI 回退与 `radv-app.sh` 启动封装、VMID 精确 TLB 应答/隔离/稀疏页表回滚、S3 休眠唤醒与 DP DDC-to-AUX 恢复，以及 `GarnetGLDrawable` OpenGL-on-Metal 表面桥接（`supportsUnalignedVertexFetch`、`commitAndHold`、`setLineWidth:`）。
- [RDNA4FB](https://github.com/somestupidgirl/RDNA4FB)（BSD 3-Clause，Copyright (c) 2026 Sunneva N. Mariu）：片上 IP 发现（`src/garnet/src/ipdiscovery.*`）、BAR5 映射与间接显存访问实现参考。
- [mac-amdgpu](https://github.com/lemonade-sdk/mac-amdgpu)（MIT，Copyright (c) 2026 Geramy Loveless）：RDNA 4 IP 模块初始化顺序参考。
- [AMFIAnchor](https://github.com/b00t0x/AMFIAnchor)（BSD 3-Clause，Copyright (c) 2026 b00t0x）：可选 OpenCore 信任锚内核扩展接口（`SchemaVersion = 1` 无代码注入契约），用于在无需 `amfi=0x80` 的前提下保持 macOS AMFI 启用。
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa)（MIT）与 [metal2vulkan](https://github.com/steelbrain/metal2vulkan)（LGPL-3.0-or-later，衍生补丁说明见 [third-party/metal2vulkan-rdna4/NOTICE](third-party/metal2vulkan-rdna4/NOTICE)）。

完整第三方许可条款及逐文件溯源见 [third-party/THIRD-PARTY-LICENSES.txt](third-party/THIRD-PARTY-LICENSES.txt)。本项目为独立开源驱动实验，与 AMD、Apple、Cobalt64 或 RX4Darwin 均无从属或官方背书关系。
