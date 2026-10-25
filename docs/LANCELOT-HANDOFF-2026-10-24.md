# Lancelot 交接：触摸验收完成，八核复位入口定位中

> 2026-10-24 最后一轮结果已回填：系统进入 iOS 桌面，但实际仅1/8核上线。
> 七个secondary的首入口证据均为EL2，ResetHandlerData正确；缺少EL2→EL1交接。
> 2026-10-25 用户要求恢复正常推进，并确认设备已切至原生FASTBOOT。

## 用户最新要求与阶段顺序

- 触摸已由用户实测确认“没有问题”，已允许提交并转入八核阶段。
- 先真正解决8核，确认稳定可用后才推进GPU；当前不继续GPU适配。
- 可以参考Redwood实现，再按Lancelot硬件、匹配内核源码适配；不能直接套Qualcomm地址/协议。
- 用户已允许修改xnuloader EFI应用及XNU CPU枚举/SMP路径。
  如果必须修改Mu UEFI固件源码，仍需先问。今日未改Mu固件源码。
- 后续不额外归档测试候选：统一当前验证产物；已有旧备份不自动删除。
- 不改GPT、不格式化、不写userdata大文件，不写触摸flash/厂测；恢复与私有固件哈希不变。
- 不关闭CPU检查、安全根assert、异常检查或伪造上线/空属性来掩盖问题。
- 用户要求今日最后一轮多核实测后收尾、写交接，不再发起下一轮刷写或新硬件阶段。
- 本会话未执行git push；不自动推送。远端跟踪状态可能被其他操作更新，不据ahead数量断言是否有人推过。

## 触摸已完成的实际结果

- MT6768 SPI0：0x1100a000，mode0/MSB/FIFO，无DMA，GPIO92不使用。
- 共享spi_sel/MAINPLL只读，按live PCW计算109.2MHz source；SPI约1.95MHz。
- polled FIFO须先STATUS1握手、等待本包线速时间，再读read-to-clear STATUS0。
  之前紧密轮询导致约12% page命令丢完成事件；修正后实机固件阶段1344/1368条传输零错误。
- 真实trim=`0a 00 00 72 66 03`；私有固件118784 bytes：
  `c41ba8aca0d0a96db5471af8a6a54fe6c6c55fc9759f26ca7381d9e2de34902d`。
- BOOT_RDY后立即读BLD CRC，避免正常固件复用R_DLM作为Header CRC；严格双匹配：
  ILM=`c95c6998`、DLM=`e4fd411f`，status=04，版本08/f7，NORMAL_RUN=a3。
- 早期EVENT十个槽FF且checksum坏，拒绝不造输入；按Redwood/vendor方式保持等待，
  GPIO低读取、80ms漏就绪兜底，活动接触继续读抬起。持续SPI I/O故障100次才退出。
- 用户确认点击拖动可响应；log最后已取样`good=4115 SPI-errors=0 down/move/up=109/212/109`。
  独立多指细节未记录，不补写为独立测量；触摸验收依用户实测确认及真实日志。
- 触摸代码本地提交`ea3a397`；验收/八核阶段记录提交`1d18339`。
  详见`docs/LANCELOT-TOUCH-HARDWARE-2026-10-24.md`。

## 八核已实现，不等于实机通过

- Android FDT实际`reg`：0x000…0x700，Aff1编号，6 A55 + 2 A75（0x600/0x700）。
  原cpu@节点名字不等于reg，禁止从名字猜MPIDR。
- PSCI 1.0 SMC，GICD=0x0c000000，GICR=0x0c040000；MAX_CPUS=8的通用XNU/PSCI路径已存在。
- loader不再强制more_cores=0；明确cpus=8时核验PSCI版本/CPU_ON特性和七个CPU OFF，
  保持firmware boot CPU，先列A75 secondary再列其余A55；不做旧A75 UEFI mailbox启动。
- `lancelot_smp.h`64-byte纯元数据ABI记录8个唯一MPIDR、boot、OFF mask、SMC；
  触摸/USB cpus=8要求交接及8个DT reg/方法均验证，不简单删除单核条件。
