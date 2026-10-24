# Lancelot / Redmi 9：硬件适配待办清单

按用户指定顺序：**触摸 → GPU → 侧边按键 → 待机/亮起 → Wi-Fi → 震动 → 传感器**。
本文件是计划，不表示已完成驱动；触摸参考源码已取得，见
`LANCELOT-TOUCH-SOURCE-2026-10-24.md`。未启用未验证硬件，回Android补采时另行通知。

## 基线与执行约束

- iOS 16.7.16 / 20H392 已由用户确认成功启动。
  基线与恢复流程：`LANCELOT-SB-STARTUP-2026-10-24.md`。
- 当前成功候选：`local/lancelot-sb-activation-20261024/`；原 profile、ESP、Boot 和备份保留。
  成功日志 boot ID=`80000000040d889f`，发布的源码 HEAD=`99801f9`。
- 本轮只先整理计划。先离线利用旧采集；需要新 Android 数据时，明确通知用户采集项目，
  再请用户按键回原生 FASTBOOT，由 AI 还原已校验的 Android Boot。
- `lancelot-uefi` 是日志接口，不支持当前系统中的 adb shell/reboot/flash；普通 reboot
  不会还原 Android。恢复镜像：`E:\shuaji\Lancelot\boot.img`，64 MiB，SHA-256
  `b5de01520fc62fd3ccd18fffb098f295d0b301838d2b82b1077b05ddf47f7e84`。
- 采集只读、按实际设备路径，原始数据/厂商固件只放 ignored `local/` 或 WSL 私有目录；
  记录大小和 SHA-256。不上传固件、校准信息、账户数据或原始日志，不格式化、不改 GPT。
- 不直接把 `touchscreen=0/gpu=0/wifi=0/adsp=0` 改成 1：现有同名入口主要是 Qualcomm
  redwood 驱动。新增 MTK 驱动须有机型选择、范围检查、错误退出和明确的启用策略。
- 每项先本地提交、独立测试再实机；不把本次已有 GitHub 推送授权扩展为自动推送未来驱动。
  保持连续启动，PC 采集结束不停止系统。新增 UEFI 源码修改仍须事先询问用户。
- 共享基础设施（GPIO/pinctrl、SPI、PMIC 等）属于当前功能的依赖，不因此改变七项验收顺序。

## 1. 触摸 — 当前第一优先级

### 已有文件与实机证据

旧采集目录：`local/lancelot-20261022/`。

| 已有文件 | 已确认内容 |
|---|---|
| `drivers.txt` | `NVTCapacitiveTouchScreen`；当时输入节点 event2；`spi0.0` 绑定 `NVT-ts` |
| `device.dtb` / `device.dts` | MTK SPI0、Novatek 节点、GPIO/clock 引用、触摸分辨率与点数 |
| `kernel.config.gz` | 当时运行 Android 内核配置，需解压核验实际启用的触摸/总线驱动 |
| `dmesg.txt` | NVT suspend/gesture 与显示休眠日志；旧环形缓冲不是完整冷启动日志 |
| `manifest.json` | 上述原始文件的采集命令、长度和 SHA-256 |

DT 的 SPI0 位于 `0x1100a000`，触摸节点 `novatek36672@0`，SPI 上限8 MHz；
reset 引用 GPIO92，IRQ 引用 GPIO1。必须解析 GPIO phandle/pinctrl/触发配置并与源码核对，
**这些数值不是允许直接操作寄存器的依据**。
触摸逻辑分辨率1080×2340，DT最多10点。坐标输出仍须依据实际 ABS 范围确认。

DT compatible 同时列出 NVT 与 FocalTech 备选，但旧实机驱动实际绑定 NVT。
不能因此使用 FocalTech/Goodix 协议，也不能仅从面板型号推定触控固件。

### 还需要哪些资料

- [x] 已取得用户提供Shockwave 17.0的匹配触摸参考源码，实际为`NT36672/nt36672*.c/.h`，
  `Kconfig`/`Makefile`、固件下载、内存映射、SPI framing、校验、reset/resume、点包解析。
  固定commit与旧设备配置已核对，新Android运行身份仍需复验；不能用通用HOSTDL目录替代。
- [x] 配套 MTK SPI 控制器、GPIO/pinctrl、clock、PMIC/PWRAP参考源码/DT引用已下载。
  寄存器/供电的实机初始化状态仍需采集，不代表驱动已经完成。
- [ ] **设备实际选择的触摸固件/配置文件**及芯片/固件/面板 ID、版本、加载日志。
  文件名从源码加载路径与运行日志确定，不预先猜文件名，不需要上传整个 vendor 分区。
