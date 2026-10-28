# Lancelot GPU：loader 三作业 shader triangle（2026-10-27）

> 本文记录 host 验证、构建结果与 2026-10-27 的实机 attempt 证据。
> `gpu_complete=false`、`rendering_verified=false`、`desktop_verified=false` 不变。
>
> **2026-10-27 收尾更新**：attempt16–22 的 tiler OOM / `0x102010000` 已收敛；
> attempt23（2 MiB 对齐 heap + `polygon_list=0`）让 VERTEX+TILER+FRAGMENT
> 全部完成（`sequence=2`、`peak-pages=525`、首像素 `0xff3366cc` 正确），
> 但只覆盖 **1/4096** 像素。loader 不启动的根因是 UEFI 对
> `SizeOfImage`/`.reloc RVA` 布局敏感，8 KiB BSS pad 可启动。
> 详见第 11 节；当前候选 **未通过**。

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

## 9. attempt18/19 继续收敛：从 MMU fault 到 tiler OOM

### 9.1 attempt17：tiler/vertex 的 MMU read fault

- attempt17（JS1=VERTEX+TILER，JS0=FRAGMENT）首链提交后失败：
  `error=-5 submitted=1 sequence=1 guards=0 value=0 peak-pages=14
  failed-stage=4 job-irq=0 job-status=8 mmu=4002c2 gpu=0 fault-pa=0`。
- `AS_FAULTSTATUS=0x4002c2`：exception type `0xC2` =
  translation fault level 2，access type READ，source id 0x40；
  `AS_FAULTADDRESS=0`。即某个 master 在 VA 0 上做了读。
- attempt17 没有 head/tail 诊断；从 MMU 类型判断是 vertex/tiler 阶段的空指针读。

### 9.2 attempt18：输入 attribute buffer 终止项修正后仍在 tiler 阶段

- 新增诊断：`fault-extra=0/0 js-head=2000100/0 js-tail=2000000/0
  gpu-fault-status=0 gpu-fault=0/0`。
- head 已到 tiler（0x2000100），tail 是 vertex（0x2000000）；vertex job 已执行，
  **tiler 阶段**在 VA0 读失败。错误仍是 `mmu=4002c2`。
- 修正：Mesa 的输入 ATTRIBUTE_BUFFER 终止项是 **type=1D、pointer/stride/size=0
  的空 buffer**，不是全零（type0）描述符；全零 type 会让属性预取解引用 NULL。
- 同时把所有未使用的 DRAW 指针（uniform/texture/sampler/push/occlusion/
  thread_storage、vertex 的 position/viewport）填成映射地址，避免硬件读取未用字段
  时解引用 0。

### 9.3 attempt19：tiler 已执行但 OOM

- attempt18 修正后仍是 tiler 阶段失败，但 attempt19 进一步：
  `job-irq=0x20000 job-status=0x60 mmu=0 gpu=0
  js-head=2000100 js-tail=2000100 peak-pages=15`。
- `0x60` = `DRM_PANFROST_EXCEPTION_OOM`。tiler 已经运行，但 tiler heap 16 KiB
  不足（64×64、hierarchy_mask=6 的层级节点/多边形列表超出）。
- 这是实质进展：VA0 MMU fault 消失，问题变成 tiler 内存大小。

### 9.4 attempt20：64 KiB tiler heap

- `LM_TRI_HEAP_BYTES=65536`（16 页）；polygon list 专用页移到
  `LM_TRI_POLY_VA=VA_BASE+0x18000`（heap 之后）。
- loader arena 扩为 32 页（1 元数据 + 30 可用 + 1 guard），triangle 峰值 27 页：
  1 描述符 + 1 着色器 + 4 输出 + 16 heap + 1 polygon list + 4 页表。
- 生产 packer 与更新后的 Mesa 参考页仍逐字节一致；host 测试 peak 27。
- 若仍 OOM，优先继续增大 heap/polygon list；当前 `unsigned page_mask` 支持到
  第 30 个可用页，再大需要改成 64-bit mask。
- 成功条件仍为：`sequence=2`、`peak-pages=27`、`value=ff3366cc`、
  `bad-stage=0`，head/tail 越过 tiler，XNU 只读恢复/SPM all-off/SpringBoard 运行。

## 10. attempt20–22：2 MiB chunk、位图 arena 与新的写故障

### 10.1 attempt20：64 KiB heap 仍 OOM

