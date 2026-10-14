# 泰山派 3.1 寸 MIPI 屏幕接入记录

## 目标

为泰山派 RK3566 与立创配套的 3.1 寸 MIPI 屏幕扩展板启用显示，并记录每次检查和配置操作。显示画面后，可再用图片验证 DRM/KMS 显示链路。

## 当前结论

**2026-10-13 最新结果：用户重新拔插 MIPI 排线后，确认实体屏幕已有显示。** 随后通过 ADB 只读核对，当前运行 `6.1.141 #4`，boot 回读哈希为 `43475c961f8b7da60829ce41e6cacce72d4a089236b5039520ed4cefa132a062`，与此前恢复的官方参数基线一致；DSI-1 为 480×800、60 Hz、实际像素时钟 27 MHz，背光亮度 100，机器人和 LightDM 服务 active，ROS 2 Humble 环境文件存在。重新插拔前后本轮没有刷写或修改显示配置。该结果强烈指向 MIPI 排线连接／接触问题，但尚未区分插入深度、接点接触或排线本体等具体原因。没有全烧录 Buildroot，也未替换 U-Boot/DDR 固件或 Ubuntu 用户空间。后续重点转为触摸和显示稳定性确认。

用户反馈首次部署后实体屏幕仍为黑屏。此前取得的 XWD/PNG 只是 Xorg framebuffer 截图，只能证明软件合成了桌面画面，不能证明 MIPI 面板实际点亮；此前“显示验证成功”的结论已更正。

黑屏反馈后，已将设备树时序及完整初始化序列对齐立创官方参考值。用户提供商品链接，官方资料确认屏幕为 `D310T9362V1 / ST7701S`。早期官方序列部署和彩条试验仍未观察到出图；这些负面反馈保留在下方历史记录中。基线设备树为 `27 MHz`、H `32/4/32`、V `9/4/3`、`480×800`，初始化序列与官方 40 条记录逐字节一致；GP7101 背光驱动已绑定。重新拔插 MIPI 排线后，用户确认该 6.1 基线已实际出图，无需继续更换内核来排查原有空白画面。

部署只写入了独立 `boot` 分区，并替换 rootfs 中单独的 `bcmdhd.ko` 文件，没有覆盖或升级 `/opt/ros/humble` 中的 ROS 2 安装和包。更新前的完整 boot 分区及旧 Wi-Fi 模块备份仍保存在项目 `build/` 目录，可用于恢复。

## 当前成功出图配置（2026-10-13）

此节记录重新拔插 MIPI 排线后实际出图的版本，作为后续复现依据。**当前是完成屏幕适配的 6.1 版本，不是修改前的原始版本，也不是临时测试的 4.19 版本。**

### 系统与显示参数

| 项目 | 当前配置 |
| --- | --- |
| 主板／屏幕 | 泰山派 RK3566 V10／立创 3.1 寸扩展板，D310T9362V1、ST7701S |
| 操作系统 | 保留原 Ubuntu 22.04.5 与 ROS 2 Humble |
| 内核标识 | `6.1.141 #4 SMP Mon Oct 12 22:54:10 CST 2026 aarch64` |
| SDK 源码基点 | `1e5deed8940d`，在此基础上完成屏幕适配并重新编译 |
| 显示链路 | DSI1（`0xfe070000`）、VP0、video PHY1；HDMI 路由关闭 |
| 面板驱动 | `simple-panel-dsi`，`bpc = 8` |
| 分辨率／刷新率 | 480×800，DRM 报告约 60 Hz |
| 当前 X11 画面方向 | `xrandr --output DSI-1 --rotate right`，桌面与表情窗口为 800×480 横屏；面板原生模式仍为 480×800 |
| 像素时钟 | 配置及实际时钟均为 27 MHz |
| 水平时序 | 前肩 32、同步 4、后肩 32 |
| 垂直时序 | 前肩 9、同步 4、后肩 3 |
| 时序极性 | HSync、VSync、DE、像素时钟配置均为 0 |
| DSI 数据 lane | 2 条 |
| DSI lane rate | 配置 1000 Mbps；此前启动日志实际为 996×2 Mbps |
| DSI 像素格式 | `MIPI_DSI_FMT_RGB888`，输出总线 `RGB888_1X24` |
| DSI 模式标志 | VIDEO、VIDEO_BURST、LPM、NO_EOT_PACKET（`0x0a03`） |
| LCD 复位 | GPIO3_PC1（GPIO113），有效低 |
| 面板延时 | prepare 20 ms、reset 30 ms、init 120 ms、enable 100 ms、disable 20 ms、unprepare 20 ms |
| 初始化序列 | 官方补丁启用的 40 条记录、327 字节；保留 `0x3a=0x55` 等原始 payload |
| 背光 | GP7101，I2C1、7-bit 地址 `0x58`，亮度寄存器 `0x03`，当前／默认亮度 100，最大 255 |
| 触摸 | `my_touch`，I2C1 地址 `0x38`，已绑定；实际事件和坐标尚未单独确认 |
| 当前服务 | `xiaocai.service` 与 LightDM 均 active；ROS 2 Humble 环境文件存在 |

初始化序列 SHA-256：`24d0efc2e68a4458868ff51fa66eb52412e5dc451fca1d6e100ae6aa0770075d`。DSI 像素格式与面板 `0x3a` 命令值分别记录，复现时保留已出图的官方组合。

### 保留的源码与部署改动

- `/home/cmx/Documents/tspi_1m_linux_kernel6.1_sdk/kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-rk3566-user-v10-linux.dts`：启用 DSI1 设备树及对应显示路由，关闭 HDMI 路由。
- `/home/cmx/Documents/tspi_1m_linux_kernel6.1_sdk/kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-rk3566-dsi-v10.dtsi`：采用上表的官方屏幕时序、初始化表、双 lane、速率与复位延时，引用 GP7101 背光，保留触摸配置。
- `/home/cmx/Documents/tspi_1m_linux_kernel6.1_sdk/kernel-6.1/drivers/video/backlight/gp7101-backlight.c` 及同目录 Kconfig、Makefile：新增并接入适配 6.1 的 GP7101 背光驱动。
- `/home/cmx/Documents/tspi_1m_linux_kernel6.1_sdk/kernel-6.1/arch/arm64/configs/rockchip_linux_defconfig`：启用 `CONFIG_BACKLIGHT_GP7101=y`；设备树 Makefile 注册目标 DTB。
- 板端已更新 boot 分区，并使用同 SDK 构建的 `bcmdhd.ko`；没有替换 Ubuntu 根文件系统、ROS 2 安装、U-Boot、DDR 固件或分区表。
- 临时 4.19 boot、厂家初始化表、高速命令初始化试验、kexec 支持版本和 panel 状态读取诊断均已回退；当前保留官方参数的 6.1 基线，诊断模块未加载。

### 成功版本产物与原始备份

当前板端 boot 回读哈希与下表的 64 MiB 基线镜像一致，核对日期为 2026-10-13。

| 产物 | 绝对路径 | SHA-256 |
| --- | --- | --- |
| 当前完整 boot 分区镜像（64 MiB） | `/home/cmx/Documents/xiaocai_robot/build/tspi-mipi/official-lane-rate/boot-partition.img` | `43475c961f8b7da60829ce41e6cacce72d4a089236b5039520ed4cefa132a062` |
| 当前 FIT boot 镜像 | `/home/cmx/Documents/xiaocai_robot/build/tspi-mipi/official-lane-rate/boot-fit.img` | `d45d6d23f78d7bac49d5cdf5e59f77483d5f4d3f65e954e713d152be141b0f5b` |
| 当前设备树 | `/home/cmx/Documents/xiaocai_robot/build/tspi-mipi/official-lane-rate/screen.dtb` | `e8e66dbfb7402ff221411ff6e144a573130884e224b363d61adcfa9bd422170b` |
| 修改前原始 boot 备份（64 MiB） | `/home/cmx/Documents/xiaocai_robot/build/tspi-mipi/backup/boot-current.img` | `c9d6c0a25d9aabd2ec2af5942aba5c06b22383289435172a7d7739f00c8b3b0f` |
| 修改前 Wi-Fi 模块备份 | `/home/cmx/Documents/xiaocai_robot/build/tspi-mipi/backup/bcmdhd-current.ko` | `ae0622523187bceacf961ac8dc087247f47cf407bfa00aca1cd444d55f413bcd` |

当前内核二进制 SHA-256 为 `e2861de1b801a2bf7d289e96847cb47e7c0982be32f0c0733a5f8f9e85cce72c`；已部署 Wi-Fi 模块 SHA-256 为 `9b781c02b5f78d56c7e87da33e592239a5f89e8e94058d9ae27fdb2ce534e724`。这些构建文件位于 Git 忽略的 `build/` 目录，文档提交不包含镜像本体。

实体结果：用户重新拔插 MIPI 排线后确认显示正常。本轮没有更改软件配置，排线连接／接触是重要原因线索；断电重启后的稳定性与实际触摸仍未单独验证。

## 操作记录

### 2026-10-12：板端只读检查

通过 ADB 检查，没有向板端写入文件，也没有刷写或重启。

