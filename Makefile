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

.PHONY: all clean load unload reload show
