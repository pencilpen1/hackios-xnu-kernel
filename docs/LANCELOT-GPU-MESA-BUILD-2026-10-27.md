# Lancelot GPU：panfrost/Mesa 用户态离线构建验收（2026-10-27）

> 本文只记录**离线构建**结果。它不改变 `gpu_complete=false`，也没有部署到手机。
> 构建树在 `dshwork/implementation`，不属于本 XNU 仓库；本文件是仓库内的验收记录。

## 1. 结论

- 用**干净的 Mesa 25.0.7 归档**加上 `dshwork/implementation/tools/mesa` 的 Lancelot overlay，
  交叉编译到 **iPhoneOS 16.5 / arm64** 成功，exit 0，`warning:` 计数为 0。
- 构建启用了 `-Dgallium-drivers=softpipe,panfrost`，产物 `libOSMesa.8.dylib` 同时含：
  - LMAL v1 libdrm shim（`drmOpenWithType`、BO/MMAP/WAIT/SUBMIT、syncobj、surface import）；
  - Mesa panfrost Gallium 驱动与 `panfrost_drm_screen_create`；
  - `osmesa_target.c` 的硬件优先分支：未设置 `OSMESA_SOFTWARE` 时先尝试
    `drmOpenWithType("panfrost")`，失败才回退 softpipe；`OSMESA_REQUIRE_HARDWARE` 可禁止回退。
- 这证明**用户态源码和 Meson 接线可以完整编译/链接**，不是设备运行、服务发布或渲染验收。

## 2. 环境与命令

- WSL Ubuntu；`meson 1.12.1`、`ninja 1.13.2`、`mako 1.4.3` 为用户级 pip 安装。
- 交叉编译器：`/home/pencilqwq/hackios-lancelot/xnu-deps/host/bin/xnu-clang`。
- SDK：`/home/pencilqwq/hackios-lancelot/local-toolchain/theos/sdks/iPhoneOS16.5.sdk`。
- 命令：

```bash
cd /mnt/d/AI_rom/iOSforLava/dshwork/implementation/tools
JOBS=8 ./mk-mesa-lancelot.sh
```

- 构建日志：`dshwork/implementation/tools/mesa/build/mk-mesa-lancelot-run1.log`
  SHA-256 `a38ab2b4244cbc5841c5997b3b896d12e0d04995b628ef8b225a5758a11b1cd9`。
- overlay fingerprint key：`be6aa45552ee9b0b`（`fingerprint.py` 输出按脚本方式去换行后哈希）。
- 干净源码：`.../build/source-be6aa45552ee9b0b/mesa-25.0.7`；
  构建目录：`.../build/darwin-be6aa45552ee9b0b`。

## 3. 产物

| 文件 | 字节 | SHA-256 |
|---|---:|---|
| `libOSMesa.8.dylib` | 15,920,584 | `843b5b59fe4a5521781695484352ca0025c06a31a1964e829f3db3448f9b7bb9` |
| `libredwoodair.dylib` | 9,736,680 | `01be285e2b925e7d56b1c1df422a5b4ff69a303922fc2b8ab4538336364f413a` |
| `air_run` | 9,685,056 | `8b6623299240340a5c54e254d7978fa76c682ca55fc6bf2c7b8dd483d3062732` |
| `air_metal_test` | 186,376 | `b7cc93593f5d686c914413410b3500477f82ca03ab43717dd5e7d2ecc172e2f4` |
| `air2nir` | 1,755,664 | `0e0d5dd7b672eead7732396b33dff3e1eee9739a812079c2ce6036b8ca6dc34a` |

- `libOSMesa.8.dylib`：Mach-O 64-bit arm64 dylib，
  install name `/redwood/lib/libOSMesa.8.dylib`，1296 个 `gl*` 导出。
- `libredwoodair.dylib`：install name `/redwood/lib/libredwoodair.dylib`。
- 依赖：`IOKit.framework`（shim transport）、`libz.1.dylib`、`libSystem.B.dylib`、
  `libc++.1.dylib`；没有新增未预期的 dylib/framework。

## 4. 已核对的符号/字符串

`libOSMesa.8.dylib` 内可见（本地符号，`fvisibility=hidden` 不导出）：

```text
_drmOpenWithType
_panfrost_drm_screen_create
_panfrost_drm_screen_create_renderonly
_lancelot_bo_map / _lancelot_bo_unmap / _lancelot_bo_import_size
_lancelot_drm_dup / _lancelot_drm_job_stats
_pan_kmod_dev_create / _pan_kmod_dev_init / _pan_kmod_bo_alloc
```

字符串：`OSMESA_SOFTWARE`、`OSMESA_REQUIRE_HARDWARE`、`panfrost`、LMAL 服务名。

## 5. 这证明了什么、没证明什么

- **证明**：从干净归档应用 overlay 后，Mesa panfrost 用户态、LMAL shim、AIR frontend、
  OSMesa 目标可以在 iPhoneOS 16.5 SDK 下完整编译并链接；OSMesa 的 panfrost 选择路径在库里。
- **没证明**：没有在设备上运行；没有创建 LancelotMali 服务；没有 WACS2/SSPM 所有权；
  没有 BO 映射、submit、像素或 GL 渲染；`rendering_verified=false`、`desktop_verified=false`。
  设备状态没有因这次构建改变。
- Mesa 构建不保证逐字节可复现；以上 hash 只标识**这一份文件**，跨次构建不可按 hash 比较代码。

## 6. 下一关

服务发布仍受 WACS2/PWRAP 所有权阻塞（见 `LANCELOT-GPU-AI-HANDOFF-2026-10-27-EOD.md` 第 6 节）。
在拿到真实 AP_EXCLUSIVE 证据或 coordinated peer handoff 之前，不部署 `lancelot_mali=4`。
拿到后，用户态最小闭环是：`libOSMesa` 放入 `/redwood/lib` → `OSMesaCreateContextExt` →
`panfrost_drm_screen_create` → BO create/map → 一个 fragment clear/triangle submit → 读回像素；
每一步都要与内核 LMAL receipt/日志分别验收，不能用一个 PASS 覆盖另一个。
