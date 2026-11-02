# ECHO MINI newfw：源码架构与改造导航

本文面向后续维护和功能改造，按代码实际组织说明固件的启动、模块边界、数据流、内存约束、构建验证和主要风险。结论以仓库 `main` 的源码、头文件、链接脚本和构建脚本为准；硬件行为仍应区分源码推导、仿真结果和真机验证。详细硬件逆向背景见根目录 `ETUDE.md`（法文，工作记录）；本文不取代各专题文档。

> **当前源码基线：**以阅读本文时 checkout 的 Git commit 为准。使用 `git rev-parse HEAD` 记录实际基线；不要把本地构建记录中的旧 commit SHA 当作当前源码版本。
>
> **真机安全：**修改更新器、eMMC 写入、USB RW 或启动代码之前，先阅读源码和 `ETUDE.md` 的对应段落。本文介绍的是危险路径的代码位置，不是刷写操作指南。未经独立恢复方案和授权，不在设备上尝试固件安装或存储写操作。

## 1. 项目边界与目录

| 路径 | 职责 | 改造时先看 |
|---|---|---|
| `fw/src/` | 主核 A 的裸机固件：板级驱动、UI、浏览器、队列、播放器、存储、更新、USB | 对应 `.c/.h`、`ui_priv.h`、`sys.h` |
| `fw/hifi/` | HiFi 核 B 的运行时、各格式解码器适配、硬件加速调用 | `rt.c`、`rt.h`、`codec_*.c`、`src/hifi_proto.h` |
| `fw/tools/` | 固件构建/打包、检查、仿真、镜像和字体生成、日志解析 | `Makefile` 与脚本头部说明 |
| `third_party/` | 随源码分发的第三方解码器与依赖 | 对应目录许可证、`THIRD_PARTY_NOTICES.md`、Makefile 规则 |
| `tools/` | 面向 IDB/扁平镜像/外设映射/交叉引用的研究工具 | 工具 docstring；部分工具要求本地 stock image 或辅助模块 |
| `docs/` | DAC、HIFIACC、MP3 加速器、USB 音频、fuzzing 和本地构建专题 | 先查专题文档，避免重复实现已有能力 |
| `ETUDE.md` | 长篇法文逆向和实验记录；不是简短 API 文档 | 查对应章节号、镜像版本和验证条件 |
| `MANUAL.md` | 用户可见按键、菜单、安装说明 | UI 行为变化时同步检查 |
| `ROADMAP.md` | 已实现、待测、待办事项 | 功能完成后据证据更新，不用它替代测试 |

主核源文件大致分层：

- **启动与系统：**`start.c`、`main.c`、`board.c`、`regs.h`、`sys.h`。
- **显示与输入：**`lcd.c`、`font.c`、`keys.c`、`ui.c`、`uidraw.c`、`menu.c`、`playscreen.c`、`settings.c`、`browser.c`。
- **媒体选择与播放：**`fat.c`、`tags.c`、`queue.c`、`playback.c`、`player.c`、`mp4.c`、`resume.c`。
- **音频与协处理器：**`audio.c`、`hifi.c`、`hifi_proto.h`；核 B 实现在 `fw/hifi/`。
- **介质和连接：**`emmc.c`、`mshc.c`、`sd.c`、`usb.c`、`msc.c`、`usbmode.c`、`uac.c`。
- **更新/诊断：**`update.c`、`install.c`、`log.c`、`probe.c`、`charge.c`、`rtc.c`。

`fw/src/ui_priv.h` 是 UI 子模块共用的内部接口和状态汇总点。新增屏幕行为时先确认现有 screen/menu/key 路由，避免再造一个旁路状态机。模块间公开接口优先放各自头文件；寄存器定义集中在 `regs.h`，不要在业务文件复制裸地址常量。

## 2. 启动链与主循环

### 2.1 启动链

