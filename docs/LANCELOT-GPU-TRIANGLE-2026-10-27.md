# Lancelot GPU：loader 三作业 shader triangle（2026-10-27）

> 本文记录**host 验证与构建结果**。没有部署到手机，也没有实机渲染证据。
> `gpu_complete=false`、`rendering_verified=false`、`desktop_verified=false` 不变。
>
> **2026-10-27 深夜更新**：attempt16 已实机部署并失败，`JS_STATUS=0x58`
> （DATA_INVALID_FAULT）。根因分析与修正见第 8 节；当前候选 attempt17 改为
> 两次 submit（JS1=VERTEX+TILER，JS0=FRAGMENT），并修正 varying/RT 描述符。

## 1. 结论

- 新增 loader-only 的三作业链：**VERTEX（compute job, type 5）→ TILER（type 7）→
  FRAGMENT（type 9）**，用 Mesa 25.0.7 的 Bifrost v7 描述符跑一个覆盖 64×64 视口的
  全屏三角形。
- 顶点着色器 `gl_Position = vec4(position, 0, 1)`；片元着色器输出
  `(0.8, 0.4, 0.2, 1.0)`，RGBA8 正好是 `0xff3366cc`，与现有 clear 测试同色，
  因此 4096 个像素必须全部等于该值。
- 生产 packer 与 Mesa 自己生成的参考页在**所有使用区域逐字节一致**：
  page A 的 `0x000..0x8ff`、page B 的 `0x000..0x1ff`。
- 全部 host 测试与 attempt16 正式 loader/XNU 构建通过；候选已打包但**未部署**。

## 2. 描述符内存布局

GPU VA 基址 `0x02000000`（`LM_VA_MIN`）。page A 可写描述符页，page B 为
`WRITE|EXEC` 着色器页，输出 4 页，tiler heap 4 页，私有页表 4 页。

| 偏移 | 内容 |
|---:|---|
| `0x000` | VERTEX job（COMPUTE_JOB，192 B） |
| `0x100` | TILER job（256 B，Draw 在 +128） |
| `0x200` | FRAGMENT job（64 B） |
| `0x240` | vertex RENDERER_STATE（64 B） |
| `0x280` | fragment RENDERER_STATE（64 B） |
| `0x2C0` | fragment BLEND（16 B，紧跟 RSD） |
| `0x300` | TILER_CONTEXT（192 B） |
| `0x3C0` | TILER_HEAP（32 B） |
| `0x400` | VIEWPORT（32 B） |
| `0x420` | 顶点输入 ATTRIBUTE（RG32F） |
| `0x440` | 顶点输入 ATTRIBUTE_BUFFER + 预取终止项 |
| `0x460` | varying 输出 ATTRIBUTE（POS、PSIZ） |
| `0x480` | varying ATTRIBUTE_BUFFER（general、pos、psiz、终止项） |
| `0x4C0` | position 输出缓冲（3×16 B） |
| `0x500` | PSIZ 输出缓冲（3×2 B） |
| `0x540` | 顶点输入数据（全屏三角形） |
| `0x800` | FRAMEBUFFER（128 B） |
| `0x880` | RENDER_TARGET（64 B） |
| `0x900` | page A 保护字 |
| page B `0x000/0x100` | tri.bin（224 B）/ solid.bin（176 B） |
| page B `0x200` | page B 保护字 |

关键语义：

- 顶点 job 的 `DRAW` 指向 vertex RSD、输入 ATTRIBUTE/ATTRIBUTE_BUFFER、
  varying 输出 ATTRIBUTE/ATTRIBUTE_BUFFER。
- tiler job 的 `DRAW` 是 fragment draw：fragment RSD、position 输出缓冲、
  viewport；`TILER` 指向 tiler context；`PRIMITIVE_SIZE` 指向 PSIZ 缓冲。
