#!/bin/bash
# Runs ON the build VM.  Upload with:  vm.sh script scripts/build_fw.sh
#
# Builds the GD32H759 openvela firmware and reports the result.
# Exits non-zero if make fails.
set -o pipefail

WS="${WS:-/home/topeet/Desktop/openvela}"
NUTTX="$WS/nuttx"
TOOLCHAIN="$WS/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin"

export PATH="$TOOLCHAIN:$PATH:/usr/bin:/bin"

[ -d "$NUTTX" ] || { echo "FATAL: no nuttx dir at $NUTTX" >&2; exit 2; }
cd "$NUTTX" || exit 2

echo "=== target ==="
grep -E '^CONFIG_ARCH_CHIP=|^CONFIG_ARCH_BOARD=' .config 2>/dev/null || \
  echo "(no .config - run the board defconfig first)"

echo "=== toolchain ==="
command -v arm-none-eabi-gcc >/dev/null || { echo "FATAL: arm-none-eabi-gcc not on PATH" >&2; exit 2; }
arm-none-eabi-gcc --version | head -1

echo "=== before ==="
ls -l nuttx.bin 2>/dev/null || echo "(no previous nuttx.bin)"

echo "=== make -j4  (start $(date +%T)) ==="
make -j4
rc=$?
echo "=== make exit $rc  (end $(date +%T)) ==="

if [ $rc -ne 0 ]; then
  echo "=== errors ==="
  exit $rc
fi

ls -l nuttx.bin nuttx.hex 2>/dev/null
# NOTE: md5 is NOT a stable identity for this firmware - lib_utsname.c embeds
# __DATE__ " " __TIME__, so every build differs.  Use cmp against a reference
# (expect only the timestamp bytes to differ), not md5.
md5sum nuttx.bin
exit 0
