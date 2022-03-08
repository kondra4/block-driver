BINARY     := test_blockdrv
KERNEL      := /lib/modules/$(shell uname -r)/build
C_FLAGS     := -Wall
KMOD_DIR    := $(shell pwd)
TARGET_PATH := /lib/modules/$(shell uname -r)/kernel/drivers/char

OBJECTS := blockdrv.o

ccflags-y += $(C_FLAGS)

obj-m += $(BINARY).o

$(BINARY)-y := $(OBJECTS)

$(BINARY).ko:
	make -C $(KERNEL) M=$(KMOD_DIR) modules

install:
	cp $(BINARY).ko $(TARGET_PATH)
test:
	sudo insmod test_blockdrv.ko
	lsmod | grep test_blockdrv
	sudo fdisk -l | grep blockdrv
clean:
	rm -f *.ko
	rm -f *.o