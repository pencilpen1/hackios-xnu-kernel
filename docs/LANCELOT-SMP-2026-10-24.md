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
