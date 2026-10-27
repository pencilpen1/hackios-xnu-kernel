# Lancelot GPU 交付复审与首轮实机采集

> 最新续接见 [SSPM只读PMIC与接管缺口](LANCELOT-SSPM-PMIC-2026-10-25.md)。
> 本文为首轮复审记录：后续已正式链接完整驱动并完成AO观测，不能用下方旧产物hash
> 或“尚未链接”描述替代 `current/candidate.json` 与独立实机证据。GPU上电/DMA/渲染仍未验收。

## 结论与范围

复审对象：`dshwork/implementation/delivery/lancelot-review-20261025`，包括 `other.md`。
133 个 SHA256SUMS 项全部一致；修改前交付 source 与 implementation 也全部一致。
CPU/缓存三份源（Windows、WSL overlay、实际 XNU）通过最新 cache source audit。
保留主仓库原有 `README.md` / `docs/STORAGE-MODEL.md` 用户改动，未整树覆盖 WSL。

这是有 host/编译证据的 GPU 候选，不是硬件渲染成品。当前仅将独立的 SPM-only
观察入口与共享 WACS2 仲裁层合入八核/触摸基线；完整 Mali engine、IOKit/Mesa/Metal
尚未合入启动链，不使用交付包的旧隔离内核，不安装实验 Metal 插件。

## 实机已取得的事实

用户确认手机在 Android 后，通过原有 root 读取驱动导出、日志、config、FDT、Boot hash；
没有 devmem、频率/电压设置、压力/厂测、手机文件落盘、GPT/格式化或 userdata 大文件操作。
原始证据位于 `local/lancelot-gpu-20261025/android-{first,exports,rails}/`。

- Android kernel：`4.19.325-cip136-st20-ShockwaveKernel-S+`，构建时间 2026-09-25。
- config 仍写 `CONFIG_MTK_GPU_VERSION="mali bifrost r25p0"`；实际内核 Mali 日志却为
  **`r54p2-01eac0`**。不能再把 config/目录名当成运行 driver release。
- sysfs：**`Mali-G52 2 cores r1p0 0x07040002`**。此值是该驱动的导出格式，
  尚未由 XNU 直接读 GPU_ID，不强行把它当成旧 32-bit GPU_ID 布局。
- 实际 OPP 30 档，299…950 MHz；表中电压单位是 10 µV，如 950 MHz 对应
  VGPU=900000 µV、VSRAM=1000000 µV。不是已经验证的 XNU DVFS。
- probe 日志报 MFGPLL=950015 kHz，标称 OPP=950000 kHz；严格向上比较可能跨档，
  后续需核对 PLL 测量/量化边界，不能用任意容差放宽供电安全门。
- 后续闲置采样两路 regulator 均 disabled；关闭状态的 microvolts 是 selector/框架返回值，
  不当成正在供电的实测电压。
- PWRAP 实际绑定 **`mt-pmic-pwrap`**；DT `/pwrap@1000d000` 为 `mediatek,mt6768-pwrap`，
  `/pwraphal@` 引用同一 PWRAP regmap。SSPM 与 SCP 均有配置/节点，日志有 SSPM ready。

### XNU SPM-only 首跑

用户同意从 Android 部署后，沿用既有校验流程：验证 Android Boot、两个 root profile、
ESP 写后只读读回与卸载，再刷固定 Mu 镜像；**没有修改 Mu 源码**。
bootargs 保留 `cpus=8 small_cores=0 small_cores_after=0 lancelot_touch=3 gpu=0`，
移除 `lancelot_cache=2`，只加 `lancelot_mali=1`。

实机日志 `startup-spm.log`，boot ID=`8000000003baa82f`：

```text
GenericARMPE: 8 of 8 processors are running
SPM status=0x06ff042c/0x06ff042c final=0x06ff042c/0x06ff042c
mask=0x7800 stable=1 doubleACK-match=1
PWR_CON MFG=0x1112 CORE0=0x1112 CORE1=0x1112 ASYNC=0x112
shared-WACS2 available=0
```

GPU 四域在两份 ACK 中均 **OFF**；MFG/CORE0/CORE1 有 SRAM PD/ACK，reset 未释放、
ISO/CLK_DIS 置位。没有因此去读取 MFG/GPU，也没有 PWRAP 命令、上电、DMA 或服务发布。
启动采集曾发生一次 USB handoff 的 `Too many links`，后来成功接上 KLOG2；不隐藏该传输错误。
已保存段无 panic，不把有限日志作为长期稳定性证明。

