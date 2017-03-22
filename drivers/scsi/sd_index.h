#ifndef _SCSI_SD_NAMING_H
#define _SCSI_SD_NAMING_H

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

struct sd_index {
	int (*alloc_index) (struct scsi_device *sd, int *index);
	int (*free_index) (int index);
};

extern struct sd_index sd_index;

#endif
