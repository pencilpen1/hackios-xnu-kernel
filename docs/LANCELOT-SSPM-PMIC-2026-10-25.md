# Lancelot GPU：SSPM 只读 PMIC 协议与接管缺口

## 2026-10-25 收尾：受保护开域 / GPU_ID / 关域实机通过

最新交接文档位于项目根目录
`D:\AI_rom\iOSforLava\LANCELOT-GPU-AI-HANDOFF-2026-10-25.md`。
本轮 `gpu-power-id/runtime-result.json` 已验收**固件阶段开域、真实G52 ID与状态读取、
本轮拥有的域关断**：`GPU_ID=0x74021000 STATUS=0 source=534999909Hz`，
`started=1 off-restored=1 teardown=0 writes=32`。240秒有限采集无panic签名、CRC错误或ring-overrun。
GPU job/DMA、像素、Mesa、IOSurface、CoreAnimation、Metal仍未验证；XNU WACS2所有权未证明。
设备收尾保持XNU KLOG2，本轮结束后GPU四域all-off，不能直接向该关域GPU发job。
今天不再开启下一轮测试；用户要求之后只推进GPU，不重复触摸/八核回归，除非被新改动破坏。

## 最新实机轮次：真实 UEFI PMIC 读取通过，新 Mu / XNU 启动通过

用户授权：从本轮起部署/重启测试无须逐次再问，直到用户明确终止；设备模式、
恢复镜像、hash/profile/CRC/访问检查仍保留。仅原生FASTBOOT可flash，XNU诊断通道
无法远程reboot/flash时仍需要物理操作，不把持续重启授权等同于新的USB命令能力。

`live-pmic-test/deploy-test.py --start` 已实际完成旧ESP/profile/Android Boot只读核验、
新ESP写入/卸载后ro读回，以及原生FASTBOOT product/64MiB Boot检查、flash/reboot。
核验记录每个设备命令exit0；不是仅启动reader或停留在Android的预检。

- 新Mu FD：`a6312edd0a99d1e0212ea9ff4ed386abd873f09521ae8f5065240428d78f9a3e`；
  Boot-v2：`2f7e2ca01df804b918e576d4fbc876e3cba864b637e0cfdc50d5f2fbe1363ff2`。
  从固定原Mu模板保留ramdisk/DTB和全部无关header字段，仅替换kernel/size/image ID；
  原Mu和完整Android Boot均保留。没有调用会删除旧镜像的build_uefi.py。
- loader：`9db4000e305e98dd4878a5add5794ad6f0cabb01fe00fca0d089e4ee935c5618`；
  XNUP：`d9b2e26b2aec77f2552744b80f72fa509cd8d28dbf65ebfe6b74db4d6b2f4824`。
  kernel仍是`5c3c5092...869b65`，维持8CPU、触摸、gpu=0/mali=1。
- 新增 `lancelot_pmic_probe=1` 通过Mu **checked GUID** 调用有界EFI_STATUS读接口，
  不探测旧void接口返回值。只读SWCID→8个rail只读地址→再次SWCID，任一失败
  不发布partial snapshot、无retry；正常输出明确标注selector不是测量电压。
  不导出XNU ownership provider，不认为UEFI snapshot证明所有secure peer排他。
- actual core/parser GCC/Clang ASan+UBSan通过：10个失败位置、非法/重复参数、
  HWCID及SWCID中途变化拒绝、错误输出不变；Mu的双GUID绑定同一实现已host核验。
  Mu正式构建、loader ARM64编译/链接、连续启动XNUP检查通过。

记录位于 `local/lancelot-gpu-20261025/live-pmic-test/`，包含source-before、build/package、
完整部署命令及 `runtime.log(.fastboot)`。420秒采集结束：2,278,182字节XNU runtime，
boot由 `0000000003babe5f` 交接到 `8000000003babe5f`；有限保存段无panic签名、
KLOG2 CRC/长度错误或ring-overrun。结果：`runtime-result.json`。

- **真实SWCID=0x5820**，前后两次确认成功；不能用Android HWCID=0x5831代替它。
- VGPU raw EN/DA_EN/selector/DA selector=`0x1/0x43/0x30/0x2830`；
  VSRAM_GPU=`0x1/0x3/0x40/0x4060`。两路EN/DA_EN均置位。
  DA selector按固定MT6358映射换算为800000/900000uV，差100000uV，
  **是寄存器编码值，不是电压仪表测量**。
