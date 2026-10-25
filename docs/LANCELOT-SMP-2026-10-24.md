# Lancelot 八核适配（进行中，未实机通过）

触摸已由用户实机确认验收，代码本地提交`ea3a397`，证据见触摸硬件文档。
当前严重卡顿妨碍进一步操作，用户要求先解决8核；完成后才推进GPU。
允许修改xnuloader EFI应用CPU枚举和XNU SMP保护，不关闭CPU检查；
如需Mu UEFI固件源码修改仍单独询问。不自动推GitHub，不新增旧测试候选归档。

## 已核对的来源和当前阻碍

- Android FDT：6×A55、2×A75，MPIDR reg为`0x000…0x700`，stride=0x100，little mask=0x3f；
  CPU节点名字不是MPIDR，必须使用reg。PSCI 1.0 SMC，GICv3 GICD=0x0c000000，GICR=0x0c040000。
- `xnuloader/loader.c`的Lancelot路径强制`more_cores=0`，设备树只有启动CPU；
  单改`cpus=8`无法让XNU拿到八核。
- 通用Redwood/PSCI路径已有CPU_ON、早期异常记录、scheduler上线和SGI/停核处理，
  需要按MTK拓扑与固件实际状态适配，不能使用旧UEFI firmware-storage mailbox。
- `lancelot_touch_init`和`lancelot_usb_init`仍明确仅允许cpus=1；
  SMP启用须保留单owner/回调串行和经过校验的交接，不仅删条件。
- eMMC读路径已有mutex序列化；所有clock/pinctrl写必须继续masked且不能影响其他资源。
- 8ms轮询使用XNU `delay_for_interval`，可阻塞让出CPU；不能未经测量宣称它一直busy-spin。

## 验收要求

1. loader枚举8个正确MPIDR；仍拒绝固件占用/不可验证的CPU，不强占已经ON的核心。
2. 七个secondary真正进入XNU、加入scheduler；日志/在线CPU数为8，而非仅参数/静态DT。
3. 各核GIC redistributor、timer/SGI与TLB/调度交互正确，无CPU_START异常或伪造上线。
4. 触摸、输入队列、eMMC和诊断日志保持SMP安全及正常运行；不以放宽验证掩盖问题。
5. 八核稳定和可操作确认后，才开始独立GPU适配。

## 当前实现与离线测试

`lancelot_smp.h`新增64-byte纯元数据交接：boot MPIDR、8个唯一物理reg、
七个OFF bit、版本/长度/SMC标识。loader只接受明确cpus=8或原单核模式，
拒绝重复cpus、cpumask、bootcpu和不支持的核数；保持原启动核心，不走旧A75 mailbox路径。
八核时先核对PSCI版本、CPU_ON能力和七个AFFINITY_INFO=OFF，任何ON/PENDING/UNKNOWN都拒绝。
交接先列原boot核，然后优先A75的0x700/0x600，再列其余A55；真实启动仍由XNU CPU_ON负责。

触摸与MUSB日志入口不再只检查cpus=1；cpus=8必须验证完整交接、SMC方法和8个DT CPU reg，
其他配置继续失败。触摸仍只有一个工作线程，input队列及eMMC已有mutex串行；
MUSB协议状态由其唯一thread_call（OPTIONS_ONCE）执行，不创建按核回调。
PSCI添加逐核请求/进入scheduler记录，GenericARMCPU报告实际processor_start失败，
不将静态枚举或固件CPU_ON返回成功当作实际上线。

本轮使用`small_cores=0 small_cores_after=0`：保留DT的真实小核标识，
禁用针对Redwood大核boot、1:1配对的SMT调度优化，让6+2八个独立核心立即参与通用调度。
这不是关闭CPU架构、启动、异常、GIC或在线数检查，也没有修改频率/PMIC电压。

