# Kernel headers path (system installed kernel headers)
KERNELDIR ?= /usr/src/linux-headers-$(shell uname -r)

PWD := $(shell pwd)
MODULE_NAME := sunfounder-tft35ips
EXTRA_CFLAGS += -I$(PWD)/include
# kernel 7.x removed the mipi-dbi simple-pipe helpers, so that path uses its
# own implementation; both build to the same module name
KMAJOR := $(if $(VERSION),$(VERSION),0)
ifeq ($(shell [ $(KMAJOR) -ge 7 ] && echo 1),1)
$(MODULE_NAME)-y := $(MODULE_NAME)-7x.o
else
$(MODULE_NAME)-y := $(MODULE_NAME).o
endif
obj-m += $(MODULE_NAME).o

# Find overlay directory
OVERLAY_DIRS := \
    /boot/firmware/overlays \
    /boot/firmware/current/overlays \
    /boot/overlays

OVERLAY_DIR := $(firstword $(wildcard $(OVERLAY_DIRS)))

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
	dtc -@ -I dts -O dtb -o $(MODULE_NAME).dtbo $(MODULE_NAME)-overlay.dts

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(MODULE_NAME).dtbo

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install
	install -m 644 $(MODULE_NAME).dtbo $(OVERLAY_DIR)
	depmod -a
