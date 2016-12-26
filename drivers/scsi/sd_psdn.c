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
#include <scsi/scsi_transport_sas.h>
#include <linux/libata.h>
#include <linux/list.h>
#include <linux/acpi.h>

#include "sd.h"
#include "scsi_priv.h"
#include "scsi_logging.h"
#include "sd_index.h"
#include "sd_psdn.h"
#include "mpt3sas/mpt3sas_base.h"

static DEFINE_SPINLOCK(sd_index_lock);
static DEFINE_IDA(sd_index_ida);

static int alloc_index (struct scsi_device *sd, int *index);
static int free_index (int index);
static int sd_get_index(struct scsi_device *sdp);
static void sd_release_index(int id);

static int alloc_index (struct scsi_device *sd, int *index)
{
	sd_index_talbe_init();
	*index = sd_get_index(sd);
	return 0;
}

static int free_index (int index)
{
	sd_release_index(index);
	return 0;
}

static int __init sd_psdn(char *str)
{
	if (!strcmp(str, "psdn")) {
		printk("psdn parametr\n");
		sd_index.alloc_index = alloc_index;
		sd_index.free_index = free_index;
	}
	return 0;

}
early_param("sdindex", sd_psdn);

static int sd_get_index(struct scsi_device *sdp)
{
	struct sd_pid * ppid = NULL;
	struct pci_dev *pdev = NULL;
	int index,ret;
	unsigned short pciid;
	struct scsi_target *starget;
	struct sas_rphy *rphy;

	if(!strncmp(sdp->host->hostt->name,"Fusion MPT SAS Host",19)){
		int phyid2slot[8]={7,6,4,5,3,2,0,1};
		pdev = to_pci_dev(sdp->host->shost_gendev.parent);
		pciid=PCI_DEVID(pdev->bus->number, pdev->devfn);
		starget = scsi_target(sdp);
		rphy = dev_to_rphy(starget->dev.parent);
		spin_lock(&sd_index_lock);
                list_for_each_entry(ppid, &sd_pid_list, list){
                        if((pciid == ppid->pci_id) && (phyid2slot[rphy->identify.phy_identifier] <= (ppid->lid->end - ppid->lid->start))){
				spin_unlock(&sd_index_lock);	
				return (phyid2slot[rphy->identify.phy_identifier]+ppid->lid->start);
                        }
                }
                spin_unlock(&sd_index_lock);
	}else if(!strncmp(sdp->host->hostt->name,"sata",4) || !strncmp(sdp->host->hostt->name,"ahci",4)){
		int portid2slot[2][6]={{2,3,1,0,6,7},{-1,-2,-1,-1,-1,-1}};
		int controllerID = 0;
		struct ata_port *ap = ata_shost_to_port(sdp->host);
		pdev = to_pci_dev(ap->host->dev);
		pciid=PCI_DEVID(pdev->bus->number, pdev->devfn);
		controllerID = (pciid == 0xfa)?0:1;
		spin_lock(&sd_index_lock);
                list_for_each_entry(ppid, &sd_pid_list, list){
			printk("-----------------pciid:%04x %04x ,%d",pciid,ppid->pci_id,portid2slot[controllerID][ap->local_port_no-1]);
                        if((pciid == ppid->pci_id) && ((ap->local_port_no-1) <= (ppid->lid->end - ppid->lid->start))){
				spin_unlock(&sd_index_lock);
				return (portid2slot[controllerID][ap->local_port_no-1]+ppid->lid->start);
                        }
                }
                spin_unlock(&sd_index_lock);
	}

	do {
		if (!ida_pre_get(&sd_index_ida, GFP_KERNEL))
			return -ENOMEM;

		spin_lock(&sd_index_lock);
		ret = ida_get_new(&sd_index_ida, &index);
		spin_unlock(&sd_index_lock);
	} while (ret == -EAGAIN);
	
	if (ret) {
		sdev_printk(KERN_WARNING, sdp, "sd_probe: memory exhausted.\n");
		return ret;
	}
	return index;
}

static void sd_release_index(int id)
{
	struct sd_pid * ppid = NULL;

	spin_lock(&sd_index_lock);
	list_for_each_entry(ppid, &sd_pid_list, list){
		if(id >= ppid->lid->start && id <= ppid->lid->end){
			spin_unlock(&sd_index_lock);
			return;
		}
	}

	ida_remove(&sd_index_ida, id);
	spin_unlock(&sd_index_lock);

	return;
}


LIST_HEAD(sd_pid_list);

/*
 * SCSI name table formate
 * start == end == -1 if dynamic alloc
 * pci_id, start, end
 * u16   , int  , int
 * */
#define MAX_HBA_CONTROLLER 10
#define MAX_HBA_BP 4
#define MAX_SATA_CONTROLLER 3
#define MAX_SATA_BP 1
#define MAX_DY 50

#pragma pack(1)
struct bp_instance {
	u8 bp_no;
	u8 sloct_count;
	u8 sloct_start_no;
	u8 reserved;
};

struct hba_instance {
	u8 hba_no;
	u16 hba_location;
	u8 bp_count; /* We can't detect backplane count, set to 4 */
	struct bp_instance bp_instance[MAX_HBA_BP];
};

