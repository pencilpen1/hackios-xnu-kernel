# Lancelot / 20H392：SpringBoard 启动路径诊断

## 决策与当前状态

继续 `LANCELOT-MEDIA-PREWARM-2026-10-24.md`。桌面尚未通过，已有媒体修复和
8-bit / 48 MHz eMMC 不改；不删除服务、不关闭 CPU 权限检查、不修改 UEFI 源码。

**本候选只观测原始启动行为，不伪造激活、不强制 setup reason=0。**
先用实机区分激活查询、系统服务初始化、主场景连接和主循环是否完成，再决定兼容修复。
新增方法都核验实际 20H392 UUID、IMP、类/实例方法种类及 ABI；读取 ivar 的两个
manager 另外核验实例大小、ivar 类型及偏移。未知版本拒绝安装，旧钩子的保护保留。

候选已构建与离线校验，**尚未部署/实机测试**。手机仍是上轮图形模式 XNU，
serial=`lancelot-uefi`，不是可刷写的原生 FASTBOOT。
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