- 板型：`lckfb tspi V10 Board`，设备树兼容串为 `lckfb,tspi-v10,rockchip,rk3566`。
- 系统：Ubuntu 22.04.5 LTS，运行内核 `6.1.141`。
- 当前设备树状态：`dsi@fe060000`、`dsi@fe070000`、`route-dsi0` 和 `route-dsi1` 均为 `disabled`；`route-hdmi` 为 `okay`。
- DRM 当前只枚举 `/sys/class/drm/card0-HDMI-A-1`，状态为 `disconnected`；没有 MIPI/DSI DRM connector。
- `/boot` 有 `vmlinuz-6.1.141`、`System.map-6.1.141` 和内核配置文件，没有泰山派 DTB。
- `/usr/lib/linux-image-6.1.141` 中没有泰山派命名的 DTB。
- `/lib/modules/6.1.141/build` 指向 `/usr/src/linux-headers-6.1.141`；检查 `/home/lckfb`、`/opt`、`/usr/src` 和 `/root` 后，没有找到完整泰山派 SDK/内核源码。
- 启动命令行表明系统从 eMMC 启动，板端 `/dev/block/by-name/boot` 指向 `mmcblk0p3`。这里只记录了分区映射，没有对其写入。

### 2026-10-12：核对立创官方资料

- 立创的 Kernel 6.1 编译文档把 SDK 目录列为 `tspi_1m_linux_kernel6.1_sdk_<日期>`，并说明 SDK 通过下载中心的分卷压缩包提供。
- 该文档的内核构建示例输出 `6.1.141` 的内核包，与板端当前内核版本相同；仍需拿到 SDK 后核对源码提交、设备树和启动配置。
- 官方 SDK 文档建议至少 100 GB 可用空间，推荐 200 GB 以上。本机工作区所在磁盘当前约有 221 GB 可用空间。
- 屏幕移植文档列出的 MIPI 参数和背光注意事项已整理到下方“官方屏幕配置参考”。尚未下载或应用任何补丁。

### 2026-10-12：找到本机 SDK 并检查设备树

- 用户指出 SDK 位于 `/home/cmx/Documents/tspi_1m_linux_kernel6.1_sdk/`。已确认此前只检查了板端和机器人项目目录，漏看了这个相邻目录。
- SDK 包含 `kernel-6.1` 完整源码，Makefile 版本为 `6.1.141`。内核源码提交为 `1e5deed8940d`（2026-08-05），工作树干净；提交说明为“适配泰山派1M：关闭MIPI启用HDMI，增加USB hub电源配置”。
- 板端运行内核构建时间为 2026-05-22，早于 SDK 当前提交时间。因此内核版本号相同，但不能据此断定 SDK 与板端正在运行的内核完全同源。
- `tspi-rk3566-user-v10-linux.dts` 当前把 `tspi-rk3566-dsi-v10.dtsi` 注释掉，并启用了 HDMI。
- `tspi-rk3566-dsi-v10.dtsi` 已包含 3.1 寸面板配置：双 lane、`480 × 800` 和面板初始化序列；其当前时序为 30.816 MHz，H 前肩/同步/后肩 `50/10/60`、V 前肩/同步/后肩 `20/2/34`。这与在线教程列出的另一组参考时序不同，需要以实际屏幕资料和该 SDK 配置核对后再选用。
- 该设备树的背光节点使用 PWM5，默认亮度为 255；SDK 搜索未发现 GP7101 驱动。基于官方对 3.1 寸屏背光电流的警告，不能只取消 DSI include 就直接启用该背光配置。
- SDK 已有 `my_touch` 触摸驱动及 `myts@38` 设备树节点；触摸可以在显示验证后单独检查。

### 2026-10-12：修改 SDK 并完成主机侧构建

- 修改的 SDK 文件：
  - `kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-rk3566-user-v10-linux.dts`：启用现有 DSI1 显示设备树，并关闭与其共用 VOP0 路由的 HDMI。
  - `kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-rk3566-dsi-v10.dtsi`：移除面板对 PWM5 背光的使用，添加 I2C1 上地址 `0x58` 的 GP7101 背光节点，默认亮度 100、范围 0–255；面板引用该背光节点，并为 RGB888 面板设置 `bpc = <8>`。保留 SDK 原有双 lane、480×800、30.816 MHz 时序和初始化序列。
  - `kernel-6.1/drivers/video/backlight/gp7101-backlight.c`、`Kconfig`、`Makefile`：添加基于 Linux backlight class 的 GP7101 I2C 驱动，亮度写入寄存器 `0x03`，检查 I2C 能力与传输结果。
  - `kernel-6.1/arch/arm64/configs/rockchip_linux_defconfig`：启用 `CONFIG_BACKLIGHT_GP7101=y`。
  - `kernel-6.1/arch/arm64/boot/dts/rockchip/Makefile`：注册板端使用的 DTB 构建目标。
- 修改前 SDK 提交为 `1e5deed8940d`（2026-08-05）。板端当前仍运行 `6.1.141`，构建时间为 2026-05-22；两边内核版本号一致，但 SDK 源码更新，因此这是一次内核更新，不是仅替换设备树。
- 主机完整构建结果：Linux `Image`、`tspi-rk3566-user-v10-linux.dtb`、resource 和 FIT `boot.img` 均成功生成。`mkimage -l` 可解析 FIT，包含 fdt/kernel/resource 三项及 SHA-256 哈希。
- FIT 文件大小为 `41,562,624` 字节；板端 `/dev/block/by-name/boot` 映射到 `mmcblk0p3`，容量 `67,108,864` 字节（64 MiB）。另生成了填充到分区容量的镜像，确认 FIT 仍可解析。
- 产物路径（位于项目忽略的 `build/` 目录）：
  - 最终 `build/tspi-mipi/release/boot-new-fit.img`：41,562,624 字节，SHA-256 `4afd15d343e6ca85bac34e6d24c69f0857e17450142fb5b0aaa33303dc19b5fe`。
  - 最终 `build/tspi-mipi/release/boot-new-partition.img`：64 MiB 分区写入镜像，SHA-256 `be97d8eaef69ce5c2fac09a6f794cdc94d5c28fbadcd1292a629dea50bf57383`。早期未设置 `bpc` 的首版镜像已被最终镜像替换。
- 更新前完整 boot 分区备份为 `build/tspi-mipi/backup/boot-current.img`，大小 64 MiB，SHA-256 `c9d6c0a25d9aabd2ec2af5942aba5c06b22383289435172a7d7739f00c8b3b0f`；已与板端分区读取到的哈希核对一致。
- 板端 `/opt/ros/humble` 存在，rootfs 位于独立的 `mmcblk0p6`。写入 64 MiB boot 分区镜像不会改写 ROS 2 安装目录或其用户空间包。
- 发现板端启动时加载了 `/lib/modules/6.1.141/kernel/drivers/net/wireless/rockchip_wlan/rkwifi/bcmdhd/bcmdhd.ko`。因此另行用同一 SDK 编译了新 `bcmdhd.ko`（3,008,912 字节，SHA-256 `9b781c02b5f78d56c7e87da33e592239a5f89e8e94058d9ae27fdb2ce534e724`）。新旧模块 `vermagic` 均为 `6.1.141 SMP mod_unload aarch64`，且两边均未启用 `CONFIG_MODVERSIONS`。为了避免新内核继续加载较早 SDK 构建的 Wi-Fi 模块，计划在更新时仅替换这一个模块文件；该操作不覆盖 ROS 2 文件。
- 当前 Wi-Fi 模块已备份到 `build/tspi-mipi/backup/bcmdhd-current.ko`，大小 3,015,704 字节，SHA-256 `ae0622523187bceacf961ac8dc087247f47cf407bfa00aca1cd444d55f413bcd`。
- 代码差异检查 `git -c core.whitespace=cr-at-eol diff --check` 通过；GP7101 驱动、目标 DTB、完整内核和外部模块均成功编译。DTC 有一条原有 CSI endpoint 双向属性警告，与显示目标无关。

### 2026-10-12：用户确认后部署并验证显示

- 用户确认更新后，先通过 ADB 上传镜像和模块到板端 `/tmp`，上传文件 SHA-256 与主机产物一致，再更新板端文件。
- 原子替换 rootfs 中的 `/lib/modules/6.1.141/kernel/drivers/net/wireless/rockchip_wlan/rkwifi/bcmdhd/bcmdhd.ko`，之后重启确认新模块处于加载状态；新模块 SHA-256 为 `9b781c02b5f78d56c7e87da33e592239a5f89e8e94058d9ae27fdb2ce534e724`。
- 将最终 64 MiB 镜像写入 `/dev/block/by-name/boot`（`mmcblk0p3`），回读 SHA-256 为 `be97d8eaef69ce5c2fac09a6f794cdc94d5c28fbadcd1292a629dea50bf57383`，与主机镜像一致。
- 重启成功，运行内核为 `6.1.141 #1 SMP Mon Oct 12 22:06:53 CST 2026 aarch64`。`/opt/ros/humble` 仍存在，`bcmdhd` 已加载。
- `/sys/class/drm/card0-DSI-1/status` 为 `connected`，模式为 `480x800`；`/sys/class/backlight/gp7101-backlight/brightness` 为 `100`，`max_brightness` 为 `255`。设置 `bpc = <8>` 后，启动日志不再出现 `Expected bpc in {6,8} but got: 0`。
- LightDM 和 Xorg 正常运行；DRM debug state 显示 Xorg 在活动 CRTC 上分配了 `480×800` framebuffer。板端 XWD 截图已转换为 PNG 并取回：`build/tspi-mipi/tspi-display-board.png`。它是 Xorg framebuffer 截图，不是面板实拍；用户随后报告实体面板黑屏，因此该截图不作为屏幕已点亮的证据。
- 首次探测时出现的 `failed to find panel or bridge: -517` 是 DSI 面板驱动尚未就绪时的延迟探测日志；随后 DRM 绑定 DSI 并设置了显示模式。面板未配置单独的 power regulator，驱动使用 dummy regulator。黑屏反馈证明之前仅凭软件状态判断显示正常是不充分的。
- 若需恢复，使用 `build/tspi-mipi/backup/boot-current.img` 恢复整个 boot 分区，使用 `build/tspi-mipi/backup/bcmdhd-current.ko` 恢复原 Wi-Fi 模块。原备份的哈希均在部署前与板端文件核对一致。

