/*
 * Emagic/Apple emi2|6, emi6|2m, a26 and a62m usb audio interface
 * firmware loader.
 * Copyright (C) 2002 Tapio Laxström (tapio.laxstrom@iptime.fi)
 * Copyright (2) 2012-2013 Monty Montgomery <monty@xiph.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, as published by
 * the Free Software Foundation, version 2.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/ihex.h>

static int emi62_midi_mode=0;
module_param(emi62_midi_mode, int, S_IRUGO);
MODULE_PARM_DESC(emi62_midi_mode,
		 "Set emi6|2m/a62m digital output to MIDI rather than "
		 "SPDIF mode. No effect on emi2|6/a26.");

#define EMI_VENDOR_ID 	        0x086a  /* Emagic Soft-und Hardware GmBH */
#define EMI26_PRODUCT_ID	0x0100	/* EMI 2|6 without firmware */
#define EMI26B_PRODUCT_ID	0x0102	/* EMI 2|6 without firmware */
#define EMI62_PRODUCT_ID	0x0110	/* EMI 6|2m without firmware */

/* Vendor specific request code for Anchor Upload/Download (This one
   is implemented in the core) */
#define ANCHOR_LOAD_INTERNAL	0xA0
/* This command is not implemented in the core. Requires firmware */
#define ANCHOR_LOAD_EXTERNAL	0xA3
/* This command is not implemented in the core. Requires
   firmware. Emagic extension */
#define ANCHOR_LOAD_FPGA	0xA5
/* This is the highest internal RAM address for the AN2131Q */
#define MAX_INTERNAL_ADDRESS	0x1B3F
/* EZ-USB Control and Status Register.  Bit 0 controls 8051 reset */
#define CPUCS_REG		0x7F92
/* yes 1023 bytes, not 1024! */
#define FW_LOAD_SIZE		1023

#define INTERNAL_RAM(address)   (address <= MAX_INTERNAL_ADDRESS)

static int emi_probe(struct usb_interface *intf,
                     const struct usb_device_id *id);
static void emi_disconnect(struct usb_interface *intf);

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE(EMI_VENDOR_ID, EMI26_PRODUCT_ID) },
	{ USB_DEVICE(EMI_VENDOR_ID, EMI26B_PRODUCT_ID) },
	{ USB_DEVICE(EMI_VENDOR_ID, EMI62_PRODUCT_ID) },
	{ } /* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, id_table);

static struct usb_driver emi_driver = {
	.name		= "emi - firmware loader",
	.probe		= emi_probe,
	.disconnect	= emi_disconnect,
	.id_table	= id_table,
};

typedef struct {

	struct usb_device *usbdev;
	struct usb_interface *intf;
	short product_id;

	const struct firmware *loader_fw;
	const struct firmware *bitstream_fw;
	const struct firmware *firmware_fw;

} emi_context;

static int emi_writememory (struct usb_device *dev, int address,
                            const unsigned char *data, int length,
                            __u8 request)
{
	int result;
	unsigned char *buffer =  kmemdup(data, length, GFP_KERNEL);

	if (!buffer) {
		dev_err(&dev->dev, "emi: kmalloc(%d) failed.\n", length);
		return -ENOMEM;
	}

	/* Note: usb_control_msg returns negative value on error or
	 * 	 length of the data that was written! */
	result = usb_control_msg (dev,
                                  usb_sndctrlpipe(dev, 0),
                                  request,
                                  0x40,
                                  address,
                                  0,
                                  buffer,
                                  length,
                                  300);
	kfree (buffer);
	return result;
}

static int emi_set_reset (struct usb_device *dev, unsigned char reset_bit)
{
	int response;
	response = emi_writememory (dev, CPUCS_REG, &reset_bit, 1, 0xa0);
	if (response < 0) {
		dev_err(&dev->dev, "emi: set_reset (%d) failed\n", reset_bit);
	}
	return response;
}

static void emi_detach(emi_context *context)
{
	if(context){
		/* release the firmware */
		if(context->loader_fw)
			release_firmware(context->loader_fw);
		if(context->bitstream_fw)
			release_firmware(context->bitstream_fw);
		if(context->firmware_fw)
			release_firmware(context->firmware_fw);

		/* release the interface */
		usb_driver_release_interface(&emi_driver, context->intf);

		/* and the device refcount */
		usb_put_dev(context->usbdev);

		/* free the context */
		kfree(context);
	}
}


