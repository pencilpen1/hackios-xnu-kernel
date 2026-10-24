# Lancelot：NT36672A 实际硬件适配（首版，未实机验证）

用户要求停止采集后转向真实触摸。已停止PC采集，未向手机发送stop/reboot；
用户随后说明设备已回原生FASTBOOT，AI只读确认serial=`116745f30503`。
不把此时KLOG不可用当USB故障；最后一次日志无法证明之后为何重启。
本轮新硬件代码尚未刷入，**不能把此前自测PASS扩展为真实触摸通过**。
仅本地提交，触摸修复前不推GitHub，新增UEFI源码修改仍须先询问。

## 已保存日志结论

`local/lancelot-touch-selftest-20261024/user-stopped-capture.json`及
`stopped-capture.analysis.json`：保存2,104,258 bytes原始日志，最后进度报告2,090,476 runtime bytes。
collector退出码4294967295来自人工终止，可能停在一条回复中间，不宣称完整900秒正常结束。
两项内存自测PASS；SpringBoard/backboardd/lsd/mediaserverd各24个样本PID/start不变，
25次eMMC width8/48MHz/batch128/errors0、fallback0，已保存段未捕获user-fault。
用户确认已启动；四组详细log仍为空。CoreData目录恢复/CloudKit无账户、RedwoodAudio未支持
属性、lsd CPU-monitor权限拒绝均保留，未通过假属性、关闭检查或删服务掩盖。

## 新实现的链路

1. `lancelot_touch_platform()`核验loader的MT6768 eMMC交接元数据；不匹配不读MMIO。
   `redwood_input.c`对Lancelot隔离Qualcomm Goodix/按键入口，五指退出默认关闭。
2. `lancelot_touch_platform.h`：按已下载的MT6768 clock/pinctrl表控制SPI0资源。
   GPIO32…35=SPI0 pad0、GPIO1输入；**GPIO92完全不动**；没有PMIC/电源电压写入。
   SPI0门控仅写infracfg bit1，与eMMC相邻bit2严格隔离。26MHz安全父时钟；
   共享mux需要变化时其他五个SPI若有活动门控就拒绝。保存状态，失败/探针结束回退。
   MODE用`+0xc` masked-write，nibble bit3为修改使能；不是普通SET/CLR。
3. `lancelot_spi.h`：SPI0 `0x1100a000` enhanced timing，mode0/MSB，32-byte FIFO分块，
   ACT/PAUSE/RESUME保持一条消息的CS，32-bit FIFO搬运，100ms整条消息上限，结束必reset。
   不写DMA地址/不启用DMA；只屏蔽DT SPI138对应GIC INTID170，不用Linux虚拟IRQ。
4. `lancelot_nt36672_hw.c/.h`：page framing与原始RX两个前导字节剥离；先读trim
   （mask固定`0a ** ** 72 66 03`），错误不自动reset未知设备。随后才允许软件bootloader reset。
   若原始trim在现有控制器状态下不可读，将明确失败并需要受控诊断；**不盲猜其他芯片协议**。
5. 固件来自私有md1的`fw/nvt_dj_fw.bin`，严格长度118784/SHA-256
   `c41ba8aca0d0a96db5471af8a6a54fe6c6c55fc9759f26ca7381d9e2de34902d`。
   装载前核验NVT末标志、110592有效长度、0x08/f7版本、header/分区上下界。
   实际15个分区条目（零长度reserved跳过），长度使用vendor的**last+1**，禁止写入寄存器区。
   有界RAM装载后验证启动状态、CRC_DONE/ILM/DLM错误位、两段实际CRC与golden值、版本和分辨率。
   不写flash，不执行F2C显示关闭、厂测或硬件reset。
6. `lancelot_touch.c`：初版8ms轮询真实点包，**暂不启用EINT/GIC触摸中断**。
   复用已测试解析器/状态机，仅在redwood_touch_post边界将1080×2340归一化为0…65535，
   不再按1125×2436缩放。坏包释放；连续WDT11帧后允许最多两次已验证固件重装，
   100次连续坏读停止本线程并释放全部点，系统继续运行。

