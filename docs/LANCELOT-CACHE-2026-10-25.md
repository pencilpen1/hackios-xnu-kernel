# Lancelot L1/L2/L3 缓存检查

用户要求先将CPU修复提交GitHub，再检查L1/L2/L3正常工作，GPU暂缓。
GitHub `pencilpen1/hackios-xnu-kernel` 的main已确认到`17f3f39ab403fe3115b134a427194015fe487487`；
上传的是触摸验收/八核交接/EL2→EL1 CPU修复，不含私有镜像、固件或原始日志。

## 各核配置实测（不是DT规格推测）

当前8核实际snapshot，`local/lancelot-cache-20261025/startup-config.log`及`cache-config-lines.txt`：

| CPU种类 | 物理MPIDR | L1I | L1D | 私有L2 | L3视图 |
|---|---|---:|---:|---:|---:|
| A55 r0p0 | 0x000…0x500 | 32KiB/4-way | 32KiB/4-way | 64KiB/4-way | 1MiB/16-way |
| A75 r1p1 | 0x600、0x700 | 64KiB/4-way | 64KiB/16-way | 256KiB/8-way | 1MiB/16-way |

- 八核均`SCTLR_EL1=0x3454593d`，MMU/D-cache/I-cache使能位全为1，未关闭缓存。
- 八核均`CLIDR=0xc3000123 CTR=0x84448004 CCIDX=0`，实际64-byte cache line。
- L3U CCSIDR全部为`0x707fe07a`，64-byte/16-way/1024sets=1MiB；
  不能仅凭每核同读数说存在8份L3，共享布局另据SoC源码/实测确认。
- A55 L2U实际`0x701fe01a`=64KiB，A75 L2U `0x703fe03a`=256KiB；不按理论规格补写容量。
- snapshot在每核initCPU中读取本核寄存器，CSSELR临时选择后恢复；
  禁止在IRQ-off callback打印，由启动线程汇总。没有cache禁用/清空、MMIO、DVFS或PMIC写入。
- `lancelot_cache=0`默认不采；`=1`只抓配置。host覆盖legacy与CCIDX布局和边界，
  实际ARM64 snapshot交叉编译及完整XNU构建通过。

**上述只证明配置、层级和容量，不能替代命中延迟/跨核一致性检查。**

## 功能检查与实机结果

`lancelot_cache=2`保留8核配置采样，另启一个普通测试线程，启动5秒后执行：

- 每次绑定到真实逻辑CPU，核验实际cpu_number；用同一批有界private wired页测试
  8/48/96/256/512KiB、2/4MiB工作集，64-byte间隔随机完整next-index循环；
  warm-up后dependent-load best-of-3，数据或绑定错误就拒绝时序结果。
- next-index读取后先验证边界再使用，固定循环上限；无prefetch流式带宽冒充命中延迟。
- CPU0与其余7核依次进行1024轮release/acquire同步数据交换，
  对每轮8个64-bit数据逐项核验。普通cacheable RAM，不用dc/ic清空或uncached mapping掩盖一致性问题。
- 每对有3秒上限，等待reader确实结束后才复用静态握手区，未结束则停止后续对；
  工作集test页在自身读测完成后释放。测试不屏蔽中断、不改缓存/时钟、电压。
- 时间曲线是cache层级行为的证据线索，但不是按PMU统计严格证明每一访问命中了某级；
  不将short test扩展为长期所有压力/频率档位稳定性通过。

正式构建、Android现有Boot块设备hash验证、ESP ro读回及卸载完成后，
实机`startup-functional.log`保存411538 runtime bytes（原log416392 bytes含NUL/采集标注），
boot ID=`8000000003e5574c`，没有ring overrun报告。结果见`cache-functional-lines.txt`和`cache-result.json`。

- 实际8/8核；8核×7种工作集=56条时序结果，全部`data-valid=1`。
- CPU0→其余每核1024轮全部`PASS done=1`，最终`errors=0`；已保存段没有panic。
- 典型A55：8KiB=5.1ns，48KiB=7.3ns，96KiB=14.8…14.9ns，
  256/512KiB≈33…34ns，2/4MiB≈120…142ns。
- 典型A75：8KiB=5.5ns，96/256KiB≈11…14ns，512KiB≈29ns，
  2/4MiB≈100…133ns。CPU2的48KiB受噪声影响读到9.7ns，不把它硬解释为L1故障。
- 工作集越过私有L2及1MiB L3后延迟阶梯上升，支持层级缓存正常工作；
  数据正确及有缓存的跨核交换通过，不是仅静态容量推测。

**结论：没有发现L1/L2/L3被关闭或缓存数据/跨核一致性异常；
有限功能检查通过。未运行逐级PMU命中计数、独立I-cache命中率或长期压力/全频点测试，
不将有限检查扩大为全部缓存事件严格验证或“整机卡顿已解决”。**

最终测试产物：

| 产物 | Bytes | SHA-256 |
|---|---:|---|
| XNU | 15220296 | `712b2f44486fa751cf1b1a85b9e4f8c1be79c5cb8096bfe46544ae2d6535d361` |
| EFI loader | 78848 | `b5ade0f659b633437b818a4c12894b30fdc35c36b3957efdffe5ec9ab9c5f3c3` |
| XNUP | 15220616 | `3d6e01762fa3fdc3aa2ac0d127dbfbd096db5365de0d7032bd357089c9f3852c` |

`current/deployment-cache-functional-20261025.log`记录部署校验；没有改Mu固件、GPT或userdata。
后续常规build未添加lancelot_cache=2，诊断代码默认不执行；本轮只保留current产物和必要证据，
不另存旧测试候选。新缓存检查代码本地提交，未把此前CPU修复push授权扩展为自动推送后续所有改动。
