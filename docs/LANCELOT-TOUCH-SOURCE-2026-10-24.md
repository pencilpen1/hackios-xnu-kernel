# Lancelot：Shockwave 17.0 触摸参考源码与 Android 补采

## 下载结果

用户提供当前 Android 内核源码：
`https://github.com/MrShockWAVEog/shockwave_kernel_xiaomi_mt6768/commits/17.0/`。
本次锁定分支快照，避免使用会变化的分支URL下载文件：

- commit：`6dd120fc1ab487a5ebc6cdcf4ab605d2b43248f7`
- tree：`840cb76830615e7ec4ee5e2aee2e2ff229d6f5bb`
- 保存：`local/lancelot-touch-source-20261024/source/`
- **135 个文件，4,597,028 bytes；全部通过镜像站下载并核验。**

这是一组按触摸依赖选择的参考文件，**不是完整 Android 内核构建树**。下载阶段没有执行
厂商代码或操作设备；随后按用户确认还原Android Boot并完成补采，结果见后文。
不能从源码常量假定设备固件版本。

每个文件均按 GitHub 固定提交目录元数据核验长度和 **Git blob SHA-1**
（包含 `blob <length>\0`），另记录 SHA-256；HTML错误页、截断、错hash会拒绝。
GitHub全仓库递归tree实际返回`truncated=true`，因此没有把它当作完整清单；下载工具
使用有界、完整的小目录列表选择文件，逐个核验。未使用待校验的归档或可变分支原始URL。

证据：`source-manifest.json`、`fetch-plan.json`、合并路由/校验记录`download-audit.json`，
以及 ignored `local/lancelot-touch-source-20261024.*.log`。
复现工具：`tools/fetch-lancelot-touch-source.py`；4项校验/路径测试通过。
`--resume`仅接受本工具同一repo/commit的输出，并复验已有文件，不覆盖不同内容。

## 真正匹配的是 NT36672，不是通用 HOSTDL_SPI

先前仅凭 NVT-ts/SPI0.0 推定通用`NT36xxx_HOSTDL_SPI`目录；解压并核验设备已保存的
`local/lancelot-20261022/kernel.config.gz`后，确认：

```text
CONFIG_TOUCHSCREEN_MTK_NT36672=y
CONFIG_TOUCHSCREEN_MTK_FT8719P=y
# CONFIG_TOUCHSCREEN_NT36xxx_HOSTDL_SPI is not set
CONFIG_SPI_MT65XX=y
CONFIG_PINCTRL_MTK_PARIS=y
CONFIG_PINCTRL_MT6768=y
CONFIG_MTK_PMIC_CHIP_MT6358=y
```

因此已补齐并以 **`drivers/input/touchscreen/mediatek/NT36672/`** 为主要依据。
通用 HOSTDL_SPI只作对照；FT8719P只保存必要的备选匹配参考，不表示设备实际使用FocalTech。
实机旧`drivers.txt`仍确认`NVTCapacitiveTouchScreen`、SPI0.0绑定`NVT-ts`。

NT36672目录的全部9个文件，其Git blob/长度与以前交接记录的源码commit
`412a5a948c67309ca5f6ecbf1450dd6caf5cfc1b` **全部一致**。
这消除了该目录的分支漂移疑问，但不代替下一轮运行内核配置/身份验证。

### 主要文件

- `NT36672/nt36672.c`：SPI封装、芯片trim匹配、初始化、点包解析、休眠/恢复。
- `NT36672/nt36672.h`、`nt36672_mem_map.h`：配置、事件/内存映射和ID表。
- `NT36672/nt36672_fw_update.c`：固件选择、下载、校验和恢复。
- `nt36672_ext_proc.c`、`nt36672_mp_ctrlram.c/.h`：用于审查信息/厂测接口的安全性，
  **不运行其厂测或固件更新逻辑**。
- 对应Kconfig/Makefile与MTK TPD glue、`tpd.h`、`tp_common`/`hqsysfs`头。
- `drivers/spi/spi-mt65xx.c`、`include/linux/platform_data/spi-mt65xx.h`。
- MT6768/MT6765 pinctrl、Paris、EINT、clock、MT6358 PMIC/regulator/PWRAP参考。
- `arch/arm64/boot/dts/mediatek/lancelot.dts`及lancelot子目录、触摸DT、
  `arch/arm64/configs/vendor/lancelot.config`和`mt6768_defconfig`。
