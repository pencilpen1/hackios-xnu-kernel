# Redmi 9 / iOS 移植：下一位 AI 独立交接文档

**工作日：2026-10-23。最终设备核验：23:46（UTC+8）；交接文档于 2026-10-24 凌晨完成。**

本文是本次工作的最终入口，可独立阅读。旧交接中的“尚未部署”、手机仍在
iOS 中等描述，均以本文的最终状态为准。

## 1. 先读结论

1. **最新 service-log 版的文件已经部署成功，并完成独立只读读回校验。**
2. **手机当前在 Android，USB ADB 在线；boot 分区仍是已知 Android boot.img。**
   本轮按用户“部署完文件后结束今天工作”的要求收工，没有再启动新候选。
3. Ubuntu 和 ESP 已在 root、adbd 两侧确认卸载；Ubuntu ext4 superblock
   `s_state=1`，Android `sys.boot_completed=1`，最后电量 100%。
4. **新候选只完成构建、文件系统校验与部署，尚未实机启动验证。**
   下一位 AI 的第一项实机工作，是刷入既有 MSDC-fixed UEFI 并采集新一轮日志。
   不必重复写入已经验证的 Ubuntu/ESP 文件。
5. 上一轮运行版已经证明：**XNU 运行时 USB 日志可用，eMMC 为 8-bit / 48 MHz /
   128 扇区批量，观测错误数和模式回退均为 0。**
6. 桌面仍未通过。已证实 **backboardd spawn EACCES** 和
   **mediaserverd 连续 SIGSEGV**；不能以“编译通过”“USB 日志可用”代替整体通过。
7. **今天没有推送 GitHub。** 源码修复和本文都应保存在 Windows 仓库的本地提交中。

最终证据目录：

```text
D:\AI_rom\iOSforLava\hackios-xnu-kernel\local\lancelot-service-log-20261023\
```

优先打开 `deployment-complete.json` 和 `final-device-audit.txt`，它们记录实际设备
读回结果。不要把目录中更早的 `deployment.log` 超时结果误认成最终结果。

## 2. 用户目标和约束

- 硬件：Redmi 9 / lancelot，MT6768/MT6769T；目标系统 iOS **16.7.16 / 20H392**。
- 目标是解决启动循环、进入稳定桌面/锁屏，并验证存储提速；后续还需要持久化读写。
- 用户屏幕部分损坏且日志滚动太快，要求 **由电脑导出日志，AI 自行分析**。
- 明确需要分析：SpringBoard、backboardd、**lsd、mediaserverd、RedwoodAudio、CoreData**。
- 默认连续启动；检查点/定时返回仅用于明确的专项诊断，不应默认停止内核。
- 已授权手机重启、已知 UEFI/test image 刷写；**新增 Mu-Silicium 固件源码修改仍需先询问**，
  每次获准的固件源码修改必须提交。本轮只改 XNU/EFI 应用/工具，没有改固件源码。
- 新改动本地提交，**组合实机整体通过之后才推 GitHub**。
- 不格式化、不改 GPT；保留分区备份、旧镜像。大 APFS 文件优先在 Ubuntu，不能暂存 userdata。
- 下载 GitHub 优先镜像站，并检查长度、哈希和压缩档完整性。
- 当前用户最后要求是“完成文件部署、写完全新的交接文档、结束今天工作”，不是继续测试到深夜。

## 3. 环境与仓库：不要直接 reset

### Windows：提交与交付的主仓库

```text
工作区：D:\AI_rom\iOSforLava                 （这个父目录不是 Git 仓库）
仓库：  D:\AI_rom\iOSforLava\hackios-xnu-kernel
分支：  main
remote：https://github.com/pencilpen1/hackios-xnu-kernel.git
origin/main 最后已知：2b1bb24f78196bca52d6aa0700ef70158231b6ac
```

未推送的功能提交（按时间）：

| 提交 | 内容 |
|---|---|
| `a40b32b` | CMD23/CMD18 多块 PIO；经校验的 8/4/1-bit、HS52 协商及回退 |
| `834bca9` | 修复按需 launchd 作业文件名覆盖；20H392 引导种子与 bootdiag |
| `22a2efb` | XNU 接管 MUSB，KLOG2/LLOG1 运行时日志与文件日志转发 |
| `4a29dca` | 补齐内核控制台/用户故障寄存器，修复日志权限并扩展服务日志 |
| `b563894` | 修复最终部署传输：有界 Base64 直达 Ubuntu，读回与卸载检查 |