- [ ] 运行时 `spi0.0` 的 driver、modalias、uevent、of_node，以及已知安全的版本节点。
- [ ] 输入能力：`getevent -lp`，实际触摸设备的 ABS_X/Y 或 ABS_MT_POSITION_X/Y、
  SLOT、TRACKING_ID、压力/面积范围。event编号可变，按名称识别，不能硬编码 event2。
- [ ] 触摸事件轨迹：单点点击/抬起、边缘滑动、拖动、两指交叉、多指与全部抬起。
  记录原始事件及对应操作；屏幕损坏区域不强求，注明可用区域和物理缺陷。
- [ ] 触摸前后 `/proc/interrupts` 对照，正常启动/熄屏/亮屏时的 NVT 初始化日志。
  本阶段只记录恢复依赖，不提前宣称第4项待机已经实现。

**什么时候需要回 Android：** 可先离线分析已保存的 DT、配置、输入代码与参考源码；
在真实触摸驱动初始化/协议实现定稿和首次触摸候选刷写前，需要一次针对性的 Android
补采，取得上述缺失的固件/ABS/事件信息。届时明确通知用户，再由 AI 还原 Boot。
用户只需配合按键返回 FASTBOOT，以及采样时执行指定触摸动作，不必手工打包全系统。

禁止盲目读取/写入 `/proc/NVTflash`、厂测、固件更新、校准/复位节点；某些“read”处理
也会操作硬件，须先审查源码。不得通过升级/擦除触控固件来采集基线。

### 实现与验收待办

- [ ] 建立独立 Lancelot/MTK 触摸入口，避免调用 `redwood_goodix_init()` 的 Qualcomm SPI/GPIO。
- [ ] 按真实芯片协议实现供电/复位、SPI访问、身份核验、必要的固件装载、状态读取。
  确认是否为RAM装载方案，不能假设触控控制器在 XNU 接管时仍保留 Android 固件。
- [ ] 离线测试点包长度/校验/点数/ID/异常帧、按下移动抬起、丢帧恢复和越界数据。
- [ ] 复用 `redwood_touch_post()` → `/dev/redwood-touch`，坐标归一化0…65535；
  核对1080×2340物理/1125×2436逻辑显示的映射、方向、边缘，不重复缩放。
- [ ] 核验 backboardd 虚拟 HID 服务及20H392事件 ABI/属性/注册时序，不能默认20F66原型全兼容。
  先用显式测试作业/离线样本验证事件链路；当前 USB 日志协议没有远程写触摸样本接口。
- [ ] **处理五指退出逻辑**：`redwood_input.c` 当前真实驱动五指长按1.5秒会返回loader。
  对连续运行的 Lancelot 禁用或显式诊断开关隔离，保留其他目标既有行为；测试不会意外停机。
- [ ] 验证输入队列丢帧/满队列不会丢失抬起并导致粘指，异常时安全释放全部接触点。
- [ ] 实机确认可点击图标、滑动、拖动、多点/抬起；至少15分钟连续操作，backboardd和
  SpringBoard不重启、无幽灵触摸/粘指、存储和USB日志无回退。坏屏区域限制单独记录。

代码入口：`xnu/files/bsd/dev/redwood_input.c`、`xnu/files/bsd/sys/redwood_input.h`、
WSL `userland/src/inject.c` 的 `input_thread` / `make_service` / `send_touch`。
新的 MTK 控制器/触摸文件名在实现阶段确定，不误标为已存在的驱动。

## 2. GPU

- [ ] 资料：Android GPU_ID/驱动版本/内核配置、实际 Mali 驱动源码、clock/电源域/OPP/
  regulator、IOMMU与保留内存、EGL/GLES库版本；必要固件按驱动实际加载路径提取。
- [ ] 现有DT有 `mali@13040000`，compatible含`arm,mali-valhall`，**不凭这个字符串判定
  真实GPU架构**；以硬件ID与匹配源码核验。不能使用Adreno驱动或加载Android ELF库进iOS。
- [ ] 顺序：安全上电/身份读取 → MMU及隔离映射 → 最小提交/完成中断/fence → 清屏/纹理/
  混合 → Mesa或经验证渲染路径 → 20H392 CoreAnimation/EAGL接入。
- [ ] 保留软件渲染回退，不向未支持的进程随意发布Metal能力，避免重新触发媒体预热崩溃。
- [ ] 验收：真实硬件提交与图像正确、无GPU fault/越界DMA、合成性能有可复现对照，
  长时运行不回归触摸和媒体服务。不能只把`gpu=1`或帧数增加算作GPU完成。

## 3. 侧边按键

