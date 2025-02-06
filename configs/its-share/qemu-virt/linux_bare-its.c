#include <config.h>

VM_IMAGE(linux_image, XSTR(BAO_DEMOS_WRKDIR_IMGS/linux.bin));
VM_IMAGE(baremetal_image, XSTR(BAO_DEMOS_WRKDIR_IMGS/baremetal.bin));


struct config config = {

    .vmlist_size = 2,
    .vmlist = {
        { /*Baremetal Guest*/
            .image = {
                .base_addr = 0x50000000,
                .load_addr = VM_IMAGE_OFFSET(baremetal_image),
                .size = VM_IMAGE_SIZE(baremetal_image)
            },

            .entry = 0x50000000,
            .cpu_affinity = 0x1,

            .platform = {
                .cpu_num = 1,
                
                .region_num = 1,
                .regions =  (struct vm_mem_region[]) {
                    {
                        .base = 0x50000000,
                        .size = 0x4000000 
                    }
                },

                .dev_num = 2,
                .devs =  (struct vm_dev_region[]) {
                    {   
                        /* PL011 */
                        .pa = 0x09000000,
                        .va = 0x09000000,
                        .size = 0x10000,
                        .interrupt_num = 1,
                        .interrupts = (irqid_t[]) {33},        
                    },
                    {
                        /* Arch timer interrupt */
                        .interrupt_num = 1,
                        .interrupts = 
                            (irqid_t[]) {27}                         
                    }
                },

                .msi_num = 1,
                .msi_id = (irqid_t[]){8195},

                .arch = {
                    .gic = {
                        .gicd_addr = 0x08000000,
                        .gicr_addr = 0x080A0000,
                        .gits_addr = 0x8080000,
                    }
                }
            },
        },
        {   /*Linux Guest*/
            .image = {
                .base_addr = 0x80000000,
                .load_addr = VM_IMAGE_OFFSET(linux_image),
                .size = VM_IMAGE_SIZE(linux_image),
            },

            .entry = 0x80000000,
            .cpu_affinity = 0x2,

            .platform = {
                .cpu_num = 1,
                
                .region_num = 1,
                .regions =  (struct vm_mem_region[]) {
                    {
                        .base = 0x80000000,
                        .size = 0x40000000,
                        .place_phys = true,
                        .phys = 0x80000000
                    }
                },
                .dev_num = 2,
                .devs =  (struct vm_dev_region[]) {
                    {   
                        /* Arch timer interrupt */
                        .interrupt_num = 1,
                        .interrupts = (irqid_t[]) {27}                      
                    },
                    {
                        /* virtio devices */
                        .pa = 0xa003000,   
                        .va = 0xa003000,  
                        .size = 0x1000,
                        .interrupt_num = 8,
                        .interrupts = (irqid_t[]) {72,73,74,75,76,77,78,79}
                    },
                },

                .pcie_region_num = 2,
                .pcie_regions = (struct vm_pcie_region[]) {
                    {   //config space
                        .cfg_space = true,
                        .pa = 0x4010000000,
                        .va = 0x4010000000,
                        .size = 0x10000000, //256MiB
                    },
                    {   //mmio space
                        .pa = 0x10000000,
                        .va = 0x10000000,
                        .size = 0x2eff0000,
                    },
                },
                .pcie_irq_num = 6,
                .pcie_irq = (irqid_t[]) {15,16,17,18,21,36},

                .msi_num = 3,
                .msi_id = (irqid_t[]){8192,8193,8194},
              
                .arch = {
                    .gic = {
                        .gicd_addr = 0x8000000,
                        .gicr_addr = 0x80A0000,
                        .gits_addr = 0x8080000,
                    }
                }
            },
        }
    },
};
