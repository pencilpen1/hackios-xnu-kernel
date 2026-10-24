# Lancelot / 20H392：SpringBoard 启动路径诊断

## 决策与当前状态

继续 `LANCELOT-MEDIA-PREWARM-2026-10-24.md`。桌面尚未通过，已有媒体修复和
8-bit / 48 MHz eMMC 不改；不删除服务、不关闭 CPU 权限检查、不修改 UEFI 源码。

**第一候选只观测原始启动行为，不伪造激活、不强制 setup reason=0。**
先用实机区分激活查询、系统服务初始化、主场景连接和主循环是否完成，再决定兼容修复。
新增方法都核验实际 20H392 UUID、IMP、类/实例方法种类及 ABI；读取 ivar 的两个
manager 另外核验实例大小、ivar 类型及偏移。未知版本拒绝安装，旧钩子的保护保留。

**续接状态：** 第一诊断候选已部署和完成 900 秒实测，手机仍为连续 XNU，
serial=`lancelot-uefi`，Apple 标志未消失。实测已确认原激活 state=1 和 setup reason=2。
根据这个证据构建了第二个 activation-only 候选，尚未部署/实测，详见后文。
回 Android 必须按键进入原生 FASTBOOT，再由 AI 刷回
`E:\shuaji\Lancelot\boot.img`，不能只执行 UEFI 普通 reboot。

## 新证据

`local/lancelot-sb-startup-20261024/` 保存代码、反汇编、候选和基线补采。

- 45 秒补采继续同一 boot ID=`800000000380974f`，收到 1,353,748 runtime bytes；
  因从 cursor=0 重新读取旧环形缓冲，明确报告 1,856,644 bytes 缺口。
  不是完整的第二份启动日志，不可宣称无丢失。已有主要进程 PID/start 未变，未见新 user-fault。
- SpringBoard 持续每秒请求 `com.apple.commcenter.xpc`，launchd 返回不存在。
  这是待调查线索，不等于已经证明其导致 Apple 标志停滞，不应删服务或掩盖拒绝。
- `SBLockdownManager.lockdownState` 仅在 `_state==0` 时调用 `_setupActivationState`。
  `brickedDevice` 返回 `_state!=2`；被检查方法设置/清除 +12 的查询中标志。
- `_setSetupRequiredReason:` 写入 +8 的 unsigned long 后执行原通知/断言管理逻辑，
  最后返回 `isInSetupMode`。`updateInSetupMode` 保存并返回这个 BOOL，
  **它不是“reason 是否变化”的标志，也不是 void 返回值。**
- `initializeSystemServicesBeforeApplicationDidFinishLaunching` 实际为 **类方法 `+`**。
  二次元数据检查发现并纠正了初版探针使用实例方法的错误；那版未部署。

方法及原始代码：`SpringBoard-code-report.json`、`SBSetupManager-updateInSetupMode.disasm.txt`、
`SBSetupManager-_setSetupRequiredReason.disasm.txt`、`SBLockdownManager-lockdownState.disasm.txt`。
匹配缓存与符号 UUID 沿用上一文档；未改 Apple 缓存或系统文件。

## 诊断实现

独立 userland：`userland/src/sb_startup_20h392.h`，从 `inject_sb.c` 引入。
Windows 保存完整源码补丁：`common/patches/userland-20h392-sb-startup.patch`。
独立 userland 本地提交=`f9f4040`，旧 build.sh/mkjobs.py 修改未纳入、未覆盖。

只有完整 boot-arg token **`sbstartup=observe`** 才启用。专用 payload 含该参数，
正常连续图形模式配方不自动添加它。不开检查点，不设返回计时器，不停止系统。

| 方法 | unslid IMP | ABI |
|---|---|---|
| `-[SBLockdownManager _setupActivationState]` | `0x1c5991a78` | `v16@0:8` |
| `-[SBSetupManager _setSetupRequiredReason:]` | `0x1c5aa608c` | `B24@0:8Q16` |
| `-[SpringBoard init]` | `0x1c587f834` | `@16@0:8` |
| `+[SpringBoard initializeSystemServicesBeforeApplicationDidFinishLaunching]` | `0x1c587f5f4` | `v16@0:8` |
| `-[SpringBoard applicationDidFinishLaunching:]` | `0x1c58808a0` | `v24@0:8@16` |
| `-[SpringBoard _completeStartupAfterMainSceneConnect:]` | `0x1c58813c8` | `v24@0:8@16` |

所有包装都调用原实现一次，保留 self、selector、原参数和返回值。
激活探针最多记录 8 次，setup 最多 16 次；记录 entry/exit、状态和调用者地址。
另记录主 dispatch queue 首次执行及主循环前三次 before-wait，不主动创建 manager
或调用 activation/setup getter。全部拒绝时不注册主循环观测器。

`tools/lancelot-bootdiag.c` 新增每 30 秒对七组日志的明确状态：大小、uid/gid/mode，
或 open/stat errno。它不会启动未运行的 daemon，也不会把空文件当作已完成详细验证。
已有 bounded forwarding/log cap 行为不改。

