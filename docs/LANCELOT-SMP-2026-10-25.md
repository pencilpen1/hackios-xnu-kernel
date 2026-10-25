# Lancelot 八核：EL2→EL1 交接与实机启动通过

**最新结果：七个secondary真实完成EL2→EL1并加入scheduler，实际8/8核；
用户确认桌面/应用操作稳定，但仍卡顿。八核启动问题已解决，不等于图形加速或整机性能已解决。**

2026-10-25 用户要求恢复正常推进，当前原生FASTBOOT。昨日已收尾并本地提交`c362609`，
完整实机结果见`LANCELOT-HANDOFF-2026-10-24.md`：七个secondary真正到达入口，但均为EL2，
物理MPIDR/ResetHandlerData正确，最终只有1/8核加入scheduler。触摸仍按用户此前实测验收。
先完成8核实际可用，再进入GPU；不新增旧测试候选归档、不自动推GitHub、不改Mu固件源码。

## 参考及实现

匹配Shockwave commit `412a5a948c67309ca5f6ecbf1450dd6caf5cfc1b`的
`arch/arm64/kernel/head.S:el2_setup`及`asm/sysreg.h/kvm_arm.h`，原文和hash保存在
`local/lancelot-smp-20261025/reference-*`。

MTK专用stackless入口按实际CurrentEL分支：

- 已是EL1：直接进入原XNU reset路径，不访问EL2寄存器。
- EL2：保留物理context、安装本地EL2异常向量；核验PSCI提供的MMU-off条件，
  初始化SCTLR_EL2 RES1、小端；HCR.RW选择AArch64 EL1，不启用stage2或中断重定向。
- 按匹配Linux配置CNTHCTL/CNTVOFF、GIC ICC_SRE_EL2.Enable/SRE、ICH_HCR，
  ID寄存器、VTTBR、HSTR、CPTR和MDCR；不修改PMIC、DVFS或MTK共享时钟。
- 设置EL1初始SCTLR、DAIF屏蔽的EL1h SPSR和物理ELR，用ERET到栈外续行，
  核验实际CurrentEL=1并记录，随后进入未修改LowResetVectorBase。
  原MPIDR查找、允许reset_handler及MMU/CPU架构检查全部保留。
- 未支持EL、MMU-on条件或EL2异常：保存ESR/ELR/FAR并请求关闭本CPU；
  不伪装scheduler成功。每CPU两条独立cache line记录入口/降级/异常证据。

离线实际入口交叉编译检查：无C调用、无隐式栈，存在EL2寄存器初始化及ERET，
EL1分支绕过EL2访问，必须真实RUNNING才成功。正式内核构建和实机结果另记。

## EL2修正版首轮：遇到关闭中断时的诊断输出panic

正式XNU构建及部署通过：kernel
`8ce356534ff982d9f73fd07695616ab18bb70fdafcbd6597a846a9b44a5bd687`，
XNUP=`17c4229f20a4030976f1d2d3bc1c188933d01d7d808c93756760f0c4129ad70a`。
短采USB未出现。用户屏幕给出`Console I/O from interrupt-disabled context @serial_console.c:168`，
caller=`0xfffffff007411134`，符号在`_cnputs`。
实际`psci_smp_cpu_running`从关闭中断的GenericARMCPU::initCPU调用，却执行printf，
违反console原有检查。修正为该callback仅发布atomic RUNNING，等待线程观察到实际RUNNING后再printf。
未增加`nointr_consio`、关闭console检查或启用中断绕过；保留真实上线判断。
新增回归禁止CPU running callback进行console输出，仍须下一轮实际日志确认EL2降级及八核。

## 实机八核通过与剩余卡顿

移出中断关闭callback里的printf后，正式kernel构建和读回部署完成：

| 产物 | Bytes | SHA-256 |
|---|---:|---|
| XNU | 15219928 | `226d12ccd6c86870714b0d55886338b36b845ee413ef543becfe0535eedfb152` |
| EFI loader | 78848 | `daa7b39538919f9cfdfe78e131c0edd17569ea08175cfa46a567983102858f95` |
| XNUP | 15220232 | `749c1c2f4eec50a3960e0dd3911413f6d2fce06c119aad8e15f057488c51c96d` |

`local/lancelot-smp-20261025/startup-el2-console.log`采得316098 runtime bytes，
boot ID=`8000000003ce46af`。cpu1…7每核均有：

```
psci: cpu N (MPIDR 0x...) joined scheduler
psci: cpu N transition initial-EL=2 final-EL=1 stage=2 HCR=80000000 CNTVOFF=0
GenericARMPE: 8 of 8 processors are running
```

这是实际scheduler上线和EL转换证据，不是静态DT或CPU_ON成功返回推测。
touch仍A3/08、双CRC正确、1376次固件传输零SPI错误；eMMC width8/48MHz/batch128/fallback0。
用户连续桌面/应用操作后选择“稳定但仍卡顿”。随后同一boot的`user-stability.log`
记录16组真实DOWN/UP，SPI错误0；已保存段没有panic。补采存在ring overrun，
不宣称覆盖全部中间日志或完成长期压力测试。结构化结果`local/lancelot-smp-20261025/smp-accepted.json`。

本轮没有改Mu固件、GPT、userdata、GPU/PMIC频率，未增加nointr_consio或放宽安全根/CPU检查。
当前运行版本保留实际cpus=8，不回退单核。所有改动只本地提交，不自动推GitHub。

下一阶段用户已指定GPU。剩余卡顿可能涉及软件图形/频率/服务负载，
不能将其未经测量全部归因GPU；先审核独立GPU实现并进行受保护硬件身份探测，
保留软件渲染回退，不宣称Mali硬件提交/Metal已通过。
