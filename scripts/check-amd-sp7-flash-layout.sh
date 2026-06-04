#!/bin/sh
# Verify AMD SP7 128MB NOR layout and boot-critical DTS on all PRB boards.
# Run after DTS cherry-picks. Usage:
#   scripts/check-amd-sp7-flash-layout.sh [path/to/sp7.conf]

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DTS_DIR="$ROOT/arch/arm64/boot/dts/aspeed"
LAYOUT="$DTS_DIR/amd-flash-layout-128.dtsi"
G7="$DTS_DIR/aspeed-g7.dtsi"
MAKEFILE="$DTS_DIR/Makefile"
CONF="${1:-$ROOT/../code_a2_support/openbmc/meta-amd/meta-sp7/conf/machine/sp7.conf}"

fail=0
warn=0
STRICT_BOARDS=""

err() {
	echo "error: $*" >&2
	fail=1
}

wrn() {
	echo "warn: $*" >&2
	warn=1
}

ok() {
	echo "ok: $*"
}

# SP7 kernel Makefile / KERNEL_DEVICETREE boards must pass; others are warnings.
board_err() {
	board=$1
	shift
	if echo " $STRICT_BOARDS " | grep -q " $board "; then
		err "$board: $*"
	else
		wrn "$board: $*"
	fi
}

hex_to_kb() {
	printf '%d' "0x$1" | awk '{ printf "%d", ($1+1023)/1024 }'
}

if [ ! -f "$LAYOUT" ]; then
	err "missing $LAYOUT"
	exit 1
fi

# --- Central flash partition table vs sp7.conf ---
env_start=$(sed -n 's/.*u-boot-env@\(.*\) {/\1/p' "$LAYOUT" | head -1)
kern_start=$(sed -n 's/.*kernel@\(.*\) {/\1/p' "$LAYOUT" | head -1)
rofs_start=$(sed -n 's/.*rofs@\(.*\) {/\1/p' "$LAYOUT" | head -1)
rwfs_start=$(sed -n 's/.*rwfs@\(.*\) {/\1/p' "$LAYOUT" | head -1)

env_kb=$(hex_to_kb "$env_start")
kern_kb=$(hex_to_kb "$kern_start")
rofs_kb=$(hex_to_kb "$rofs_start")
rwfs_kb=$(hex_to_kb "$rwfs_start")

get_conf_kb() {
	grep "^$1:flash-131072" "$CONF" 2>/dev/null | sed 's/.*= "//;s/"$//' || true
}

if [ -f "$CONF" ]; then
	check_kb() {
		if [ "$1" != "$2" ]; then
			err "flash layout $3: amd-flash-layout-128.dtsi=${1}KB sp7.conf=${2}KB"
		fi
	}
	check_kb "$env_kb" "$(get_conf_kb FLASH_UBOOT_ENV_OFFSET)" "u-boot-env boundary"
	check_kb "$kern_kb" "$(get_conf_kb FLASH_KERNEL_OFFSET)" "kernel"
	check_kb "$rofs_kb" "$(get_conf_kb FLASH_ROFS_OFFSET)" "rofs"
	check_kb "$rwfs_kb" "$(get_conf_kb FLASH_RWFS_OFFSET)" "rwfs"
	ok "flash layout matches $CONF"
else
	wrn "sp7.conf not found at $CONF (KB: env=$env_kb kernel=$kern_kb rofs=$rofs_kb rwfs=$rwfs_kb)"
fi

grep -q 'reg = <0x1220000' "$LAYOUT" && \
	err "amd-flash-layout-128.dtsi still uses legacy rofs@0x1220000"

# --- aspeed-g7.dtsi eSPI (all boards) ---
grep -q 'aspeed,syscon = <&syscon1>' "$G7" || \
	err "aspeed-g7.dtsi: espi0/espi1 need aspeed,syscon = <&syscon1>"
# Legacy "syscon =" (without aspeed, prefix) breaks 6.18 eSPI driver binding.
grep -E '^[[:space:]]+syscon = <&syscon1>' "$G7" >/dev/null && \
	err "aspeed-g7.dtsi: espi must not use legacy syscon = property"

# --- Makefile: AMD entries must be .dtb; record SP7 build DTBs ---
if [ -f "$MAKEFILE" ]; then
	for line in $(grep 'aspeed-bmc-amd-' "$MAKEFILE" || true); do
		case "$line" in
		*.dts) err "Makefile lists $line (must be .dtb not .dts)" ;;
		*.dtb) board=${line%.dtb}; STRICT_BOARDS="$STRICT_BOARDS $board" ;;
		esac
	done
	ok "Makefile AMD DTB targets:${STRICT_BOARDS}"
