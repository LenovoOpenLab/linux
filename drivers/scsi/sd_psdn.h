#include <linux/types.h>
#include <linux/pci.h>
#include <uapi/linux/pci.h>
#include <scsi/scsi_device.h>

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

extern struct list_head sd_pid_list;

int sd_index_talbe_init(void);
int sd_offset_register(u16 pciid, get_offset get_offset_fun);
int sd_offset_unregister(u16 pciid);
int sd_reserve_idr(int start,int end);
