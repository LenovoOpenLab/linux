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

#include "sd.h"
#include "scsi_priv.h"
#include "scsi_logging.h"
#include "sd_index.h"

static DEFINE_SPINLOCK(sd_index_lock);
static DEFINE_IDA(sd_index_ida);

static int alloc_index (struct scsi_device *sd, int *index);
static int free_index (int index);

static int alloc_index (struct scsi_device *sd, int *index)
{
	int error;
	printk("sd_index alloc_index\n");
	do {
		if (!(error = ida_pre_get(&sd_index_ida, GFP_KERNEL)))
			return error;

		spin_lock(&sd_index_lock);
		error = ida_get_new(&sd_index_ida, index);
		spin_unlock(&sd_index_lock);
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

static int __init init_sd_index(void)
{
	printk("init_sd_index\n");
	return 0;
}

struct sd_index sd_index = {
	.alloc_index = alloc_index,
	.free_index = free_index,
};

EXPORT_SYMBOL(sd_index);

MODULE_LICENSE("GPL");
module_init(init_sd_index);
