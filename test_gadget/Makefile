ifneq ($(KERNELRELEASE),)
obj-m	:= scale_usb.o
else
KERNELDIR     ?= /home/other/tq/kernel_imx/#/usr/src/linux-headers-`uname -r`/
#INCLUDE       := $(KERNELDIR)/drivers/usb/gadget
PWD       	  := $(shell pwd)
ARCH          := arm
CROSS_COMPILE :=/opt/Embedsky/gcc-4.6.2-glibc-2.13-linaro-multilib-2011.12/tq-linaro-toolchain/bin/arm-linux-
#EXTRA_CFLAGS += -I$(INClUDE)
#export EXTRA_CFLAGS
default:
	$(MAKE)  -C $(KERNELDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) INCDIR=$(INCLUDE) modules  
endif

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions modules.order Module.symvers


