#include <linux/module.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/bio.h>
#include <linux/genhd.h>
#include <linux/hdreg.h>
#include <linux/errno.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/string_helpers.h>
#include <linux/async.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/pr.h>
#include <linux/t10-pi.h>
#include <asm/uaccess.h>
#include <asm/unaligned.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_ioctl.h>
#include <scsi/scsicam.h>
#include <linux/efi.h>
#include <linux/vmalloc.h>

#include "sd.h"
#include "scsi_priv.h"
#include "scsi_logging.h"
#include "sd_index.h"
#include "sd_uuid.h"

static DEFINE_SPINLOCK(sd_index_lock);
static DEFINE_IDA(sd_index_ida);

static int alloc_index (struct scsi_device *sd, int *index);
static int free_index (int index);
static int read_efi_vdp83(char **data, unsigned long *size);

static unsigned char max_vpd83_len = MAX_VPD83_LEN;
static efi_char16_t name[100];
static efi_guid_t guid = EFI_GUID(0xa990f257, 0xccb3, 0x4114,  0x88, 0xdd, 0x78, 0x7a, 0xc0, 0x8e, 0x57, 0x97);
static u32 attr = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;

struct vdp83_map {
	char vpd83[MAX_VPD83_LEN];
	int index;
};

static void str_to_str16(const char *str, efi_char16_t *str16)
{
	size_t i;

	for (i = 0; i < strlen(str); i++)
		str16[i] = str[i];

	str16[i] = '\0';
}

static int read_efi_vdp83(char **data, unsigned long *size)
{
	unsigned long read_size = 0;
	u8 *read_data = NULL;
	efi_status_t status;
	int i;

	str_to_str16("CDN", name);

	status = efi.get_variable(name, &guid,&attr, &read_size, read_data);
	printk("%s: status 0x%lx\n", __func__, status);
	printk("%s: read_size %lu\n", __func__, read_size);
	if (status == EFI_BUFFER_TOO_SMALL) {
		read_data = (u8*)vmalloc(read_size);
		if (read_data) {
			status = efi.get_variable(name, &guid, &attr, &read_size, read_data);
			if (status == EFI_SUCCESS) {
				*data = read_data;
				*size = read_size;
				printk("efi var data\n");
				for (i=0; i<read_size; i++)
					printk("%02x", read_data[i]);
				printk("efi var data end\n");
			}
		}
	}
	else if (status == EFI_NOT_FOUND) {
		struct vdp83_map vdp83_map[MAX_DISK];
		printk("No CDN EFI var found, reate new one.\n");
		memset (vdp83_map, 0xff, sizeof(vdp83_map));
		status = efi.set_variable(name, &guid, attr, sizeof(vdp83_map), vdp83_map);
		*data = read_data;
		*size = read_size;
		if (status == EFI_SUCCESS)
			printk("CDN EFI var created.\n");
	}
	return status;

}

int cdn_efi_init(void)
{
	char *efi_data = NULL;
	unsigned long efi_size;
	int i, error, ret, index;

	error = read_efi_vdp83(&efi_data, &efi_size);
	if (error == EFI_SUCCESS) {
		for (i = 0; i < MAX_DISK; i++) {
			if (((struct vdp83_map *)efi_data + i)->index >= 0) {
				if (!(ret = ida_pre_get(&sd_index_ida, GFP_KERNEL)))
					return ret;
				spin_lock(&sd_index_lock);
				ida_get_new_above(&sd_index_ida, ((struct vdp83_map *)efi_data + i)->index, &index);
				spin_unlock(&sd_index_lock);
				printk("set ida %d according efi var\n", ((struct vdp83_map *)efi_data + i)->index);
			}
		}
	}
	return error;

}
EXPORT_SYMBOL(cdn_efi_init);

static int alloc_index (struct scsi_device *sd, int *index)
{
	int error, i;

	/* vpd */
	const int vpd_len = 64;
	unsigned char *buffer = kmalloc(vpd_len, GFP_KERNEL);

	/* efi */
	char *efi_data = NULL;
	unsigned long efi_size;

	char zerostr[MAX_VPD83_LEN];
	memset(zerostr, 0xff, sizeof(zerostr));

	if (!buffer || scsi_get_vpd_page(sd, 0x83, buffer, vpd_len)) {
		printk("read vpd 0x83 failed\n");
		return -1;
	}
	printk("vpd 0x83 len: %d\n", buffer[7]);

	error = read_efi_vdp83(&efi_data, &efi_size);

	if (error == EFI_SUCCESS) {
		for (i = 0; i < MAX_DISK; i++) {
			if (memcmp(((struct vdp83_map *)efi_data + i)->vpd83, zerostr, MAX_VPD83_LEN) == 0)
				continue;
			if (memcmp(((struct vdp83_map *)efi_data + i)->vpd83, buffer + 8, min(max_vpd83_len, buffer[7])) == 0) {
				printk("efi_data->index == 0x%x", ((struct vdp83_map *)efi_data + i)->index);
				*index = ((struct vdp83_map *)efi_data + i)->index;
				return 0;
			}
		}
	}
	do {
		if (!(error = ida_pre_get(&sd_index_ida, GFP_KERNEL)))
			return error;

		spin_lock(&sd_index_lock);
		error = ida_get_new(&sd_index_ida, index);
		spin_unlock(&sd_index_lock);

		/* Update efi var */
		for (i=0; i<MAX_DISK; i++) {
			if (memcmp(((struct vdp83_map *)efi_data + i)->vpd83, zerostr, MAX_VPD83_LEN) == 0) {
				printk("not found index in efi var, write index %d\n", *index);
				((struct vdp83_map *)efi_data + i)->index = *index;
				memcpy(((struct vdp83_map *)efi_data + i)->vpd83, buffer + 8, min(max_vpd83_len, buffer[7]));
				error = efi.set_variable(name, &guid, attr, efi_size, efi_data);
				if (error == EFI_SUCCESS)
					printk("not found index in efi var, write index %d sucess\n", *index);
				break;
			}
		}
	} while (error == -EAGAIN);
	return error;
}

static int free_index (int index)
{
	spin_lock(&sd_index_lock);
	ida_remove(&sd_index_ida, index);
	spin_unlock(&sd_index_lock);
	return 0;
}

static int __init sd_uuid(char *str)
{
	if (!strcmp(str, "uuid")) {
		sd_index.alloc_index = alloc_index;
		sd_index.free_index = free_index;
	}
	return 0;

}
early_param("sdindex", sd_uuid);