static void emi_firmware_handler(const struct firmware *fw,
				 void *in)
{
	emi_context *context = (emi_context *)in;
	struct usb_device *dev = context->usbdev;
	const struct ihex_binrec *rec;
	int err;
	int i;
	__u32 addr;	/* Address to write */
	__u8 *buf=0;

	if (fw == NULL) {
		dev_err(&context->usbdev->dev,
			"emi: failed to request firmware file\n");
		goto err;
	}
	if(ihex_validate_fw(fw)){
		dev_err(&context->usbdev->dev,
			"emi: firmware contains invalid ihex\n");
		goto err;
	}

	context->firmware_fw=fw;
	dev_dbg(&dev->dev, "emi: got firmware file\n");

	buf = kmalloc(FW_LOAD_SIZE, GFP_KERNEL);
	if (!buf) {
		dev_err(&dev->dev, "emi: unable to allocate tranfer buffer\n");
		goto err;
	}
	dev_dbg(&dev->dev, "emi: got firmware kmalloc\n");

	/* Assert reset (stop the CPU in the EMI) */
	dev_dbg(&dev->dev, "emi: resetting device\n");
	err = emi_set_reset(dev,1);
	if (err < 0) {
		dev_err(&dev->dev,"emi: unable to reset device: %d\n",err);
		goto err;
	}
	dev_dbg(&dev->dev, "emi: reset complete\n");

	rec = (const struct ihex_binrec *)context->loader_fw->data;
	/* 1. Put the loader for the FPGA into the EZ-USB */
	dev_dbg(&dev->dev, "emi: uploading EZ-USB loader to device\n");
	while (rec) {
		err = emi_writememory(dev, be32_to_cpu(rec->addr),
				      rec->data, be16_to_cpu(rec->len),
				      ANCHOR_LOAD_INTERNAL);
		if (err < 0) {
			dev_err(&dev->dev,
				"emi: error uploading EZ-USB loader to device: %d", err);
			goto err;
		}
		rec = ihex_next_binrec(rec);
	}
	dev_dbg(&dev->dev, "emi: done uploading EZ-USB loader\n");

	/* De-assert reset (let the CPU run) */
	dev_dbg(&dev->dev, "emi: activating CPU\n");
	err = emi_set_reset(dev,0);
	if (err < 0) {
		dev_err(&dev->dev,"emi: unable to restart device CPU: %d", err);
		goto err;
	}
	dev_dbg(&dev->dev, "emi: CPU running\n");
	msleep(250);	/* let device settle */

	/* 2. Upload the FPGA firmware into the EMI */
	rec = (const struct ihex_binrec *)context->bitstream_fw->data;
	dev_dbg(&dev->dev, "emi: uploading FPGA bitstream to device\n");
	do {
		i = 0;
		addr = be32_to_cpu(rec->addr);

		/* intel hex records are terminated with type 0 element */
		while (rec && (i + be16_to_cpu(rec->len) < FW_LOAD_SIZE)) {
			memcpy(buf + i, rec->data, be16_to_cpu(rec->len));
			i += be16_to_cpu(rec->len);
			rec = ihex_next_binrec(rec);
		}
		err = emi_writememory(dev, addr, buf, i, ANCHOR_LOAD_FPGA);
		if (err < 0) {
			dev_err(&dev->dev,
				"emi: error uploading FPGA bitstream to device: %d", err);
			goto err;
		}
	} while (rec);
	dev_dbg(&dev->dev, "emi: done uploading FPGA bitstream\n");

	/* Assert reset (stop the CPU in the EMI) */
	dev_dbg(&dev->dev, "emi: resetting CPU\n");
	err = emi_set_reset(dev,1);
	if (err < 0) {
		dev_err(&dev->dev, "emi: unable to reset device: %d", err);
		goto err;
	}
	dev_dbg(&dev->dev, "emi: CPU reset\n");

	/* 3. Put the loader back into the EZ-USB. */
	dev_dbg(&dev->dev, "emi: re-uploading EZ-USB loader to device\n");
	for (rec = (const struct ihex_binrec *)context->loader_fw->data;
	     rec; rec = ihex_next_binrec(rec)) {
		err = emi_writememory(dev, be32_to_cpu(rec->addr),
				      rec->data, be16_to_cpu(rec->len),
				      ANCHOR_LOAD_INTERNAL);
		if (err < 0) {
			dev_err(&dev->dev,
				"emi: error re-uploading EZ-USB loader to device: %d", err);
			goto err;
		}
	}
	dev_dbg(&dev->dev, "emi: done re-uploading EZ-USB loader\n");
	msleep(250);	/* let device settle */

	/* De-assert reset (let the CPU run) */
	dev_dbg(&dev->dev, "emi: activating CPU\n");
	err = emi_set_reset(dev,0);
	if (err < 0) {
		dev_err(&dev->dev,"emi: unable to restart device CPU: %d", err);
		goto err;
	}
	dev_dbg(&dev->dev, "emi: CPU running\n");
	msleep(250);	/* let device settle */

	/* 4. Put the part of the firmware that lies in the external
	 *    RAM into the EZ-USB */
	dev_dbg(&dev->dev, "emi: uploading firmware data to device external RAM\n");
	for (rec = (const struct ihex_binrec *)context->firmware_fw->data;
	     rec; rec = ihex_next_binrec(rec)) {
		if (!INTERNAL_RAM(be32_to_cpu(rec->addr))) {
			err = emi_writememory(dev, be32_to_cpu(rec->addr),
					      rec->data, be16_to_cpu(rec->len),
					      ANCHOR_LOAD_EXTERNAL);
			if (err < 0) {
				dev_err(&dev->dev,
					"emi: error uploading firmware data to device external RAM: %d", err);
				goto err;
			}
		}
	}
	dev_dbg(&dev->dev, "emi: done uploading firmware data\n");

	/* Assert reset (stop the CPU in the EMI) */
	dev_dbg(&dev->dev, "emi: resetting CPU\n");
	err = emi_set_reset(dev,1);
	if (err < 0) {
		dev_err(&dev->dev, "emi: unable to reset device: %d", err);
		goto err;
	}
	dev_dbg(&dev->dev, "emi: CPU reset\n");

	dev_dbg(&dev->dev, "emi: uploading final internal firmware data to device\n");
	for (rec = (const struct ihex_binrec *)context->firmware_fw->data;
	     rec; rec = ihex_next_binrec(rec)) {
		if (INTERNAL_RAM(be32_to_cpu(rec->addr))) {
			err = emi_writememory(dev, be32_to_cpu(rec->addr),
					      rec->data, be16_to_cpu(rec->len),
					      ANCHOR_LOAD_INTERNAL);
			if (err < 0) {
				dev_err(&dev->dev,
					"emi: error uploading final internal firmware data to device: %d", err);
				goto err;
			}
		}
	}
	dev_dbg(&dev->dev, "emi: done uploading firmware data\n");

	/* De-assert reset (let the CPU run) */
	dev_dbg(&dev->dev, "emi: starting CPU\n");
	err = emi_set_reset(dev,0);
	if (err < 0) {
		dev_err(&dev->dev,"emi: unable to restart device CPU: %d", err);
		goto err;
	}
	dev_dbg(&dev->dev, "emi: CPU started\n");
	dev_info(&dev->dev, "emi: firmware load successful\n");
	msleep(250);	/* let device settle */

	/* fall though and detach-- it's time to let usbaudio have it */
err:
	if(buf)
		kfree(buf);
	emi_detach(context);
	return;
}

