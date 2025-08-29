# combo driver: MT6620
# If KERNELRELEASE is defined, we've been invoked from the
# kernel build system and can use its language.
ifneq ($(KERNELRELEASE),)
	#subdir-ccflags-y can be used in 2.6.34 in the future
	#subdir-ccflags-y += -I$(src)/common/include -I$(src)/common/linux/include 
   # obj-y += common/
    obj-$(CONFIG_MT6632) += drv_wlan/MT6632/
    obj-n := dummy.o
	# skip bt hci driver since kernel 5.x introduced btmtk driver
	# obj-y	+= drv_bt/
	# We could use a stub bt_wifi directory and kconfig,
	# but it's too much maintenance

# Otherwise we were called directly from the command
# line; invoke the kernel build system.
else
    KSRC ?= /lib/modules/$(shell uname -r)/build
    KERNELDIR := $(KSRC)
    PWD  := $(shell pwd)
default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean: driver_clean

driver_clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

endif