本文另有本地文档提交；用 `git log -8 --oneline` 和 `git status --short --branch`
获取最后 HEAD，不要把上表当成固定的最终提交号。

### WSL：实际构建目录

```text
发行版：Ubuntu
/home/pencilqwq/hackios-lancelot
Windows 访问：\\wsl.localhost\Ubuntu\home\pencilqwq\hackios-lancelot
XNU：xnu-src/xnu-8796.121.2
userland：userland/src/、userland/mkjobs.py、userland/libmini/
工具链：local-toolchain/theos、xnu-env.sh
```

- WSL Git HEAD 仍是 `2b1bb24`，本轮源文件以逐文件核对后复制的方式同步，**有未提交修改**。
  WSL 的 `origin/main` 引用较旧，显示 ahead 11，不能与 Windows 的 ahead 数直接比较。
- 收工核对显示关键内核/加载器、`tools/lancelot-bootdiag.c`、profile 生成器、PC 日志读取器
  与 Windows 当前内容相同。最新的两份部署工具在 WSL 中缺失，部署应使用 Windows 仓库。
- `userland` 在当前布局中不是独立 Git 仓库；在那里运行 git status 实际显示父仓库。
  不要因此假定 `mkjobs.py`、注入库源文件没有本地修改。
- 详细快照：`local/lancelot-service-log-20261023/source-state-final.json`
  （采集于 `b563894` 和本文提交之前）。
- 早期源文件备份：WSL `local/lancelot-runtime-log-20261023/source-before/`。
- 构建目录和已有修改必须先比较再同步；**不要用 reset --hard 或整目录覆盖清理它们**。

## 4. 最新、已部署的候选：service-log-20261023

以下 SHA-256 是本轮最终审计读回值，不是仅主机计算值。

| 文件 | 字节数 | SHA-256 |
|---|---:|---|
| `mach.lancelot`（打包的内核） | 15185232 | `099d6539e24ebc95732ecbd11b45adf71c9cd4fd22eb43d02a1cde2868cb7d5c` |
| `loader.efi` | 76800 | `ac98357adf12bb0e0f78cbcdbc6180d520c0e4e062a332435a3a87d758a28d30` |
| `boot.xnup` | 15185432 | `ba4f41757b574a8633791a6eef9da5e2331c6124e90422cb16e934f638f3b89e` |
| `root.apfs` | 4194304 | `8089e522be3d774cb568b853e380421c02ed3defacffd23e4a3b263f20c5ae64` |
| `profile.json` | 713 | `a34fd7fffd21950ce66bd1dc286e74e3a6526bf7233b5e8cfdeacf9f827897b1` |

主机文件：

```text
local/lancelot-service-log-20261023/mach.lancelot
local/lancelot-service-log-20261023/EFI/HackiOS/loader.efi
local/lancelot-service-log-20261023/EFI/HackiOS/boot.xnup
local/lancelot-service-log-20261023/root.apfs
local/lancelot-service-log-20261023/profile.json
```

手机部署位置：

```text
Ubuntu: /hackios/20H392/service-log-20261023/root.apfs
        /hackios/20H392/service-log-20261023/profile.json
        /hackios/20H392/service-log-20261023/SHA256SUMS
ESP:    /EFI/HackiOS/loader.efi
        /EFI/HackiOS/boot.xnup
        /EFI/BOOT/BOOTAA64.EFI   （与 loader.efi 相同）
```

这版 profile 含 **414 个 launchd plist、467 个 APFS 条目**，apfsck 和内核 APFS reader
均读回一致。XNUP 校验为连续模式、boot CPU 单核、无检查点和定时返回。

最新部署证据：