### 2026-10-12：黑屏反馈后修正屏幕时序并重新部署

- 用户反馈板端实体屏幕仍为黑屏。复核发现原设备树使用 `30.816 MHz`、H `50/10/60`、V `20/2/34`，与立创针对该 3.1 寸屏给出的参数不同。
- 按立创官方参数修改 `kernel-6.1/arch/arm64/boot/dts/rockchip/tspi-rk3566-dsi-v10.dtsi`：像素时钟 `27 MHz`；H 前肩/同步/后肩 `32/4/32`；V 前肩/同步/后肩 `9/4/3`；极性维持 0。保留 `480×800`、双 lane、面板初始化序列及 GP7101 背光配置。
- 重新编译 FIT 成功。FIT SHA-256：`4603533e4e797a07de74cf68331d93a42cb8f671233c23fde5f9abafee8849b8`；64 MiB boot 分区镜像 SHA-256：`3c8c8d6ddaa3f2007cfd628dc3749b7f71d698d03e0609b276ee3decb8d2797f`。设备树 SHA-256：`5c2ac927b46ebac7ad6c711b3aae59a7a80d81529caf388bf2e351b237907f75`。
- 通过已连接 ADB 将 64 MiB 镜像写入 `/dev/block/by-name/boot`（`mmcblk0p3`），板端回读 SHA-256 与主机一致；重启后系统正常启动，ROS 2 Humble 仍存在。没有修改 rootfs 或 Wi-Fi 模块。
- 从板端正在运行的 `/proc/device-tree/dsi@fe070000/panel@0/display-timings/timing0` 读取到时序为 `27000000`、H `480/32/4/32`、V `800/9/4/3`。DSI DRM mode 仍为 `480x800`；`gp7101-backlight` 已绑定，亮度 `100/255`、`bl_power=0`。
- **用户已确认：重新部署后仍黑屏，但背光亮。** 后续重点检查 DSI 面板复位、排线、供电和初始化链路。

### 2026-10-12：背光亮但黑屏的运行时诊断

- 面板复位 GPIO 为 GPIO3_PC1（全局 GPIO 113），板端显示物理高电平、有效低，表示复位已释放。DSI 正在以 27 MHz 输出，host 为双 lane、RGB888；这些软件状态不证明面板端实际接收到信号。
- 临时开启 `/sys/kernel/debug/fe070000.dsi/vpg` 的内置彩条测试。用户确认屏幕依旧黑屏但背光亮。
- 用同一内核源码构建临时诊断模块，在视频输出期间读取 DCS `0x04/0x09/0x0a/0x0c/0xda/0xdb/0xdc`，均返回 `-110`（没有读回 payload）。该结果不能单独认定面板断路：此 SDK 的低功耗消息函数会调整 DSI 时钟，且测试发生在视频模式。
- 已通过 Xorg 对 DSI 输出执行关闭、重新开启，恢复标准驱动初始化；后续诊断模块显式保存并恢复 DSI 视频与时钟寄存器。记录到重新初始化后的高速时钟请求位为 1、PHY PLL 已锁定。
- 原初始化序列 `0x3a=0x55` 与 host RGB888 存在格式疑点，临时测试 `0x3a=0x77`；这是运行时试验，尚未认定为修复，不直接写入最终配置。
- 首版初始化阶段诊断在复位后、初始化前发出 DCS 读请求，该请求导致此控制器命令 FIFO 阻塞，随后初始化写命令超时。这些写入错误由诊断引入，不能当作原黑屏故障的证据；已移除初始化前预读。
- 修正后的诊断仅在完整初始化结束后、视频输出开始前读取一次 DCS `0x0a`。运行内核为 `6.1.141 #3 SMP Mon Oct 12 22:42:45 CST 2026`，设置返回包长度成功，初始化写命令未报告失败，但 `0x0a` 仍返回 `-110`，没有读回 payload。该结果尚不足以确定面板或排线故障。
- 该诊断镜像临时使用 `0x3a=0x77`；没有取得实体出图的确认。64 MiB 镜像 SHA-256 为 `4b6064f31e089e834975586eb20fe645912ced5b64181cddf358b3671c3537db`。诊断镜像、模块及源码保存在忽略的 `build/tspi-mipi/diagnostic/`，原系统备份保留。
- 初次彩条反馈发生在运行时读请求诊断期间；该阶段 DSI 高速时钟曾受读请求影响。之后已经重新初始化显示并恢复时钟，因此不能仅凭首次彩条反馈就排除所有软件配置问题。

### 2026-10-12：依据用户提供的商品确认型号并对齐官方初始化

