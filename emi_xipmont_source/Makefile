obj-m   := emi.o

TARGET  := emi
CURRENT := $(shell uname -r)
KDIR    := /lib/modules/$(CURRENT)/build
FWDIR   := /lib/firmware/emi
DEST    := /lib/modules/$(CURRENT)/kernel/drivers/usb/misc
PWD     := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

install: default
	su -c "mkdir -p $(FWDIR)"
	su -c "cp -v firmware/loader.fw $(FWDIR)"
	su -c "cp -v firmware/emi26_bitstream.fw $(FWDIR)"
	su -c "cp -v firmware/emi26_firmware.fw $(FWDIR)"
	su -c "cp -v firmware/emi62_bitstream.fw $(FWDIR)"
	su -c "cp -v firmware/emi62_firmware_midi.fw $(FWDIR)"
	su -c "cp -v firmware/emi62_firmware_spdif.fw $(FWDIR)"
	su -c "cp -v $(TARGET).ko $(DEST) && /sbin/depmod -a"

remove-old:
	su -c "rm -rf $(FWDIR)26"
	su -c "rm -rf $(FWDIR)62"
	su -c "rm -f $(DEST)/emi62.ko && rm -f $(DEST)/emi26.ko && /sbin/depmod -a"

clean:
	-rm -f *~ *.o *.ko .*.cmd .*.flags *.mod.c Module.symvers modules.order
	make -C $(KDIR) M=$(PWD) clean
