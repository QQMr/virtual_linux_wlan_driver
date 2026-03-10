# Makefile for minimal_wlan driver
#
# Usage:
#   make              — build the kernel module
#   make clean        — remove build artifacts
#   sudo make load    — insert the module
#   sudo make unload  — remove the module
#   sudo make reload  — unload + load (quick development cycle)

# Name of the kernel module
obj-m += minimal_wlan.o

# Path to the running kernel's build directory
KDIR := /lib/modules/$(shell uname -r)/build

# Current directory (where our source lives)
PWD := $(shell pwd)

# Default target: build the module
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Clean build artifacts
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Insert module and watch kernel log
# On Raspberry Pi 5, brcmfmac holds cfg80211 resources that can cause
# ieee80211_alloc_hw() to fail.  We temporarily unload it before inserting
# our virtual driver and restore it on unload.
load:
	@if lsmod | grep -q brcmfmac; then \
		echo "[minimal_wlan] suspending brcmfmac to free cfg80211 resources"; \
		rmmod brcmfmac brcmutil || true; \
	fi
	modprobe mac80211
	insmod minimal_wlan.ko
	@echo "--- kernel log ---"
	dmesg | tail -10

# Remove module and restore brcmfmac if it was active
unload:
	rmmod minimal_wlan || true
	@echo "--- kernel log ---"
	dmesg | tail -5
	@echo "[minimal_wlan] restoring brcmfmac"
	modprobe brcmfmac || true

# Quick reload for development iteration
reload: unload load

# Blacklist brcmfmac for the current boot so it does not compete
# with our driver.  Reverted on next reboot.
blacklist-brcmfmac:
	@echo "blacklist brcmfmac" | tee /etc/modprobe.d/minimal_wlan_dev.conf
	rmmod brcmfmac brcmutil 2>/dev/null || true
	@echo "[minimal_wlan] brcmfmac blacklisted until next reboot"

# Remove the temporary blacklist
restore-brcmfmac:
	rm -f /etc/modprobe.d/minimal_wlan_dev.conf
	modprobe brcmfmac || true
	@echo "[minimal_wlan] brcmfmac restored"

# Show the wireless interface after loading
show:
	@echo "=== ip link ==="
	ip link show
	@echo ""
	@echo "=== iw dev ==="
	iw dev 2>/dev/null || echo "(iw not installed — try: apt install iw)"
	@echo ""
	@echo "=== cfg80211 wiphy list ==="
	ls /sys/class/ieee80211/ 2>/dev/null || echo "(none)"

.PHONY: all clean load unload reload blacklist-brcmfmac restore-brcmfmac show