- fragment job 的 FBD `Tiler` 指向同一 tiler context；FBD/RT 写输出页。
- 依赖：tiler `Dependency 1 = 1`（顶点 job），fragment `Dependency 1 = 2`
  （tiler job）；链顺序 vertex→tiler→fragment。
- `TILER_FEATURES=0x209` → `max_levels=2`，64×64 的 `hierarchy_mask=6`。
- 关闭 dithering（RT bit15=0、blend `round_to_fb_precision=1`、PU 转换），
  使 shader 的 0.8/0.4/0.2/1.0 精确落在 `0xcc/0x66/0x33/0xff`。

## 3. Mesa 参考链与逐字节校验

- 参考生成器（gitignored）：`local/mesa-bifrost-host/triangle_ref.c`，
  用 Mesa 25.0.7 的 `genxml/v7_pack.h` / `pan_shader_prepare_rsd` 生成
  `local/mesa-bifrost-host/triangle-ref/pageA.bin`、`pageB.bin`。
- 生产 packer：`lm_v7_tri_prepare()`（`xnu/files/bsd/dev/lancelot_mali_private_dma.h`）。
- 对比结果：page A `0x000..0x8ff`、page B `0x000..0x1ff` **完全一致**；
  `0x900`/`0x200` 之后只是本实现加的 guard 模式。
- 着色器二进制由 Mesa Bifrost 编译器生成并嵌入 `xnuloader/lancelot_gpu_shaders.h`：
  - tri.bin 224 B `775a1f606610b86fbbff92b45d75cb2bc13ae7f62178aad02627c27e8c7c0aa4`
  - solid.bin 176 B `e5b6df1817209ab08e10c05bcd4cd5bfca419bf36c80b2d6e9ee7450bf95447f`
  - gpu_id `0x7402`，`no_idvs=true`；NIR 先 inline/lower 再编译。
- 参考 `triangle_ref` 与生产页的逐字节一致，证明 RSD、blend、属性、tiler
  context/heap、FBD/RT 的位域编码没有手工偏移错误。

## 4. Host 测试

- `tools/mali-tests/core_test.c` 新增 `private_triangle` / `private_triangle_mismatch`：
  fake MMIO 按 job type 解析三作业链，逐项检查：
  - job 类型/索引/依赖/next 顺序；
  - vertex RSD 的 shader 指针、attribute/varying 计数、preload；
  - 输入 RG32F 属性与全屏三角形顶点数据；
  - tiler primitive（Triangles、3 顶点）、hierarchy_mask、heap 描述符；
  - fragment RSD/blend 的精确字；FBD/RT 的宽高、tile、输出地址；
  - 模拟顶点/片元写入后，`lm_v7_tri_validate` 检查输出 4096 像素、
    position 12 个浮点、PSIZ 3 个 f16、page A/B guard；
  - 负向：一个像素错误必须以精确 offset/value 失败，cleanup 仍成功。
- `tools/mali-tests/uefi_dma_test.c` 新增真实固件适配器 triangle：
  `peak_pages==14`（1 描述符 + 1 着色器 + 4 输出 + 4 heap + 4 页表），
  `jobs_run==3`，负向像素错误返回 EIO。
- `local/.../test-runtime-analysis.py` 新增 `pixel-triangle` 与
  `triangle-wrong-value` 两个 receipt 夹具，共 17 个 gate + v3 replay 通过。
- attempt16：`receipt/uefi-dma/uefi-power/mali/loader-pmic` 全部 exit 0；
  loader 与 XNU 正式构建 exit 0。

## 5. attempt16 候选

| 产物 | Bytes | SHA-256 |
|---|---:|---|
| loader | 130,560 | `6f9fdb69f4fcc9d4c0717317354fe50eb137402e671c46f30e412bc5882cc52f` |
| XNUP | 15,288,408 | `b21c246e7aedf622...`（完整 hash 见 `candidate.json`） |
| XNU | 15,287,960 | `90e93d9e4bb1b1da97202bbca73eeccc24c496a32be376031c120b9bab745412`（与 attempt14/15 相同） |
| 测试 Mu | 2,097,152 | `ac7b0f8f18eb972e08e7f4df08bbf47bf39f5eb0a1c7403d2e91c1289e4fcdfe` |