已通过：arm64 两个二进制编译；真实源码 ASan/UBSan 保护条件/参数/BOOL 返回值测试；
4 项文件状态测试；6/6 类/实例方法及 ABI 二次缓存核验；源码补丁 reverse-check；
APFS clean 与内核 reader 467 条目读回；连续图形 payload 校验。

## 候选与回退文件

私有复制 **已验证 media-prewarm profile/root**，只替换
`lib/libredwood-sb.dylib` 和 `bin/bootdiag`。保留全部 414 plist、媒体库与 RAM 日志权限。
不用旧 `userland/root`，不覆盖 prebuilt；不改完整系统容器。

| 文件 | SHA-256 |
|---|---|
| root.apfs，4194304 bytes | `393a9d99172bc270dc85505b85b3eaf6704b2526b2b9d1730028c4547cbd8c43` |
| profile.json | `9b38d6623083cf484943c09cc116660fd8857101d0d9b03faefdc520246f1c33` |
| libredwood-sb.dylib | `19217d7f11f3e0c6ffb9a63cd14e0560570baa04b25e2d126cb715280d2482e3` |
| bootdiag | `d6a6800c0bbaf61d14d7d38ac88f5a127f1fe420b254d9f13c6ac0bfe70f6d91` |
| loader.efi | `c42963d86028d1fe4f6de760e3a38342c313df26d18d6916813005b2c8614b4a` |
| boot.xnup，15185448 bytes | `c7d6c061ca8f99360663b554b56f149d75854cd6ec8b4a55ead8d7c55e1420c8` |

root pin=`/hackios/20H392/sb-startup-20261024/root.apfs`。
kernel 未变=`099d6539e24ebc95732ecbd11b45adf71c9cd4fd22eb43d02a1cde2868cb7d5c`；
媒体未变=`2b027166a1a849926e2438245bcf600f2f3edbcff5102b0518b5a9d194bfe1e2`。
SB 二进制来自当前源码的新构建；不假设旧 prebuilt 等于当前源码，也不把旧源码功能
自动算作本候选实机通过。

只使用已验证 UEFI：
`local/lancelot-cp5-hardware-20261023/Mu-lancelot-msdc-fixed.img`，608256 bytes，
SHA-256=`a23bdf6ddf8bcde9275b57240ba87abe60373651f2e3a5fd254ed2e4c6074f7c`。
Android Boot：67108864 bytes，
SHA-256=`b5de01520fc62fd3ccd18fffb098f295d0b301838d2b82b1077b05ddf47f7e84`。
原 media profile、图形 ESP、完整 Ubuntu/ESP/Boot 备份全部保留。

## 实机步骤及判读

1. 请求用户按键返回原生 FASTBOOT，验证 serial=`116745f30503`、product=lancelot。
2. AI 校验并还原 Android Boot 后 reboot，验证 Android、root、分区与挂载状态。
3. `deploy-lancelot-profile.py --profile sb-startup-20261024` 直接部署新目录，
   不覆盖已验证旧目录；部署匹配 ESP。独立只读读回，检查 root/adbd 两命名空间卸载与 ext4 clean。
4. 刷同一既有 UEFI，连续图形启动，单 USB 客户端采集新文件至少 900 秒。
5. 验证 6/6 保护条件日志。activation enter 无 exit 才支持查询内部阻塞；
   setup 原请求/返回值确认实际设置状态；系统服务、主场景和主循环标记划分阻塞阶段。
   不凭 PID 稳定或缺少某条消息认定桌面通过。
6. 逐项分析 SpringBoard、backboardd、lsd、mediaserverd、RedwoodAudio、CoreData；
   查七组文件为何空/缺失、故障寄存器、PID/start、eMMC error/fallback 与实际屏幕。
   采集结束不停止系统，正常完成后单次检查 KLOG2。

改动先本地提交，组合实机整体通过前不推 GitHub。USB 物理拔插失败仍未定位，
本候选不修改 USB 驱动或固件，也不声称解决该问题。

## 诊断候选实机结果（已部署并测试）

AI 已按用户要求先还原 Android Boot，整 Boot 分区 SHA-256 匹配，
Android `sys.boot_completed=1`、电量100%。第一次主机审计在尝试打印二进制
ext4 superblock 时遇到 GBK 编码错误；没有刷写失败。后续不打印原始二进制，
重新独立核验并保存 `android-restored.json`、`boot-readback-preflight.txt`。

新 root/profile/ESP 独立只读读回全匹配；旧 media-prewarm profile 也复验保留。
root/adbd 两命名空间均卸载，ext4 state=1。后刷回同一既有 UEFI，未改固件源码。
Windows 准备候选本地提交=`965450b`，没有推送。

证据：`local/lancelot-sb-startup-20261024/` 的部署审计、`boot-manifest.json`、
`services-first-boot.log(.fastboot)`、分析和 `hardware-observation.json`。