1. `fw/src/start.c` 提供 `.vectors`、`Reset_Handler` 和 fault/default handler。链接脚本把向量表放在启动加载器期望的位置。
2. `Reset_Handler` 复制初始化数据、清零 BSS、设置处理器状态后进入 C 端初始化；异常路径最终进入 `fault_screen()` 或默认中断处理。
3. `fw/src/main.c` (`main()`) 初始化板级时钟/定时器、看门狗、LCD、键、日志、电池和 eMMC/卷状态，处理低电量等启动边界，然后进入 `ui_main()`。
4. `ui.c` 的 `ui_main()`：服务电池/电源/SD 检测、屏幕熄灭和恢复、按键事件、当前 screen 绘制，并持续调用后台播放服务。
5. 长读或 UI 工作期间，`ui_background()` 给播放管线让出服务机会；不要在新增的长循环中饿死音频、USB、看门狗或电源事件。

启动行为的细节必须以 `start.c`、`main.c`、`link.ld` 和 `ETUDE.md` 的加载器章节联合判断。改动向量表、初始化顺序、时钟或内存映射时，普通 host test 不足以证明硬件正确。

### 2.2 UI 事件路由

`ui_main()` 维护当前 screen 状态；`menu_open()` 将主菜单选择路由到浏览器、播放页、设置页、USB Mass Storage、USB DAC、安装器或系统诊断。子屏幕通常由各自模块提供绘制和 key handler，`ui_priv.h` 声明共享状态/接口。

用户可见功能的典型路径是：

`按键扫描 (keys.c) → key event → ui.c 当前 screen 分发 → 模块状态变更 → 绘制函数 → LCD (lcd.c)`

修改按键时同时检查短按/长按/重复/组合键和插拔事件；修改绘制时注意 LCD 是 320×170 的设备面板，字体/裁切/滚动由 `lcd.c`、`font.c` 和 `uidraw.c` 共同承担。手册中已有按键约定时要同步 `MANUAL.md`。

## 3. 音乐浏览、队列与播放

### 3.1 存储到目录项

- `emmc.c` 和 `mshc.c` 负责内部 eMMC 及 SD/MMC host command/data 操作；`sd.c` 使用 host controller 访问 microSD。
- `fat.c` 实现内部存储 FAT32 与卡片 FAT32/exFAT 的挂载、目录遍历和文件读取。`fat_vol`、`fat_file`、`fat_dirent` 带有 volume、cluster、size 等信息；目录名以 UTF-8 表示。
- `browser.c` 负责目录浏览和列表；内部目录/队列共享的数据布局由 `ui_priv.h` 中 `entry`、`listing`、`dir_buf` 等结构表达。exFAT contiguous 状态通过 `F_CONTIG`/`contig` 一路传递，不能在复制目录项时漏掉。
- `tags.c` 按 codec/container 读取 ID3、Vorbis comment、FLAC、WAV/AIFF、MP4、APE 等标签，并解析 ReplayGain。容器定位与音频 sample table 由 `mp4.c` 处理。

目录/队列容量和内存地址是链接脚本的一部分，不应只增加数组大小：检查 `DIR_MAX`、`Q_DIRS`、`struct dir_buf`、`.dirbuf`、`.queuebuf` 和链接脚本 ASSERT。

### 3.2 播放状态与主核 A

- `queue.c` 管理目录或目录树建立的播放队列、排序、循环/单曲/随机模式及前后曲选择。
- `playback.c` 是 UI/队列与低层播放器之间的协调层：打开队列曲目、codec 选择、跳曲/停止/暂停、无缝衔接、ReplayGain、音量和 seek。
- `player.c` 是 A 核播放管线：读取输入数据、向 B 核输入 ring 供数、轮询控制状态、搬运输出 PCM 到 DAC ring、处理 seek/end/fault，并依据负载调整 B 核时钟。
- `audio.c` 控制两颗 CS43131、I2S、DMA 和 SYSRAM PCM ring；其详细上电序列、接线和采样率切换见 `docs/CS43131.md`。