- XNU四域仍all-off；TOP parent1、MFGPLL CON1=`0x021493b1`按source公式
  得到ceil 534999909Hz（约535MHz），不能套用早先Android的950MHz采样。
  已把此组合加入实际production平台host fixture：继承rail不调压/不enable/不disable；
  换成950MHz的不兼容组合在任何SPM/PMIC/GPU写前拒绝。仅host控制流证据，
  UEFI rail与后续XNU PLL读取并非一个已证明原子、独占的交接快照。
- 新XNU 8/8 CPU、SSPM production RAM自测、触摸固件CRC/A3均通过。
  最后触摸 `good=5896 down/move/up=56/669/56 SPI-errors=0 reloads=0`；
  坏包严格丢弃，用户视觉效果单独验收。

新增checked GUID已在UEFI云端提交 `aa4ace05`，仍是同一driver对象，无OS分支。
当前设备继续运行本轮XNU。**本轮只完成UEFI有界真实PMIC观测与启动回归，
没有GPU上电/调压/DMA/像素PASS；XNU/secure-world WACS2接管仍未证明。**

## 测试后修复与最新授权

用户随后授权继续真实PMIC/GPU供电、调压、私有DMA测试，并允许修复其Mu依赖，
要求不影响Windows/Linux启动、**不增加OS兼容模式**。云端范围已再次明确：
**UEFI现在提交云端；iOS/XNU继续等待实机验收，不推送。**

已按新增的本地主线构建 `namespace-base-psi-20261021` 的 `source` 符号链接定位
Linux 6.18的 `linux-mt6768-mainline`，核对MT6768 PWRAP、MFG四域和Panfrost寄存器。
该树不能等同于运行Android的r54p2；主线的GPU compatible回退/电源域NOACK也不直接照搬。
恢复后的Android被动补采全部exit0，仍确认r54p2、SSPM ready、MT6358 HWCID=0x5831；
不把HWCID代替SWCID，不读取主动SSPM debug节点。原始记录：`android-after-sspm-test/`。

实际增量：

- SSPM末尾deadline由误报EPROTO改为ETIMEDOUT；两个fixture校验真实请求地址，
  八个rail地址成功路径、15个故障位置的精确错误码、所有写后deadline/回拨均回归通过。
  复核确认原核心已在最终ACK复验后才发布值；未声称发现不存在的提前发布漏洞。
- 私有DMA测试将目标移到页内offset128，核验全部输出页及job描述符后的哨兵。
  新RAM测试覆盖2032个逐字损坏位置；目标值正确但相邻内存变更也失败。
  这仍是host验证，**没有执行实机DMA**。
- Mu统一有界PWRAP事务、EFI_STATUS错误返回、firmware lock及原子UpdateBits，
  MT6358消费者传播错误，修正LDO selector误用电压值。没有OS分支或另一个legacy driver。
  实际Mu源码GCC/Clang ASan+UBSan四组编译/执行通过，正式AARCH64 RELEASE构建exit0。
  DSC/FDF、内存映射、镜像配置、既有storage/build修改和原Mu镜像hash均不变。
  仍未进行该FD的Windows/Linux实机启动回归，不能以未修改配方替代验收。