- boot ID=`8000000003b9751f`，900 秒，**2,479,589 runtime bytes**。
- 六个探针均通过 UUID/IMP/ABI/layout 保护。system-services、init、
  applicationDidFinishLaunching、completeStartupAfterMainSceneConnect 均有 entry/exit；
  主 dispatch queue 执行，主循环出现前三次 before-wait。**不能再把停滞描述为这些方法卡死。**
- 原始激活查询返回 `_state=1`、`_settingUpActivationState=0`。
  setup 原始调用三次都是 reason=2，BOOL 返回1。调用者=`0x1c5aa5f24`，
  精确对应 `updateInSetupMode`；launchd 明确启动了 `com.apple.purplebuddy` / Setup。
- 用户仍确认 Apple 标志/加载画面。原因范围缩小到未激活导致的设置路径及后续显示，
  **尚不能声称它是画面的唯一原因**。缺失 CommCenter 持续产生请求失败，
  但主循环与启动已完成，不支持简单断言“CommCenter 把启动方法卡死”。
- zero user-fault，媒体保护仍启用。SpringBoard/backboardd/wifid 各26个稳定 PID/start
  样本；mediaserverd/lsd各25；mobileactivationd 24。实际 PID分别8/13/11/31/27/43。
- 26次 eMMC 都为 width8、48MHz、batch128、errors0、fallback0。
  最后 sectors=4,119,138，commands=122,666，pio_us=240,007,908。
- 七个文件都能 open/stat：**lsd、lsd-system、wifid、coredatad 26次均为0字节**。
  是实际空流，不是缺失或 EACCES；不能当作已详细验证这些 daemon。
  SpringBoard/backboardd/mediaserverd有非零块。CoreData日志仍从其他进程/控制台出现，
  PDSCDCache恢复及 CloudKit无账户134400分类沿用；不删库、不关闭 CPU 检查。
- 采集未报告 CRC/cursor gap，结束后单次 `getvar log-version` 正常返回 KLOG2。
  正常日志通道通过，USB物理拔插仍未验证。没有后台采集遗留。

## 第二候选：只适配已证实的未激活结果

不启用旧“所有 setup reason强制0”的策略。新头文件默认仍是纯观测，
**只有同时存在完整 token `sbstartup=observe sbactivation=local gpu=0 root_apfs=lancelot_ios`**
才允许 local mapping，六个方法的 UUID/IMP/ABI/layout 保护不变。

原 `_setupActivationState` 仍执行一次。它完成后，仅在 native state=1 且 pending flag=0
时，将 **此移植进程内** 的 `_state` 改为兼容状态2。状态0/2/3或 pending 非零不改。
这不是 Apple 服务器激活、不写激活记录，也不是修改原完整系统。
独立 userland 本地提交=`e05549d`，未推送，旧 build.sh/mkjobs.py 修改保留。
`updateInSetupMode` 自行重算 reason；setup 原实参、原副作用和 BOOL 返回都保留，
迁移/其他设置原因不会被统一压为0。

测试执行了16种 native state/pending/opt-in 组合，验证只改变这一已证实结果；
原始调用一次、设置参数/BOOL原样返回、完整token和未知版本保护继续通过。
所有测试只执行 host fixture，不执行假缓存地址。

候选：`local/lancelot-sb-activation-20261024/`，从本轮实机通过诊断链路的
sb-startup profile/root 私有复制，**只改变 libredwood-sb.dylib**。
bootdiag、全部414 plist、467条APFS、媒体库及kernel不变。

| 文件 | SHA-256 |
|---|---|
| root.apfs，4MiB | `86fbf42a6cef3610a260420394f9c0dd9eeecb43bc21089bab2b69d984e867a0` |
| profile.json | `3338fef1a352e470500303e5264703f723c4e9f6184e0140741c82cef7152a97` |
| libredwood-sb.dylib | `0c0e26145e3501748fdb540d1fab2437db377377e5f1e2770bb7da48d7f84840` |
| loader.efi | `0a8052965d122175be4e6ab82304633fe9af2e18218e5bd7ded5abc1a28df484` |
| boot.xnup，15185464 bytes | `279566f64ab0c347b85643c037b9c70c9ce77f597268c7745abdf0518bca6fe4` |

pin=`/hackios/20H392/sb-activation-20261024/root.apfs`，新的专用连续图形payload
含上述opt-in，不增加检查点或计时返回。APFS独立读回、保护条件/ABI测试已通过。

下一实机步骤仍是按键到原生FASTBOOT，由AI再次还原Android Boot，部署新路径
与匹配ESP并读回，再刷同一既有UEFI、连续采集至少900秒。
需要验证 local mapping日志、原setup自然请求reason0/返回0、Setup是否仍启动、
各服务和eMMC以及实际锁屏/桌面。若设置路径退出但画面仍不变，继续调查显示/窗口服务器，
不再用激活兼容解释所有故障。整体桌面仍未通过，所有提交只保存在本地。
