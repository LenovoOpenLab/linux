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
#include <linux/efi.h>
#include <linux/vmalloc.h>

#include "sd.h"
#include "scsi_priv.h"
#include "scsi_logging.h"
#include "sd_index.h"
#include "sd_phy.h"
#include "mpt3sas/mpt3sas_base.h"

typedef int (* get_offset)(struct scsi_device *, int *);
enum dmr_type {/* domain map rule type */ 
	DYN_ALLOC, /* dynamic alloc */
	OFF_ALLOC, /* offset alloc */
	MAN_ALLOC, /* manual alloc */
};

struct sd_pid {/* physical index domain */
	struct list_head list;
	u16 pci_id;              /* PCI id */
	enum dmr_type dmr;
	struct sd_lid *lid; /* null if DYN_ALLOC */
};

/* used for OFF_ALLOC */
struct sd_lid {/* logical index domain*/
	int start;
	int end;
	get_offset  get_offset_fun;
};

/*
 * SCSI name table formate
 * start == end == -1 if dynamic alloc
 * pci_id, start, end
 * u16   , int  , int
 * */
#define MAX_DY 10

#pragma pack(1)
struct dy_name_item {
	u16 pci_id;
	enum dmr_type type;
	int start;
	int end;
};
#pragma pack ()

static DEFINE_SPINLOCK(sd_index_lock);
static DEFINE_IDA(sd_index_ida);

static int alloc_index (struct scsi_device *sd, int *index);
static int free_index (int index);
static int sd_get_index(struct scsi_device *sdp);
static void sd_release_index(int id);
static int sd_index_talbe_init(void);
static int sd_reserve_idr(int start,int end);
static LIST_HEAD(sd_pid_list);
static int read_cdn(void);
static void str_to_str16(const char *str, efi_char16_t *str16);

static struct dy_name_item *dy_name_p = NULL; 

static int alloc_index (struct scsi_device *sd, int *index)
{
	read_cdn();
	if (dy_name_p != NULL) {
		sd_index_talbe_init();
		*index = sd_get_index(sd);
	}
	return 0;
}

static int free_index (int index)
{
	if (dy_name_p != NULL) {
		sd_release_index(index);
	}
	return 0;
}

static int __init sd_phy(char *str)
{
	if (!strcmp(str, "phy")) {
		printk("phy parametr\n");
		sd_index.alloc_index = alloc_index;
		sd_index.free_index = free_index;
	}
	return 0;

}
early_param("sdindex", sd_phy);

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

static void str_to_str16(const char *str, efi_char16_t *str16)
{
	size_t i;

	for (i = 0; i < strlen(str); i++)
		str16[i] = str[i];

	str16[i] = '\0';
}

static int read_cdn()
{
	efi_char16_t name[10];
	efi_guid_t guid = EFI_GUID(0x4a67b082, 0x0a4c, 0x41cf,  0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xbb);
	u32 attr = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
	unsigned long data_size = 0;
	void *read_data = NULL;
	efi_status_t status;
	int i;

	str_to_str16("CDN", name);
	status = efi.get_variable(name,&guid,&attr,&data_size,read_data);
	printk("status 0x%lx\n", status);
	printk("data_size %lu\n", data_size);
	if (status == EFI_BUFFER_TOO_SMALL) {
		/* Allocate read_data buffer of data_size bytes */
		read_data = (void *)vmalloc(data_size);
		if (!read_data) {
			/* Your handling here */
			return -1;
		}

		/* Get variable contents into buffer */
		status = efi.get_variable(name,&guid,&attr,&data_size,read_data);
		if (status != EFI_SUCCESS) {
			/* Your handling here */
			return -1;
		}
		else {
			/* Variable is now in read_data */
			for (i=0; i<3; i++) {
				printk("0x%x", (*((struct dy_name_item *)read_data + i)).pci_id);
				printk("%d", (*((struct dy_name_item *)read_data + i)).start);
				printk("%d", (*((struct dy_name_item *)read_data + i)).end);
			}
			printk("\n");
			dy_name_p = (struct dy_name_item *)read_data;
		}   
	} 
	else if (status == EFI_NOT_FOUND) {
		/* There is no Boot0001 variable. Try Boot0000 maybe? */
			return -1;
	} 
	else {
		/* Your handling here */
	}
	return 0;

}

static int sd_index_talbe_init()
{
	int i;
	struct sd_pid *pptr;
	struct sd_lid *lptr;

	/* scsi_physical */
	for (i=0; i<MAX_DY; i++) {
		if (dy_name_p[i].type == OFF_ALLOC){
                        pptr = kzalloc(sizeof(struct sd_pid), GFP_KERNEL);
                        pptr->pci_id = dy_name_p[i].pci_id;
                        pptr->dmr = dy_name_p[i].type;
                        list_add_tail(&pptr->list, &sd_pid_list);
			lptr = kzalloc(sizeof(struct sd_lid), GFP_KERNEL);
			lptr->start = dy_name_p[i].start;
			lptr->end = dy_name_p[i].end;
			pptr->lid = lptr;

			if(sd_reserve_idr(lptr->start,lptr->end) != 0){
				ACPI_DEBUG_PRINT((ACPI_DB_INIT,"psdn init error\n"));
				return -ENOSPC;
			}
		}
	}

	return 0;
}

static int sd_reserve_idr(int start,int end)
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
