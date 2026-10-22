#!/bin/bash
# Start a port of HackiOS to another device: a profile and a loader platform description to fill in.
#
# usage: common/new-device.sh <codename> [path/to/device.dtb]
#
# Writes common/devices/<codename>.conf (from _template.conf) and common/devices/<codename>.platform.c,
# a `struct platform` for xnuloader/loader.c. Given the phone's device tree (from its vendor_boot, or
# `adb pull /sys/firmware/fdt`), it fills in what can be read from it: the GIC addresses, the number of
# CPU cores and how MPIDR numbers them, and which cores are the small ones. What it cannot read is left
# as TODO; docs/PORTING.md walks through all of it.
. "$(dirname "$(readlink -f "$0")")/lib.sh"
CODE=$1; DTB=$2
[ -n "$CODE" ] || die "usage: common/new-device.sh <codename> [device.dtb]"
[[ "$CODE" =~ ^[a-z0-9_-]+$ ]] || die "codename: lower-case letters, digits, - and _ only"
CONF=common/devices/$CODE.conf PLATC=common/devices/$CODE.platform.c
[ -e "$CONF" ] && die "$CONF exists already"

sed -e "s/^NAME=.*/NAME=\"TODO vendor model ($CODE)\"/" -e "s/^UEFI_TARGET=.*/UEFI_TARGET=$CODE/" \
	common/devices/_template.conf > "$CONF"
[ -n "$DTB" ] && sed -i "s|^DTB=.*|DTB=$(readlink -f "$DTB")|" "$CONF"

GIC_DIST=TODO GIC_REDIST=TODO CORES=8 SHIFT=8 SMALL=0x0 HVC=0
if [ -n "$DTB" ]; then
	command -v fdtget >/dev/null || die "fdtget is needed to read $DTB (package device-tree-compiler)"
	FIELDS=$(python3 common/dtb-platform.py "$DTB") || die "cannot extract platform from $DTB"
	read -r GIC_DIST GIC_REDIST CORES SHIFT SMALL HVC <<< "$FIELDS"
	echo "new-device: from $DTB: GIC $GIC_DIST / $GIC_REDIST, $CORES cores, MPIDR shift $SHIFT, small cores $SMALL"
fi

cat > "$PLATC" <<EOF
/*
 * $CODE: paste into xnuloader/loader.c next to platform_redwood and add &platform_$CODE to platforms[].
 * Every value is explained in docs/PORTING.md ("The loader").
 */
static const struct platform platform_$CODE = {
	.name = "$CODE",
	.io_base = 0x00400000, .io_size = 0x3FC00000,   /* TODO: one window over the SoC's peripherals, non-zero base */
	.gic_dist = $GIC_DIST, .gic_redist = $GIC_REDIST,
	.uart = 0,                                      /* phones: no PL011 */
	.log_other_cpu = 1,
	.cores = $CORES, .core_shift = $SHIFT,
	.psci_hvc = $HVC,
	.small_cores = $SMALL,                          /* TODO: a bit per "small" core (capacity-dmips-mhz in the DT) */
	.fb_base = 0x0,                                 /* TODO: the framebuffer base the UEFI prints ("display ... at") */
};
EOF
echo "new-device: wrote $CONF and $PLATC"
echo "new-device: next -- docs/PORTING.md, then: common/build.sh $CODE && common/run.sh $CODE"