项目的主要播放路径：

`浏览器选择目录/曲目 → q_from_dir()/q_from_tree() → play_index()/play_step() → q_open() → player_open() → HIFI 核解码 → player_service() → audio_write() → I2S/DMA → DAC`

结束一曲时需要区分“停止并丢弃尾部”“跳曲并衔接”“队列结束后排空 DMA”；`PLAY_CHAIN`、`player_chain_end()`、`player_drain_service()` 和 `STOP_NOW/STOP_SKIP/STOP_DRAIN` 表达这些不同语义。改播放模式或队列完成逻辑时要验证队列索引、当前解码曲目、当前实际听到的曲目及 DAC ring 的尾部，不要只看 UI 图标变化。

### 3.3 核 B codec 协议

共享 ABI 定义在 `fw/src/hifi_proto.h`，两核实现分别在 `player.c`/`hifi.c` 与 `fw/hifi/rt.c`/`codec_*.c`。大体过程：

1. `hifi_load_own(codec)` 将对应 codec image 装入 HiFi RAM；必要时 `hifi_prepare()`/`hifi_start_own()` 控制电源、时钟和 reset。
2. B 核 runtime 初始化 `hp_ctl`，写入 magic/version/codec/state；A 核等待 `HP_READY`。
3. A 核发送 `HP_CMD_OPEN`，并通过文件输入环提供压缩数据。B 核解析格式，进入 `HP_FORMAT` 并填充采样率、声道、位深、帧大小、总时长等字段。
4. A 核配置音频输出并发送播放命令；B 核解码，把 PCM 写到输出环；A 核消费 PCM 并写入 DMA ring。
5. B 核以 `HP_END` 或 `HP_ERROR` 结束；双方通过序号/ack、ring 游标和 doorbell 协调。seek、重读输入、Ogg page hint、MP4 sample table 都有专门字段/路径。

协议修改要同时检查：结构字段偏移和对齐、A/B 双端定义、ring 环绕、可用空间/guard、cache/共享内存假设、命令序列和错误终态。关键常量包括 `HP_IN_BASE`/`HP_IN_SIZE`、`HP_OUT_BASE`/`HP_OUT_SIZE`、`HP_OUT_GUARD` 和 `struct hp_ctl`。新增 codec 通常涉及 `codec_*.c`、runtime/codec ID、`Makefile` 编译链接、`mkpack.py` codec 表、A 核文件类型判别与 seek/tags，以及测试素材/模拟器支持。

各 codec 和硬件加速器的内部实现较专门：阅读 `docs/HIFIACC.md`、`docs/MP3ACC.md`、`ETUDE.md` 对应 codec 章节。不要把加速器仿真通过等同于真机实时性能通过。

## 4. Settings、恢复状态和日志

- `settings.c` 定义当前设置、菜单编辑和持久化字段编码/校验；设置块经 `settings_put()`/`settings_get()` 与 resume 状态一起保存/恢复。
- `resume.c` 保存/恢复曲目、位置、队列/目录状态；启动时由 UI 选择恢复策略。新增持久字段时考虑版本兼容、校验和、旧状态缺省值及掉电中断写入。
- `log.c` 提供 RAM ring、日志文件挂接/刷新、启动注记和状态存取。设备诊断导出名为 `NEWFWLOG.TXT`，请将日志当潜在隐私数据处理，发布前脱敏。
- `rtc.c` 提供实时时钟与 FAT 时间转换；USB 主机写入后的时钟同步在 `usbmode.c` 路径中处理。

settings/resume/log 都涉及持久状态或设备数据，host test 的成功只能证明所覆盖的序列，不表示掉电、损坏介质和升级中断恢复正确。

## 5. 固件镜像格式生成和安装路径

### 5.1 构建生成

`fw/Makefile` 的依赖关系大致为：