## 明确启用级别（默认均关闭）

- 无`lancelot_touch`或`=0`：不映射/读写触摸MMIO。
- `lancelot_touch=1`：只读继承的GPIO/clock快照，随后退出线程。
- `lancelot_touch=2`：配置受控资源，SPI读取trim并回退；不发NVT reset，不上报触摸。
- `lancelot_touch=3`：正确固件存在且完整校验后才配置硬件、身份核验/装载/CRC，再读取真实点包。
  缺固件或hash不匹配在**任何硬件写之前**退出。

只允许已识别Lancelot与`cpus=1`。继续保留`touchscreen=0 buttons=0 gpu=0 wifi=0 adsp=0`
和连续图形/activation参数；不把同名`touchscreen=1`当作MTK启用开关。

## 已通过的离线测试与仍待验证

`tools/test-lancelot-touch-hardware.py/.c`编译实际实现，ASan/UBSan模拟器验证：
FIFO长度1…258和四种对齐、跨chunk CS/暂停、完整RX、无DMA、超时reset；
GPIO masked-write/共享时钟拒绝/只改许可位和回退；真实私有固件15条目解析、逐段RAM内容、
hash先验拒绝、CRC/启动错误、半途I/O失败；实际XNU入口的off/model/stage/SMP保护。
这些是host模型，**不能证明真实时钟、pinmux、SPI时序或HID ABI已通过**。

待完成：真实SPI trim与固件CRC、
点包日志及用户点击/滑动/多指，15分钟回归。若需恢复Android部署，先通知用户并按已验证Boot流程执行；
当前手机保持原生FASTBOOT，本轮没有再刷写或重启。

## 正式编译和候选准备完成（部署前）

- 驱动本地提交`cd57701`；正式XNU build退出0，实际编译`lancelot_touch.o`、
  `lancelot_nt36672_hw.o`并链接六个触摸关键符号，新增文件无编译警告。
- kernel 15,203,408 bytes / SHA-256
  `b8d09840e41f64842cbe0e0dd7b30b4a82c00867b623f47248903f0f61bbcb1d`。
- 成功activation profile私有复制，仅增加`fw/nvt_dj_fw.bin`，414个plist和所有旧文件原样保留。
  新root.apfs 4MiB、APFS clean、469条目独立内核reader读回；SHA-256
  `81a86d265596387f227c7b8a3e021f991293c2900b69a89a38b0f29be6436763`。
- loader pin改为新`/hackios/20H392/touch-hardware-20261024/root.apfs`，不改UEFI源码。
  loader SHA=`ce304334fe18a94599cb5df787d9aa76b22e06ac2302f682071031d8b0cf6ea6`。
- XNUP 15,203,672 bytes / SHA=`35818e41a6b42f530db6095ac58213b17b84a3dce18b694bc40bd8179ded5457`，
  使用`lancelot_touch=3`，保留`touchscreen=0 buttons=0 touch_exit=0`及其他已验证启动保护。
- 证据`local/lancelot-touch-hardware-20261024/`，包含源码同步审计、构建、双编译器host测试、
  APFS检查、候选manifest。此处尚无真实SPI成功证据。

下一步已通知用户：从当前原生FASTBOOT还原校验Android Boot，再部署新目录/读回、卸载，
之后刷同一已验证UEFI直接试真实触摸链路；不将内存/模型测试当作实机通过。

## 实机定位：资源保护退出；r1已读到真实trim

用户确认原候选处于iOS桌面且点击无反应。短时间KLOG2抓取成功，
`local/lancelot-touch-hardware-20261024/desktop-diagnostic.log`保存了启动期关键三行：

```
lancelot-touch: stage=3 inherited clock=0x1000101 gate=0 mode1=0 spi-mode=0x1111 dir=0xcf43d098/0x287e ies=0xffffffff
lancelot-touch: resource setup refused/failed -2
lancelot-touch: worker ended (stage=3); system continues
```