- `small_cores=0 small_cores_after=0`：八个独立核立即参与通用调度，
  不套Redwood对称的1:1 SMT配对；没有关闭CPU启动/架构/异常检查。
- CPU请求与加入scheduler逐核记录；只能`RUNNING`视作成功，不能将更大的`OFF`误判上线。
- eMMC已有mutex串行，触摸单worker/input mutex，USB唯一OPTIONS_ONCE thread_call。
  尚需真正多核运行后验证这些路径，不以代码审查当作运行并发测试。

## 已确认的两类实机失败

### 1. 安全根等待平台专家超时：已修正并实机恢复

用户屏幕panic：`IOKitBSDInit.cpp:1042 Assertion failed: pe`。
真实`IOSecureBSDRoot`等IOPlatformExpert 30秒；原GenericARMPE在七个PSCI同步等待
（每核5秒）后才registerService，可超过30秒。
修正：super初始化、CPU登记、boot CPU ready后先发布真正PE，再启动secondary。
未改IOSecureBSDRoot、未延长超时或删除assert。修正实机USB/root/userland继续运行。

### 2. CPU_ON获接受，但七个secondary未进入C：仍未解决

`local/lancelot-smp-20261024/startup-pe-order.log`（原始混有1个NUL，grep可能忽略）明确记录：
八个CPU注册，boot核入scheduler；七次CPU_ON成功后每次5秒无arm_init_cpu迹象；
最终`GenericARMPE: 1 of 8 processors are running`。不要将mcache的8 CPU或静态DT当作8核上线。
提取文本`smp-stage-lines.txt`保留实际阶段信息。触摸双CRC/A3/版本及SPI0零错误、
eMMC width8/48MHz/batch128/fallback0依然通过。

早期panic返回时七个secondary被firmware报告ON，并且USB未重枚举。
不要在同一运行中重新执行内核；须重启到原生FASTBOOT再部署。
`mmu: translation INVALID for c344a4a8`是返回后的TTBR0页表观察，不是根因证明；
旧成功checkpoint返回也有同类输出。EFI已加有界panic屏幕摘录，无磁盘写入。

## 今日最后一轮的诊断代码

`psci_smp.c`新增MTK专用stackless入口，通过PSCI context_id接收kernel自有物理64-byte note：
第一条指令记录entered/MPIDR/CurrentEL/SCTLR，读取ResetHandlerData.cpu_data_entries，
然后跳入未修改LowResetVectorBase（保留MPIDR和handler验证）。boot核同时打印
ResetHandlerData实际/预期、CPU数据物理地址和reset handler。
entry note每CPU独立cache line；原代码/CPU数据缓存clean并显式clean ResetHandlerData及CpuDataEntries；
失败后invalidate读取note。不主动改复位数据来掩盖未初始化问题。

离线测试：GCC/Clang ASan/UBSan交接/坏DT/ON/PENDING/UNKNOWN拒绝、实际guard；
实际AArch64入口cross-compile确认无栈/BL/BLR，跳原vector；实际PE发布顺序静态回归。
第一次正式构建因OSFMK引入BSD libkern头导致类型冲突失败，未部署该失败产物；
共享header改为仅依赖DT和builtin定长复制，避免BSD类型污染，不改变校验语义。

## 位置、构建与恢复

- Windows仓库：`D:\AI_rom\iOSforLava\hackios-xnu-kernel`。
- WSL实际构建：`/home/pencilqwq/hackios-lancelot`，XNU：`xnu-src/xnu-8796.121.2`。
  **有其他未提交改动，不能整树复制/reset。** 每次同步按`source-audit.json`先验hash，仅更新所需路径。
- 当前唯一验证产物：`local/lancelot-touch-hardware-20261024/current/`，
  `candidate.json`为当前kernel/EFI/XNUP hash，不额外复制旧候选。