struct sata_instance {
	u8 sata_no;
	u16 sata_location;
	u8 bp_count; /* We can't detect backplane count, set to 4 */
	struct bp_instance bp_instance[MAX_SATA_BP];
};

struct acpi_csdn {
	struct acpi_table_header hdr;
	u8 hba_count;
	struct hba_instance hba_instance[MAX_HBA_CONTROLLER];
	u8 sata_count;
	struct sata_instance sata_instance[MAX_SATA_CONTROLLER];
};

struct dy_name_item {
	u16 pci_id;
	enum dmr_type type;
	int start;
	int end;
};
#pragma pack ()

static struct dy_name_item dy_name[MAX_DY]; 
static int dy_num = 0;

int sd_index_talbe_init()
{
	int i, j;
	struct sd_pid *pptr;
	struct sd_lid *lptr;
	struct acpi_csdn *buff;
	acpi_status status;

	/* Read CSDN ACPI table */
	status = acpi_get_table("CSDN", 0,
				(struct acpi_table_header **)&buff);

	if (ACPI_FAILURE(status)) {
		printk(KERN_ERR "%s: ERROR - Could not get CSDN table\n", __func__);
		return -EIO;
	}

	for (i=0; i<buff->hba_count; i++) {
		for (j=0; j<buff->hba_instance[i].bp_count; j++) {
			if (buff->hba_instance[i].bp_instance[j].sloct_count) {
				dy_name[dy_num].pci_id = buff->hba_instance[i].hba_location;
				dy_name[dy_num].type = OFF_ALLOC;
				dy_name[dy_num].start = buff->hba_instance[i].bp_instance[j].sloct_start_no;
				dy_name[dy_num++].end = buff->hba_instance[i].bp_instance[j].sloct_start_no
					+ buff->hba_instance[i].bp_instance[j].sloct_count - 1;
			}
		}
	}
	for (i=0; i<buff->sata_count; i++) {
		for (j=0; j<buff->sata_instance[i].bp_count; j++) {
			if (buff->sata_instance[i].bp_instance[j].sloct_count) {
				dy_name[dy_num].pci_id = buff->sata_instance[i].sata_location;
				dy_name[dy_num].type = OFF_ALLOC;
				dy_name[dy_num].start = buff->sata_instance[i].bp_instance[j].sloct_start_no;
				dy_name[dy_num++].end = buff->sata_instance[i].bp_instance[j].sloct_start_no
					+ buff->sata_instance[i].bp_instance[j].sloct_count - 1;
			}
		}
	}
#ifdef DEBUG_AHCI
	dy_name[dy_num].pci_id = PCI_DEVID(0x00, PCI_DEVFN(0x1f, 0x02));
	dy_name[dy_num].type = OFF_ALLOC;
	dy_name[dy_num].start = dy_name[dy_num-1].end + 1;
	dy_name[dy_num].end = dy_name[dy_num].start+5;
	dy_num++;
#endif
	printk("CSDN debug: dynamic domain %d\n",dy_num);
	for (i=0; i<dy_num; i++) {
		printk("pci 0x%x; start %d; end %d\n", dy_name[i].pci_id, dy_name[i].start, dy_name[i].end);
	}

	/* scsi_physical */
	for (i=0; i<sizeof(dy_name)/sizeof(struct dy_name_item); i++) {
		if (dy_name[i].type == OFF_ALLOC){
                        pptr = kzalloc(sizeof(struct sd_pid), GFP_KERNEL);
                        pptr->pci_id = dy_name[i].pci_id;
                        pptr->dmr = dy_name[i].type;
                        list_add_tail(&pptr->list, &sd_pid_list);
			lptr = kzalloc(sizeof(struct sd_lid), GFP_KERNEL);
			lptr->start = dy_name[i].start;
			lptr->end = dy_name[i].end;
			pptr->lid = lptr;

			if(sd_reserve_idr(lptr->start,lptr->end) != 0){
				ACPI_DEBUG_PRINT((ACPI_DB_INIT,"psdn init error\n"));
				return -ENOSPC;
			}
		}
	}

	return 0;
}

int sd_reserve_idr(int start,int end)
{
	int m,index,error;

	for(m = start;m <= end; m++){
		do {
			if (!ida_pre_get(&sd_index_ida, GFP_KERNEL))
				goto error_ret;

			spin_lock(&sd_index_lock);
			error = ida_get_new_above(&sd_index_ida, m, &index);
			spin_unlock(&sd_index_lock);
		} while (unlikely(error == -EAGAIN));

		if (error) {
			ACPI_DEBUG_PRINT((ACPI_DB_INIT, "sd_probe: memory exhausted.\n"));
			goto error_ret;
		}

		if (m != index) {
			//ACPI_DEBUG_PRINT((ACPI_DB_INIT,"memory exhausted...error:%d\n",error));
			spin_lock(&sd_index_lock);
			ida_remove(&sd_index_ida,index);
			spin_unlock(&sd_index_lock);
			continue;
		}
	}
	return 0;

error_ret:
	return -ENOSPC;
}
