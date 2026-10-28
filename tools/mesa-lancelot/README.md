# Lancelot Mesa (panfrost + LMAL libdrm shim)

Builds Mesa 25.0.7 for the lancelot iOS userland from the pinned upstream
archive plus this overlay only. The result is the OpenGL ES library that the
LancelotMali kernel service drives:

- `libOSMesa.8.dylib` — OSMesa (OpenGL ES profile included) with the panfrost
  Gallium driver wired to the LMAL libdrm shim (`lancelot-drm/`) and softpipe
  as the fallback;
- `libredwoodair.dylib`, `air_run`, `air_metal_test`, `air2nir` — the AIR/Metal
  frontend pieces kept for the Metal path;
- `gl.xml` / `exported-gl.txt` — what `userland/mkeagl.py` needs to build the
  EAGL bridge (`libredwood-gl.dylib`).

## Build

```bash
cd tools/mesa-lancelot
REFERENCE=/home/pencilqwq/hackios-lancelot ./mk-mesa-lancelot.sh
```

Environment:

| Variable | Meaning |
|---|---|
| `REFERENCE` | workspace that holds `local-toolchain/theos` and `xnu-deps` (default `/home/pencilqwq/hackios-lancelot`) |
| `SDK` / `CC` | override the iPhoneOS SDK and the `xnu-clang` driver |
| `MESA_BUILD` | build root (default `<repo>/local/mesa-lancelot-build`, gitignored) |
| `JOBS` | ninja parallelism |

The script downloads `mesa-25.0.7.tar.xz` (SHA-256 pinned), applies the overlay
to a fresh extraction whenever the overlay fingerprint changes, and writes the
outputs to `$MESA_BUILD/out`. The source archive and venv are cached there.

## Notes

- The overlay adds the OSMesa **ES profile** (`OSMESA_ES_PROFILE`), the
  screen/BO hooks, a fixed 16 MiB tiler heap and the shared LMAL UAPI header
  from `xnu/files/iokit/Kernel/lancelot_mali_uapi.h`; changing any of those
  changes the build key.
- Mesa builds are not byte-reproducible; compare source trees, not hashes.
- The library's install name is `/redwood/lib/libOSMesa.8.dylib`, matching the
  userland image layout.