bootargs 新增 `lancelot_gpu_triangle=1`，与 `lancelot_gpu_clear*`/`fault` 互斥。
`current/candidate.json`：`gpu_triangle_test_requested=true`、
`gpu_triangle_test_hardware_verified=false`、`gpu_complete=false`。
候选**未部署**；设备仍停在 attempt15 采集后的 XNU。

## 6. 实机验收时应看到

成功 receipt：

- `error=0 submitted=1 guards=1`
- `peak-pages=14`
- `value=ff3366cc`
- `failed-stage=0`、`teardown-state=0x1b`、`sequence=1`
- UEFI+XNU 432-byte v4 receipt CRC 一致
- `lancelot-gpu-triangle: jobs=3 vertex+tiler+fragment color=ff3366cc guards=1
  bad-stage=0 bad-offset=0 actual=ff3366cc`
- XNU 只读恢复、SPM 四域 all-off、SpringBoard 运行

任何一项不符都按实际 stage 定位：0 输出像素、1 position、2 PSIZ、3/4 guard。

## 7. 风险与下一步

- 三作业在**同一个 JC** 内用 dependency 表达 VERTEX→TILER→FRAGMENT；Mesa 正常
  路径把 fragment job 放在第二个 submit。若硬件拒绝跨 job 的 fragment 依赖，
  回退方案是：先提交 VERTEX+TILER 并等待，再提交 FRAGMENT（或改用 IDVS
  `INDEXED_VERTEX_JOB`）。
- 位置/PSIZ 缓冲的预期值来自 Mesa 的 `varyings.pos`/`PSIZ` 约定；若实机写出的
  编码不同，先保留原始 `tri_bad_stage/tri_bad_word/tri_actual`，再按硬件手册
  修正，不能直接放宽校验。
- 14 页是当前 16 页 arena 的上限（1 元数据 + 14 可用 + 1 guard）。若实机页表
  需要额外页，先扩 arena 再重试，不要复用失败 attempt。
- triangle 只证明 shader/光栅/写回路径；纹理、blend、深度、IOSurface、
  CoreAnimation、Metal 和桌面仍未验证。
- OS 用户态服务仍受 WACS2/PWRAP 所有权阻塞；在拿到真实 handoff 证据前不部署
  `lancelot_mali=4`。

## 8. attempt16 实机失败与 attempt17 修正（2026-10-27 深夜）

### 8.1 attempt16 实机结果

- 候选已部署：attempt16 loader `6f9fdb69...`、XNUP `b21c246e...`、Mu `ac7b...`；
  180 秒采集完成，XNU 只读恢复、SPM 四域 all-off、SpringBoard 运行。
- GPU 结果：`error=fffffffffb (-EIO)`、`submitted=1`、`sequence=1`、
  `guards=0`、`value=0`、`peak-pages=14`、`failed-stage=4`、
  `job-irq=0x10000`、`job-status=0x58`、`mmu=0`、`gpu=0`、
  `reset=1`、`quarantined=0`、`teardown-state=0x1b`。
- 输出面首字为 0，position/PSIZ 未进入输出；cleanup/release/safe 均正常。
- `0x58` 在主线 Linux panfrost 中对应
  `DRM_PANFROST_EXCEPTION_DATA_INVALID_FAULT`
  （`drivers/gpu/drm/panfrost/panfrost_device.h`）。
- 结论：job chain 被 JM 接受并启动，但某个 job 的数据访问无效；不是 MMU fault、
  不是 GPU IRQ fault、不是超时。

### 8.2 根因与修正