`主核 C/汇编 → fw.elf`；`每个 HiFi codec + 第三方 decoder → hifi_*.elf`；`mkpack.py → codecs.bin`；`mkfusion.py → font.bin/font_ascii.c`；`mkimg.py → HIFIEC70.IMG`。

`mkpack.py` 汇集编号 codec 的核 B image；`mkimg.py` 按固件镜像布局、日期字段、主核代码、codec pack 和字体生成最终 image。源码通常用固定输出 `fw/build/`；改动构建路径可用 `BUILD=dir`、最终文件可用 `OUT=path`。构建输出已由 `.gitignore` 忽略，生成镜像和中间文件不应提交。

本机 Debian 的实际依赖/命令、构建 hash 和 `make check` 结果见 `docs/LOCAL_BUILD.md`。编译产物的 hash 会受源码 commit 日期及工具链版本影响，比较之前先核对两者。

### 5.2 设备更新器路径

- `install.c` 从内部卷枚举 `.IMG` 文件，显示 firmware 信息并控制确认流程。
- `update.c:fw_check()` 检查文件头 `RKnanoFW`、header copy、尺寸/对齐和尾随 CRC；这表示设备端安装器执行了结构与 CRC 校验，**不能据此推断存在或不存在数字签名**。
- `fw_install()` 写入 firmware copy，逐块回读比较，并按实现顺序写入/核对 header。
- 写入落到 eMMC firmware 区域，通过 `emmc_write_fw()`；这与用户卷写入是不同接口和区域。
- `install.c` 在安装前还有电量/电源条件和交互确认；成功后执行重启。

这条路径会改变设备 firmware 区。只在静态代码研究和受控构建中改造；除非另有明确授权、已验证的恢复方案和可接受的真机测试安排，否则不执行设备安装。

## 6. USB 两条功能路径

USB 底层在 `usb.c`，定义 device/class descriptor 与控制/数据传输状态；主菜单把两个功能分开：

### USB Mass Storage (MSC)

`usbmode.c → msc_start()/msc_poll() → usb.c bulk endpoints → emmc.c`

MSC 通过 SCSI transparent command set 暴露 eMMC user volume 的 sector 范围。只读/读写由菜单模式决定；写入必须经过 `emmc_write_user()` 的范围 fence。MSC 不应访问 firmware 区。改动此处要验证 LBA 范围、SCSI command、eject/suspend/disconnect、写入围栏和重新挂载行为。`docs/USB_AUDIO.md` 不涵盖 MSC；读 `msc.c`、`usbmode.c`、`emmc.c`。

### USB Audio Class (UAC)

`menu.c → uac_mode() → usb_start_class() / isochronous endpoints → UAC packet/feedback → audio.c`

`uac.c` 实现 USB Audio class descriptor、控制请求、采样率/位深 profile、同步反馈和 I2S/DAC 协调。细节和已测主机见 `docs/USB_AUDIO.md`。USB IRQ 与主循环之间共享状态需使用现有 IRQ block 协议；不能在 isochronous packet path 中加入不可预测的长操作。

## 7. 板级硬件和内存约束

### 7.1 板级代码

`board.c`/`regs.h` 管时钟、GPIO、SARADC、电源键、按键 ladder、timer、delay、watchdog、reboot/power off。`lcd.c` 管面板初始化、VOP/LCD 时序、颜色/文字绘制。`emmc.c`/`sd.c`/`mshc.c` 管介质和控制器；`audio.c` 管 I2S/DMAC/DAC；`usb.c` 管 DWC2 USB。

寄存器位宽、write-mask、时钟 gate/reset 顺序和延迟是硬件语义的一部分。改动寄存器代码时交叉阅读 `regs.h`、对应驱动注释、链接/平台文档和真机/仿真证据；不要仅凭寄存器地址“看起来合理”修改。

### 7.2 链接脚本

主核：`fw/link.ld`；HiFi 核：`fw/hifi/link.ld`。