用户实测确认桌面、点击、拖动正常。后续 `touch-spm-operation.log` 有
`good=18716 down/move/up=16/128/16 SPI-errors=0`，与用户反馈一致；坏包仍严格丢弃。
该次操作采集报告 ring overrun 丢失 **1267056 bytes**，因此不能声称整个操作日志无丢失。

## 复审缺口：哪些已经修、哪些仍阻断

### 1. WACS2 共享仲裁——软件层已实现，板级交接未证明 [P1]

主项目新增 `xnu/files/bsd/dev/lancelot_pwrap.{h,c}`：当前及未来 WACS2 用户使用同一锁，
thread/IRQ-enabled、nonrecursive、有限 try-lock 等待、driver→WACS2→peer 锁顺序。
支持 AP 独占交接证据或真实 peer acquire/release；未注册 board handoff 时明确 ENOTSUP。
同通道 coordinated peer 的自有命令结果不确定时，poison 后保留 peer 排他 token，
只释放 XNU 软件锁；不能将未消费的迟到响应交还 firmware，恢复须另行证明。
没有 no-op provider、bootarg 绕过或根据 IDLE 推断所有权。别的处理器用 WACS0/1/3
不自动等于 WACS2 冲突，不要求独占整个 PWRAP。

`other.md` 中旧 HAL 的 `#undef CONFIG_MTK_TINYSYS_SSPM_SUPPORT` 确实存在，
但该 HAL 在相关 config 下还可成为 `PMIC_WRAP_NO_PMIC` stub；它不是当前运行仲裁路径的证明。
已补取固定 commit 的 `mtk_pmic_wrap.c`，其中 CONFIG_MTK_PMIC_WRAP 路径通过共享
`wrp_lock` 调同一 regmap（regmap 还有自己的串行锁）。这可指导 XNU 软件仲裁，
**不能证明固件不直接访问 WACS2**。

后续完整 adapter 必须把 `lm_gpu_register_pwrap_owner()` 接到这个真实共享层，
并证明/协调同通道 firmware 使用者；目前尚未安装这个 full-mode glue。
只添加 mutex 或缺省 peer callback 不构成板级验收。

### 2. REQ 位漏检与自有命令超时的跨驱动风险——已修候选并回归 [P1]

交付 `lancelot_mali_pmic.c` 的 wait_wacs 只检查 FSM/SYNC_IDLE，漏掉独立 REQ bit19，
会放行 `FSM=IDLE + SYNC_IDLE=1 + REQ=1`。交付的自有命令超时只失效局部 PMIC state，
另一个未来 WACS2 用户仍可能消费其迟到响应。

本轮修改 **implementation 源**，冻结的 delivery 包不覆盖：

- 初始 foreign REQ 拒绝且零 CMD/VLDCLR 写；自有 IDLE 完成也要求 REQ=0。
- CMD 写前记录 pending（传输报错也可能已经发出），只有确认清除/IDLE 才解除。
- 自有 pending 命令在错误退出时调用必需的 **shared-channel poison**，再释放仲裁；
  不清除外来响应，不以重新 probe 偷偷恢复。
- `lmp_pmic_io` / `lm_gpu_pwrap_owner` 增加 poison callback；full adapter 缺少它也不启动。
  该 callback 后续必须接入公共 `lancelot_pwrap_poison()`，不能是局部标志。

故障测试覆盖 foreign IDLE+REQ 零写、自有 timeout→poison→release、后续 probe 零 MMIO。
这些是 host 的控制流证据，不是实机排他交接或 PWRAP 传输证据。
另外使用同一个 IDLE+REQ fixture 分别编译冻结交付与修订 production：原交付实际
`result=0 writes=2`（发 CMD 并清响应），修订为 `result=-16 writes=0`。
复现证据为 `local/lancelot-gpu-20261025/review-tests/req-regression.{c,json}`，不是只读源码推测。

### 3. 运行源码与固定参考不一致，不能盲用寄存器布局 [P1]

