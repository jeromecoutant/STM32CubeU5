/*
 * Copyright (c) 2017-2019, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <arm_cmse.h>
#include "region_defs.h"
#include "secure_fw/spm/spm_api.h"
#include "tfm_api.h"

/**
 * \brief Check whether the current partition has access to a memory range
 *
 * This function assumes, that the current MPU configuration is set for the
 * partition to be checked. The flags should contain information of the
 * execution mode of the partition code (priv/unpriv), and access type
 * (read/write) as specified in "ARMv8-M Security Extensions: Requirements on
 * Development Tools" chapter "Address range check intrinsic"
 *
 * \param[in] p      The start address of the range to check
 * \param[in] s      The size of the range to check
 * \param[in] flags  The flags to pass to the cmse_check_address_range func
 *
 * \return TFM_SUCCESS if the partition has access to the memory range,
 *         TFM_ERROR_GENERIC otherwise.
 */
static enum tfm_status_e has_access_to_region(const void *p, size_t s,
                                              int flags)
{
    int32_t range_access_allowed_by_mpu;

    /* Use the TT instruction to check access to the partition's regions*/
    range_access_allowed_by_mpu =
                          cmse_check_address_range((void *)p, s, flags) != NULL;

    if (range_access_allowed_by_mpu) {
        return TFM_SUCCESS;
    }

    /* If all else fails, check whether the region is in the non-secure
     * memory
     */
    if ((check_address_range(p, s, NS_CODE_START,
                             NS_CODE_LIMIT) == TFM_SUCCESS) ||
#if defined(NS_DATA_START_2) && defined(NS_DATA_LIMIT_2)
        (check_address_range(p, s, NS_DATA_START_2,
                             NS_DATA_LIMIT_2) == TFM_SUCCESS) ||
#endif
        (check_address_range(p, s, NS_DATA_START,
                             NS_DATA_LIMIT) == TFM_SUCCESS)) {
        return TFM_SUCCESS;
    } else {
        return TFM_ERROR_GENERIC;
    }
}

enum tfm_status_e tfm_core_has_read_access_to_region(const void *p, size_t s,
                                                     uint32_t ns_caller,
                                                     uint32_t privileged)
{
    int flags = CMSE_MPU_READ;

    if (privileged == TFM_PARTITION_UNPRIVILEGED_MODE) {
        flags |= CMSE_MPU_UNPRIV;
    }

    if (ns_caller) {
        flags |= CMSE_NONSECURE;
    }

    return has_access_to_region(p, s, flags);
}

enum tfm_status_e tfm_core_has_write_access_to_region(const void *p, size_t s,
                                                      uint32_t ns_caller,
                                                      uint32_t privileged)
{
    int flags = CMSE_MPU_READWRITE;

    if (privileged == TFM_PARTITION_UNPRIVILEGED_MODE) {
        flags |= CMSE_MPU_UNPRIV;
    }

    if (ns_caller) {
        flags |= CMSE_NONSECURE;
    }

    return has_access_to_region(p, s, flags);
}