主核链接脚本说明了以下关键区（准确边界以脚本为准）：

- 主核代码 overlay 位于 `0x03050000` 一带；启动数据/向量区从 `0x03004000` 开始。
- `.bss` 必须在 `0x03020000` DAC ring 区之前结束；脚本含 ASSERT。
- `.dacring` 固定在 `0x03020000`，容量上限 `0x30000`（192 KiB）。
- `.queuebuf` 从 `0x03070000` 开始、上限 `0x8000`（32 KiB）。
- `.dirbuf` 从 `0x03078000` 开始、上限 `0x18000`（96 KiB），与 USB/更新阶段共享的缓冲区设计相关。
- `.pmusram` 从 `0x03090000` 起，日志区和队列目录预算由 ASSERT 限制。
- 主核栈/数据和加载器运行区彼此有固定关系；细节见 `fw/link.ld` 顶部注释和 `ETUDE.md`。

HiFi 核链接脚本把代码放在其视角的地址 0（HIGHRAM0），数据/BSS 放 HIGHRAM1，并预留控制块空间。跨核共享地址不能只改一侧。

任何静态 buffer、队列上限、字体或 decoder 增长都应检查 map、BSS 余量、codec RAM 和 ASSERT。不要用单纯“链接成功”代替空间预算复核。

## 8. 构建与验证层次

仓库提供不同强度的检查，结果必须分开报告：

1. **编译：**在 `fw/` 执行 `make`，生成 `build/HIFIEC70.IMG`。
2. **快速检查：**`make check` / `sh tools/check.sh`；脚本包含 warnings-as-errors 构建、BSS 余量、log format 检查、`log_test`/`msc_test`，可选 Ruff/pyflakes 与 codespell（未安装时会跳过）。
3. **完整检查：**`make check-full`；在快速检查基础上构建多个测试宏变体并构建 fuzz harness。先读 `fw/tools/check.sh`、fuzz build 脚本，确认额外依赖和资源后再运行。
4. **仿真：**`fw/tools/emu_fw.py` 使用 Unicorn 执行新的整机固件模型；`tools/emu_host.py` 用模拟主核驱动 HiFi codec。它们是指定模型的模拟，不等同于所有真实外设/时序已验证。
5. **真机：**设备启动、安装、按键、音频、存储、USB host compatibility 等各自是更高层的验收。本文和本地 build record 不代表当前改动通过真机测试。

改造后按变更层次选择验证：纯解析/算法可先 host test；A/B 协议需两侧构建和 emulation；链接布局必须检查 map/ASSERT；更新器/USB/存储/电源/音频外设需额外审查硬件副作用，真机测试需单独安排。

## 9. 字体效果与字体改造

### 9.1 当前字体和显示路径

- 字体为 TakWolf 的 **Fusion Pixel 12px proportional**，SIL Open Font License 1.1；当前默认 `FONT_LANG=ja`。BDF 声明 pixel size 12、字形框 12×16、ascent 13、descent 3。比例字体每个字符 advance 可不同。
- 固件中不是每次显示时解析 BDF/TTF：`fw/tools/mkfusion.py` 在构建阶段把 BDF 转成紧凑的 `FPXF` 字库和内置 ASCII C 数组；`mkimg.py` 把字库打进固件 image。运行时 `font.c` 从 FW1/FW2 读字库索引，缓存 256 条字形记录，并以 16 个字符为一组按需从 eMMC 读取。
- `lcd.c` 解码 UTF-8 码点，读取每个字形的位图，再用 LCD 像素写入逐点绘制。当前字形是单色 bitmap；传入的前景/背景色在绘制时决定颜色。代码有 `sc=1/2` 的整数倍缩放路径，但它只是放大已有像素，不会增加轮廓细节。
- 字库有 37,690 个 glyph，当前 BDF 源文件约 5.1 MiB，构建后的字体记录约 1,000 KiB。完整索引表约 34 KiB 常驻 RAM，字形缓存和其他静态区也占用受限的 SRAM；`.bss` 必须留在固定 DAC ring 地址之前，见 `fw/link.ld` 和 `fw/src/font.c`。
- 当前仓库只保留日文 BDF。构建参数列有 `zh_hans`、`zh_hant`、`zh_hk`、`zh_tw` 等变体，但相关 BDF 不在 checkout；若要构建指定地区字形，需先按上游字体的许可证取得并纳入对应字体源文件，再验证许可和重现命令。当前 ja 字体包含汉字/假名等码点，缺字时 `font_glyph()` 回退到 ASCII 问号。