- `LM_TRI_HEAP_BYTES=65536`、polygon list 4 KiB、arena 32 页、peak 27。
- 结果仍是 `job-status=0x60`（OOM），tiler 阶段（head=tail=0x2000100），
  `peak-pages=27`，XNU 只读恢复正常。
- 说明 64 KiB 仍低于 tiler 的分配粒度。

### 10.2 attempt21：48 KiB heap + 32 KiB polygon list 仍 OOM

- 为了在 30 个可用页内同时放大 heap 和 polygon list：
  heap 12 页（48 KiB）、polygon list 8 页（32 KiB），peak 30。
- 仍是 `0x60` OOM，tiler 阶段；说明不是简单的 polygon list 4 KiB 太小。

### 10.3 2 MiB chunk 线索

Mesa `driinfo_panfrost.h` 明确注释：

```text
/* 64 x 2M = 128M, which matches the tiler_heap BO allocated in
 * panfrost_open_device() for pre-v10 HW. */
```

Bifrost（pre-v10）的 tiler heap 以 **2 MiB chunk** 为单位；小于 2 MiB 的
heap 无法满足第一次 chunk 分配，表现为 `JS_STATUS=0x60` OOM。这解释了
16/48/64 KiB 全部 OOM。

### 10.4 attempt22：4 MiB heap（2 chunks）+ 64 KiB polygon list

- `LM_TRI_HEAP_BYTES=4194304`（1024 页，2 个 2 MiB chunk）；
  `LM_TRI_POLY_BYTES=65536`（16 页）；
  `LM_TRI_POLY_VA=VA_BASE+0x408000`（heap 之后）。
- loader arena 改为 **位图 page_mask**（1054 页：1 元数据 + 1052 可用 + 1 guard），
  `LFD_PAGES=1054`、`LFD_USABLE_LAST=1052`；metadata 用独立 64 KiB 池，
  因为 core 的 `lm_core_vm_map` 会复制 1024 个 `lm_page`（16 KiB），
  原来的 4 KiB metadata 池会在 heap 映射时 ENOMEM。
- peak 1052 页：1 描述符 + 1 着色器 + 4 输出 + 1024 heap + 16 polygon list
  + 6 页表（heap 跨 3 个 2 MiB VA 区域；root/level1/level2 + 3 个 level3）。
- 生产 packer 与 Mesa 参考页仍逐字节一致；host 测试 peak 1052。
- 实机结果：OOM **消失**，但出现新的失败：

```text
error=fffffffb submitted=1 sequence=1 guards=0 value=0 peak-pages=1052
failed-stage=4 job-irq=0 job-status=8
mmu=670003c1 gpu=0 fault-pa=102010000
js-head=2000100 js-tail=2000100
fault-extra=0/0 gpu-fault-status=0
```

- `JS_STATUS=8` = ACTIVE（slot1），head=tail=tiler，说明 tiler 仍在运行/写数据。
- `AS_FAULTSTATUS=0x670003c1` 解码：
  - exception type `0xC1` = translation fault level 1；
  - access type `0x3` = **WRITE**；
  - source id `0x6700`；
  - `AS_FAULTADDRESS=0x102010000`（4 GiB + 0x02010000，超出本测试映射的
    0x02000000–0x0241xxxx 范围）。
- 这不是 OOM，也不是读 NULL；tiler 在向一个 **高位为 1 的地址**写入。
  最可能的方向：
  1. 某个 64-bit 地址字段的高 32 位/标志位被写错（heap top/bottom、
     polygon_list、tiler context 或 DRAW 指针）；
  2. 4 MiB heap 跨 2 MiB/4 GiB 边界时，tiler 的地址算术或我们的高位打包
     出现 carry/bit32；
  3. polygon list 专用页与 heap 的 top 相邻（0x2408000），硬件可能把
     top/polygon 地址组合成 0x102010000。
- 下一步建议（按顺序）：
  1. 在失败日志中直接打印 tiler context 的 word0..7、heap descriptor 的
     word0..7、polygon_list/heap/top/bottom 的 64-bit 值，确认运行时值；
  2. 试 **4 MiB heap + polygon_list=0**（Mesa 原值），看故障是否移动；
     Mesa 的 Bifrost 路径不设 polygon_list，heap 足够大时可能不需要它；
  3. 若仍写高位地址，检查 `lm_v7_tri_prepare` 所有 `(uint32_t)(addr>>32)`
     写入点，以及 4 MiB heap 是否让某个 32-bit 字段溢出；
  4. 可试 2 MiB 整 chunk（heap=2 MiB）加 poly=0，排除 4 MiB/边界因素。