- 当前Dijing面板参考源码，只为核对触摸初始化/显示恢复依赖，不提前实现待机功能。

## 新的实机确认点

1. 真正的NT36672头文件为`NVT_TOUCH_SUPPORT_HW_RST=0`，而通用HOSTDL目录为1。
   **不能因为DT有reset GPIO就直接拉动它，也不能复制Linux GPIO编号980/943到XNU。**
2. NT36672按`LCM_name`选择：Tianma=`nvt_tm_fw.bin`，Dijing NT36672=`nvt_dj_fw.bin`，
   Dijing NT36672D=`nvt_dj_72d_fw.bin`。旧运行DT为Dijing A，预期第二个，
   **仍须用新Android的cmdline/日志/身份复验，不能把预期当作已验证固件**。
3. 这些真实文件名不是通用目录中的`novatek_ts_fw.bin`/G6等名字。
4. 源码启用启动/恢复时固件装载，点包使用checksum、Protocol B、最多10点。
   XNU接管时不能假设触控固件仍然存在；真实ABS范围和事件轨迹仍待采集。

## Android补采计划与安全边界

源码准备已完成，**下一步需要回 Android**。请求用户先按键到原生 FASTBOOT，
AI按已验证流程还原 `E:\shuaji\Lancelot\boot.img` 并读回确认，然后采集：

- 新`uname`/build/config、cmdline、运行FDT、SPI0.0 driver/of_node/uevent。
- 按名称重新识别触摸event节点，`getevent -lp`/ABS能力。
- 正常启动NVT日志、固件/trim/PID/面板信息，以及真实使用的普通固件文件（长度/hash）。
- `/proc/tp_lockdown_info`是已审查的缓存字符串输出，可读取；不做校准。
- 用户配合的单点、拖动、边缘、两指/多指、全部抬起轨迹及IRQ计数对照。
  坏屏区域限制单独记录，不要求在坏区完成采样。

**禁止**读取`/proc/NVTSPI`、raw/baseline/diff、self_test/tp_selftest等厂测入口来采集。
这些读取可能执行SPI写或固件重载。`nvt_fw_version`/`nvt_tp_info`打开时会调用
`nvt_get_fw_info()`，并非纯缓存；优先从启动日志/安全缓存取版本，若需主动查询应
单独注明已审查调用路径，不把它混入通用“cat所有节点”脚本。

不改变SPI速率、GPIO/供电、触摸设置，不刷触摸固件。原始采集和固件只留本地，
不推GitHub；新触摸驱动尚未实现或刷写。恢复Boot与采集状态以随后实机审计为准。

## Android补采完成：最终状态与有效资料

用户确认进入原生FASTBOOT并配合触摸采样；AI还原已验证Android Boot，
整分区读回SHA-256仍为`b5de01520fc62fd3ccd18fffb098f295d0b301838d2b82b1077b05ddf47f7e84`。
**手机现在保持Android，ADB/root在线，电量100%，没有刷回UEFI或新触摸候选。**
Ubuntu/ESP在root/adbd两命名空间均未挂载，Ubuntu ext4 state=1。

采集目录：`local/lancelot-touch-android-20261024/`。
入口：`collection-complete.json`、`touch-details.json`、`touch-gestures.analysis.json`、
`firmware-offline-check.json`、`final-device-audit.txt`。
恢复/一般采集日志保存在相邻`local/lancelot-touch-android-20261024.*.log/.json`。

### 硬件/固件已确认

- 新运行配置仍启用NT36672、SPI_MT65XX、MT6768/Paris pinctrl，通用HOSTDL选项关闭。
  内核标识：`4.19.325-cip136-st20-ShockwaveKernel-S+`，构建日期2026-09-25。
  当前分支快照不自动等于整个运行内核的精确Git提交；触摸9文件与旧参考相同、
  配置/运行驱动/初始化日志都一致，作为本次触摸参考依据。
- `spi0.0`实际绑定`NVT-ts`，of_node为`/spi0@1100a000/novatek36672@0`；
  正常启动日志确认 **SPI mode0，max_speed_hz=8000000**。