1. **JS slot 用错（主因）**：attempt16 把整条 VERTEX→TILER→FRAGMENT 链提交到
   JS0。主线 panfrost 的 `panfrost_job_get_slot()` 明确：
   **JS0 = fragment jobs，JS1 = vertex/tiler jobs**。
   修正：两次 submit：
   - JS1：head=`LM_PRIVATE_JOB_VA`，链为 VERTEX(index 1) → TILER(index 2)，
     tiler 的 `next=0`；
   - 等待完成；
   - JS0：head=`LM_PRIVATE_JOB_VA+0x200`，单独 FRAGMENT(index 1)，
     `dependency_1=0`、`next=0`。
   `lfd_write_allowed` 的 head 白名单增加 `LM_PRIVATE_JOB_VA+LM_TRI_FRAG_JOB`。
2. **varying ATTRIBUTE 的 offset_enable 错误**：attempt16 设为 1。Mesa 的
   `pan_emit_vary()` 只在 `PAN_ARCH <= 5` 设置 offset_enable；v7 必须为 0。
   修正后 POS/PSIZ 描述符分别为 `0x17800001`、`0x31c00002`。
3. **RT 的 clean pixel write / dithering 错误**：attempt16 把 clear-only 的
   `clean_pixel_write_enable=1` 用在普通 draw，并把 dithering 关闭。Mesa 的
   `pan_prepare_rt()` 对普通 draw 设 `clean_pixel_write_enable=0`、
   `dithering_enable=1`。修正后 RT word1=`0x06888299`。
4. **输入 attribute buffer size 过小**：attempt16 设为 24 字节。Mesa 使用资源
   （BO）大小；现改为从顶点数据到页尾的 `4096-0x540=2752` 字节。
5. **fragment RSD 的 forward-kill 位**：补上
   `allow_forward_pixel_to_kill=1`，word4 由 `0x41502200` 改为 `0x41582200`。

### 8.3 重新校验

- 修正后的生产 packer 与更新后的 Mesa 参考页再次**逐字节一致**
  （page A `0x000..0x8ff`、page B `0x000..0x1ff`）。
- `core_test`/`uefi_dma_test` 改为两次 submit：
  - 第一次 fake 链必须是 VERTEX→TILER，tiler `next=0`；
  - 第二次必须是单独 FRAGMENT，index=1；
  - 断言 `tri_tiler_result.sequence=1`、`job_result.sequence=2`、
    `jobs_run=3`、`peak_pages=14`。
- analyzer 的 triangle 成功条件改为 `receipt.sequence==2`；fixture 同步为 2。
- attempt17 正式构建 PASS：
  - loader 130,560 B `b99130692955cadb741cbc8f8d806898686ad5addf674afba829d03d03805625`
  - XNUP 15,288,408 B `b21c246e7aedf622c6daab800a007f73bb919b742989f3078228710425e41c48`
    （kernel/cmdline 未变，与 attempt16 相同）
  - XNU 15,287,960 B `90e93d9e...745412`
- attempt17 已打包为唯一 `current/`，**未部署**；设备在 attempt16 采集后停在 XNU。

### 8.4 下一次实机验收

- 需要物理进入原生 FASTBOOT → 恢复 Android → `deploy-test.py --build-attempt 17`
  → 采集 → `analyze-runtime.py --attempt 17`。
- 成功应看到：`error=0 submitted=1 guards=1 peak-pages=14 value=ff3366cc
  sequence=2`、`lancelot-gpu-triangle: jobs=3 ... bad-stage=0 actual=ff3366cc`。
- 若仍失败，下一步先加诊断再改描述符：
  - 读 `JS_HEAD_LO/HI`、`JS_TAIL_LO/HI`（0x1800/0x1804/0x1808/0x180c）和
    `GPU_FAULT_STATUS`（0x3c）进入 receipt；
  - 用 `JS_STATUS` 区分是 vertex/tiler 链还是 fragment 链；
  - 再检查 shader 二进制/RSD、tiler heap 大小（当前 16 KiB）、shader 页 EXEC
    和描述符对齐；不要直接放宽像素校验。