这说明原候选并非成功启动触摸但HID失灵，而是在任何SPI传输前被共享时钟保护拒绝。
PC采集已结束；这次短采有ring overrun，**不宣称全部运行日志完整**。

修正不是删除保护/强制切换26MHz。代码现在只读共享spi_sel和MAINPLL，按MT6768源码的
22-bit PCW、8 integer bits、CON1[26:24] postdiv和/10、/14、/12计算继承source；
SPI0分频和CS setup据此配置。共享TOP clock与PLL从准备到回退均**零写入**。
关闭/隔离/不稳定或不合理parent继续在任何写入前失败；SPI0自身门控bit1和pinmux仍有界配置/回退。
host模型覆盖四个parent、PLL电源隔离、parent关闭、其他SPI全部启用和回退，并禁止所有TOP/PLL写。
SPI PAUSE判定按匹配Linux驱动检查bit1，status=2/3均为PAUSE，FINISH-only仍拒绝，非忽略错误。

用户切回原生FASTBOOT后，r1经Android Boot恢复及块设备hash校验部署；旧ESP候选另存
`EFI/HackiOS/touch-hardware-before-clock-r1/`并读回，两个root profile均hash正确未改。
新XNUP hash=`6b9ca34fe64b2e9ba8dcb30d221c35eb8cf88396161c9f847b54d8b3d6caa683`，
kernel=`8b2290d4dee90a6cfeebc6ca3a5e9fa731f447c853e4ef110c159fd14abb7cb7`。
ESP只读重挂载读回、Ubuntu ext4 clean，再刷同一UEFI；未改UEFI源码。

`clock-r1/startup.log`首次取得真实SPI成功证据：

```
lancelot-touch: preserved shared parent source=109200000Hz PLL=0xff8001e1/0x81150000/0x80000001 result=0
lancelot-touch: trim=0a0000726603 result=0; GPIO92 untouched
lancelot-touch: SPI status=0x2 messages=2 errors=0 timing=0x190037/0x37/0x1b001b
lancelot-touch: RAM firmware result=-11 version=00 reset=00 CRC=00 ILM=00000000 DLM=00000000
lancelot-touch: resource restore=0
```

**目前仅资源、SPI短传输和真实trim通过；RAM装载返回LN_IO，尚无CRC或真实点包通过。**
下一r2增加最后NVT地址、读写/长度、SPI完成字节数、status0/status1、CMD和累计错误诊断，
不改固件hash、CRC、身份或启动验证，继续定位实际失败传输。触摸完全验收前不推GitHub。

### r2/r3：间歇SPI完成事件缺失和CRC寄存器复用

r2仍可能在首次page传输超时。r3加入与eMMC PIO同样的成对短critical_enter/exit，
SPI单消息上限10ms（实际2MHz、最多258 bytes约1.1ms线速），所有退出路径恢复原中断状态；
NVT传输按原厂CTP_SPI_READ/WRITE最多五次重试，重试间100us等待在critical之外。
host测试覆盖timeout/FINISH/非法配置恢复、瞬态恢复和持续五次错误拒绝。

r3实机仍有175/1511条SPI失败；不能声称短critical修复了根因。
首八条错误均为`wire=0/3 status=0/0x1 cmd=0x33010`：page命令已经PAUSE，未捕获status0事件。
对照匹配Linux驱动发现原实现把PAUSE/IE配置与ACT合在同一次写入；r4改为先配置并读回，再单独ACT，
host模型禁止在IE/PAUSE未预先配置时启动。该差异仍须用r4实机错误计数确认，不凭源码宣称解决。

r3成功运行到`reset=a1 CRC=04 ILM=c95c6998 DLM=98257076`，但驱动按既定校验返回LN_CRC，
没有上报真实点包。私有固件ILM golden为`c95c6998`、DLM为`e4fd411f`；
读到的DLM `98257076`恰为实际第14条Header分区的CRC（BIN=0、SRAM=0x24c70、last=0xff）。
这提示应用初始化后R_DLM被复用，并非可以跳过DLM验证。
r4将两个结果和DONE/error位用单FIFO寄存器快照在BOOT_RDY后立即有界捕获，
必须同时匹配ILM/DLM golden且DONE=1、错误位=0，才继续等a1/版本/分辨率；
host模型模拟5ms后R_DLM被Header CRC覆盖，保证保存的是复用前两段实际CRC。

