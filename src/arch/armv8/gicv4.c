/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 */

#include <arch/gic.h>


void gicr_set_vpropbaser(cpuid_t gicr_id, uint64_t phy_addr, size_t id_bits){
    spin_lock(&gicr_lock);
    gicr[gicr_id].VPROPBASER = phy_addr |
                            GICR_PROPBASER_InnerShareable |
                            GICR_PROPBASER_RaWaWb |
                            id_bits;
    spin_unlock(&gicr_lock);
}

void gicr_set_vpendbaser(cpuid_t gicr_id, uint64_t phy_addr){

    if((gicr[gicr_id].VPENDBASER & GICR_VPENDBASER_VAL_BIT) == 0){
        spin_lock(&gicr_lock);
        gicr[gicr_id].VPENDBASER = phy_addr |
                                GICR_PROPBASER_InnerShareable |
                                GICR_PROPBASER_RaWaWb |
                                GICR_VPENDBASER_VAL_BIT | GICR_VPENDBASER_IDAI_BIT;
        spin_unlock(&gicr_lock);
    }
}