- [ ] 资料：`mtk-kpd` 的真实输入节点及 `getevent` 短按/长按/松开轨迹；keypad/PMIC
  源码、DT键码表/去抖/IRQ/唤醒配置。旧证据为`10010000.kp`，不能假定所有键都由同一GPIO提供。
- [ ] 实现 MTK 按键采样、去抖、按下/抬起与重复策略，接入现有 consumer HID事件。
  不照抄 `redwood_buttons.c` 的Qualcomm PMIC/SPMI寄存器。
- [ ] 验收：电源/音量加/音量减正确，短按与长按分离，组合键无重复/粘键；
  保留原生硬件强制恢复入口。仅按键事件成功不代表第4项实际待机唤醒成功。

## 4. 待机以及亮起以及电池

- [ ] 资料：Android熄屏/亮屏期间的显示/NVT/PMIC日志，DSI面板和背光源码、reset/供电
  时序、wake IRQ/clock、电源管理配置；旧证据可见NT36672A面板与KTD3137背光。
- [ ] 区分“锁屏”“背光关闭”“面板休眠”“SoC低功耗”，按阶段实现，不把黑屏当待机。
- [ ] XNU控制面板/背光及触控低功耗、恢复次序，清理接触点；核对powerd与backboardd。
  真正接管后不能依赖已经退出Boot Services的UEFI回调。
- [ ] 先可靠灭亮屏，再降低空闲提交/唤醒频率与功耗；USB日志/唤醒方式的限制需记录。
- [ ] 验收：至少20次熄屏/亮屏循环，按键可唤醒、首帧正常、触摸恢复，无黑屏/花屏/
  丢时钟；待机电流或功耗有对照，不把“锁屏但不断渲染”算作低功耗通过。
- [ ] 电池信息： 当前iOS无法正常识别电池电量以及充电，需要适配后使其能够识别充电状态/充电协议以及充电/电池剩余电量，不能把当前显示的低电量当作已经修复完成
## 5. Wi-Fi

- [ ] 资料：实际MediaTek WLAN/CONN平台驱动、bus/中断/电源/保留内存与协议，
  必需固件、WMT/平台配置与设备校准数据；Android扫描/关联/断线日志。
  旧DT为`wifi@18000000`，不能据此套用Qualcomm WCN6750/ath11k。
- [ ] 固件和设备校准只留本地；采集不需要用户Wi-Fi密码、Apple账户或完整NV分区上传。
- [ ] 顺序：芯片启动/链路 → 收发帧 → XNU网络接口 → Wi-Fi管理/扫描/认证/关联 → iOS接入。
- [ ] 验收：扫描、连接测试AP、DHCP/DNS、真实双向数据、断线重连及待机恢复；
  丢包/吞吐有记录，不用静态网络图标或虚构状态冒充连通。

## 6. 震动

- [ ] 资料：运行中的vibrator节点、Android震动轨迹/日志、PMIC/驱动源码、供电限制。
  旧证据为`mediatek,vibrator`及MT6358 VIBR电源路径，不默认存在AW8697。
- [ ] 实现有界强度/时长、可靠关闭及异常/休眠关断，再适配iOS调用链；没有对应硬件能力
  不宣称支持复杂haptic波形。
- [ ] 验收：用户实际感知，短/长/重复触发正确，超时必停，无持续震动/过热，待机后仍可用。

## 7. 传感器

- [ ] 资料：Android`dumpsys sensorservice`/传感器列表和采样，实际芯片/轴向/单位/频率/
  校准、I2C或SCP sensor-hub源码、firmware与通信协议。旧DT/dmesg包含SCP/sensorHub，
  不能假设所有传感器可由AP直接读取，也不能假装设备有并不存在的陀螺仪。
- [ ] 子项顺序：加速度/方向 → 光线/接近 → 实际存在的磁场/陀螺仪等。
  根据设备实际硬件修订子项，不改变七大项的先后顺序。
- [ ] 实现电源/采样/时间戳/轴向/单位、速率和休眠控制，再接入iOS传感器服务。
- [ ] 验收：转动方向正确、光线/遮挡响应真实、采样和功耗合理、休眠恢复正常；
  不用固定伪造数值绕过服务请求。

## 完成标记规则

每项记录：**资料齐备 → 离线测试 → 构建 → 部署/读回 → 实机功能 → 回归验收**。
一个阶段通过不自动勾选后续阶段。每项保留原始证据和可回退候选；涉及实际操作时再通知
用户配合，不在本清单阶段擅自重启或刷写。下一次 Android 补采可顺带保存其余硬件的
安全静态清单以减少往返，但不提前启用或调试后面的功能。
