# Lancelot GPU：从零部署（原始 iOS + 本项目源码）

> 2026-10-28。适用于“设备上只有原始 iOS 16.7.16 系统、仓库里只有本项目源码”的
> 重建流程。GPU 相关的构建参数已经并入 `common/build-lancelot.sh`，不再依赖
> `local/` 下的增量脚本。

## 1. 前置条件

- 设备：Redmi 9 / lancelot（MT6768），已按项目方式分区（ESP=`mmcblk0p48`，
  Ubuntu=`mmcblk0p49`，iOS 卷在 Ubuntu 分区内），Android 可 root（ADB）。
- 固定产物：Android Boot（`E:\shuaji\Lancelot\boot.img`）、测试 Mu
  `ac7b0f8f18eb972e08e7f4df08bbf47bf39f5eb0a1c7403d2e91c1289e4fcdfe`
  （`local/lancelot-gpu-20261025/gpu-power-id/Mu-gpu-power-id.img`）。
- WSL Ubuntu 构建环境（theos、xnu-deps、`/home/pencilpwq/hackios-lancelot`）。

## 2. 构建（含 GPU）

```bash
# 内核（含 lancelot_mali / WACS2 handoff / private DMA）
JOBS=4 bash common/build-lancelot-kernel.sh

# ESP 载荷：现在默认带 GPU 探针与只读 XNU 观察
KERNEL=out/lancelot/mach.lancelot bash common/build-lancelot.sh boot
```

`common/build-lancelot.sh boot` 默认 cmdline（新增部分）：

```text
... lancelot_gpu_power=1 lancelot_gpu_features=1 lancelot_gpu_dma=1 \
    lancelot_gpu_triangle=1 lancelot_mali=1
```

可用环境变量（均为可选）：

| 变量 | 取值 | 含义 |
|---|---|---|
| `LANCELOT_MALI_MODE` | `1`（默认）/`3`/`4` | 1=只读观察；3=XNU 完整上电+私有 DMA；4=再发布 LancelotMali 服务 |
| `LANCELOT_GPU_TRIANGLE` | `1`（默认）/`0` | loader 侧 VERTEX→TILER→FRAGMENT 三角形 |
| `LANCELOT_GPU_ARGS` | 任意 | 整体替换 loader 的 `lancelot_gpu_*` 参数集合 |

例：

```bash
LANCELOT_MALI_MODE=4 bash common/build-lancelot.sh boot        # 发布服务
LANCELOT_MALI_MODE=3 LANCELOT_GPU_TRIANGLE=0 bash common/build-lancelot.sh boot
```

> 注意：Mali tiler 的 position 是**窗口（像素）空间**。loader 三角形直接喂
> 屏幕空间顶点 `(0,0),(128,0),(0,128)`；真实用户态 shader 由 Mesa 的
> `nir_lower_viewport_transform` 完成变换。

## 3. 部署（设备侧）

```bash
# 1) ESP：写入 loader.efi + boot.xnup，自动创建 EFI/HackiOS 并保留旧 BOOTAA64
python tools/deploy-lancelot.py --serial 116745f30503 \
  --files out/lancelot/EFI/HackiOS

# 2) 原生 FASTBOOT：校验后刷测试 Mu（boot 分区必须 64 MiB）
python tools/prepare-lancelot-flash.py   # 或按项目既有 fastboot 流程
fastboot flash boot local/lancelot-gpu-20261025/gpu-power-id/Mu-gpu-power-id.img
```

`tools/deploy-lancelot.py` 逐文件校验手机端 SHA-256；`tools/android/deploy-lancelot-esp.sh`
不依赖旧 ESP 内容，适合从空白 ESP 开始部署。

## 4. 采集与判据

```bash
python tools/read-lancelot-log.py --runtime --quiet \
  --fastboot E:\shuaji\toolfile\buildtools\android-sdk\platform-tools\fastboot.exe \
  --serial lancelot-uefi --out runtime.log --duration 180
```

- **loader triangle**：`lancelot-gpu-triangle: ... guards=1 bad-stage=0 actual=ff3366cc`
  且 receipt `error=0 sequence=2 value=ff3366cc`。
- **XNU mode 3**：`lancelot-mali: WACS2 handoff registration=0`、
  `MT6358/PWRAP probe=0 chip=0x5820`、`powered GPU_ID=0x74021000`、
  `private WRITE_VALUE result=0 output=0x4d414c49`、`DMA smoke=0 ready=1`。
- **XNU mode 4**：同上，并出现 `render service requested=1`；
  用户态可通过 LancelotMali 服务打开 GPU。
- 结束后 XNU 只读恢复、SPM 四域 all-off、SpringBoard 运行；把手机物理带回
  Android（`restore-android.py` 校验固定 Android Boot 后刷回）。