| 证据文件 | 含义 |
|---|---|
| `profile-deployment-final-v2.log(.json)` | 成功的有界 Base64 部署，两轮哈希匹配且已卸载 |
| `esp-deployment-final.log` | 三个 ESP 目标文件复制/哈希验证 |
| `final-device-audit.txt` | 另一次 Ubuntu/ESP 只读挂载读回、Android boot 哈希、最终设备状态 |
| `ubuntu-superblock-final.bin` | 最终 ext4 superblock，state=1 |
| `android-mounts-final.txt` | adbd/shell 命名空间最终挂载表，无 Ubuntu/ESP 临时挂载 |
| `deployment-complete.json` | 文件已部署=true，新候选启动测试=false |
| `build-report.json`、`kernel-build.log` | 构建产物与编译证据 |

旧 `root.apfs`、`bootfix-20261023`、`runtime-log-20261023` 仍保留。
这次超时生成的空目录保存为：
`/hackios/20H392/service-log-20261023.incomplete-20261023-2325`。
它不是有效 profile，加载器不会使用。

## 5. boot 分区、UEFI、分区与备份

### 当前 boot 是 Android

```text
文件：E:\shuaji\Lancelot\boot.img
大小：67108864 bytes
SHA256：b5de01520fc62fd3ccd18fffb098f295d0b301838d2b82b1077b05ddf47f7e84
```

最终审计对 `/dev/block/by-name/boot` 计算了相同哈希。现在普通重启会启动 Android。

### 下一次 iOS 测试必须使用这一份既有 UEFI

```text
仓库相对路径：local/lancelot-cp5-hardware-20261023/Mu-lancelot-msdc-fixed.img
大小：608256 bytes
SHA256：a23bdf6ddf8bcde9275b57240ba87abe60373651f2e3a5fd254ed2e4c6074f7c
固件提交：251bd657
```

`E:\shuaji\Lancelot\MuUEFI-lancelot.img` 是更早的基线，**不是这份 MSDC 修复版**。

| 分区 | 设备 | 容量 |
|---|---|---:|
| Ubuntu | `/dev/block/mmcblk0p49`，`/dev/block/by-name/Ubuntu` | 33285963776 |
| ESP | `/dev/block/mmcblk0p48`，`/dev/block/by-name/esp` | 1073741824 |
| boot | `/dev/block/mmcblk0p33`，`/dev/block/by-name/boot` | 67108864 |