`tools/test-lancelot-smp.py`用GCC/Clang ASan/UBSan编译实际交接实现与实际kernel guard，
覆盖8种boot位置、七个只读查询、所有ON/PENDING/UNKNOWN/负错误拒绝、64 bytes变异和DT损坏。
触摸入口测试覆盖无交接时cpus=8拒绝、合法交接时唯一worker；payload检查显式要求唯一cpus=8。
尚待正式构建和实机日志证明八核上线，不宣称SMP已通过。

## 首次实机失败：平台专家发布晚于安全根等待

正式构建通过，kernel SHA-256=`89cd27b8d9fb4ee1b4fc31eae0fdba63d6559be5b34fcc93c89e702071d3131d`。
八核XNUP=`9a576c8aa12aa45a2a3896d42502655d17257fe52a59d39cf43382ff47c38a62`。
ESP读回/Android Boot块设备hash/两个root profile hash/Ubuntu clean均通过；
仍使用原固定Mu固件。启动后设备停在UEFI，USB没有重新枚举，短采0字节，不能宣称八核成功。

用户屏幕信息：七个secondary由固件报告ON，kernel已返回且留下35749 bytes日志。
`mmu: translation INVALID for c344a4a8`是返回后的页表观察，旧成功checkpoint返回也出现过，
不据此关闭MMU或推定根因。USB拔插仍未恢复；EFI应用只新增有界保留panic屏幕摘要，
不写FAT/原始分区，也不在有核心遗留ON的同一轮重新运行内核。

用户随后提供实际panic：

```
panic(cpu 0 caller 0xfffffff007a737e8): IOKitBSDInit.cpp:1042 Assertion failed: pe
```

已核对真实源文件：`IOSecureBSDRoot`用`waitForMatchingService("IOPlatformExpert", 30s)`后assert(pe)。
GenericARMPE原先在启动boot核、逐个等待七个secondary（PSCI每核最多5秒）之后才registerService，
可超过30秒。修正为super::start和CPU登记完成、boot核ready后，先发布**真正平台专家**，
再继续secondary启动，保留安全根查找与SecureRootName调用，不延长超时或删除assert。
尚待实机确认此顺序修正及获取逐核启动结果；ON不等于加入XNU scheduler，遗留ON也仍待解决。

### 发布顺序修正实机结果：安全根启动恢复，在线仍为1/8

`startup-pe-order.log`取得约516843 runtime bytes，启动log中混有一个NUL；
关键行已原样提取到`local/lancelot-smp-20261024/smp-stage-lines.txt`，不能因为grep把原log
当二进制而误认为阶段日志不存在。
平台专家在boot CPU加入scheduler后、secondary启动前发布，BSD和用户空间继续运行，
USB KLOG2恢复；触摸trim、A3/08、ILM/DLM CRC正确，SPI errors=0；eMMC仍width8/48MHz/batch128/fallback0。
七个CPU_ON均获固件接受，却逐个5秒超时，没有进入arm_init_cpu，最后实际计数为
`GenericARMPE: 1 of 8 processors are running`。**八核尚未通过，不推进GPU。**

下一当前版本在Lancelot已验证八核交接下增加stackless PSCI入口诊断，
context_id指向kernel自有、每CPU独立64-byte物理cache line；首指令记录MPIDR/EL/SCTLR，
读取实际ResetHandlerData.cpu_data_entries，再跳入未修改LowResetVectorBase。
原MPIDR查找和reset_handler验证保持，不更改CPU数据或MMU来绕过失败；
显式clean复位数据及CpuDataEntries，在等待后invalidate读取入口证据。
同时严格将scheduler RUNNING视作成功，不以数值更大的OFF误判上线。
最终构建及实机结果：详见`LANCELOT-HANDOFF-2026-10-24.md`。
七个secondary首入口均到达、实际EL=2、物理ResetHandlerData正确，仍只1/8核上线。
下一阶段须实现EL2→EL1交接，而非继续猜测固件未进入入口或改MPIDR。
2026-10-25用户要求继续正常推进，设备已确认原生FASTBOOT。
