#!/bin/bash
# test_fake_ap.sh — Test script for minimal_wlan fake AP feature
#
# Usage:
#   sudo bash test_fake_ap.sh
#
# What it does:
#   1. Build and load the driver
#   2. Detect the interface name
#   3. Run iw scan and capture results
#   4. Verify each expected fake AP appears
#   5. Report PASS / FAIL per check
#   6. Unload the driver

set -euo pipefail

# ── Colour helpers ──────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { echo -e "${GREEN}[PASS]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAILURES=$((FAILURES + 1)); }
info() { echo -e "${YELLOW}[INFO]${NC} $*"; }

FAILURES=0

# ── 0. Privilege check ──────────────────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
    echo "Run as root: sudo bash $0"
    exit 1
fi

# ── 1. Build ─────────────────────────────────────────────────────────────────
info "Building module..."
make -C "$(dirname "$0")" >/dev/null 2>&1 || { echo "Build failed"; exit 1; }
pass "Build succeeded"

# ── 2. Load ──────────────────────────────────────────────────────────────────
# Unload any previous instance first (ignore error if not loaded)
rmmod minimal_wlan 2>/dev/null || true
sleep 0.5

info "Loading minimal_wlan.ko..."
insmod "$(dirname "$0")/minimal_wlan.ko"
sleep 1
pass "Module loaded"

# ── 3. Detect interface ───────────────────────────────────────────────────────
IFACE=$(iw dev 2>/dev/null | awk '/Interface/{print $2}' | grep -m1 'wlan' || true)
if [[ -z "$IFACE" ]]; then
    fail "No wlan interface found after insmod"
    rmmod minimal_wlan 2>/dev/null || true
    exit 1
fi
pass "Interface detected: $IFACE"

# ── 4. Bring up interface ─────────────────────────────────────────────────────
info "Bringing up $IFACE..."
ip link set "$IFACE" up
sleep 0.5
pass "Interface is UP"

# ── 5. Run scan ───────────────────────────────────────────────────────────────
info "Running iw $IFACE scan (this may take ~3 s)..."
SCAN_OUTPUT=$(iw dev "$IFACE" scan 2>&1 || true)

if [[ -z "$SCAN_OUTPUT" ]]; then
    fail "iw scan returned no output"
else
    pass "iw scan returned output"
fi

echo
echo "── Scan output ──────────────────────────────────────────────────────────"
echo "$SCAN_OUTPUT"
echo "─────────────────────────────────────────────────────────────────────────"
echo

# ── 6. Per-AP verification ────────────────────────────────────────────────────
info "Verifying fake APs..."

check_ap() {
    local desc="$1"; local pattern="$2"
    if echo "$SCAN_OUTPUT" | grep -q "$pattern"; then
        pass "$desc"
    else
        fail "$desc  (expected: $pattern)"
    fi
}

# BSSID checks
check_ap "AP-A BSSID present"   "02:00:00:aa:bb:01"
check_ap "AP-B BSSID present"   "02:00:00:aa:bb:02"
check_ap "AP-C BSSID present"   "02:00:00:aa:bb:03"
check_ap "AP-hidden BSSID present" "02:00:00:aa:bb:04"

# SSID checks
check_ap "SSID MinimalNet-A"    "MinimalNet-A"
check_ap "SSID MinimalNet-B"    "MinimalNet-B"
check_ap "SSID MinimalNet-C"    "MinimalNet-C"

# Frequency / channel checks
check_ap "Channel 1  (2412 MHz)" "2412"
check_ap "Channel 6  (2437 MHz)" "2437"
check_ap "Channel 11 (2462 MHz)" "2462"

# Signal strength checks (iw shows e.g. "signal: -45.00 dBm")
check_ap "Signal -45 dBm (AP-A)" "\-45"
check_ap "Signal -65 dBm (AP-B)" "\-65"
check_ap "Signal -80 dBm (AP-C)" "\-80"

echo

# ── 7. dmesg tail ─────────────────────────────────────────────────────────────
info "Kernel log (last 20 lines):"
dmesg | grep "minimal_wlan" | tail -20

echo

# ── 8. Summary ────────────────────────────────────────────────────────────────
if [[ $FAILURES -eq 0 ]]; then
    pass "All checks passed!"
else
    fail "$FAILURES check(s) failed"
fi

# ── 9. Cleanup ────────────────────────────────────────────────────────────────
info "Unloading module..."
ip link set "$IFACE" down 2>/dev/null || true
rmmod minimal_wlan
pass "Module unloaded"

exit $FAILURES