Ubuntu 仍是 **ext4**，APFS 是其中的文件，不是把整个 Ubuntu 分区改成 APFS。
完整原 Ubuntu、ESP、Android boot 备份在：
`D:\AI_rom\ubuntuforlava\work\hackios-backup-20261022\`。
`Ubuntu-before-ios.img.gz` 的解压原始分区 SHA-256：
`5c5327b280c1197065f2a82489a1959405e6782dfc0f5cc8661b96872a6ac031`。

## 6. 启动结构与本轮修复

### 存储 / 引导

- EFI 应用在交 CPU 前只读加载并校验两个 4 MiB APFS。
- `/hackios/20H392/flashboot.apfs` 是 `md0`，参数 `rd=md0`；其 SHA-256：
  `14485e7c5ce125ef516168445c2500ac04ca5e40a908c2aef516b2d92c24ba0c`。
- 新小 root profile 是 `md1`，挂载 `/redwood`。加载器锁定它的路径和 SHA-256。
- XNU 原生 MSDC0/ext4 reader 把 Ubuntu 中的 `ios-containers.img` 提供成
  **只读** `/dev/lancelot_ios`。参数 `root_apfs=lancelot_ios` 让 init 切换完整系统与 cryptex。
- `/private/var` 仍是 RAM 文件系统；下一次启动重建种子，持久化媒体写入尚未实现。
- 多块 PIO 使用 CMD23 + CMD18，最大 128 扇区（64 KiB）一批。
  实测 384 MHz 源时钟对应所选 48 MHz SDR；没有实现新 HS200/HS400 激活或 DMA。

### 已解决的构建配置缺陷

- `mkjobs.py` 的环境变量循环覆盖了 `name`，389 个按需作业反复写成一个 `RWA_PACKED16`
  文件；修复后实际 plist 从 25 个恢复到 414 个。保留按需启动语义，不全改 RunAtLoad。
- 迁移种子从旧 `20F66` 匹配到 `20H392`。
- CPU/表面压力演示被轻量 bootdiag 替代。

### 最新 service-log 版新增、待实机验证的部分

- 原诊断目录 root:root 0755 会阻止 mobile 创建日志；新版改成 mobile:mobile 0755，
  并预建七个 mobile:mobile 0644 日志文件。真实 POSIX uid-501 测试复现旧 EACCES，
  新权限下七个文件均可打开。是否解决全部 backboardd EACCES 要看新实机日志。
- 详细 stdout/stderr/os_log：SpringBoard、backboardd、mediaserverd、lsd、lsd-system、
  wifid、coredatad。RedwoodAudio 写 mediaserverd stderr，进入同一文件。
- 文件位于 `/private/var/log/hackios/<name>.log`；bootdiag 不筛选地转入 USB RAM 日志，
  带文件 offset 标记。每个文件超过 4 MiB 会记录未转发字节数后截断。
- 采样增加 UID、启动时间微秒；改用时间安排约 30 秒周期，避免旧版 300 次转发循环拖慢采样。
- 把日志入口从仅用户控制台的 `kmoutput` 移到 `console_write`，补齐 framebuffer-only
  内核 printf，包括 CPU monitor、存储等输出。旧 tap 已移除以避免双写。
- VM 处理失败、即将交付正常 Mach exception 时记录 `user-fault:`：
  进程名/PID、PC/LR/SP/FP、ESR/FAR、VM 结果。没有替换异常处理或解引用用户地址。

## 7. USB 日志：已经能用，下一轮用同一工具

旧固件 LLOG1 回调在单核 XNU 接管后暂停，因此之前主机只能看到跳内核前的信息。
XNU 现在原生接管 MUSB，不调用固件回调，保留原 Windows fastboot 驱动绑定。

```text
手机 Android/native-fastboot serial：116745f30503
诊断 USB serial：lancelot-uefi
VID:PID：18d1:d00d，interface 0，OUT=01，IN=81
MUSB MAC：0x11200000；USB2 PHY：0x11cc0800；MSDC0：0x11230000
固件 LLOG1 RAM 环：0x512c0000，0x30000 bytes
```

这是 USB bulk 诊断协议，**不是 Windows COM 串口**。

- PC 工具：`tools/read-lancelot-log.py --runtime`。
- 自动查询 `getvar log-version`：固件期读 LLOG1，XNU 出现后切 KLOG2。
- KLOG2 经 `oem klog <cursor>` + `get_staged` 读取，带 boot ID、范围、长度、CRC32。
- 新脚本每 15 秒打印接收进度；等待固件交接不再刷 `unsupported oem klog` 错误。
- XNU 缓冲保留最初 256 KiB 和最新 1 MiB；覆盖会报 cursor gap，不是无限无损保存。
- 不要同时启动多个 fastboot/采集进程争用同一个接口。
- 多份历史 capture 都可能从 cursor=0 开始，内容重叠；不要直接拼接当成独立事件计数。
- 主机采集结束不会停止手机。不要把超时当作内核死机；先看当前协议/boot ID/实际字节。

### 已证实的上一轮捕获

目录 `local/lancelot-runtime-log-20261023/`，XNU boot ID **8000000003f74d4f**。

- `xnu-runtime-services.log`：300 秒收到 **1908135 字节**；无报告的 CRC 错误/cursor gap。
- `xnu-runtime-follow.log`：更早的 180 秒收到 409876 字节，与上项有重叠。
- `xnu-runtime-services.analysis.json`：关键事件、行号、统计。
- `hardware-observation.json`：USB、存储、进程观测汇总。
- 部分原始日志含 NUL/非 UTF-8 字节，文本工具可能拒读。使用 Python
  `Path(...).read_bytes().decode('utf-8', 'replace')` 分析，同时保留原始文件不改写。

## 8. 已有分析：哪些是真故障，哪些仍待证据

| 项目 | 已证实 | 下一步 |
|---|---|---|
| eMMC | 三次样本 width=8、hz=48000000、batch=128、errors=0、fallback=0；最后 sectors=2809418 | 新候选持续复验；不要把 PIO 计时当完整开机速度对比 |
| backboardd | 49 条 spawn EACCES，xpcproxy exit 78；APFS 中程序本身是 0755 | 验证日志目录/文件权限修复后能否执行；如仍拒绝，定位实际拒绝路径 |
| SpringBoard | 多次 exit 255，PID 变化；旧版本钩子多处拒绝 | 先恢复 backboardd，再分析新详细日志 |
| mediaserverd | Launchd 明确 SIGSEGV，连续崩溃计数到 26；PID 30→163→304 | 用新 `user-fault` PC/LR 对实际 20H392 dyld cache 符号化 |
| RedwoodAudio | 同一捕获有 80 次插件初始化；选择器请求伴随媒体服务重启 | 不能仅靠补空属性“修复”；关联 fault 和 mediaserverd 文件前后文 |
| lsd | 后两轮样本 PID=28、start=93 不变；尚无重启循环证据 | 抓详细 mobile/system lsd 日志，并关联内核 CPU-monitor 输出 |
| CoreData | knowledgeC.db、IdentityServices/PDS/PDSCDCache 缺失；Code=512、底层 errno=2；后者明确报告恢复成功 | 检查哪些缺目录能自恢复，哪些持续失败；不要删除数据库来隐藏日志 |

RedwoodAudio 请求完整选择器已捕获：`cust`、`clok`、`cstb`、`taps`、`accs`、`clk#`、
`auto`、`cfsz`、`bcvl`、`lmod`、`srnd`，还有 object/scope/element。它们是属性探测，
不等于音频代码就是 SIGSEGV 的实际位置。

