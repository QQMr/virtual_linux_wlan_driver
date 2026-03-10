# Makefile for virtual Linux driver study modules
#
# Modules:
#   minimal_wlan    — mac80211-based virtual WiFi driver
#   v4l2_virtual_cam — V4L2 virtual camera driver (colour-bar test pattern)
#
# Usage:
#   make              — build all modules
#   make clean        — remove build artifacts
#   sudo make load    — insert minimal_wlan module
#   sudo make unload  — remove minimal_wlan module
#   sudo make reload  — unload + load (quick development cycle)
#   sudo make load-cam   — insert v4l2_virtual_cam module
#   sudo make unload-cam — remove v4l2_virtual_cam module

# Kernel modules to build
obj-m += minimal_wlan.o
obj-m += v4l2_virtual_cam.o

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
load:
	insmod minimal_wlan.ko
	@echo "--- kernel log ---"
	dmesg | tail -10

# Remove module
unload:
	rmmod minimal_wlan || true
	@echo "--- kernel log ---"
	dmesg | tail -5

# Quick reload for development iteration
reload: unload load

# Show the wireless interface after loading
show:
	@echo "=== ip link ==="
	ip link show
	@echo ""
	@echo "=== iw dev ==="
	iw dev 2>/dev/null || echo "(iw not installed — try: apt install iw)"

# --- V4L2 virtual camera targets ---

# Insert v4l2_virtual_cam and confirm the /dev/videoN node
load-cam:
	insmod v4l2_virtual_cam.ko
	@echo "--- kernel log ---"
	dmesg | tail -10
	@echo ""
	@echo "=== video devices ==="
	v4l2-ctl --list-devices 2>/dev/null || echo "(v4l2-ctl not installed — try: apt install v4l-utils)"

# Remove v4l2_virtual_cam
unload-cam:
	rmmod v4l2_virtual_cam || true
	@echo "--- kernel log ---"
	dmesg | tail -5

# Show info about the virtual camera (run after load-cam)
show-cam:
	@echo "=== V4L2 device info ==="
	v4l2-ctl --all 2>/dev/null || echo "(v4l2-ctl not installed)"

.PHONY: all clean load unload reload show load-cam unload-cam show-cam