### 10.5 当前候选

- attempt22 已打包为唯一 `current/`，未通过，`gpu_complete=false`。
- loader `c677c01f...`、XNUP `b21c246e...`、XNU `90e93d9e...`、Mu `ac7b...`。
- 设备在 attempt22 采集后停在 XNU/KLOG2；下一次实机需物理进入原生 FASTBOOT。

## 11. attempt23（2026-10-27 收尾）：loader 布局、2 MiB 对齐 heap 与 1 像素覆盖

### 11.1 loader 不启动：UEFI 镜像布局敏感

- attempt23 loader（`fd2c429b…`，132,096 B）在 ESP/FAT/hash 全部正确的情况下，
  UEFI 只输出 239 B USB 前导，没有 `== XNULOADER v3 ==`，屏幕停在 logo。
- 把 attempt22 loader（`c677c01f…`，132,608 B）写回 ESP、保留新 XNUP，
  loader 立即启动并加载 XNUP，证明 UEFI/ESP/FAT/XNUP 正常。
- PE 对比：节结构/96 个 DIR64 重定位完全一致；只有 `.data` BSS 因 2 MiB heap
  少 8 KiB，`SizeOfImage 0x7B9000→0x7B7000`、`.reloc RVA 0x7B8000→0x7B6000`。
- 在 loader 加 8 KiB `used` BSS pad（Variant P，文件仍 132,096 B，hash
  `b8cffb3d…`）后 `SizeOfImage/.reloc` 回到 `0x7B9000/0x7B8000`，**立即启动**。
  说明不是 FAT/文件大小，而是 UEFI 镜像加载器对布局的敏感点。
- 生产源码 `xnuloader/lancelot.h` 已加该 pad（loader 侧 workaround；UEFI 源/
  镜像未改）。exact 约束仍未明，后续用 0/4/8/12 KiB pad 找阈值。

### 11.2 2 MiB 对齐 heap + polygon_list=0

- `LM_TRI_HEAP_BYTES=0x200000`、`LM_TRI_HEAP_VA=0x02400000`（2 MiB 对齐）、
  top `0x02600000`；`LM_TRI_POLY_BYTES=4096`（仅 mapped dummy）；
  TILER_CONTEXT 的 `polygon_list=0`（Mesa Bifrost 值）。
- 结果：attempt19–22 的 `JS_STATUS=0x60` OOM 和 `0x102010000` bit32 WRITE
  fault 都消失；`error=-5` 但 `submitted=1 sequence=2 job-irq=1 job-status=0
  mmu=0 gpu=0 failed-stage=0`。
- receipt：`sequence=2`、`peak-pages=525`、`value=0xff3366cc`（首像素正确）、
  `guards=0`、`bad-offset=4`、`expected=0xff3366cc`、`observed=0`。
  输出 4096 像素只有 1 个匹配，其余 0。

### 11.3 tiler 侧证据（scan/posprobe）

- 顶点 position buffer 正确：`(-1,-1,0,1) (3,-1,0,1) (-1,3,0,1)`。
- tiler context 被 GPU 更新：`polygon_list=0x02408000`、mask 6、fb 63/63、
  heap 描述符 `0x020003c0`。
- heap 描述符被 GPU 更新：`size=0x200000`、`base=0x02400000`、
  `bottom=0x02408100`、`top=0x02600000`。
- 整个 2 MiB heap 只有 4 个非零字：
  `0x8000=0x84`、`0x8004=0x80`、`0x8080=0x9c080006`、`0x8084=0x0400bf7f`。
- viewport max 64→32、scissor 31（`e6cf3e81…`）输出/heap 完全不变；
  Mesa-exact tiler DRAW 输入置 0（`25526d34…`）回归为
  `mmu=4002c2`（level2 READ at VA0），证明 tiler 会预取这些指针，mapped
  dummy 必需。
- 结论：tiler 仍未正确 binning 全屏三角形；待查 position 读取侧/viewport/
  hierarchy/polygon body 格式/vertex→tiler 缓存可见性。详见
  `D:\AI_rom\iOSforLava\LANCELOT-GPU-AI-HANDOFF-2026-10-27-EOD.md` 第 13 节。

### 11.4 当前候选/设备