`lsd` 实际签名没有 `com.apple.private.kernel.override-cpumon`。内核那条拒绝提示会
恢复默认 CPU 监控参数，不会直接终止进程；不应关闭权限检查来消除它。

`inject_media.c` 已有针对旧 20F66 的 GPU-less portrait filter prewarm workaround，
而其地址保护在 20H392 未验证。可以作为调查线索，**不可直接认定就是本轮 SIGSEGV 原因**。
同样，`inject.c`、`inject_sb.c` 的原地址保护必须保留，不能用日志里“found”的地址
不经 ABI/代码核实就替换 expected，更不能整体放宽。

Launchd 常延迟成批输出旧时间戳；不要把文本相邻当作严格因果顺序。

## 9. 下一位 AI 的首轮操作

在用户恢复工作后，先读取本文的最终审计文件、确认设备仍是 Android，并检查工作树。
**当前所有启动文件已经部署，无需先运行 profile 部署工具。**

下面命令从 Windows 仓库运行；每个刷写步骤都必须检查实际返回值：

```powershell
Set-Location 'D:\AI_rom\iOSforLava\hackios-xnu-kernel'
$adb = 'E:\shuaji\toolfile\buildtools\android-sdk\platform-tools\adb.exe'
$fb = 'E:\shuaji\toolfile\buildtools\android-sdk\platform-tools\fastboot.exe'
$uefi = 'local/lancelot-cp5-hardware-20261023/Mu-lancelot-msdc-fixed.img'

git status --short --branch
& $adb devices -l
if ((Get-FileHash $uefi -Algorithm SHA256).Hash.ToLower() -ne 'a23bdf6ddf8bcde9275b57240ba87abe60373651f2e3a5fd254ed2e4c6074f7c') { throw 'UEFI hash mismatch' }
& $adb -s 116745f30503 reboot bootloader
& $fb -s 116745f30503 getvar product
# 确认输出 product: lancelot 后继续
& $fb -s 116745f30503 flash boot $uefi
if ($LASTEXITCODE -ne 0) { throw 'UEFI flash failed' }
& $fb -s 116745f30503 reboot
if ($LASTEXITCODE -ne 0) { throw 'reboot failed' }
python tools/read-lancelot-log.py --runtime --quiet --fastboot $fb `
  --out local/lancelot-service-log-20261023/services-first-boot.log --duration 900