static void emi_bitstream_handler(const struct firmware *fw,
				  void *in)
{
	emi_context *context = (emi_context *)in;
	struct usb_device *dev = context->usbdev;
	int err;
	char *fw_file =
		(context->product_id == EMI62_PRODUCT_ID ?
		 (emi62_midi_mode ? "emi/emi62_firmware_midi.fw" : "emi/emi62_firmware_spdif.fw" ):
		 "emi/emi26_firmware.fw");

	if (fw == NULL) {
		dev_err(&context->usbdev->dev,
			"emi: failed to load bitstream file\n");
		emi_detach(context);
		return;
	}
	if(ihex_validate_fw(fw)){
		dev_err(&context->usbdev->dev,
			"emi: bitstream file contains invalid ihex\n");
		emi_detach(context);
		return;
	}

	context->bitstream_fw=fw;
	dev_dbg(&dev->dev, "emi: got bitstream file\n");

	err = request_firmware_nowait(THIS_MODULE,
				      FW_ACTION_HOTPLUG,
				      fw_file,
				      &dev->dev,
				      GFP_KERNEL,
				      context,
				      emi_firmware_handler);
	if (err) {
		dev_err(&dev->dev,
			"emi: failed to request firmware file\n");
		emi_detach(context);
	}
}

static void emi_loader_handler(const struct firmware *fw,
			       void *in)
{
	emi_context *context = (emi_context *)in;
	struct usb_device *dev = context->usbdev;
	int err;
	char *fw_file =
		(context->product_id == EMI62_PRODUCT_ID ?
		 "emi/emi62_bitstream.fw" :
		 "emi/emi26_bitstream.fw");

	if (fw == NULL) {
		dev_err(&context->usbdev->dev,
			"emi: failed to load EZ-USB loader\n");
		emi_detach(context);
		return;
	}
	if(ihex_validate_fw(fw)){
		dev_err(&context->usbdev->dev,
			"emi: EZ-USB loader file contains invalid ihex\n");
		emi_detach(context);
		return;
	}

	context->loader_fw=fw;
	dev_dbg(&dev->dev, "emi: got EZ-USB loader\n");

	err = request_firmware_nowait(THIS_MODULE,
				      FW_ACTION_HOTPLUG,
				      fw_file,
				      &dev->dev,
				      GFP_KERNEL,
				      context,
				      emi_bitstream_handler);
	if (err){
		dev_err(&context->usbdev->dev,
			"emi: failed to request bitstream file\n");
		emi_detach(context);
	}
}