### 9.2 做出更美观字体的可行方案

推荐先保留运行时 bitmap 架构，只换构建输入/栅格化流程：

1. 选择明确授权可嵌入/再分发的 TTF/OTF 字体及语言变体。
2. 在主机侧用 FreeType/fontTools 等栅格化为合适字号和灰阶深度；可先做黑白 1-bit，再评估 2-bit/4-bit 灰阶抗锯齿。
3. 生成当前 `FPXF` 记录格式或一个版本化的新格式，包含 glyph advance、bearing、尺寸、bitmap 和 Unicode 映射；改 `mkfusion.py`/新 converter 与 `font.c`/`lcd.c` 的解析保持一致。
4. 明确只打包 UI 所需字符集、常见 CJK 字符集或全量字库。完整 CJK 字体会增加 image 大小；按需从 eMMC 读能降低常驻 RAM，但会增加首次遇到字形时的存储读取延迟。
5. 验证字形覆盖、fallback、字体缓存命中/缺失、UTF-8、行高/裁切、滚动和长文件名；检查字体 blob 大小、RAM map、`.bss` 余量及 LCD 刷新时间。

若只想改善外观，离线栅格化是改动面和运行期开销较小的方向。运行时 TrueType/OpenType 渲染需要额外的字体解析、曲线栅格化、缓存/内存管理和排版逻辑；在 320×170 LCD 与当前裸机 SRAM/CPU 约束下，先做原型并测 RAM、构建体积和每帧绘制耗时，再决定是否采用。它不是当前代码已有的功能。

`lcd.c` 当前通过逐像素写面板传输绘制黑白 bitmap。即使上游提供灰阶 glyph，仍需设计 LCD 的灰阶呈现（例如空间抖动/子像素时序或多次写入），验证视觉效果和总线吞吐；不能只换字体文件就宣称有抗锯齿。

## 10. 仿真器：可先在 PC 验证什么

本项目不需要所有改动都每次上设备测试。仓库有两种 Unicorn 模拟路径，但依赖和模型范围不同。

### 整机固件模拟

`fw/tools/emu_fw.py` 加载已构建镜像，模拟 IDB loader 的加载方式，并执行主核；实现 LCD/panel、按键、eMMC 磁盘镜像、可选 SD 镜像、部分 ADC/GPIO/I2C/RTC 与 DAC 行为。启用 `fw/tools/emu_coreb.py` 时可运行实际新固件的 HiFi 核 codec。

仓库提供的基本流程（只使用你有权使用的 boot dump/测试素材；boot dump 不得提交）：

```sh
cd fw
make
# 准备磁盘镜像；mkdisk 的 --boot 需要本地 BOOTDUMP.BIN 或 ECHO_BOOTDUMP
python3 tools/mkdisk.py /path/to/test-disk.img build/HIFIEC70.IMG \
  --boot /path/to/BOOTDUMP.BIN /path/to/test-audio.flac=TEST.FLAC
python3 tools/emu_fw.py build/HIFIEC70.IMG --disk /path/to/test-disk.img \
  --keys 3000:MENU,3150:NONE --shots 5000:/path/to/screen.png --dac /path/to/heard.raw
```