所有候选通过Android恢复hash、旧候选ESP另存/hash、两个root profile hash、ESP读回和Ubuntu clean流程；
UEFI始终使用原已验证二进制，没有UEFI源码改动。

r4实机没有证实prepare-before-ACT已解决问题：仍有187/1488条失败，
最终在Header分区`0x24c70`写入持续五次失败，返回LN_IO，**尚未执行新CRC快照**。
r5继续按coreboot增加STATUS1启动握手，再按实际分频等待本chunk线速时间+2us，
之后才首次读read-to-clear STATUS0，避免在短page传输尚进行时紧密清读完成事件锁存器。
仍必须看到PAUSE完成事件，STATUS1绝不当成成功替代；超时仍有界reset并回退。
host模型延迟event有效时间并禁止提前清读，不把即刻完成的理想模型当作真实时序证据。

### r5：SPI与两段真实CRC首次完全通过，点包仍未验收

实机`clock-r5/startup.log`：

```
lancelot-touch: RAM firmware result=0 version=08 reset=a1 CRC=04 ILM=c95c6998 DLM=e4fd411f
lancelot-touch: last read addr=0x21c78 length=16 SPI=18/18 status=0x2/0x1 cmd=0x33010 messages=1344 errors=0
lancelot-touch: real reports enabled, mode0 FIFO only, poll=8000us; no EINT/GIC touch IRQ enabled
lancelot-touch: stopping after 100 consecutive invalid reads
```

这证明当前SPI传输和BOOT_RDY早期双CRC核验链路已实机通过，不再是仅host模型；
但真实点包未通过，用户触摸未验收。原代码在固件A1（ReK开始）就读EVENT，
且无视Android只在ready IRQ才读点包的条件，可能反复读空闲/未初始化EVENT。
当前修正等待固件自身到A3 NORMAL_RUN（没有发送厂测或baseline指令），
以GPIO1 active-low作为空闲读取条件；接触活动期间继续读最后点包，以捕获抬起。
前四个无效包记录完整66 bytes和解析错误，便于继续定位；checksum、ID、边界和100次坏读停止规则不变。

用户更新要求：**后续不再额外保留测试候选**。自这一修正起统一使用私有`current/`验证产物，
部署不新增旧候选ESP归档；只保留必要诊断和恢复基线，之前已存在的备份不自动删除。
触摸通过前不推GitHub的要求不变。

### 当前点包处理按Redwood结构、NT36672原厂语义修订

r6再次通过真实SPI、版本08、NORMAL_RUN=a3、双CRC，1368条传输errors=0。
最初四个点包完全相同，十个槽位标记均FF，其余为未变化的SRAM内容，checksum不成立。
这不能当成有效空帧，也不能证明有真实点击。原100次坏包退出依然使线程在用户触摸前终止。

用户明确可以参考Redwood，再按Lancelot硬件及内核源码适配。
已对照`redwood_goodix.c:1472…1499`的长期GPIO轮询/80ms漏就绪兜底，及匹配
`NT36672/nt36672.c:1375…1380,1488…1492`的坏checksum仅丢弃本帧、保持驱动存活。
当前采用同一结构：坏帧仍严格拒绝、接触状态机释放旧点、记录bad计数，但不因初始化空闲垃圾永久退出；
100次连续**SPI I/O失败**才停止，WDT最多两次哈希固定固件恢复的保护仍保留。
GPIO1低即读；接触活动时持续读以收抬起；空闲GPIO高每80ms兜底一次以免漏短脉冲。
没有接受坏checksum、伪造HID事件，亦未直接使用Redwood的Qualcomm寄存器或芯片协议。
仍需用户真实点击/滑动/多指后确认点包变化和backboardd响应；当前不宣称触摸已修复。
