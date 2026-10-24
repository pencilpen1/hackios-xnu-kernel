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
