/*
 * Copyright (c) 2018-2019, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

/*********** WARNING: This is an auto-generated file. Do not edit! ***********/

#ifndef __TFM_PARTITION_DEFS_INC__
#define __TFM_PARTITION_DEFS_INC__

#define TFM_SP_STORAGE_ID (TFM_SP_BASE + 0)

#define TFM_SP_ITS_ID (TFM_SP_BASE + 1)

#ifdef TFM_PARTITION_AUDIT_LOG
#define TFM_SP_AUDIT_LOG_ID (TFM_SP_BASE + 2)
#endif /* TFM_PARTITION_AUDIT_LOG */

#define TFM_SP_CRYPTO_ID (TFM_SP_BASE + 3)

#ifdef TFM_PARTITION_PLATFORM
#define TFM_SP_PLATFORM_ID (TFM_SP_BASE + 4)
#endif /* TFM_PARTITION_PLATFORM */

#define TFM_SP_INITIAL_ATTESTATION_ID (TFM_SP_BASE + 5)

#ifdef TFM_PARTITION_TEST_CORE
#define TFM_SP_CORE_TEST_ID (TFM_SP_BASE + 6)
#endif /* TFM_PARTITION_TEST_CORE */

#ifdef TFM_PARTITION_TEST_CORE
#define TFM_SP_CORE_TEST_2_ID (TFM_SP_BASE + 7)
#endif /* TFM_PARTITION_TEST_CORE */

#ifdef TFM_PARTITION_TEST_SECURE_SERVICES
#define TFM_SP_SECURE_TEST_PARTITION_ID (TFM_SP_BASE + 8)
#endif /* TFM_PARTITION_TEST_SECURE_SERVICES */

#ifdef TFM_PARTITION_TEST_CORE_IPC
#define TFM_SP_IPC_SERVICE_TEST_ID (TFM_SP_BASE + 9)
#endif /* TFM_PARTITION_TEST_CORE_IPC */

#ifdef TFM_PARTITION_TEST_CORE_IPC
#define TFM_SP_IPC_CLIENT_TEST_ID (TFM_SP_BASE + 10)
#endif /* TFM_PARTITION_TEST_CORE_IPC */

#ifdef TFM_ENABLE_IRQ_TEST
#define TFM_IRQ_TEST_1_ID (TFM_SP_BASE + 11)
#endif /* TFM_ENABLE_IRQ_TEST */

#ifdef TFM_PARTITION_APP_ROT
#define TFM_APP_ROT_ID (TFM_SP_BASE + 12)
#endif /* TFM_PARTITION_APP_ROT */

#define TFM_MAX_USER_PARTITIONS (13)

#endif /* __TFM_PARTITION_DEFS_INC__ */