- UEFI限定9个相关源码/测试/说明文件，本地及云端提交：
  [`c744ab7c`](https://github.com/pencilpen1/Mu-Silicium/commit/c744ab7c8da002825a42cc2828225e6f48aec59d)。
  原始设备日志和二进制固件未上传，用户已有storage/build/submodule修改未提交。
  本地完整记录：`mu-pwrap-fix/`，含source-before、hash审计、host/build日志与git记录。
- XNU增量12项回归、正式编译/链接、XNUP验证均通过，PC唯一 `current/` 已更新：
  kernel `5c3c509244336f788084194a9c72b400b0a77b887b0cfb37cb352f09f7869b65`；
  XNUP `936a8cd737f7295fbfbdadd15f41ec3eebfd2ddbb5644a458589625cb02cb79a`。
  新增代码尚未实机运行/部署，PASS字段已清回false，旧实机证据另存。
  记录：`post-runtime-fixes/`；没有XNU commit/push。

设备最新仍在已恢复的Android；ESP仍是前一节已实测的旧XNU，不是PC的新包。
新Mu FD为2,097,152 bytes/hash `cc5f3c50cf875922c4791685d5b9b230a3b94bf268150aec15a06dae014b2dea`，
尚未打包/刷入。**本次Mu锁只仲裁UEFI协议调用者，不证明secure-world/WACS2排他交接。**
真实mailbox backend和XNU PWRAP ownership provider仍缺失；不安装no-op provider、
不根据IDLE推定所有权，不将 `lancelot_mali=3/4` 的host编译扩大为实机供电/DMA验收。

## 实机结果：RAM 自测通过，真实邮箱仍关闭，已恢复 Android

用户明确授权从 Android 开始重启测试。先只读复验 ESP/profile/Android Boot，随后刷入
hash 固定的既有 Mu（未改固件源码），启动已部署 XNU。单 reader 完成360秒跟踪：
`local/lancelot-gpu-20261025/resume-deploy/startup-sspm-run.log`，boot 从
`0000000004242a6f` 交接到 `8000000004242a6f`，收到1,751,831字节XNU runtime。
已保存段没有 panic 签名、ring overrun、KLOG2 CRC错误或传输超时。

- 实机明确输出 `lancelot-sspm: production protocol RAM selftest=PASS line=0`；
  同时输出 `real backend=absent`。**这是同一production core的合成RAM fixture通过，
  不是物理邮箱、SWCID、rail selector或真实电压的读取结果。**
- `GenericARMPE: 8 of 8 processors are running`；GPU四域仍为all-off。
  修正后的AO诊断为 `own-channel-active=0 shared-sync-busy=6`，没有把共享SYNC_BUSY
  误计为自有WACS2请求。没有新增MFG/GPU读写、PMIC命令、上电或DMA。
- 触摸固件双CRC、A3/version08成立；最后统计 `good=17629 bad=1577 messages=39788`，
  `SPI-errors=0 reloads=0 down/move/up=2/0/2`。原始按下/抬起各两次已取得；
  屏幕点击/拖动的视觉效果仍以用户实际操作为准，不由日志自行宣称。
- 结构化结果：`resume-deploy/runtime-result.json`；部署和启动命令保留在
  `xnu-test-start.{log,json}`。早先Android中启动的空reader退出属预期，与本次成功采集分开。

测试结束后，XNU诊断串口按设计返回 `product: lancelot-xnu`，并拒绝
`partition-size:boot`（`diagnostic logs only`）。首次恢复脚本误用Android serial查询而超时，
**未执行flash**。随后用户按键进入原生FASTBOOT，核验 `product: lancelot` 和64MiB Boot，
才刷回固定Android Boot。恢复后主机打印二进制superblock时发生一次GBK异常，
不再重复刷写；改为只读字节比较完成核验。Android已boot_completed，整Boot hash、
两个profile、未改ESP和Ubuntu clean header全部匹配，所有挂载已释放。
证据：`restore-android-sspm-fastboot.log`、`post-restore-readback.log` 和
`restore-android-sspm.json`；失败记录也未删除。随后修订 `restore-android.py`：
先要求原生serial在场，严格匹配product与64MiB Boot，捕获/保存超时，拒绝覆盖日志，
并在Android启动后核对整Boot hash。离线mock覆盖正常恢复、diagnostic-only拒绝及
错误hash拒绝，未因此再次操作设备。诊断接口本身仍须用户物理按键退出。

当前仍按后文门槛继续协议/交接修复。用户要求每项功能完整修复后再上传GitHub；
本轮没有commit/push，也不把RAM自测或AO观测扩大为GPU功能完成。

## 历史部署状态：XNU 已链接并部署到 ESP，尚未重启实测

> 此节记录实机测试前的状态；最新结果以上一节为准。

用户本轮授权：继续XNU修复/采集/编译/部署，**先不重启**，完成后等待用户确认。
此节覆盖下文首轮“单文件编译、未链接/部署”的历史状态。

- 接入 `lancelot_sspm_pmic.c` / `lancelot_sspm_boot.c` 的正式XNU构建与init链。
  `lancelot_sspm_pmic=1` 只执行共享production core的**RAM模拟自测**；
  缺少真实backend/运行固件ABI/PMIC-pin交接/权限时仍不进行物理邮箱访问。
  其他mode明确拒绝，没有因新增启动参数放开PMIC上电或DMA。
- 增加同一启动期间SWCID变化拒绝，以及清ACK后的IN请求位复验；
  任一不确定失败保持poison，不发布读取结果、不消费迟到响应重试。
- 根据上轮AO日志改正诊断标签：`own-channel-active`仅计FSM/REQ，
  `shared-sync-busy`单独记录，不再将其他PWRAP通道活动误标为WACS2命令。
  SPM双ACK判断覆盖前后两对状态，输出all-off/all-on/partial/unresolved，不扩大访问权限。
- 复查上轮 `startup-ao-trace.log`：有限已保存日志没有panic签名或capture-overrun，8/8核上线。
  `good=0`为未操作采样，固件双CRC/A3与SPI零错误，不以此重做已验收触摸。
  证据：`local/lancelot-gpu-20261025/resume-deploy/previous-runtime-check.json`。
- 12项host回归全部exit0，正式 `xnu-build.sh -j4 build` 编译/链接exit0，
  XNUP连续启动/8CPU/Ubuntu/graphics校验通过，核验新诊断字符串确实进入linked kernel。
  build日志保留原有jobserver及linker选项warning，不声称零warning。
- 所有审计范围内Windows/WSL overlay/实际XNU源码hash一致；未整树覆盖未提交改动。
  loader未重建/改动，Mu固件源码未动；没有GitHub推送。

| 本次实际部署产物 | Bytes | SHA-256 |
|---|---:|---|
| XNU | 15287608 | `5e204cb215ece197a1be3f6d8fa8380c296d792b5a4ed7f41cec367d44959bdb` |
| loader | 78848 | `f0ff737d504c98c93c4d23e31665a2edf96ab87ed4063983307a2ba582e3f304` |
| XNUP | 15287976 | `9e9ed9719f2df3e744388b405ef03247d6acabd33f8b0178c33624c818216065` |

只更新既有 `current/` 与设备ESP：先校验两个原profile和旧ESP文件、以`.new`写入并比较，
同步/卸载后ro重挂载验证3个实际EFI/XNUP文件。Android Boot hash、Ubuntu clean header均不变，
临时ADB staging的3个文件与自身目录已清理；没有新增旧候选/日志归档。
首次ADB push因root创建的staging不能被shell写入而失败，**当时还未写ESP**；
仅删除本轮自己的空目录，改为同一ADB shell创建，保留首次与成功重试日志。

部署前后Android boot ID均为 `8f83e32b-1cd0-457f-8ada-8eaf3c8dbad6`，ADB仍在Android：
**未reboot、未fastboot flash、未更换Boot/Mu，新增XNU尚未运行，不能写RAM自测实机PASS。**
精确记录：`local/lancelot-gpu-20261025/resume-deploy/deployed-no-reboot.json`，
核验：`final-audit.json`；测试、build及部署失败/重试日志同目录。

下一步必须等用户明确确认，再按既有流程切换到固定Mu/XNU并读取KLOG2；
普通Android重启不会自行使用ESP中新候选。先检查 `lancelot-sspm: ... RAM selftest=PASS`
以及8/8核、真实操作触摸、eMMC/USB；真实PMIC/GPU bring-up仍受后文交接条件阻断。

## 本次进展

从会话 `ses_e9cb41c5cfferDm0pl27cWOSQy` 续接。旧会话最后的多次失败是模型限流，
不是新增的实机故障。触摸、8/8 CPU 和有界缓存正确性测试已有验收，不从头重做。
完整 Mali 驱动曾正式链接，但尚无 XNU 硬件 GPU_ID、GPU job、像素或桌面加速验收。

2026-10-25 本次确认设备在 Android（serial `116745f30503`），完成被动补采：
`local/lancelot-gpu-20261025/android-sspm-passive/manifest.json` 的全部命令 exit0，
原始输出及 stderr 都记录长度和 SHA-256。没有 IPI 请求、devmem、手机文件落盘、
改频率/电压、厂测、重启、刷写或分区写入。

- 正常启动日志：`SSPM is ready to service IPI`、`SSPM platform service is ready`。
- PMIC **HWCID=0x5831**；这不是下一步将读取的 **SWCID**，不可混用身份白名单。
- Mali 运行 DDK 仍是 **r54p2-01eac0**，不能用 config 中的 r25p0 替代。
- Boot、SSPM、SCP 存储分区 hash 与前次一致：分别为
  `b5de01520fc62fd3ccd18fffb098f295d0b301838d2b82b1077b05ddf47f7e84`、
  `bd167ec3c99dd2d260da0adf32654f7579cad7b80f24916f86efc48b64c54110`、
  `cdb40ba25c52b04e7c11ef049e7478fbe1e3ffc48bfd26e01f8f5b1763b67a31`。
- 整个 FDT 与前次不同；已逐属性比较 SSPM/PWRAP/Mali 和两个专用桥节点，均一致。
  证据：`fdt-resource-comparison.json`。不把局部相同扩大为整树相同。
- Android mailbox IRQ 有计数，只证明该次 Android 有活动，不证明 Mu→XNU 后仍可用。

## 容易误操作的地方

1. `/sys/devices/virtual/misc/sspm/sspm_alive` 权限虽然是只读，参考源码
   `sspm_plt.c:sspm_alive_show()` 会发送 `0xDEAD` 的同步 PLATFORM IPI。
   **本次仅列出名称，没有读取它**。不能批量 cat 所有 debug/sysfs 文件。
2. SSPM 与 SCP 被审到的 PWRAP 例程走各自桥，不自动证明全部固件/secure-world
   都不访问 AP WACS2。`SYNC_BUSY` 是共享总线活动，不等于 WACS2 同通道命令。
3. SSPM PMIC 服务是另一条 transport，不需要假装拥有 AP WACS2；但它需要
   **PMIC mailbox pin/共享 slot/ACK consumer 的真实排他交接、运行 ABI 和访问权限**。
   不能复用一个 WACS2 锁来声称 mailbox 也被仲裁了。
4. PMIC 读取不会改 PMIC 寄存器，但发送它会写 mailbox payload/doorbell/ACK。
   “只读 PMIC 协议”与“被动 Android 采集”不是同一风险等级。
5. 该 ABI 的 ACK 没有序号、寄存器回显或长度字段。两个可读的 word 不是
   “已验证回复长度/来源”；迟到 ACK 必须靠交接和不可重试的 quarantine 防混淆。

## 固定参考与协议

Linux 参考固定 commit：`6dd120fc1ab487a5ebc6cdcf4ab605d2b43248f7`。
文件清单、下载 URL、长度和 hash 在 `local/lancelot-gpu-20261025/reference-sspm/manifest.json`。
参考来源与运行 Android 二进制的完全匹配尚未证明。

| 项目 | 来源与取值 |
|---|---|
| pin | `sspm_ipi_pin.h`：`IPI_ID_PMIC=8` |
| mailbox | `sspm_ipi_define.h`：mailbox1，start pin=6 |
| doorbell/ACK bit | `sspm_ipi_mbox.c` 传 `mid - mbox->start`，所以 **bit2=0x4**，不是 bit8 |
| slot | `sspm_ipi_mbox_layout.h`：offset10，共5个32-bit slot |
| data/control | 实机 DT：`mbox1_base=0x10460000`，`mbox1_ctrl=0x10461000` |
| aperture | DT data 长度0x100；此ABI实际32 slot/0x80，不能按资源长度猜协议 |
| command | `pmic_ipi.c`：`[0x202, RegNum, 0xffff, 0, 0]` |
| ACK | 同 mailbox、同 slot，两个word：`[status, value]`；无 sequence/length |
| IRQ register | `sspm_define.h`：IN offset0，OUT offset4；OUT 为本位 W1C |
| PMIC map | `lancelot_mali_pmic.c` 注明的 MT6768 `upmu_hw.h` 固定hash，复用其9个只读地址 |

存储固件的已有静态解码出现 pin8、0x202/0x201 和 PMUI 读写失败字符串，
以及专用 MD32 PWRAP 接口。未知指令/分支仍明确标注；不等于整个 handler 的形式证明，
也不等于本次启动正在运行该存储镜像。禁止执行或补丁固件来取得证据。

## 已实现但没有接通实机

`xnu/files/bsd/dev/lancelot_sspm_pmic.{c,h}` 是可编译的协议核心，
**没有 XNU MMIO backend、bootarg 启动入口或 ownership provider**。

- 仅接受 SWCID 和8个 GPU rail status/selector 地址；不存在 PMIC 写 opcode。
- 必须先读到 MT6358 SWCID E1/E2/E3，再接受 rail 读取；失败不修改调用者输出。
- 缺少 backend/交接时，在零 MMIO 下拒绝。
- 排他锁覆盖身份状态、5个请求word、doorbell、读取回复与 ACK readback。
- 外来本pin IN/OUT 忙立即拒绝，零写；别的pin活动不要求整个 mailbox 空闲。
- deadline 10ms 和最多10000 poll，时钟停滞也有界；backend 自身也必须有界。
- 先读回复再仅清自身 bit2，并检查清除结果；不清其他pin的ACK。
- 从第一次可能已应用的payload write起，任何不确定失败都先 poison 共享pin再 release。
  迟到响应不得清掉重试；不能 reset SSPM 或释放潜在的 firmware 排他token来“恢复”。

测试：`tools/test-lancelot-sspm-pmic.py`，GCC/Clang ASan+UBSan 均通过：
65527个非法地址、14个传输故障位置、外来/其他pin IRQ、身份、服务错误、
ACK粘住/请求未清、超时/停钟、poison后零MMIO重试。仅 host fixture，不是实机。
`tools/test-lancelot-sspm-collector.py` 的5项离线测试通过。
实际 XNU ARM64 flags 单文件编译 exit0；见 `local/lancelot-gpu-20261025/sspm-core-check/`。
这不是新增代码的全内核链接或部署声明。

CPU/cache/touch/GPU原有七项 host 回归本次全部 exit0：
`local/lancelot-gpu-20261025/resume-regressions/report.json`。
未覆盖或 reset WSL 已有未提交改动；未触碰 Mu 固件源码；未推 GitHub。

## 下一步：具体还缺什么，怎么取得

1. **固件端 ABI**：继续离线核对固定 SSPM container、mailbox表、PMUI dispatch、ACK写回、
   专用桥与原始字节；保留未知指令，不用字符串存在替代控制流。公开参考只按必要文件下载，
   需记录固定 commit/hash；运行 r54p2 Mali 的板级来源另行匹配，不下载不明“provider”。
2. **Mu启动链的运行身份/访问权限**：核对实际 loader、preloader/TEE/SSPM 初始化与资源保护。
   Android 的ready和存储hash不足以证明另一条启动链。若需新Mu固件源码修改，先问用户。
   只读检查也要先证明对应寄存器/SRAM窗口可访问；不盲读关闭/受保护总线，不 dump 整个RAM。
3. **PMIC pin交接**：证明 XNU 运行时没有 UEFI/AP peer 的请求或 ACK ISR，且专用服务仍运行。
   `xnuloader/loader.c` 保留 UEFI context，不能把“UEFI resident”误当成 ExitBootServices。
   当前源码的同核跳转/中断屏蔽只是证据之一，不覆盖 secure-world 或所有异步使用者。
4. 条件充分后再安装单一 shared-pin backend，先只读 **SWCID**，再采样 EN/DA_EN、
   selector/DA selector。超时/异常立即停止，不开电源、不调压；需要模式切换/部署提前通知。
5. 此后仍按供电/时钟/双ACK/总线保护 → GPU_ID/勘误 → 私有MMU/DMA sentinel → job/pixel
   → Mesa/EAGL/IOSurface → CoreAnimation/opt-in Metal 逐级验收，保持软件回退。
   `lancelot_mali=3/4` 不因协议core编译通过而放行。

## LOG与资料采集办法

Android 被动采集（已有root，无需用户打包整个系统）：

```powershell
python tools/collect-lancelot-sspm-android.py `
  --adb 'E:\shuaji\toolfile\buildtools\android-sdk\platform-tools\adb.exe' `
  --serial 116745f30503 --out local/lancelot-gpu-20261025/android-sspm-next
```

输出目录必须为空；每次命令结束即保存manifest，包括超时/失败和stderr。
`sspm_alive`、厂测、校准、频率/电压/寄存器debug节点不在allowlist。
新原始日志/固件仅放 ignored `local/`，不上传账户、校准、厂商固件或整份原始日志。

XNU/UEFI 使用 Windows fastboot **bulk诊断**，不是COM串口，也不是 adb shell：

```powershell
python tools/read-lancelot-log.py --runtime --quiet `
  --fastboot 'E:\shuaji\toolfile\buildtools\android-sdk\platform-tools\fastboot.exe' `
  --serial lancelot-uefi --out local/lancelot-gpu-20261025/xnu-next.log --duration 180
```

仅在实际UEFI/XNU模式启动这条采集，且同时只能一个reader；允许跟随LLOG1→KLOG2重枚举。
保存 `.fastboot` 传输诊断、boot ID、CRC/长度错误和ring-overrun，不能删掉丢失标记声称全程无错。
FASTBOOT原生模式没有XNU运行日志是正常的；普通reboot不一定恢复Android，不能贸然reboot/flash。

首轮续接时PC `current/candidate.json` 是旧的 AO trace 八核/触摸基线；当时尚未覆盖其产物。
最新已部署版本与等待重启状态见本文第一节。
其manifest某些硬件验收字段未回填，应同时看独立 `spm-result.json` / `ao-trace-result.json`
与实际部署日志，而非只读一个布尔值。已验证profile、Android Boot、固定Mu和旧备份保留。