static int emi_probe(struct usb_interface *intf, const struct usb_device_id *id)
{

	struct usb_device *dev = usb_get_dev(interface_to_usbdev(intf));
	emi_context *context;
	char *fw_file;
	int ret;
	int product = le16_to_cpu(dev->descriptor.idProduct);
	char *product_name;

	switch (product){
	case EMI26_PRODUCT_ID:
		product_name = "emi2|6 / a26";
		break;
	case EMI26B_PRODUCT_ID:
		product_name = "emi2|6 / a26 rev B";
		break;
	case EMI62_PRODUCT_ID:
		if(emi62_midi_mode)
			product_name = "emi6|2m / a62m (MIDI mode)";
		else
			product_name = "emi6|2m / a62m (SPDIF mode)";
		break;
	default:
		dev_info(&dev->dev,
			 "emi: unknown device type; not loading firmware\n");
		return -EINVAL;
	}
	dev_info(&dev->dev, "emi: starting firmware load for %s\n",
		 product_name);

	context=kzalloc(sizeof(*context),GFP_KERNEL);
	if(!context){
		dev_err(&context->usbdev->dev,
			"emi: failed to allocate context memory\n");
		return -ENOMEM;
	}

	context->usbdev = dev;
	context->intf = intf;
	context->product_id = product;

	fw_file = "emi/loader.fw";

	ret = request_firmware_nowait(THIS_MODULE,
				      FW_ACTION_HOTPLUG,
				      fw_file,
				      &dev->dev,
				      GFP_KERNEL,
				      context,
				      emi_loader_handler);
	if (ret){
		dev_err(&context->usbdev->dev,
			"emi: failed to request EZ-USB loader file\n");
		kfree(context);
	}

	return ret;
}

static void emi_disconnect(struct usb_interface *intf)
{
}

module_usb_driver(emi_driver);

MODULE_AUTHOR("Tapio Laxström");
MODULE_AUTHOR("Monty Montgomery");
MODULE_DESCRIPTION("Emagic EMI a26/a62m firmware loader.");
MODULE_LICENSE("GPL");

MODULE_FIRMWARE("emi/loader.fw");
MODULE_FIRMWARE("emi/emi26_bitstream.fw");
MODULE_FIRMWARE("emi/emi26_firmware.fw");
MODULE_FIRMWARE("emi/emi62_bitstream.fw");
MODULE_FIRMWARE("emi/emi62_firmware_midi.fw");
MODULE_FIRMWARE("emi/emi62_firmware_spdif.fw");
/* vi:ai:syntax=c:sw=8:ts=8:tw=80
 */