fi

# --- Per-board PRB DTS checks ---
check_board() {
	board=$1
	file="$DTS_DIR/$board.dts"
	[ -f "$file" ] || { err "missing $file"; return; }

	# FMC must use shared 128MB layout include
	if grep -q '&fmc' "$file" && grep -q 'flash@0' "$file"; then
		grep -q '#include "amd-flash-layout-128.dtsi"' "$file" || \
			err "$board: FMC flash@0 must #include \"amd-flash-layout-128.dtsi\""
		grep -q '0x1220000' "$file" && \
			err "$board: contains legacy rofs offset 0x1220000"
	fi

	# Broken eSPI eDAF phandle (maps to GIC @ 0x12200000)
	if grep -q 'flash-edaf-tgt-addr = <0x' "$file"; then
		err "$board: flash-edaf-tgt-addr must be <&edaf0> not a numeric phandle"
	fi

	# LPC PCC: 6.18 driver requires pcc-ports in the enabled override block
	lpc0_block=$(sed -n '/&lpc0_pcc/,/};/p' "$file" | head -20)
	if echo "$lpc0_block" | grep -q 'status = "okay"'; then
		echo "$lpc0_block" | grep -q 'pcc-ports' || \
			board_err "$board" "&lpc0_pcc enabled but missing pcc-ports"
		echo "$lpc0_block" | grep -q 'port-addr' && \
			board_err "$board" "&lpc0_pcc still uses legacy port-addr (use pcc-ports)"
	fi
	lpc1_block=$(sed -n '/&lpc1_pcc/,/};/p' "$file" | head -20)
	if echo "$lpc1_block" | grep -q 'status = "okay"'; then
		echo "$lpc1_block" | grep -q 'pcc-ports' || \
			board_err "$board" "&lpc1_pcc enabled but missing pcc-ports"
	fi

	# eSPI0 with eDAF properties
	if grep -A20 '&espi0' "$file" | grep -q 'status = "okay"'; then
		if grep -A20 '&espi0' "$file" | grep -q 'flash-edaf-tgt-addr'; then
			grep -A20 '&espi0' "$file" | grep -q 'flash-edaf-tgt-addr = <&edaf0>' || \
				err "$board: &espi0 flash-edaf-tgt-addr must be <&edaf0>"
		fi
	fi

	ok "$board.dts"
}

# All AMD PRB sources in tree
for dts in "$DTS_DIR"/aspeed-bmc-amd-*.dts; do
	[ -f "$dts" ] || continue
	board=$(basename "$dts" .dts)
	check_board "$board"
done

for dtb in $STRICT_BOARDS; do
	if [ ! -f "$DTS_DIR/$dtb.dts" ]; then
		err "Makefile lists $dtb.dtb but $dtb.dts is missing"
	fi
done

# Optional: preprocess + dtc for Makefile DTBs (matches kernel build)
if command -v dtc >/dev/null 2>&1 && command -v cpp >/dev/null 2>&1; then
	echo "--- DTB compile smoke test (Makefile targets) ---"
	DTC_INC="-I$DTS_DIR -I$ROOT/scripts/dtc/include-prefixes"
	for board in $STRICT_BOARDS; do
		dts="$DTS_DIR/$board.dts"
		pre="/tmp/${board}-check.dts"
		out="/tmp/${board}-check.dtb"
		if ! cpp -nostdinc $DTC_INC -undef -D__DTS__ -x assembler-with-cpp \
			-P "$dts" "$pre" 2>/tmp/cpp-err-$$; then
			board_err "$board" "cpp failed ($(head -1 /tmp/cpp-err-$$))"
			rm -f /tmp/cpp-err-$$
			continue
		fi
		if ! dtc -@ -Wno-unit_address_vs_reg -I dts -O dtb -o "$out" "$pre" 2>/tmp/dtc-err-$$; then
			board_err "$board" "dtc failed ($(head -1 /tmp/dtc-err-$$))"
		fi
		rm -f "$pre" "$out" /tmp/cpp-err-$$ /tmp/dtc-err-$$
	done
	ok "Makefile AMD DTBs compile (cpp + dtc)"
else
	wrn "dtc/cpp not in PATH; skipping DTB compile smoke test"
fi

if [ "$fail" -ne 0 ]; then
	echo "FAILED: fix DTS/Makefile before cherry-picking more integ_sp8 changes" >&2
	exit 1
fi

if [ "$warn" -ne 0 ]; then
	echo "PASSED with warnings (non-Makefile boards need SDKv10 LPC/eSPI updates)"
	exit 0
fi
echo "PASSED: amd-flash-layout + all aspeed-bmc-amd-* board checks"
exit 0