- 八核证据/选择性同步：`local/lancelot-smp-20261024/`：`build.py`、`rebuild-current.py`、
  `source-audit.json`、`build-result.json`及逐轮必要日志。源码hash审计更新可能先于编译成功，
  必须同时核对build退出/manifest与部署日志，不凭source-audit判定产物成功。
- 部署脚本：`local/lancelot-touch-hardware-20261024/current/deploy.py`，
  先恢复hash固定Android Boot、校验块设备，ro/noload验证Ubuntu两个profile，写ESP文件/ro读回/卸载，
  检查ext4 clean，再刷相同固定Mu镜像。脚本日志用独立文件名，避免覆盖证据。
- Android Boot=`E:\shuaji\Lancelot\boot.img`：
  `b5de01520fc62fd3ccd18fffb098f295d0b301838d2b82b1077b05ddf47f7e84`。
- Mu镜像=`local/lancelot-cp5-hardware-20261023/Mu-lancelot-msdc-fixed.img`：
  `a23bdf6ddf8bcde9275b57240ba87abe60373651f2e3a5fd254ed2e4c6074f7c`。
- 私有touch root路径=`/hackios/20H392/touch-hardware-20261024/root.apfs`：
  `81a86d265596387f227c7b8a3e021f991293c2900b69a89a38b0f29be6436763`，不重建/改写。
- 原成功activation profile保留：hash `86fbf42a6cef3610a260420394f9c0dd9eeecb43bc21089bab2b69d984e867a0`。
- USB诊断仅getvar/oem log/KLOG2，不远程刷写或reboot；没有日志时先问设备是否FASTBOOT/UEFI，
  不将正常模式切换误诊USB失败。不启动并行日志采集器。

## 最后一轮最终结果与下一步

- 正式kernel：15,203,544 bytes，SHA-256
  `52e11a1df28a71530352c28f9ef78ca2b1deace488b0ec8b443abfe4cdb7bdda`。
- EFI：78,848 bytes，`d7ed1a8b3cafd168cbbd5fae308bd3527180b4a86e638c003f4c180b602b8b4a`。
- XNUP：15,203,848 bytes，`a0c7ed9a41f8ef6bb8ee46c55c9ca92ecefbd5773baff187dd7da93269615fea`。
- 最后部署由已校验Android直接执行`current/deploy.py --already-android`，
  `current/deployment-smp-first-entry.log`有ESP三文件ro读回、profile hash与卸载验证。
- `final-first-entry.log`：90秒有界采集完成，567391 runtime bytes，原文件568091 bytes含1 NUL，
  boot ID=`8000000003f96ac7`。提取`final-stage-lines.txt`，结构化`final-test-result.json`。
- 七个secondary：`first-entry=1 EL=2 SCTLR_EL1=0x30d00800`，
  实际MPIDR=`0x81000700/600/100/200/300/400/500`，低16位和预期一致；
  `reset-entries=0xc3b376d8`与boot核打印的实际/预期完全相等。
  固件确实进入了XNU自有入口，不能再按“CPU_ON未执行”或“MPIDR错误”处理。
- 最终仅`cpu0 joined scheduler`和`1 of 8 processors are running`，七个secondary仍5秒超时。
  当前复位代码配置EL1系统寄存器/页表，但未将实际EL2启动核切至EL1，这是下一修复点。
- 本轮触摸固件双CRC、A3、版本08、SPI零错误；eMMC width8/48MHz/batch128/fallback0。
  没有本轮手势复测，不把good=0的启动空闲计数写成触摸回归通过；此前用户验收仍有效。
- 收尾时用户确认iOS桌面。2026-10-25再次确认原生FASTBOOT，后续根据最新状态操作。
- 次日先按匹配Linux的EL2 setup与XNU reset路径实现栈外EL2→EL1 handoff，
  核对HCR/计时器偏移/GIC系统寄存器权限及ERET入口，保留原MPIDR/handler/异常检查。
  不改boot CPU的EL1路径、不以关闭MMU或CPU检查修复；如必须改Mu固件另问。
- 八核全部实际加入scheduler且稳定可用后，才推进GPU；不从头重做已验收触摸。