- ESP loader：`25526d34…`（mesadraw，已否决）；XNUP
  `1f1384d8…`（kernel `254ac100…`）；Mu `ac7b0f8f…`。
- 下一轮先部署 `loader-variantP.efi`（`b8cffb3d…`）或 `loader-scan.efi`
  （`243bd783…`）做基线；证据在
  `local/lancelot-gpu-20261026/private-dma/attempt23/loader-compare/`。
- `gpu_complete=false`、`rendering_verified=false`、`desktop_verified=false`。

## 12. attempt24（2026-10-28）：独立 tiler 探针

- 设计：TILER job 改为独立链（index 1、dependency 0、JS1 head 指向 tiler），
  vertex job 打包但不入链；CPU 直接填 POS_BUF/PSIZ（与着色器输出同样的全屏三角形）。
- 结果：两次 submit 完成（`sequence=2`、`job-status=0`、`mmu=0`）；fragment 仍只写
  1 像素 (0,0)；`heap+0x8000` 的 4 个非零字与 attempt23 **完全相同**：
  `+0:0x84`、`+4:0x80`、`+0x80:0x9c080006`、`+0x84:0x0400bf7f`。
- 含义：tiler 不依赖 vertex job 的执行；但两次 POS_BUF 内容相同，因此还不能
  断定 tiler 忽略 position。下一个单变量是几何：in-bounds 三角形
  `(-0.5,-0.5),(0.5,-0.5),(0,0.5)`，loader `8a119dad…` 已构建待部署。
- 证据：`local/lancelot-gpu-20261026/private-dma/attempt24/`（`NOTES.md`、
  `tilerprobe-runtime.log`、`make_*.py`、`loader-*.efi`）。

## 13. attempt24 第二轮：根因——tiler 读屏幕空间坐标

- in-bounds 三角形 `(-0.5,-0.5),(0.5,-0.5),(0,0.5)` 实测 `match=0`（零像素），
  heap 4 个非零字不变（`8a119dad…`，日志 `centered-runtime.log` `a144f41d…`）。
- 根因：Mesa Bifrost 编译器对 Mali 全系在**顶点着色器内**执行
  `nir_lower_viewport_transform`，把 `gl_Position` 变成**屏幕空间（像素）坐标**；
  tiler 直接用 position.xy 作为像素坐标。我们的 `tri.bin` 从 I/O-lowered NIR
  （`@store_output`）编译，该 pass 只匹配 `store_deref`，所以变换没有进 shader
  （反汇编无 FMUL/FADD/uniform load），position 仍是裁剪空间值。
  于是全屏三角形在像素空间只剩 (0,0) 一个像素，in-bounds 三角形退化为 0 像素，
  viewport 64→32 也因此毫无影响。
- 验证：`loader-screen.efi` `fbf5ed14…`，顶点输入改为屏幕空间
  `(0,0),(128,0),(0,128)`，应覆盖全部 4096 像素（待实机）。
- 长期修复：用未 I/O lower 的 NIR，在 I/O lower 之前运行 viewport 变换，
  并通过 push uniform/UBO 提供 viewport scale/offset；或直接使用 Mesa 用户态
  编译产物。

## 14. attempt24 生产 loader 实机 PASS（2026-10-28）

- loader `c61cc59f…`（生产源码，屏幕空间顶点）实机结果：
  `error=0 sequence=2 job-status=0 mmu=0 value=ff3366cc guards=1 bad-offset=0
  bad-stage=0`；UEFI/XNU receipt 一致 `crc=0xb868599b`；XNU 只读恢复、
  SPM all-off、SpringBoard 运行。
- 诊断 loader `fbf5ed14…` 的 `tri-cover match=0x1000`（4096/4096 像素）证明
  全屏覆盖；tiler heap 随几何变化，说明位置读取/binning 正常。
- 生产源码提交 `a515822`：顶点改为屏幕空间 `(0,0),(128,0),(0,128)`，
  校验器按窗口空间检查 position；host tests 全 PASS。
- 证据：`local/lancelot-gpu-20261026/private-dma/attempt24/`
  （`pass-runtime.log` `3a0fa0a9…`、`loader-pass.efi`、`NOTES.md`）。
- 仍未证明：viewport 变换（由用户态 shader 完成）、纹理/深度/IOSurface/
  CoreAnimation/Metal、XNU GPU 服务；`gpu_complete=false` 不变。
