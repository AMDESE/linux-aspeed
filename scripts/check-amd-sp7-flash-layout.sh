#!/bin/sh
# Verify AMD 128MB NOR DTS partitions match OpenBMC sp7.conf image layout.
# Run after DTS cherry-picks that touch flash, eSPI, or g7.dtsi.
# Usage: scripts/check-amd-sp7-flash-layout.sh [path/to/sp7.conf]

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DTS="$ROOT/arch/arm64/boot/dts/aspeed/amd-flash-layout-128.dtsi"
G7="$ROOT/arch/arm64/boot/dts/aspeed/aspeed-g7.dtsi"
MOROCCO="$ROOT/arch/arm64/boot/dts/aspeed/aspeed-bmc-amd-morocco.dts"
CONF="${1:-$ROOT/../code_a2_support/openbmc/meta-amd/meta-sp7/conf/machine/sp7.conf}"

if [ ! -f "$DTS" ]; then
	echo "error: missing $DTS" >&2
	exit 1
fi

hex_to_kb() {
	printf '%d' "0x$1" | awk '{ printf "%d", ($1+1023)/1024 }'
}

# Partition start addresses from amd-flash-layout-128.dtsi (must match sp7.conf KB)
env_start=$(sed -n 's/.*u-boot-env@\(.*\) {/\1/p' "$DTS" | head -1)
kern_start=$(sed -n 's/.*kernel@\(.*\) {/\1/p' "$DTS" | head -1)
rofs_start=$(sed -n 's/.*rofs@\(.*\) {/\1/p' "$DTS" | head -1)
rwfs_start=$(sed -n 's/.*rwfs@\(.*\) {/\1/p' "$DTS" | head -1)

env_kb=$(hex_to_kb "$env_start")
kern_kb=$(hex_to_kb "$kern_start")
rofs_kb=$(hex_to_kb "$rofs_start")
rwfs_kb=$(hex_to_kb "$rwfs_start")

get_conf_kb() {
	grep "^$1:flash-131072" "$CONF" 2>/dev/null | sed 's/.*= "//;s/"$//' || true
}

if [ -f "$CONF" ]; then
	expect_env=$(get_conf_kb FLASH_UBOOT_ENV_OFFSET)
	expect_kern=$(get_conf_kb FLASH_KERNEL_OFFSET)
	expect_rofs=$(get_conf_kb FLASH_ROFS_OFFSET)
	expect_rwfs=$(get_conf_kb FLASH_RWFS_OFFSET)

	fail=0
	check() {
		if [ "$1" != "$2" ]; then
			echo "error: $3 DTS=${1}KB sp7.conf=${2}KB" >&2
			fail=1
		fi
	}

	check "$env_kb" "$expect_env" "u-boot-env boundary"
	check "$kern_kb" "$expect_kern" "kernel"
	check "$rofs_kb" "$expect_rofs" "rofs"
	check "$rwfs_kb" "$expect_rwfs" "rwfs"

	if [ "$fail" -ne 0 ]; then
		echo "Fix amd-flash-layout-128.dtsi and meta-amd/meta-sp7/conf/machine/sp7.conf together." >&2
		exit 1
	fi
	echo "flash layout: DTS matches $CONF"
else
	echo "warn: sp7.conf not found at $CONF (skipping Yocto cross-check)" >&2
	echo "flash layout DTS (KB): env=$env_kb kernel=$kern_kb rofs=$rofs_kb rwfs=$rwfs_kb"
fi

grep -q 'aspeed,syscon = <&syscon1>' "$G7" || {
	echo "error: espi nodes must use aspeed,syscon (not syscon) in aspeed-g7.dtsi" >&2
	exit 1
}
grep -q 'pcc-ports = <0x80>' "$MOROCCO" || {
	echo "error: Morocco lpc_pcc needs pcc-ports" >&2
	exit 1
}
grep -q 'flash-edaf-tgt-addr = <&edaf0>' "$MOROCCO" || {
	echo "error: Morocco espi0 needs flash-edaf-tgt-addr = <&edaf0>" >&2
	exit 1
}
if grep -rq 'flash-edaf-tgt-addr = <0x' "$ROOT/arch/arm64/boot/dts/aspeed/aspeed-bmc-amd-"*.dts 2>/dev/null; then
	echo "error: numeric flash-edaf-tgt-addr phandle breaks eSPI (use <&edaf0>)" >&2
	exit 1
fi

echo "boot-critical DTS checks passed"
