# Lancelot：Shockwave 17.0 触摸参考源码与 Android 补采

## 下载结果

用户提供当前 Android 内核源码：
`https://github.com/MrShockWAVEog/shockwave_kernel_xiaomi_mt6768/commits/17.0/`。
本次锁定分支快照，避免使用会变化的分支URL下载文件：

- commit：`6dd120fc1ab487a5ebc6cdcf4ab605d2b43248f7`
- tree：`840cb76830615e7ec4ee5e2aee2e2ff229d6f5bb`
- 保存：`local/lancelot-touch-source-20261024/source/`
- **135 个文件，4,597,028 bytes；全部通过镜像站下载并核验。**

这是一组按触摸依赖选择的参考文件，**不是完整 Android 内核构建树**，没有下载/执行
整个内核、修改设备、重启或刷写。厂商固件本体仍需从手机提取，不能从源码常量假定版本。

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