## 5. WACS2/PMIC 约定（不要绕过）

- AP 拥有 WACS2 命令窗口；SSPM 使用 P2P/MD32 窗口；硬件仲裁保持使能。
- XNU 以证据字符串注册 `LANCELOT_PWRAP_AP_EXCLUSIVE`（见
  `xnu/files/bsd/dev/lancelot_mali.c`），只允许 MT6358 GPU 电压/使能相关白名单寄存器。
- 不安装空 provider、不使用 no-op ownership、不以 bootarg 绕过证据。

## 6. 用户态 GPU（下一步）

1. mode 4 启动后，LancelotMali 服务发布；
2. 将 `libOSMesa.8.dylib`、`libredwoodair.dylib` 部署到 iOS 的 `/redwood/lib`
   （见 `docs/LANCELOT-GPU-MESA-BUILD-2026-10-27.md` 的产物哈希）；
3. 运行 OSMesa 最小闭环：`OSMesaCreateContextExt` → `panfrost_drm_screen_create`
   → BO 创建/映射 → clear/triangle submit → 读回像素。

## 7. 用户态 GPU（2026-10-28 实机打通）

这一节记录把 GPU 交给 iOS 用户态进程的已验证做法；本轮已用 `gl_test`
（OSMesa + panfrost）在真机上跑通，桌面合成（CoreAnimation）仍是下一步。

### 7.1 构建 lancelot Mesa（panfrost + LMAL shim）

```bash
cd tools/mesa-lancelot
REFERENCE=/home/pencilqwq/hackios-lancelot JOBS=8 ./mk-mesa-lancelot.sh
# 产物：local/mesa-lancelot-build/out/libOSMesa.8.dylib
# install name /redwood/lib/libOSMesa.8.dylib；overlay/shim 源码就在本目录
llvm-strip -x libOSMesa.8.dylib   # 15.9 MB -> 14.8 MB，装进 4 MiB 级别的 root.apfs 需要
```

overlay（`tools/mesa-lancelot/apply-lancelot.py`）必须包含 **OSMesa ES profile** 两处修改
（等价于用户态仓库的 `tools/mesa/osmesa-es-profile.patch`）：接受
`OSMESA_ES_PROFILE(0x38)` 并把 profile 映射为 `API_OPENGLES2`。缺了它，
`OSMesaCreateContextAttribs` 返回 NULL，gl_test 会打印
`no OpenGL ES context`。

### 7.2 造 GPU 用户态 root.apfs

在已解包的 root 目录（原 profile 的 root.apfs 可用 `apfs-tool census/cat` 还原）里：

- 放 `lib/libOSMesa.8.dylib`（上一步的 strip 产物）；
- 编译并放 `bin/gl_test`：
  `xnu-clang -arch arm64 -isysroot <iPhoneOS16.5.sdk> -O1 -framework CoreFoundation -framework IOKit \
   <libOSMesa.8.dylib> -o gl_test <userland>/src/gl_test.c`
- 在 `etc/rc` 的 `exec /sbin/launchd.ios` 之前加 `/redwood/bin/gl_test fill 64 64 &`；
- 用 `tools/apfs/mkapfs.py -z --plain fw -o root.apfs redwood=<rootdir>` 重新打包。

### 7.3 部署

- 把 root.apfs 写入 `/hackios/20H392/touch-hardware-20261024/`，更新该目录
  `SHA256SUMS`（root.apfs 与 profile.json 两行）；先完整备份原 profile。
- loader 的 ramdisk 表（`xnuloader/lancelot_ubuntu.h`）按 **size + SHA-256**
  接受多个变体：原始 4 MiB (`81a86d26…`) 或 GPU 镜像（当前 `501f6a4e…`，
  5,976,064 B）。换镜像后如果大小变化，需要同步该表并重建 loader。
- 启动参数至少 `lancelot_mali=4`；`lancelot_gpu_*` loader 探针可保留也可去掉。

### 7.4 判据

```text
LancelotMali: service published name=LancelotMali
LancelotMali: user client opened type=0x4c4d0001
LMAL: service opened
LMAL: panfrost device fd=3
gl_test: Mesa; Mali-G52 r1 (Panfrost); OpenGL ES 3.1 Mesa 25.0.7
gl_test: fill: ... error 0
```

### 7.5 仍未完成

- CoreAnimation/backboardd 使用该 GL（需要 `GL=1` 的 libredwood-gl/inject 镜像，
  且 `userland/build.sh` 的编译分支要求 `ios165/rootfs` 已解包）；
- IOSurface/CoreAnimation/Metal 全链路、桌面合成与长期稳定性。