- `fw/tools/emu_fw.py` 的**当前文档帮助/源码描述**列出 `--keys`、`--shots`、`--disk`、`--sd`、`--ms`/`--steps`、LCD trace，以及 `--adc1`、`--power`、`--sel`、`--fast`、`--sd-out`、`--adc0-at`、`--gpio-at` 等参数；`--png` 会在模拟结束时保存 framebuffer，`--pcm` 捕获 B 核输出、`--dac` 捕获 DMA 消费的数据，`--dump-disk` 另行导出模拟写入后的磁盘副本。查看脚本文件头与当前版本参数实现以确认准确语法。

当前环境在本次文档核查时缺少 Python `unicorn` 模块，直接运行脚本报 `ModuleNotFoundError: No module named 'unicorn'`；安装/创建隔离 Python 环境后再运行。除此之外，整机磁盘模拟的 `mkdisk.py` 还需要可用的设备 boot-area dump；该文件属于设备数据，保存在本地私有目录，不能进 Git。只想做 UI/启动片段实验时，检查 emulator 是否支持不带 disk 的运行模式；播放/存储场景则需要有效的镜像输入和测试媒体。

### HiFi codec 主机模拟

`tools/emu_host.py FW.IMG MODULE FILE OUT.raw [--codec N] [--seconds S] [--trace]` 用 Unicorn 从 HiFi 模块 reset vector 启动，提供模拟主核 mailbox 和文件服务，捕获 decoder PCM。它适合 codec 协议、单 codec 解码和 PCM 对照；不是完整主核 UI/存储模拟。它同样需要 Unicorn Python 依赖以及对应固件模块和测试音频文件。

### 仿真边界

- 适合：UI/按键流程、文件/队列逻辑、格式解析、A/B mailbox、PCM 输出一致性、可注入的异常路径和回归场景。
- 不足以代替：真实板级时序/电气、所有寄存器副作用、eMMC/SD 物理差异、真实 LCD 刷新成本、DAC 声音质量/底噪、USB host 互操作、时钟负载、功耗、掉电行为及实际升级/恢复。
- 某些加速器在 emulator 中是软件模型，不等于硅片逐周期一致；源码研究已有案例说明 emulator 本身也可能有缺陷。因此需区分模拟通过、主机单元测试、构建通过和设备验收。
- 推荐节奏：静态检查/host test → emulator 场景 → 真机只验证 emulator 无法覆盖的板级风险。真机测试前先有可恢复的 stock firmware、确认设备型号/容量、评估失败后恢复手段，并单独安排允许的写入操作。

## 11. 常见改造入口索引

| 目标 | 起点 | 必须追到的下游 | 专题/验证 |
|---|---|---|---|
| 新增音频格式 | codec 探测（browser/playback/player） | `hifi_proto.h`、A 核数据供给、B 核 decoder、Makefile、`mkpack.py`、seek/tag | `tools/emu_host.py`、codec-specific tests、设备性能验证 |
| 修改连续播放/随机/重复 | `queue.c`、`playback.c` | `player.c` drain/chain、`ui_priv.h` 状态、resume | 队列边界/错误曲/曲尾行为 |
| 新增标签/ReplayGain | `tags.c`、`tags.h` | `playback.c:rg_apply()`、playscreen 展示、字符编码/截断 | 各 container 样本和无标签/坏标签输入 |
| 修改目录/卡片支持 | `fat.c`、`sd.c`、`browser.c` | `fat_dirent` flags、queue/resume、容量/内存预算 | FAT32/exFAT、长文件名、卡拔出路径 |
| 新增设置项 | `settings.c`、`ui_priv.h` | `settings_put/get()`、持久 state/checksum、默认和旧 state | 编解码/恢复测试、断电边界说明 |
| 修改启动/电源/时钟 | `start.c`、`main.c`、`board.c` | `link.ld`、向量/中断、UI/电量状态 | map、警告构建、仿真和单独真机计划 |
| 修改 DAC/采样/音量 | `audio.c`、`player.c` | ring frame size、HiFi 输出协议、PLL/I2S/DMA | `docs/CS43131.md`、多采样率/欠载/真机测量 |
| 修改 USB MSC | `usbmode.c`、`msc.c` | `usb.c` class/BOT/SCSI、`emmc.c` write fence | `make check` 的 host MSC tests、只读优先 |
| 修改 USB DAC | `uac.c` | `usb.c` IRQ/iso、`audio.c`、时钟/feedback | `docs/USB_AUDIO.md`、模拟及真实 host/device 分层 |
| 修改固件容器/安装 | `mkimg.py`/`mkpack.py` 或 `update.c`/`install.c` | 头、CRC、copy、eMMC 区域、加载器 | 镜像静态解析、写入前安全审查；不自动真机刷写 |
| 修改字体/本地化 | `fw/font/`、`mkfusion.py`、UI 文本 | 字体 blob 体积、字形覆盖、map | 字体许可证、目标语言真机显示 |
| 改研究工具 | 根 `tools/` | 私有 stock image/外部模块的依赖说明 | 不要提交 stock image、设备 dump 或本机路径 |