- 新cmdline确认Dijing A面板；Android驱动实际请求 **`nvt_dj_fw.bin`**，不是猜测。
  文件来自`/vendor/firmware/nvt_dj_fw.bin`，**118784 bytes**，手机和PC的SHA-256均为
  `c41ba8aca0d0a96db5471af8a6a54fe6c6c55fc9759f26ca7381d9e2de34902d`。
  保存为`firmware-nvt_dj_fw.bin`，只留ignored本地目录。
- 源码规则与文件独立校验：有效装载长度**110592 bytes / 0x1b000**，末尾标志NVT，
  固件版本`0x08`、补码`0xf7`；启动日志也显示fw_ver0x08、fw_type0x01。
  这里只证明读回hash、末尾/版本匹配，不宣称完成全部固件分区CRC或XNU装载验证。
- trim ID为**`0a 00 00 72 66 03`**。按源码mask表匹配NT36672A_memory_map、hw_crc=1；
  EVENT_BUF_ADDR=`0x21c00`。这是**通过SPI访问的芯片内部地址**，不是SoC MMIO物理地址。
- DT物理GPIO/EINT为1，Android日志的Linux GPIO为326，虚拟IRQ为135。
  `/proc/interrupts`确认`mt-eint 1 Edge NVT-ts`，probe的trigger_type=1对应上升沿。
  **不要把Linux GPIO326/IRQ135作为XNU物理寄存器编号或GIC号。**

### 输入能力及实际轨迹

按名称识别本轮输入节点为`/dev/input/event2`，不是硬编码旧编号。
`getevent-capabilities.txt`确认：

- ABS_MT_POSITION_X：0…1080；Y：0…2340；SLOT：0…9（10槽）。
- TOUCH_MAJOR：0…255；PRESSURE：0…1000；软件TRACKING_ID：0…65535。
  Android的跟踪ID不等于芯片点包的硬件finger ID，不能把它直接用作0…9数组下标。

用户配合30秒安全页面触摸，`touch-gestures-30s.log`保存**303753 bytes / 4161条事件**，
612个SYN_REPORT。实际观察到槽0…4、最多5指，最后BTN_TOUCH=UP、全部接触释放；
没有SYN_DROPPED或越界槽。NVT IRQ总计数655→1546，增加891。
轨迹覆盖X131…1079、Y2…2108；不宣称覆盖坏屏区域、全屏或实际10指测试。

采集开始时已有一指在移动，初始接触状态未知，所以tracking开始99次、结束100次；
分析在第一次明确全抬起后才建立确定状态，不能把这一个差值当作丢帧或粘指。
活动期SYN_REPORT间隔中位数约8.367ms，只是本次evdev轨迹统计，不等于控制器扫描率承诺。
**这是evdev输出，不是实际SPI点包抓取，也不代表XNU触摸已经通过。**

### 保留的资料缺口与安全边界

- 运行配置关闭GPIO_SYSFS和DEBUG_FS：gpiochip/pinctrl导出为空，clk_summary不存在。
  某些su包装会把cat错误输出到stdout且返回0；已把这些资料标为不可用，不按exit0算成功。
- SPI子设备runtime_status为`unsupported`，平台SPI在采样后为`suspended`；
  这不等于触摸失败，也不是可直接照搬的XNU初始化状态。
- 未开启debugfs、导出GPIO、运行厂测、改变触摸参数、刷触摸固件或直接读写寄存器。
  正常Android启动时其驱动自身的固件RAM装载不是AI主动执行的更新/厂测命令。
- 未主动打开nvt_fw_version/nvt_tp_info；版本取自正常启动日志，
  只读取已审查的缓存`/proc/tp_lockdown_info`。芯片内部raw/baseline和SPI原包未采集。

## 接下来可以离线做什么

当前源码、正确固件、运行身份、ABS范围及单/多点事件基线已具备。先离线实现/测试
NT36672A点包解析与接触状态机，再建立机型隔离的MTK SPI/GPIO/EINT/clock访问，
完成身份读取、受控RAM固件装载和HID事件链路。保留范围、超时、checksum/CRC和异常释放保护。
运行pinmux/clock状态仍是未取得信息，必须从匹配源码/DT和后续受控诊断验证，不拼凑。

在候选通过保护条件/构建/读回前不启用`touchscreen=1`，不调用Qualcomm Goodix驱动；
保留五指误退出防护这一独立必做项。此阶段不启动GPU或后续六项功能，不自动推GitHub。
下一次需要真实SPI/候选实机验证时再通知用户；目前保持Android，不必立即刷回iOS。