- 用户确认使用 [立创商城商品 44319751](https://item.szlcsc.com/44319751.html)，商品型号为 `LCKFB-mipi-3.1inch-screen`，商品编号 `C42388916`。
- 查阅商城提供的 [官方 PDF 资料](https://atta.szlcsc.com/upload/public/pdf/source/20250318/892FCAA3E4DF0744082E1E5105D42E4E.pdf) 六页内容：屏幕型号 `D310T9362V1`，显示驱动 `ST7701S`，分辨率 `480×800`、MIPI Video Mode；触摸 IC 标注为 CST 或 FT 系列。商品屏幕型号与此前采用的胖妞手机教程一致。
- PDF 第 3 页列出面板 24-pin 接口：RESET 为 7 脚，IOVCC 为 11 脚，VDD 为 13–14 脚，D1 为 16–17 脚，CLK 为 19–20 脚，D0 为 22–23 脚。扩展板通过配套 31-pin MIPI 排线和 6-pin 触摸排线连接泰山派；背光与面板数据、复位及供电是不同链路，背光亮不能单独证明面板通信正常。
- 从官方教程原始 HTML 提取完整 `panel-init-sequence`，按字节与 SDK 比对。官方示例共有 40 条记录，SDK 多出 `EE 42`，普通命令延时、B1/B3 延时和 D0 延时也不同。将设备树完整序列对齐官方示例，包括 D0 后等待 100 ms，并恢复官方 `0x3a=0x55`。每条记录的 payload 长度均已检查，与头部声明一致。
- 移除 `panel-simple.c` 中临时状态读代码及设备树诊断属性；保留 GP7101 背光驱动、双 lane、RGB888、`bpc=8` 和官方 27 MHz 时序。SDK DTSI 恢复原有 CRLF 行尾，差异检查通过。
- 新 FIT 构建成功，包含 kernel/fdt/resource，64 MiB 填充镜像能由 `mkimage -l` 正常解析。此前时序修正版产物另保存在 `build/tspi-mipi/diagnostic/release-before-official-sequence/`。
- 第一版官方序列镜像（64 MiB SHA-256 `b904234f09a3201ad0dee8a11d4844c504988643a47a3e6088b8e2e1531db500`）已上传、写入、回读校验并重启。读取实时设备树，确认 327 字节的初始化序列与官方 40 条记录完全一致，诊断属性不存在；未报告初始化写入错误。**用户确认仍然黑屏、背光亮。**
- 清理阶段发现恢复面板源码时保留了旧时间戳，编译器复用了此前诊断对象文件；设备树诊断属性已经删除，因此该代码未启用。随后清除面板对象缓存，重新编译内核并检查最终 Image，确认诊断字符串已完全移除。
- 清理后的最终产物：`build/tspi-mipi/release/boot-new-fit.img`（41,562,624 字节），SHA-256 `7ad2561495dcb96a103297a8d21aca7cd2578bad0576f2e36de885493cd8587b`；`boot-new-partition.img`（64 MiB），SHA-256 `5fbff2330736ee45baa4f957271df5da6330445cc042a5c089a9ab4873df9f05`；DTB SHA-256 `3d5a77eaf8c1809d4b59d0d8e4aa77de2242f789539036d27f7b23fa459a932d`。
- 清理镜像先上传到板端 `/tmp/boot-panel-clean.img`；为避免用户拍照断电时发生刷写，曾暂停板端写入与重启。用户随后确认锁扣都已关好，暂不继续拆排线。
- 用户提供实体屏幕正面实拍：画面整片发白、略带淡紫色，没有可辨认的桌面或图像。此实拍证实背光点亮，但角度未露出 MIPI 排线两端插座及锁扣，不能据此判断连接方向或接触质量。已请用户先断电，再拍背面连接处，暂不拆排线。
- 用户补充叠装侧面照片：可见右侧弯折排线，但接头被屏幕遮挡，仍不能确认两端接触面、插入深度及锁扣状态；照片中板端绿色指示灯亮。已提醒先断开供电及 USB，再从排线所在侧拍接头近照，可以分开拍两端，无需仅为拍照拆屏。
- 板端只读补查：复位 GPIO 113 为高电平、有效低；Xorg framebuffer 与活动 CRTC 为 `480×800`；I2C `1-0038` 的 `my_touch` 驱动已绑定并注册 input 设备。尚无实体触摸事件验证。
- 在通知用户保持供电后，通过 ADB 确认在线、上传镜像哈希及 boot 分区容量，写入清理镜像并回读校验；分区 SHA-256 与最终 `5fbff…` 镜像一致。本次仅写 boot，没有改动 rootfs 或 Wi-Fi 模块。最终镜像内核标识为 `6.1.141 #4 SMP Mon Oct 12 22:54:10 CST 2026`。
- 此前均为软件重启，面板电源未完全断开。请用户拔掉供电和 USB，等待 10 秒后再接回；用户反馈仍无画面、只有背光。随后观察到 ADB 离线再恢复，确认板端运行 `6.1.141 #4 SMP Mon Oct 12 22:54:10 CST 2026`，启动后约 59 秒，DRM connected、背光 100、ROS 2 Humble 存在、`bcmdhd` 已加载，未报告面板初始化写入失败。
- 在冷启动后的最终内核上重新开启控制器彩条，临时模块使用 `read_status=0`，仅检查寄存器，不发 DCS 读请求，检查后卸载模块。记录到 video mode 为 `0`、彩条模式寄存器为 `0x00013f02`、高速时钟请求寄存器为 `1`、PHY 状态为 `0x000015b9`。**用户确认没有彩条，仍为空白背光。** 此测试修正了早期读请求干扰时钟导致的诊断限制；反馈后已将 `vpg` 恢复为 `0`，结束临时彩条测试。

### 2026-10-12：检查用户下载的官方 Buildroot 整包

- 用户提供 [立创开发板屏幕安装视频](https://www.douyin.com/video/7633654314023554344)。观看组装和固件章节，视频引导下载小手机案例的屏幕固件；作者在“背光亮没画面”的评论下回复使用泰山派小手机案例固件。这是对照版本的线索，尚不能证明当前故障原因。
- 用户从“7.手机综合项目 / 系统镜像 / buildroot”下载 `/home/cmx/Downloads/update.img`。文件 SHA-256 为 `d2c3bf973bbb641850f57b0f7607ea5b376d85951a4d2180558a9aaacf47f886`，已通过 SDK 的 `rkImageMaker -unpack` 和 `afptool -unpack` 解包成功。本次操作仅在主机分析，未烧录板端。
- 整包清单含 bootloader、parameter、uboot、misc、boot、recovery、rootfs、oem、userdata；rootfs 为 ext4。读取镜像内部 `/etc/os-release`，确认为 `Buildroot 2018.02-rc3`；内核为 `Linux 4.19.232 #27`，构建日期 2024-05-10。镜像内 `/opt` 仅列出 unixbench，没有当前 Ubuntu 的 `/opt/ros` 环境。
- **若按整包升级方式烧录该镜像，会替换当前 Ubuntu 根文件系统和已安装的 ROS 2 环境。** 清单还包含 userdata 和分区表，板端项目、配置、数据和自启设置不能假设保留。此前只备份 boot 和 Wi-Fi 模块，不足以恢复整个 Ubuntu 环境；如需整包测试，应先备份当前分区布局及全部所需分区并核对可恢复性。
- 提取 FIT 内核和设备树，核对 SHA-256 与 FIT 声明一致。设备树型号为 `lckfb tspi V10 Board`，使用 DSI1、双 lane、RGB888、480×800、27 MHz 和 GP7101。327 字节面板初始化序列与当前最终 6.1 镜像完全一致。
- 发现两个明确差异：该官方镜像 `rockchip,lane-rate = 1000`，当前 6.1 配置为 `500`（单位 Mbps）；官方面板 `reset-delay-ms = 30`，当前为 `200`。这些是后续对照排查线索，尚未验证为黑屏根因，也尚未调整板端配置。
- 该镜像内核与现有 6.1 系统及模块版本不同，不能直接承诺仅替换旧 boot 后 Wi-Fi、音频和 ROS 2 外设功能正常。优先基于现有 6.1 SDK 对照屏幕相关配置，保留 Ubuntu 根文件系统。
- 分区清单、分区参数、FIT 信息、提取的设备树及比较结果保存于忽略目录 `build/tspi-mipi/official-buildroot-inspection/`。完整解包临时目录为 `/tmp/xiaocai-official-buildroot-inspect/`，原始下载文件保持不变。

### 2026-10-12：在 6.1 内核中对齐官方 DSI 速率与复位延时

- 用户同意继续移植，并提供 `Tspi_Linux_config.cfg`。该文件为瑞芯微烧录工具的二进制配置，提取 UTF-16 字符串可见旧 Windows SDK 路径和 loader、parameter、uboot、boot、recovery、rootfs、userdata 等条目；它不是内核源码或镜像，不按其整包条目烧录当前板端。
- 将当前 SDK 中启用的 3.1 寸面板块的 `rockchip,lane-rate` 从 `500` 改为 `1000`，`reset-delay-ms` 从 `200` 改为 `30`，并保留 CRLF。此前产物备份到 `build/tspi-mipi/diagnostic/release-before-official-lane-rate/`。
- 编译成功。逐项验证 FIT 中 kernel、fdt、resource 的 SHA-256，确认 kernel 二进制与当前运行的 `6.1.141 #4` 完全相同；比较新旧 DTB 全部节点和属性，只有上述两项值变化，327 字节面板初始化序列保持一致。
- 产物保存于 `build/tspi-mipi/official-lane-rate/`：FIT SHA-256 `d45d6d23f78d7bac49d5cdf5e59f77483d5f4d3f65e954e713d152be141b0f5b`；64 MiB boot 分区镜像 SHA-256 `43475c961f8b7da60829ce41e6cacce72d4a089236b5039520ed4cefa132a062`；DTB SHA-256 `e8e66dbfb7402ff221411ff6e144a573130884e224b363d61adcfa9bd422170b`。对应 SDK 补丁和 manifest 一并保存。
- 上传、校验后仅写 boot 分区，回读 SHA-256 一致，重启正常。实时设备树为 lane-rate `1000`、reset-delay `30`；DSI 日志实际链路速率 `996 × 2 Mbps`，CRTC 仍为 480×800、27 MHz。未报告面板初始化命令写入失败。
- `/opt/ros/humble/setup.bash` 存在，加载环境后 `ros2 --help` 成功；`xiaocai.service` 为 active/running，`bcmdhd` 已加载。未写 rootfs、userdata 或分区表，Ubuntu 和 ROS 2 保留。
- **用户确认仍无画面、只有背光。** 官方速率和复位延时对齐尚未解决实体显示问题，后续继续核对跨内核版本的初始化与供电路径，不能将此结果认定为硬件故障。

### 2026-10-12：复核用户下载的完整小手机项目资料

- 资料根目录为 `/home/cmx/Downloads/【智能小手机】项目资料/`。逐个检查 `7.手机综合项目/代码补丁/` 内的 7 个压缩包，读取设备树、补丁变更清单、Linux 背光驱动及厂家初始化文本；没有执行压缩包中的程序或直接覆盖当前 SDK。
- 与本屏对应的主要资料是 `3.1寸大显MIPI补丁.zip`、`3.1寸屏幕背光驱动Linux补丁.zip`、`3.1寸屏幕触摸Linxu驱动补丁.zip`，以及 `数据手册/3.1寸屏幕资料/D310T9362V1 SPEC/`。`5.屏幕选型与调试/MIPI屏资料/` 中的 800×1280、ILI9881C 示例和 `6.触摸驱动/` 中的 10.1 寸、GT9271 示例属于其他屏幕，不用于本屏显示配置。
- **7 个压缩包中启用的面板初始化序列均与当前 SDK 逐字节相同**：327 字节、40 条记录，SHA-256 为 `24d0efc2e68a4458868ff51fa66eb52412e5dc451fca1d6e100ae6aa0770075d`。逐条检查记录长度，均与 payload 长度一致。压缩包中的 Android 横竖屏版本也没有另一套面板初始化序列。
- Linux 参考补丁启用 DSI1、VP0 和 video PHY1，关闭 HDMI；复位为 GPIO3_PC1，有效低；双 lane、RGB888、1000 Mbps、27 MHz、480×800，reset-delay 30 ms、init-delay 120 ms，与当前显示配置对齐。补丁里未发现需要额外移植的面板供电 GPIO 或 regulator 控制代码。GP7101 背光驱动通过 I2C 写 `0x03` 调整亮度，没有另一段面板电源开启逻辑；这不代替实体供电测量。
- 早期背光 Linux 补丁仍附带 GT9xx、地址 `0x5d` 的触摸示例；后续 `3.1寸屏幕触摸Linxu驱动补丁.zip` 才配套 `my,touch`、地址 `0x38`。当前已使用后者。Android 综合补丁还使用 VP1 路由，不能直接把整个 Android 设备树覆盖到当前 Linux VP0 配置。
- 厂家文本 `st7701s 2018.9.26.txt` 的主初始化部分有 39 条命令，当前补丁有 40 条。完整解析后确认：**两者开头都有 `0x01` 软件复位和 `0x11` 退出休眠，且分别等待 120 ms**。此前即时说明中将开头顺序描述为差异不准确，以本次完整解析为准。实际差异为厂家 C7=`0x04`、36=`0x10`（旋转方向），补丁均为 `0x00`；补丁末尾另发一次 `0x11` 并等待 120 ms，`0x29` 后等待 20 ms。厂家硬件复位示例是先高 10 ms、低 120 ms、再高 50 ms；补丁复位延时采用另一组值。这些是对照线索，尚未证实为黑屏原因。
- `2.SDK编译/Linux配置文件/烧录内核/Tspi_Linux_config.cfg` 与 `全烧录/Tspi_Linux_config.cfg` 均为 6129 字节，SHA-256 均为 `ad1ae893c2a96c9043dbd31865940dc62a53d93ae834fced114a7d880af37b55`，内容完全相同。不能根据父目录名称推断实际烧录范围；导入后还须核对工具勾选的分区及镜像路径。
- 此目录中的 `7.手机综合项目/系统镜像/buildroot/update.img` 为 522,918,474 字节，SHA-256 与此前提供的 `/home/cmx/Downloads/update.img` 一致，并非另一版固件。此前整包分析与替换 Ubuntu/ROS 2 的影响结论仍适用；本次没有烧录整包。
- 基于 ST7701S 像素格式说明，曾在当前干净内核上临时发送 `0x3a=0x77`；发送返回 0，保存并恢复 DSI 视频、命令与高速时钟寄存器，未发 DCS 读请求，随后卸载诊断模块。**用户反馈没有变化，仍只有背光。** 已发送 `0x3a=0x55` 恢复官方配置，返回 0，并再次卸载模块；`vpg=0`，内核仍为 6.1.141，ROS 2 环境文件存在，`xiaocai.service` active。命令返回成功仅证明 host 接受发送，不能证明面板实际收到。
- 审核清单、各压缩包及序列哈希、厂家逐条命令比较和临时试验结果保存于忽略目录 `build/tspi-mipi/downloaded-project-review/`。本轮未刷写或重启，现阶段仍未找到能解释实体黑屏的确定根因。

### 2026-10-12：用户同意再次试验厂家初始化表

- 用户询问是否再尝试，继续在现有 6.1 内核上做一次有源码依据的对照试验。使用厂家 `st7701s 2018.9.26.txt` 主初始化部分（不包含后面的旋转辅助函数），转换为 DCS 记录：单字节命令用 `0x05`、双字节用 `0x15`、其余用 `0x39`。共有 39 条、323 字节。
- 表内 payload 及命令间延时按厂家文本保留：C7=`0x04`、36=`0x10`，像素格式仍为 `0x55`；去掉原补丁末尾重复的 `0x11`，`0x29` 后不再插入表内 20 ms 延时，原 panel enable-delay 100 ms 保留。开头的软件复位、退出休眠各等待 120 ms，D0 后等待 100 ms。
- 设备树 reset-delay 改为 120 ms，init-delay 改为 50 ms，使复位有效低阶段与释放后的等待对应厂家示例。没有修改 `panel-simple.c`，也没有添加厂家示例复位前的单独高电平 10 ms 阶段，因此不宣称完全复现厂家 GPIO 波形。
- 构建成功，并校验 FIT 中 kernel/fdt/resource 的 SHA-256。新旧 kernel 二进制完全一致，仍为 `6.1.141 #4`；对比整个 DTB，仅 init-delay、reset-delay、panel-init-sequence 三项变化。DSI 速率、时序、ROS 环境、Wi-Fi 模块没有改动。
- 产物位于 `build/tspi-mipi/factory-init-trial/`。FIT SHA-256 为 `8a1126c1f885f70a548fc8953461e531e333d3fa69f68c7c77de99cbbba4fc0d`；64 MiB 镜像为 `a643273cb6a45810584e620bfdc31ed16ff6dcd51dbd77df8755765f7ed55f0a`；DTB 为 `7738076d813a25b5316cfca5ea07ff8edebc386efa9ee158e874874e11c2a715`。原设备树、构建日志、SDK 补丁与 manifest 一并保存。
- 写入前回读板端 boot 哈希为 `43475c961f8b7da60829ce41e6cacce72d4a089236b5039520ed4cefa132a062`，与保留的 `build/tspi-mipi/official-lane-rate/boot-partition.img` 一致；可用此镜像回退本轮试验。板端在线、机器人自启服务 active。
- 上传临时镜像到 `/tmp/boot-panel-factory-init.img`，核对上传 SHA-256 和 boot 分区 64 MiB 容量后，仅写 `/dev/block/by-name/boot`。写入后回读 SHA-256 与 `a643273…` 完全一致，随后重启；没有写 rootfs、userdata、uboot 或分区表。
- 重启后 ADB 恢复，内核仍为 `6.1.141 #4 SMP Mon Oct 12 22:54:10 CST 2026`。读取实时设备树，确认 reset-delay=120、init-delay=50，初始化序列为 323 字节，SHA-256 为 `a10d4d2013c3aead13ff166339591a6686dd5ef116187c9ba5263b99b513053c`，与厂家试验序列一致。
- ROS 2 Humble 环境文件存在，`ros2 --help` 成功；`xiaocai.service` active/running、ExecMainStatus=0，LightDM active、bcmdhd 已加载。DRM connected，背光 100，DSI 日志 996×2 Mbps；未报告面板初始化发送失败。已向用户询问实体屏幕变化，**物理显示结果待确认**，不以这些软件状态认定出图成功。
- 将上一版镜像额外上传到板端 `/tmp/boot-before-factory-trial.img`，SHA-256 与 `43475c…` 一致，方便反馈无效后回退。该文件在 tmpfs，断电后可能消失；主机原镜像长期保留。
- **用户随后反馈仍然不行。** 厂家表与延时试验未取得实体出图结果，不能作为修复保留。

### 2026-10-13：回退厂家试验，对照高速命令发送

- 用户要求继续。中断后重新检查：板端仍运行厂家试验镜像 `a643273…`，期间重新启动过，板端 tmpfs 中回退文件已丢失。重新上传主机保留的上一版镜像，仅恢复 boot 分区，回读 SHA-256 为 `43475c961f8b7da60829ce41e6cacce72d4a089236b5039520ed4cefa132a062`，随后重启。
- SDK DTSI 也从 `factory-init-trial/dsi-before-trial.dtsi` 恢复。重启后读取实时设备树：复位延时 30 ms，初始化表 327 字节，机器人服务 active，确认回退生效。
- 下载并阅读 [Rockchip 官方 develop-4.19 DSI 源码](https://raw.githubusercontent.com/rockchip-linux/kernel/develop-4.19/drivers/gpu/drm/rockchip/dw-mipi-dsi.c)，与当前 `drivers/gpu/drm/bridge/synopsys/dw-mipi-dsi.c` 比较。此分支源码是参考，不宣称是下载镜像构建时的确切 commit。发现 6.1 低功耗命令分支会清除 `PHY_TXREQUESTCLKHS`，4.19 的对应分支保留该位；两版视频启动阶段均开启高速时钟。两版 PHY 的 LP/HS 转换时间参考值相同，因此本轮不盲目改 PHY 定时。
- 阅读并渲染用户下载的 ST7701S 手册第 152 页：DCS 单参数写入同时有 LPDT、HSDT 示例；后续页给出其他 DCS 写入示例。依据这些接口能力，准备一轮高速命令发送对照试验，尚未将驱动差异认定为黑屏根因。
- **本轮只去掉启用的 3.1 寸面板 `MIPI_DSI_MODE_LPM` 标志**，`dsi,flags` 从 `0x00000a03` 改为 `0x00000203`。保留官方 327 字节初始化表、RGB888、`0x3a=0x55`、1000 Mbps、27 MHz 时序、双 lane 和原复位延时。注释中的其他屏幕配置保持原样。
- 构建成功，校验 FIT 内三个镜像哈希；kernel 二进制与当前 #4 完全相同。新旧 DTB 全量比较仅有上述 flags 一项变化。产物在 `build/tspi-mipi/hs-command-trial/`：FIT SHA-256 `7462bc6e0462a9f631ecea8865440d72555a09b65c540d31dc0688692994e13b`；64 MiB boot 镜像 `fbce8b80632b6103ba7e94631cde6f5548384219ec768a1bb4fc64993b363f79`；DTB `80b5467e34ab41975454d90918d9c73032761d557c1d56fbdc1b1d2fb2992cf9`。前版源码、参考源码、构建日志和 manifest 已保留。
- 上传、核对 64 MiB 分区容量及文件 SHA-256 后，仅写 boot，回读 SHA-256 为 `fbce8b…`，与主机一致，随后重启。读取实时设备树，flags=`0x203`，初始化表哈希仍为 `24d0ef…`，内核仍为 6.1.141 #4。
- 重启后 ROS 2 CLI 正常，xiaocai.service 与 LightDM active，bcmdhd 已加载。临时诊断模块仅使用 `read_status=0` 读取控制器寄存器，没有发送 DCS 读写请求，随后卸载：CMD_MODE_CFG=`0`（高速命令配置）、LPCLK_CTRL=`1`（高速时钟请求开启）、FIFO 状态=`0x15`、INT_ST0/1=`0`，实际链路 996×2 Mbps。这些是 host 状态，不代替面板接收或实物出图确认。
- **用户反馈高速初始化后仍然不行。** 本轮没有写 rootfs 或其他分区。随后重新上传基线镜像，仅恢复 boot，回读 SHA-256 为 `43475c961f8b7da60829ce41e6cacce72d4a089236b5039520ed4cefa132a062`；SDK DTSI 同步恢复低功耗命令标志。重启后确认内核 #4、双 lane、官方初始化表哈希 `24d0ef…`，机器人和 LightDM 服务 active。
- 同时为后续官方 4.19 boot 对照做只读准备：当前 Ubuntu rootfs 是 ext4（含 64bit、metadata_csum 等特性），ADB 由 `/usr/bin/usbdevice` 配置 configfs 启动，当前系统仅有 6.1.141 模块目录。官方 4.19 镜像包含内建 EXT4、USB configfs/FunctionFS 驱动，但仅凭这些不能承诺它在现有 Ubuntu 上启动后 ADB、音频及机器人外设正常；本轮尚未写入旧内核。

### 2026-10-13：核对用户提供的飞书教程

- 用户提供 [七、手机综合项目](https://lceda001.feishu.cn/wiki/K51kwVQ9ViBTeDkYmsRc0YOLnle)。网页读取工具未能打开，随后使用浏览器读取公开正文，不需要登录。
- 阅读“三、软件开发 → 1.1 打开设备树驱动、1.2 GP7101 背光驱动、1.3 屏参数调试”。教程要求包含 DSI DTSI、将 lane 数改为 2、使用厂家适配后的初始化序列，并设置 27 MHz、480×800、水平 32/4/32、垂直 9/4/3、四项极性为 0。初始化表含 C7=00、36=00、3A=55、末尾重复 11 并等待 120 ms，29 后等待 20 ms，与此前下载的 MIPI 补丁和当前配置相符；未在这些步骤中发现另一套显示参数。
- 教程“4.3.5 测试验证”建议首次使用附件提供的固件确认显示和触摸，再调试自己的代码。这支持进行官方固件对照，但不能据此直接全烧录：当前板端是保留 ROS 2 的 Ubuntu，整包 Buildroot 会替换系统。
- 教程“2.2 ubuntu系统”在本次读取时只有 Qt/PyQt、机器人 UI、关键词唤醒和模型 API 等小节标题，没有看到针对当前 6.1 内核的屏幕移植实现。不能把该标题当作 6.1 显示驱动已适配的证据，也不能据此认定内核版本就是黑屏原因。
- 为官方内核对照准备仅从内存启动的 kexec 方案。已完成 6.1 支持内核构建，仅在临时构建 `.config` 开启 KEXEC 及其依赖 TRANS_TABLE、CRASH_CORE、KEXEC_CORE，SDK defconfig 未改。FIT 内 kernel/fdt/resource 哈希验证通过，DTB 与基线逐字节相同。64 MiB boot SHA-256 为 `67ce2b56befe32ef3505b83aedf576519776c560586dd3cd6007563ed6e93c6b`，产物和配置差异保存于 `build/tspi-mipi/kexec-reference-trial/`。
- 从 Ubuntu 官方软件包提取临时 ARM64 kexec 工具，上传 `/tmp` 并仅运行 `--help`，确认支持 Image、`--dtb` 和命令行参数；没有安装整个软件包。systemd 的正常 kexec 关机路径会停止服务、卸载文件系统，再调用 `/sbin/kexec`。截至此记录，支持内核尚未部署，官方 4.19 内核尚未加载或运行。

### 2026-10-13：官方 4.19 内核 RAM 对照

- 核对当前 boot 基线和上传文件 SHA-256、64 MiB 分区容量后，仅部署支持 kexec 的 6.1 boot 镜像。回读 SHA-256 为 `67ce2b56…`。重启进入 `6.1.141 #5 SMP Tue Oct 13 07:53:37 CST 2026`，kexec_loaded=0、kexec_load_disabled=0；机器人与 LightDM active，bcmdhd 已加载，DSI 仍为 996×2 Mbps。没有修改 SDK defconfig，也没有写 rootfs 镜像或分区表。
- 阅读 Ubuntu 官方 kexec-tools 2.0.22 ARM64 源码，确认指定 `--dtb` 时不会自动补充 `/memory`。配套固件中的原始 DTB 没有 memory 节点，因此从板端实时 FDT 复制 U-Boot 填充的 memory 描述，保留低地址和安全固件占用区间；全量比较 DTB 属性，仅新增 `/memory/device_type` 和 `/memory/reg` 两项，所有显示和外设配置保持官方值。该临时 DTB SHA-256 为 `2dcd65120a9de5919ea226c69a14c8accfd25951e21a4407a27025d9857f426d`。
- 官方 4.19 Image 保持原始字节，SHA-256 为 `da81bb5b4e5e5abc301ead8f17992a171864f72f4855374eb0fa80a4c38467a8`。上传两个文件并验证哈希后执行 `kexec -l`，追加一次性的 `systemd.mask=xiaocai.service`，避免这次诊断运行机器人外设业务。工具提示无法读取 `/proc/kcore`，随后使用 `/proc/iomem` 回退；命令返回 0，kexec_loaded=1。此时仍运行 6.1，尚未把加载成功当作 4.19 启动成功。
- 确认 `/sbin/kexec` 原先不存在后，临时放置从 Ubuntu 官方包提取的工具，供 systemd 最后的正常关机步骤使用。文件 SHA-256 为 `ca623abefcad2746a5f5b3e698ec7a8caef0534f112dc3ead16484e65016f4b5`，诊断结束后按此哈希确认并清理。没有安装 kexec 的自启脚本或软件包。
- 执行 `systemctl kexec`，ADB 随后恢复，`uname` 确认已实际运行 `4.19.232 #27 SMP Fri May 10 02:49:10 EDT 2024`。`/etc/os-release` 仍为 Ubuntu 22.04.5，ROS 2 Humble 环境文件存在；机器人服务在这次启动中 inactive、LightDM active。持久 boot SHA-256 仍为 `67ce2b56…`，证明没有把旧内核写入 boot。官方内核仅存在于 RAM；正常重启会回到 6.1 #5，恢复机器人自启；诊断完成后可恢复 #4 基线。
- 4.19 官方驱动已绑定 DSI，GP7101 亮度 100，fb0 注册成功。DRM summary 显示 VP0、Smart0-win0 active、XR24、480×800、实际 27 MHz；DSI 日志 996×2 Mbps。Xorg 曾尝试 Cluster0 线性格式并报 Unsupported linear format，最终活动平面是 Smart0；因此继续用控制器内建彩条消除桌面内容因素，不以桌面服务 active 判定物理出图成功。
- 旧内核没有当前 6.1 的 vpg debugfs 入口，依据 Rockchip DSI 源码，在 `/dev/mem` 映射 DSI1 的 `0xfe070000` 寄存器区域，先保存视频配置再只设置 `VID_MODE_CFG` 的 VPG_EN 位（bit 16）。操作前 PWR_UP=1、MODE_CFG=0、LPCLK_CTRL=1、PHY_STATUS=0x1529、FIFO=0x15、INT_ST0/1=0；VID_MODE_CFG 从 `0x3f02` 变为 `0x13f02`，回读一致，没有发送 DCS 读写请求。保存值位于板端 `/tmp/xiaocai-official-vpg-state.json`，`python3 /tmp/xiaocai-official-vpg.py restore` 可恢复；重启同样会清除该临时状态。
- **用户确认仍然空白、只有背光。** 状态日志及临时设备树审查记录保存在 `build/tspi-mipi/kexec-reference-trial/`。该方案只对照内核及设备树，未复现官方整包中的 U-Boot、DDR 固件和 Buildroot 用户空间，且属于 warm kexec 启动，因此不能把失败直接认定为硬件故障。
- 恢复彩条前再次读取寄存器：INT_ST1=`0x80`，参考驱动 error_report 的 bit 7 对应 DPI pixel payload FIFO overflow；此前开启彩条前为 0。该状态在彩条试验之后出现，应作为诊断限制保留，不视为原始黑屏的已知根因。VID_MODE_CFG 已恢复 `0x3f02`，回读一致。
- 重新上传并校验基线 boot 镜像，仅回退 boot，回读哈希 `43475c961f8b7da60829ce41e6cacce72d4a089236b5039520ed4cefa132a062`。按工具哈希确认后删除此次新增的 `/sbin/kexec`，随后正常重启。最终确认内核 #4、机器人和 LightDM active、bcmdhd 已加载、ROS 环境文件存在，启动命令行不再含临时服务屏蔽参数，临时工具已移除。

### 2026-10-13：按用户要求，再对照原始官方 boot 的正常引导

- 用户表示有万用表，但希望先再尝试一轮，测量留到这轮之后。因此选择排除上一轮 kexec 热切换的限制：让 U-Boot 正常引导配套固件中的原始 4.19 boot，不修改内核、设备树或 resource，也不烧录整包 Buildroot。
- 从用户已下载且校验过的镜像提取原始 `boot.img`，逐项验证 FIT 内 kernel/fdt/resource 的 SHA-256。kernel 与 RAM 试验的官方 Image 相同，DTB 保持原始 `babe03f2b3e1337c5b65aefc5e3ec2a5f72ebed8b76afecc7d103843df4d04a3`，实际内存信息交由 U-Boot 按正常启动流程填充。
- 原始 FIT SHA-256 为 `1cbd9bb599c2c5b45caccc5bb104b503a8535852f8996897a68add0c9f8406d1`；补零后的 64 MiB 分区镜像为 `94076922a0fb4aff2908b98a18be6756e404041410e2577bcdf62a00b281db92`。产物和 manifest 在 `build/tspi-mipi/official-4.19-boot-trial/`。
- 上传后核对文件哈希、分区容量以及当前 6.1 基线哈希，只写入 `/dev/block/by-name/boot`，回读为 `94076922…`，随后执行正常重启。没有写入 rootfs、userdata、uboot 或分区表。U-Boot、DDR 固件和 Ubuntu 用户空间仍是原系统版本，因此这仍不是官方整包固件的完整复现；此轮也不把软件重启等同于完全断电冷启动。
- 正常启动后 ADB 恢复，内核确认为 `4.19.232 #27`，boot 回读哈希 `94076922…`，Ubuntu 22.04.5、ROS 2 环境文件保留。机器人和 LightDM active，启动命令行不含临时服务屏蔽参数。
- 本轮 DRM 日志已解析 U-Boot 的显示平面分配（VP0 plane mask=0x3f、primary plane id=4），并释放 1384 KiB 的 drm_logo 内存，与 RAM 试验缺少 loader memory 的情况不同。当前 VP0、Smart0-win0 active，480×800、实际 27 MHz、XR24、GP7101 亮度 100。没有再开启彩条或改写 DSI 寄存器，先保持官方启动链的原始输出；软件状态仍不代替实物确认。
- 请求用户拔掉所有供电（包含 ADB USB），等待 10 秒后重新接上并观察约 30 秒，以排除面板残留状态；**用户随后反馈仍然没有显示**。状态记录保存在 `official-4.19-boot-trial/board-normal-boot-state.log`。仅更新官方 boot 仍保留原 U-Boot/DDR 固件与 Ubuntu，不能将此结果等同于官方整包在板端的完整验证。
- 本轮无效后，重新上传、核对 6.1 基线镜像和当前官方 boot 哈希，只恢复 boot。回读为 `43475c961f8b7da60829ce41e6cacce72d4a089236b5039520ed4cefa132a062`，重启后确认为 6.1.141 #4；机器人和 LightDM active、bcmdhd 已加载、ROS 环境文件存在。没有保留 4.19 为日常系统。
- 按用户同意的顺序转入实体测量准备：当前 GPIO113（GPIO3_PC1、LCD reset、有效低）软件输出为 high，这不证明实体屏幕 RESET 上实际电压正确。厂家模块资料的引脚定义列有独立 IOVCC、VDD、RESET；教程扩展板使用 VCC_3V3。需确认供电和复位从扩展板到 LCD 的实际情况，而不是重复使用背光亮度作为 LCD 供电证据。
- 用户已表示有万用表。现有照片为斜侧面，无法准确辨认测点与元件丝印，已请求先断电并提供扩展板背面、屏幕 FPC 插座周边清晰近照，再给出具体探针位置；尚未取得实体电压读数。

### 2026-10-13：继续诊断，核对用户提供的背面安装照片

- 用户要求继续测试。检查时 `adb devices -l` 列表为空，USB 枚举中也未见板端设备，因此尚未执行本轮显示命令，没有刷写或更改板端配置。
- ST7701S 手册提供 `ALLPON (0x23)`、`ALLPOFF (0x22)` 和正常显示恢复 `0x13`，可作为后续屏幕自身全白／全黑模式诊断，与此前 host 彩条不同。本轮仅查阅，尚未发送；发送成功也不能单独证明面板实际接收到命令。
- 用户随后提供展开后的扩展板与主板背面照片。照片可见宽排线连接主板 DSI 插座，窄排线连接触摸插座，屏幕显示与触摸排线分别连接扩展板下方对应插座。未从照片发现明确的插错接口，但接点方向、插入深度和锁扣压紧状态不能仅凭此角度确认。
- 重新查阅官方教程“外壳 → 组装 → 第三步”：主板与扩展板之间使用 **0.3 mm 间距 31 PIN 同方向 MIPI 排线**及 **0.5 mm 间距 6 PIN 同方向触摸排线**。需要核对实际两端裸露接点是否同侧，以及与插座接点是否对应；不能仅用排线商标朝向判定。
- 接下来需在断电状态核对排线与锁扣，安装完成后恢复供电和 ADB，才能继续软件诊断。照片判断不代替线路导通和供电／复位电压测量。

### 2026-10-13：重新拔插 MIPI 排线后实体显示成功

- 用户反馈：“现在有显示了，刚才重新拔插了一下mipi的线”。这是本次实体出图的直接确认，区别于此前仅有 framebuffer 截图或 DRM 软件状态。
- 用户随后进一步确认：“现在是正常的了”。本轮空白画面问题已解决；该确认未单独说明触摸或断电重启验证结果。
- ADB 已重新在线，序列号 `a4aaebe90219e215`。只读检查内核为 `6.1.141 #4 SMP Mon Oct 12 22:54:10 CST 2026`；boot SHA-256 为 `43475c961f8b7da60829ce41e6cacce72d4a089236b5039520ed4cefa132a062`，与 `build/tspi-mipi/official-lane-rate/` 保存的基线镜像一致。
- DRM summary：VP0、DSI-1、Smart0-win0 active，RGB888 输出，480×800p60，实际像素时钟 27 MHz；背光亮度 100。`xiaocai.service`、LightDM 均 active，ROS 2 Humble 环境文件存在，bcmdhd 已加载。
- 本轮没有修改板端配置、发送全白／全黑命令、刷写或重启。出图发生在用户重新拔插 MIPI 排线之后，强烈支持排线连接／接触为相关因素；尚未通过导通测量或替换排线区分具体失效位置，因此不宣称排线损坏已证实。
- 保留当前成功出图的 6.1 基线。触摸实际事件、坐标方向、图片显示以及断电重启后的稳定性尚待确认，暂不记录为已通过。

### 2026-10-13：按用户要求显示全屏笑脸

- 用户要求在屏幕上显示笑脸。板端现有 LightDM、Xorg `:0` 和 XFCE 会话正常，`libX11.so.6` 已安装。使用 Python ctypes 调用现有 X11 绘图接口创建 480×800 的临时全屏窗口，显示深色背景、黄色笑脸；未安装新软件或修改显示参数。
- 主机脚本保存于 `/home/cmx/Documents/xiaocai_robot/build/tspi-mipi/display-demo/smile.py`，上传到板端 `/tmp/xiaocai-smile.py`。普通 ADB 后台启动未持续运行，随后使用 `systemd-run --unit=xiaocai-smile-demo --collect` 启动临时任务，未配置开机自启。
- 任务使用 `DISPLAY=:0`、`XAUTHORITY=/var/run/lightdm/root/:0`。运行后 `xiaocai-smile-demo` 为 active，日志报告已创建 480×800 窗口 `0x4200001`。此为软件启动确认，笑脸的实体显示效果等待用户确认。
- 笑脸窗口当前保留。退出可执行 `adb shell 'systemctl stop xiaocai-smile-demo'`，窗口销毁后回到原桌面；重启同样不会自动启动该任务。脚本在忽略的 build 目录，板端副本在临时目录。

### 2026-10-13：部署 display_node 横屏表情

- 按用户选定的简约青色风格新增 `display_node`，完成八个节点的 AArch64 交叉编译。运行包为 `xiaocai_robot_20261013_232435.tar.gz`，SHA-256 为 `f643a53d1819e77d8678016fadb0668931140f815fa875689357c597cf94c6c3`。
- 按用户要求通过 ADB 部署到 `/home/lckfb/xiaocai_robot`，迁移原有 DeepSeek 密钥和记忆数据库；上一版运行目录保存在 `/home/lckfb/.xiaocai_robot_runtime_backup_20261013_232539_34466`。本次更新机器人运行包和服务配置，内核仍为 `6.1.141`。
- 停止临时 `xiaocai-smile-demo.service`，由正式 `display_node` 创建 X11 全屏窗口。服务使用 `lckfb` 用户、`DISPLAY=:0`、`XAUTHORITY=/home/lckfb/.Xauthority`，启动脚本执行 `xrandr --output DSI-1 --rotate right`。核对 X11 桌面和显示窗口均为 800×480，DSI 面板原生模式仍为 480×800、约 60 Hz。
- `dialogue_node` 根据唤醒、音频播放和对话事件决定 `RobotState`，发布 `/xiaocai_robot/state`；`display_node` 订阅该状态并映射到待机、聆听、思考、说话、免打扰和静音六种静态表情。双方使用可靠、transient-local QoS，显示节点可以读取最近一次状态。六张 800×480 PPM 素材位于板端 `resource/display/`。
- 首次启动发现 `/userdata/log/display_node` 没有写权限。已创建归属 `lckfb:lckfb`、权限 755 的目录并重启服务，文件日志初始化错误消失；部署脚本同步补充所有节点默认日志目录的创建步骤。
- 最终核对 `xiaocai.service` 为 active、enabled，八个 ROS 节点均可发现，自动重启次数为 0。状态话题有一个 `dialogue_node` 发布者和一个 `display_node` 订阅者。显示程序及六张素材的板端 SHA-256 与主机产物一致。
- 取回并检查 800×480 X11 截图，显示黑底青色待机笑脸。截图保存在主机 `build/display_node/board-display.png`，该构建目录不纳入 Git。截图确认软件绘制内容；实体屏幕的横屏方向和表情观感等待用户确认，触摸坐标尚未调整或验证。

### 2026-10-14：部署横屏歌词、对话续听和音乐恢复

- 按用户确认的纯歌词方案扩展 `display_node`：800×480 黑底，显示标题、歌手、上一句、青色当前句、下一句及进度；音乐页面不显示表情。唤醒、思考和回答时显示对应表情。
- `music_node` 在独立线程通过 ncm-cli 获取 LRC，按歌曲编号发布完整时间线；播放状态增加歌曲编号与歌手，按 MPV 播放位置对齐歌词，暂停时冻结进度。无时间歌词显示“暂无歌词”。
- 音乐中唤醒改为暂停并保留原进度；整轮对话结束且最后一句回答实际播放结束后恢复。用户明确暂停、停止、切歌、重新点歌或说“闭嘴”会取消旧的自动恢复。
- 有无音乐均使用相同对话续听策略：机器人追问播完后直接续听，不重复播放唤醒应答。`[speech_input].followup_timeout_sec = 15.0`，范围 1–120 秒；首次唤醒仍使用 `inactivity_timeout_sec = 120.0`。两种监听窗口均有最大时长，背景噪声不会无限续期；超时关闭监听、等待下次唤醒。
- 新增会话状态及语音请求/结果接口。对话结束依据关联 WAV 的真实播放完成结果，不把音频合成完成或唤醒应答播完当作整轮结束。所有八个节点已重新进行 AArch64 编译以匹配新消息接口，修改过的 C++ 使用 clang-format 格式化。
- 显示新增 Xft/FreeType 中文文字绘制。主机 sysroot 补齐所需开发头文件和库链接；板端已有 `libXft.so.2`、`libfontconfig.so.1`、`libfreetype.so.6` 及 Noto CJK 字体，本轮未在板端安装新软件。
- 部署包：`xiaocai_robot_20261014_222042.tar.gz`（约 322 MB），SHA-256：`da08e21eabf1cfcc8946e3691c060360944ba3210e7a1ed930afa645e3b9a0c7`。通过 ADB 更新 `/home/lckfb/xiaocai_robot`，旧运行目录备份为 `/home/lckfb/.xiaocai_robot_runtime_backup_20261014_222144_30260`。
- 部署后确认 `xiaocai.service` active/running、自动重启次数 0，ROS 图可发现八个节点，板端配置包含 15 秒续听上限。核心程序哈希与主机编译产物一致。等待 ROS 发现完成后，`audio/result`（音频节点 → TTS）及 `tts/result`（TTS → 对话节点）均为 1 个发布者、1 个订阅者，`dialogue/state` 有 2 个订阅者。
- 本次只更新机器人运行包。板端内核保持 `6.1.141`，Ubuntu、ROS 2、DeepSeek 密钥及记忆数据库保留；未刷写 boot、修改设备树或重启系统。
- 本轮完成编译和启动状态核对，尚未进行实体语音追问、超时恢复及歌词同步播放的交互验证；实体效果待后续使用确认。


### 2026-10-14：网易云授权、播放超时和 Wi-Fi 图标

- 23:06 运行包 `xiaocai_robot_20261014_230611.tar.gz`（SHA-256 `7b84d2101425904d02f0d59b13acf8fadcfa04aa4c4966cf5d3acc1e34e908f7`）部署了 Wi-Fi 常驻状态、点歌搜索时思考表情和屏幕扫码登录。账号失效时由 `MusicLoginManager` 调用官方 CLI 生成登录链接，屏幕本地生成二维码；用户在手机确认后自动关闭登录页。Wi-Fi 只反映链路、IP 和信号强度，不代表外部服务一定可用。
- 用户扫码确认后，`login --check` 已返回登录成功，歌曲搜索返回 `code=200`，30 条结果中有 10 条允许播放。但实际点歌仍报后台超时，说明恢复登录并未解决播放链路。
- 逐步跟踪本机安装的 `@music163/ncm-cli 0.1.7`：歌曲地址请求需要先查询公网 IP，然后签名请求歌曲详情，后台固定的 3 秒超时会在流程完成前使播放控制进程退出。诊断中还有独立启动的后台未继承 `ALSA_CONFIG_PATH`，MPV 报 `Playback open error: No such file or directory`，找不到 `xiaocai_speaker`；该错误来自独立诊断环境，不能推断所有原先失败均由音频配置造成。
- 用正确音频环境和延长等待的临时诊断方式已实际播放成功：MPV 的 `idle-active=false`、`pause=false`，`audio-out-params` 显示 ALSA 双声道输出，播放位置由 35.2905 秒增长到 37.29575 秒。该阶段确认软件音频输出和播放时钟，未用命令回显代替播放状态。
- 正式修复保存为 `music_node/assets/ncm_player_compat.cjs`，运行包放在 `resource/music/`。启动脚本通过 `NODE_OPTIONS` 预加载；仅匹配 `@music163/ncm-cli 0.1.7`，将其 3 秒计时调整到 20 秒，其他程序及版本保持原计时。未覆盖安装的 CLI、未删除登录凭证；后续升级 CLI 时需重新评估这个适配。
- 音乐节点每次最多等待 25 秒并确认 MPV 音频输出已初始化，失败最多重试一次。首曲就绪后立即开始播放，不再暂停等待五首后续歌曲入队；队列准备期间仍发布播放进度和歌词状态。准备队列时先循环当前歌曲，完成后切到整队列循环。
- 根据用户要求，Wi-Fi 改为右上角纯图标，不显示“已连接”等文字；青色弧线表示信号强度，未连接或接口不可用为灰色图标加斜线。表情、歌词、扫码页面均保留图标。
- 新包 `xiaocai_robot_20261014_232626.tar.gz`（约 322 MB），SHA-256 `ad0e5f1ba2509a117c4035ac675aef4412937088927663d3bd25569f7199b7c3`。音乐和显示节点完成 AArch64 增量编译，修改的 C++ 已 clang-format；部署前关闭带临时诊断钩子的后台，使正式服务重新创建带统一音频配置的播放后台。

- 正式部署于 23:28 完成，旧运行目录备份为 `/home/lckfb/.xiaocai_robot_runtime_backup_20261014_232721_45555`。新音乐程序、显示程序及兼容适配文件的板端 SHA-256 与主机产物一致；`xiaocai.service` 为 active，自动重启次数 0。后台及 MPV 已确认继承正式 `NODE_OPTIONS` 和 `ALSA_CONFIG_PATH`，临时诊断后台已关闭。
- 23:29:22 通过 `/xiaocai_robot/speech/text` 的正常意图入口发出一次“播放两只老虎”；23:29:39 音乐节点确认开始播放，23:29:47 完成 32 条时间歌词加载。MPV 返回非空闲、未暂停、扬声器为 `alsa/xiaocai_speaker`，进度正常增长；X11 实际截图显示歌曲、三行歌词、播放进度和右上角纯 Wi-Fi 图标。截图位于主机 `/tmp/xiaocai-wifi-icon.png`，不纳入运行包。23:30:13 后续队列准备完成，播放器仍持续播放，期间歌词进度保持更新。该结果确认正式点歌及显示链路，实体音量感受由用户现场确认。
- 本轮只更新机器人运行包，未重启系统，内核仍为 `6.1.141`，Ubuntu、ROS 2、登录凭据、DeepSeek 设置和记忆数据库保留。

## 官方屏幕配置参考

立创的手机综合项目文档针对 3.1 寸屏给出了以下配置。接入前要先将其与本机实际 SDK、扩展板和屏幕型号核对，不能直接把旧补丁覆盖到当前系统。

- MIPI DSI 使用两条数据 lane。
- 分辨率为 `480 × 800`，参考像素时钟为 `27 MHz`。
- 参考时序：H 前肩/同步/后肩 `32/4/32`；V 前肩/同步/后肩 `9/4/3`；同步、DE、像素时钟极性均为 0。
- 面板需要完整的 MIPI 初始化序列，不能只启用 DSI 控制器。
- 3.1 寸屏幕扩展板通过 I2C1 上的 GP7101（7-bit 地址 `0x58`）控制板载背光电路。立创提醒泰山派板载背光输出约 `110 mA`，该屏背光最大约 `25 mA`，不要把屏幕背光直接接到板载背光输出，以免损坏屏幕。
- 触摸屏驱动是独立部分；先点亮显示可暂不启用触摸，但后续需单独核对触摸 IC、I2C 地址与驱动补丁。

官方参考：

- [立创泰山派手机综合项目：3.1 寸屏幕、背光、设备树和补丁说明](https://wiki.lckfb.com/zh-hans/tspi-rk3566/project-case/fat-little-cell-phone/mobile-phone-integrated-project.html)
- [立创泰山派 Linux Kernel 6.1 SDK 编译说明](https://wiki.lckfb.com/zh-hans/tspi-rk3566/sdk-compilation/kernel6-1-linux-compilation.html)
- [立创泰山派资料下载中心](https://wiki.lckfb.com/zh-hans/tspi-rk3566/download-center.html)
- [立创泰山派 MIPI 屏幕选型与调试](https://openkits-wiki.easyeda.com/zh-hans/tspi-rk3566/project-case/fat-little-cell-phone/screen-selection-debugging.html)

## 后续实施步骤

1. 当前屏幕已出图，接下来核对 `my_touch` 的实际触摸事件与显示稳定性。
2. 检查触摸坐标方向与 800×480 横屏设置是否一致；`display_node` 表情显示应用已部署。
3. 如果后续修改内核或设备树，保留当前 boot 与 Wi-Fi 模块备份，并重新记录镜像哈希和板端验证结果。

## 尚待解决

- `my_touch` 已绑定地址 `0x38` 并注册 input 设备，但触摸型号及实际坐标、事件尚未验证，横屏后的触摸坐标仍需核对。
- 重新拔插排线后，用户已确认实体显示；断电重启后的稳定性、显示方向及长期运行情况尚未确认。