## 12. 修改前的维护清单

1. 记录 `git status --short --branch`、`git rev-parse HEAD`；保留已有工作，不 reset/clean。
2. 搜索功能入口和调用链，不只看同名文件；确认是否有 A/B 两核、多种输出模式或持久状态。
3. 检查 buffer、结构布局、section、链接 ASSERT、堆栈和栈使用；裸机代码不能假定有 OS、malloc 或标准 libc。
4. 搜索已存在的专题文档、ETUDE 对应实验和 ROADMAP 状态；区分源码事实、作者硬件观测、仿真和待验证假设。
5. 在最小影响范围改动；保留第三方 LICENSE/notice，修改 vendor code 前核对许可证和本地改动标记。
6. 先编译再跑匹配的检查；记录跳过的可选工具和实际返回码。
7. 检查构建产物是否被忽略；只提交源码/文档，不提交 `fw/build/`、设备镜像、stock image、私有 logs、用户音频或本机路径。
8. 更新用户文档时区分“源码实现”“本机 build/test”“模拟器验证”“真机验证”，不要用一个 PASS 覆盖所有层次。

## 13. 公开内容与许可证边界

仓库 README 声明实现代码是项目作者的 MIT 许可，但 `third_party/` 和字体各自保留许可证；以 `LICENSE`、`THIRD_PARTY_NOTICES.md` 和每个依赖目录为准。不要把它们合并成单一许可证结论。项目文档描述不包含 stock firmware/SDK 代码；根目录 `ETUDE.md` 对本地研究资料也有明确边界。不要把本机下载的官方固件、设备 dump、私有日志、音乐样本或提取 payload 放入 fork。需要新增代码时，保持来源/许可证可追溯，并检查已有第三方改动与上游差异。

## 14. 相关文档

- `README.md`：项目能力、限制和布局总览。
- `MANUAL.md`：菜单、按键、播放和用户安装说明。
- `ROADMAP.md`：开发状态和后续事项。
- `ETUDE.md`：逆向研究、协议和实验记录（法文，约 3,700 行）。
- `docs/LOCAL_BUILD.md`：Debian 13 本机依赖、构建 hash、`make check` 实测记录；不含设备验收。
- `docs/CS43131.md`：DAC 电路连接、上电和采样率路径。
- `docs/HIFIACC.md`、`docs/MP3ACC.md`：芯片音频加速器与调用/验证。
- `docs/USB_AUDIO.md`：USB Audio profile、feedback、主机差异和已知限制。
- `docs/FUZZING.md`：解析器 fuzz 工作流。

最后更新时应同步核对：当前 commit SHA、文件/符号是否移动、linker layout 是否变化，以及上游新版本是否重构了调用链。本文是仓库导航，不是 API 保证；最终以当前源码和实际验证结果为准。
