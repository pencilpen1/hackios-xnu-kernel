# Lancelot：触摸离线代码审查与启动自测候选

用户要求：编译前再次审查，本地提交后在Android部署并重启验证；触摸修复前不推GitHub。
**本候选没有MT6768 SPI/GPIO/固件装载驱动，不会产生真实触摸。**
下一轮补采后继续直接做真实触摸实机测试；不能把本页自测结果标成触摸硬件通过。

## 编译前复审及修复

相对本地提交`cdbbe43`：

- 包格式明确为CTP_SPI_READ输出66字节（第0字节为命令/offset），不是原始SPI RX；
  原始RX含两字节前导，共67字节，后续总线层必须正确解包。
- 所有解析失败均清除report.valid，状态机拒绝无效/部分report，避免坏包被当成全部抬起。
- WDT短包/空指针以及transport/parse错误清除连续帧计数；11个连续FD/FE帧才报告恢复。
- 掌压DATA_PROTOCOL=30、page=4单独隔离，不能把它后面的数据当普通接触点。
- 修复队列511条时孤立MOVE额外DOWN导致写入513条的边界；同步屏障释放十个HID槽，
  再恢复仍按下的接触点。共享队列中的四个按键也按同样原则保留释放和当前状态。
- `touch_exit=0`在候选明确设置，其他机型默认五指退出不变。没有新增返回计时器。
- 共享纯C启动自测验证真正同时十指、反序ID、最小width/pressure、压力高字节、
  重复/越界/坏包、恢复、掌压和完整释放；host还执行20,000轮有界随机帧/短分配测试。
- 队列共享启动自测覆盖已被HID读取的DOWN、被丢弃的UP、511+2边界、按键释放、
  坐标钳制及uint32计数回绕。原先测试未覆盖这些情况，不能沿用旧“全部通过”结论。

## 自测隔离与构建

`touch_selftest=1`只在`redwood_input_init()`创建设备节点/启动producer之前执行。
协议emit只写私有内存sink；队列fixture在无人读取时执行，退出前清零队列和producer状态。
日志明确标注`hardware=off HID=off`。**测试事件不进入backboardd，不自动点击桌面。**
两个共享头位于`xnu/files/bsd/dev/lancelot_touch*_selftest.h`；主机ASan/UBSan与内核执行
同一组fixture。内核编译包含`lancelot_nt36672_protocol.c`，构建一致性检查覆盖全部新文件。

实际构建在WSL现有XNU树。只在已知旧hash一致时同步本次文件；构建脚本及redwood.patch
应用最小增量，保留旧未提交修改并备份被改文件。不能reset、整树覆盖或误用旧prebuilt。

## 部署原则与状态

候选目录：`local/lancelot-touch-selftest-20261024/`，保留编译前审计、构建/部署/采集日志。
沿用成功activation profile及loader，root pin不变；只替换新编译内核的XNUP及自测bootargs。
`touchscreen=0 buttons=0 touch_exit=0 touch_selftest=1 gpu=0 wifi=0 adsp=0`；
保留`cpus=1 root_apfs=lancelot_ios sbstartup=observe sbactivation=local`，无`-v`或返回计时器。
既有成功ESP文件独立备份，Ubuntu/ESP需卸载和读回确认后才刷同一已验证UEFI。
本阶段不改Mu-Silicium源码，不写触摸固件，不改GPT或格式化；Android Boot备份保留。

此文提交时host共享测试已通过；正式编译、部署、启动自测和900秒回归结果以随后证据补记为准。