```

采集要回答：

1. 新 root 哈希确为 `8089e522...`、内核日期/产物正确，仍为连续启动。
2. 是否出现此前漏掉的 `lancelot-emmc:`、内核 CPU-monitor、`user-fault:`。
3. backboardd 是否越过 EACCES 并稳定运行，SpringBoard 是否停止 exit 255。
4. 七个日志文件是否有 `bootdiag: file-chunk ...` 转发，mediaserverd SIGSEGV 的 PC/LR 是什么。
5. lsd 的 PID/启动时间是否变化，CoreData 哪个进程、数据库路径持续失败。
6. 8-bit / 48 MHz / 128 batch 是否维持，I/O errors 是否增长。

如果诊断接口驻留且不支持重启，通常仍要用户电源＋音量下回原生 FASTBOOT；
当前协议没有 reboot/download/flash 命令，不要反复发它们。

## 10. 构建、部署工具和本轮超时经验

### 构建入口

```text
common/build-lancelot-kernel.sh boot
common/build-lancelot.sh boot
tools/prepare-lancelot-boot-profile.py
```

实际在 WSL `/home/pencilqwq/hackios-lancelot` 构建。固定 XNU 为 xnu-8796.121.2；
硬件页表 granule 是 4 KiB，用户 ABI 查询曾显示 page size 16384，不能据此判断页表改为 16 KiB。
最新 `make build`、检查点 teardown 回归、MUSB/ring ASan/UBSan 与 payload 检查均已通过。

新 profile 的构建目录：WSL `local/lancelot-service-log-20261023/profile/root/`。
该目录之外的 `userland/root/` 仍可能是旧生成器输出，不能误打包它替换最终镜像。

### profile 部署已改为有界 Base64

- 原 TCP/nc 接收曾超时，新目录为空，并遗留 root 命名空间中的 Ubuntu 挂载。
  根因未仅凭空目录定论；新工具移除了独立 listener/readiness/EOF 等待环节。
- 只 grep `mmcblk0p49` 不可靠：挂载来源可能显示 `/dev/block/by-name/Ubuntu`，
  adbd 与 root 看到的来源名称也可能不同。新工具用 `readlink -f` 比较真实块设备。
- 本机 KernelSU 的 stdin 通道对二进制不透明，直接 64 KiB 二进制探测哈希不匹配；
  分行 Base64 + `head -n <行数>` 的探测匹配，最终完整 4 MiB profile 传输和读回也匹配。
- `tools/deploy-lancelot-profile.py` 现在通过 ADB 传小脚本，Base64 数据直接解码到
  Ubuntu 的唯一 `.incoming-<token>` 目录；校验后 rename 到最终路径，再只读重挂载复验。
  APFS 数据没有写入 userdata 临时文件。
- 临时试验中给 ext4 挂载加 SELinux `context=` 被 Android 拒绝，已放弃；最终工具
  使用普通 root ext4 挂载，没有放宽 SELinux，也没有修改盘上标签。
- 当前 profile 部署器锁定 `service-log-20261023` 路径和哈希；新镜像要相应更新。
  已有正确目标目录时它会拒绝覆盖。不要为了重复部署删除有效目录。
- 超时残留空目录已更名保存，临时挂载清理完成；没有活跃接收/采集后台任务留待下一位接管。

### 关键源码

```text
xnu/files/bsd/dev/lancelot_usb.c        XNU 适配器、sysctl、故障记录
xnu/files/bsd/dev/lancelot_musb.h       EP0/bulk 状态机、KLOG2 快照
xnu/files/osfmk/arm64/loader_return.c   RAM 日志与返回固件
xnu/redwood.patch                     console_write 与用户 abort 观察点
xnuloader/lancelot.h、loader.c         USB handoff / 返回恢复
xnuloader/lancelot_ubuntu.h            profile 路径/哈希
tools/read-lancelot-log.py             电脑采集器
tools/lancelot-bootdiag.c              进程、存储与七个日志文件转发
tools/prepare-lancelot-boot-profile.py profile 生成与 mobile 日志种子
tools/deploy-lancelot-profile.py       Ubuntu 文件部署
tools/android/deploy-lancelot-profile.sh
```

userland 注入源码在 WSL `userland/src/{inject.c,inject_sb.c,inject_media.c,audio_plugin.c}`。
实际 20H392 cache 在 WSL `ios-local/20H392/dyld-cache/`；系统源在同目录 `rootfs/`。

## 11. 完成标准与后续方向

先用新日志完成 backboardd、media、lsd、CoreData 的因果定位与针对性修复，再验证
稳定桌面/锁屏和持续无错读取。只有组合实机整体通过后，才同步本地提交到 GitHub。
之后再开展可持久化数据写入、MTK 输入/显示、性能基准等工作。

**今天的收工交付是：有效文件已落盘并读回一致、Android 在线且分区干净卸载、
证据与源码本地保存、本文完整交接。不是宣称 iOS 已修好或新版实机已通过。**