当前分支固定 commit `6dd120fc1ab487a5ebc6cdcf4ab605d2b43248f7` 的
`drivers/soc/mediatek/mtk-pmic-wrap.c` 未包含实机的 `mt6768-pwrap` / MT6358 match，
不能把其 MT6765 表拿来替代。2026-09-17 的参考文件 hash 也与上述相同，
仅按日期猜测源码仍不匹配。
另一个固定 Kbuild 目录虽叫 r25p0，却默认 release r26p0，仍不是实机 r54p2。
参考允许验证通用协议，但需补齐与运行二进制对应的板级 PWRAP/Mali 来源与 hash。
不存在要求用户寻找未知“provider 下载”的结论；后续优先核对实际驱动来源、板级通道/MPU
与交接条件，只补取必要文件，不整仓下载。

### 4. 平台供电、时钟与失败恢复 [P1]

仍缺 XNU 的 TOP/mfg parent、MFGPLL、infra bus protection 及 rail 受保护采样；
SPM-only 只证明关域状态。完整平台只接受已开启的上游时钟，若 boot 关闭专用 MFGPLL，
现有代码没有经过验收的启 PLL 路径。不能从关域状态跳到 core_start/DMA。
待核对 OPP/PMIC SWCID vs HWCID、enable/DA、所有权、时序和受限失败保留策略。
不改共享 MAINPLL/M4U/SMI，不凭 DT valhall 字符串改架构。

### 5. GPU/MMU/JM 与客户端验收 [P1 before mode4]

四级 GPU4K 页表、非连续 PA、AP/NX、AS0 换根、有限 job wait、故障 reset/quarantine
有实际 production host 回归，但仍缺：真实 GPU_ID/特性/完整 revision 与勘误对应、
私有 MMU/DMA sentinel、job/fence、fault/reset/cache 完成证据。
后续需按实际 driver/架构审核整个相关 errata 清单，不把几项 configuration readback
扩大为所有 G52 勘误覆盖。

IOKit 仍需真实 VM/port 的 Map64→Unmap64→BO_UNMAP→Close、并发 Map/Close、task death、
失败撤销、4K/16K backing 实测。Surface 的 crop/offset/planes/format 限制也须逐项在
实际接纳入口核对，不能只依赖上层调用者。host doubles 不替代内核运行时。

### 6. Mesa、Metal 与桌面仍未验收

softpipe 像素是真实软件结果，但没有 Mali clear/triangle/texture/blend 或 pixel writeback。
还需实际 LMAL renderer 打开、硬件 submitted/completed/fault 计数、签名/trust/loader 与库依赖、
IOSurface/EAGL，再到 opt-in Metal 与 CoreAnimation/UI。默认不注册实验 Metal 插件，
保留软件回退，并明确硬件验收时禁止静默 fallback。

## 本轮验证与产物

- 共享 WACS2 production：GCC/Clang ASan+UBSan，各 local/peer 4 线程×500 次，
  递归/等待超时/peer失败/poison测试通过；无 MMIO。
- 主项目正式 XNU 编译、CPU/缓存/触摸 host、linked checkpoint 与 XNUP 验证通过。
- 修订 implementation 的 platform/core/client/export 共 10 个 compile/run 命令 exit0，
  client 3221 checks、实际 Export.h 62 checks；quarantine 两 fixture 仅关闭刻意泄漏检查。
  生产源测试前后 hash 一致。Windows 日志包装器末尾遇过 GBK 编码异常，
  runner 的 report 已保存并明确区分其 exit0 与外层输出失败。
- 修订 PMIC、adapter、core 使用当前实际 XNU ARM64 headers/flags 单文件编译均 exit0。
  共享层最后增加 poisoned peer token 保留的 host 回归；这项增量未重刷，
  已部署观测内核没有注册/使用 peer，因此当前实机不涉及该路径。
  **修订的完整 GPU 尚未重新全内核链接或部署**；已部署的是下面的观测内核。

| 当前观测产物 | Bytes | SHA-256 |
|---|---:|---|
| XNU | 15221048 | `a12a1614fbb5197411c8c8c460de52a8c835310d6874c440015e55567004d1ea` |
| loader | 78848 | `83cb2ead0a01fcb3e32a67a474b82dd783dc4346e7911b730569790e4ddc4075` |
| XNUP | 15221368 | `43cb74208a882c9bbac42c59c09c00c5f643de9bccc75a45f7002165a9ccadf4` |

当前只更新 `current/`，没有额外归档旧候选；已有参考/部署依赖不删除。
GPU 改动没有推送 GitHub。下一步是匹配运行驱动/通道归属和受保护 AO 观测，
之后才允许 PMIC identity/上电→私有 DMA→像素→Mesa→IOSurface→Metal。
