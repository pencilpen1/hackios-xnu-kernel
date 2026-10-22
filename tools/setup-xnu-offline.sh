#!/bin/bash
# Install the archives fetched by fetch-xnu-toolchain.py into this workspace only.
# No global THEOS or SDK is changed. Sources may have been pre-fetched separately.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
ARCHIVES=$(realpath "${1:?usage: tools/setup-xnu-offline.sh ARCHIVE_DIRECTORY}")
export THEOS=$PWD/local-toolchain/theos
export JOBS=${JOBS:-4}
python3 - "$ARCHIVES" <<'PY'
import hashlib, json, sys
from pathlib import Path
directory = Path(sys.argv[1])
manifest = json.loads((directory / 'manifest.json').read_text())
for item in manifest:
    path = directory / item['file']
    if path.stat().st_size != item['bytes']:
        raise SystemExit('archive length mismatch: ' + str(path))
    digest = hashlib.sha256()
    with path.open('rb') as file:
        for data in iter(lambda: file.read(4 << 20), b''):
            digest.update(data)
    if digest.hexdigest() != item['sha256']:
        raise SystemExit('archive SHA-256 mismatch: ' + str(path))
print('Build archive hashes verified')
PY
for file in ios-toolchain.tar.xz ios-sdk.tar.xz macos-sdk.tar.xz; do
  xz -t "$ARCHIVES/$file"
done
mkdir -p "$THEOS/toolchain" "$THEOS/sdks"
if [ ! -x "$THEOS/toolchain/linux/iphone/bin/ld" ]; then
  tar -xJf "$ARCHIVES/ios-toolchain.tar.xz" -C "$THEOS/toolchain"
fi
if [ ! -d "$THEOS/sdks/iPhoneOS16.5.sdk/usr/include" ]; then
  tar -xJf "$ARCHIVES/ios-sdk.tar.xz" -C "$THEOS/sdks"
fi
if [ ! -d xnu-deps/MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework/Headers/DriverKit ]; then
  tar -xJf "$ARCHIVES/macos-sdk.tar.xz" -C xnu-deps --wildcards \
    '*MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework*'
fi
test -x "$THEOS/toolchain/linux/iphone/bin/ld"
test -d "$THEOS/sdks/iPhoneOS16.5.sdk/usr/include"
"$THEOS/toolchain/linux/iphone/bin/ld" -v
bash xnu-deps/setup.sh
echo "Ready: THEOS=$THEOS bash xnu-build.sh -j$JOBS -k"
