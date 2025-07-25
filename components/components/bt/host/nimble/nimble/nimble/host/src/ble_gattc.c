/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/**
 * GATT client - Generic Attribute Profile; client operations.
 *
 * Design overview:
 *
 * GATT client procedures are initiated by the application via function calls.
 * Such functions return when either of the following happens:
 *
 * (1) The procedure completes (success or failure).
 * (2) The procedure cannot proceed until a BLE peer responds.
 *
 * For (1), the result of the procedure if fully indicated by the function
 * return code.
 * For (2), the procedure result is indicated by an application-configured
 * callback.  The callback is executed when the procedure completes.
 *
 * Notes on thread-safety:
 * 1. The ble_hs mutex must never be locked when an application callback is
 *    executed.  A callback is free to initiate additional host procedures.
 * 2. The only resource protected by the mutex is the list of active procedures
 *    (ble_gattc_procs).  Thread-safety is achieved by locking the mutex during
 *    removal and insertion operations.  Procedure objects are only modified
 *    while they are not in the list.  This is sufficient, as the host parent
 *    task is the only task which inspects or modifies individual procedure
 *    entries.  Tasks have the following permissions regarding procedure
 *    entries:
 *
 *                | insert  | remove    | inspect   | modify
 *    ------------+---------+-----------|-----------|---------
 *    parent task | X       | X         | X         | X
 *    other tasks | X       |           |           |
 */

#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "os/os_mempool.h"
#include "nimble/ble.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "ble_hs_priv.h"
#include "ble_gattc_cache_priv.h"
#if MYNEWT_VAL(BLE_GATT_CACHING)
#include "host/ble_esp_gattc_cache.h"
#endif
#if NIMBLE_BLE_CONNECT

#if MYNEWT_VAL(BLE_GATTC)
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

#endif
/*****************************************************************************
 * $definitions / declarations                                               *
 *****************************************************************************/

/**
 * The maximum time to wait for a single ATT response.  The spec defines this
 * as the ATT transaction time (Vol. 3, Part F, 3.3.3)
 */
#define BLE_GATTC_UNRESPONSIVE_TIMEOUT_MS       30000 /* ms */

#define BLE_GATT_OP_NONE                        UINT8_MAX

#define BLE_GATT_OP_MTU                         0
#define BLE_GATT_OP_DISC_ALL_SVCS               1
#define BLE_GATT_OP_DISC_SVC_UUID               2
#define BLE_GATT_OP_FIND_INC_SVCS               3
#define BLE_GATT_OP_DISC_ALL_CHRS               4
#define BLE_GATT_OP_DISC_CHR_UUID               5
#define BLE_GATT_OP_DISC_ALL_DSCS               6
#define BLE_GATT_OP_READ                        7
#define BLE_GATT_OP_READ_UUID                   8
#define BLE_GATT_OP_READ_LONG                   9
#define BLE_GATT_OP_READ_MULT                   10
#define BLE_GATT_OP_READ_MULT_VAR               11
#define BLE_GATT_OP_WRITE                       12
#define BLE_GATT_OP_WRITE_LONG                  13
#define BLE_GATT_OP_WRITE_RELIABLE              14
#define BLE_GATT_OP_INDICATE                    15
#define BLE_GATT_OP_CNT                         16

/** Procedure stalled due to resource exhaustion. */
#define BLE_GATTC_PROC_F_STALLED                0x01

/** Represents an in-progress GATT procedure. */
struct ble_gattc_proc {
    STAILQ_ENTRY(ble_gattc_proc) next;

#if MYNEWT_VAL(BLE_GATTC_AUTO_PAIR)
    struct ble_gatt_error error;
#endif
    uint32_t exp_os_ticks;
    uint16_t conn_handle;
    uint16_t cid;
    uint8_t op;
    uint8_t flags;

    union {
        struct {
            ble_gatt_mtu_fn *cb;
            void *cb_arg;
        } mtu;

        struct {
            uint16_t prev_handle;
            ble_gatt_disc_svc_fn *cb;
            void *cb_arg;
        } disc_all_svcs;

        struct {
            ble_uuid_any_t service_uuid;
            uint16_t prev_handle;
            ble_gatt_disc_svc_fn *cb;
            void *cb_arg;
        } disc_svc_uuid;

        struct {
            uint16_t prev_handle;
            uint16_t end_handle;

            uint16_t cur_start;
            uint16_t cur_end;

#if MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
            ble_gatt_disc_incl_svc_fn *cb;
#else
            ble_gatt_disc_svc_fn *cb;
#endif
            void *cb_arg;
        } find_inc_svcs;

        struct {
            uint16_t prev_handle;
            uint16_t end_handle;
            ble_gatt_chr_fn *cb;
            void *cb_arg;
        } disc_all_chrs;

        struct {
            ble_uuid_any_t chr_uuid;
            uint16_t prev_handle;
            uint16_t end_handle;
            ble_gatt_chr_fn *cb;
            void *cb_arg;
        } disc_chr_uuid;

        struct {
            uint16_t chr_val_handle;
            uint16_t prev_handle;
            uint16_t end_handle;
            ble_gatt_dsc_fn *cb;
            void *cb_arg;
        } disc_all_dscs;

        struct {
            uint16_t handle;
            ble_gatt_attr_fn *cb;
            void *cb_arg;
        } read;

        struct {
            ble_uuid_any_t chr_uuid;
            uint16_t start_handle;
            uint16_t end_handle;
            ble_gatt_attr_fn *cb;
            void *cb_arg;
        } read_uuid;

        struct {
            uint16_t handle;
            uint16_t offset;
            ble_gatt_attr_fn *cb;
            void *cb_arg;
        } read_long;

        struct {
            uint16_t handles[MYNEWT_VAL(BLE_GATT_READ_MAX_ATTRS)];
            uint8_t num_handles;
            bool variable;
            ble_gatt_attr_fn *cb;
            ble_gatt_attr_mult_fn *cb_mult;
            void *cb_arg;
        } read_mult;

        struct {
            uint16_t att_handle;
            ble_gatt_attr_fn *cb;
            void *cb_arg;
#if MYNEWT_VAL(BLE_GATTC_AUTO_PAIR)
            struct os_mbuf * om;
#endif
        } write;

        struct {
            struct ble_gatt_attr attr;
            uint16_t length;
            ble_gatt_attr_fn *cb;
            void *cb_arg;
        } write_long;

        struct {
            struct ble_gatt_attr attrs[MYNEWT_VAL(BLE_GATT_WRITE_MAX_ATTRS)];
            uint8_t num_attrs;
            uint8_t cur_attr;
            uint16_t length;
            ble_gatt_reliable_attr_fn *cb;
            void *cb_arg;
        } write_reliable;

        struct {
            uint16_t chr_val_handle;
        } indicate;
    };
};


#if MYNEWT_VAL(BLE_GATTC_PROC_PREEMPTION_PROTECT)
static struct ble_gattc_proc_list temp_proc_list;
#endif
STAILQ_HEAD(ble_gattc_proc_list, ble_gattc_proc);

/**
 * Error functions - these handle an incoming ATT error response and apply it
 * to the appropriate active GATT procedure.
 */
typedef void ble_gattc_err_fn(struct ble_gattc_proc *proc, int status,
                              uint16_t att_handle);

#if MYNEWT_VAL(BLE_GATTC)
static ble_gattc_err_fn ble_gattc_mtu_err;
static ble_gattc_err_fn ble_gattc_disc_all_svcs_err;
static ble_gattc_err_fn ble_gattc_disc_svc_uuid_err;
static ble_gattc_err_fn ble_gattc_find_inc_svcs_err;
static ble_gattc_err_fn ble_gattc_disc_all_chrs_err;
static ble_gattc_err_fn ble_gattc_disc_chr_uuid_err;
static ble_gattc_err_fn ble_gattc_disc_all_dscs_err;
static ble_gattc_err_fn ble_gattc_read_err;
static ble_gattc_err_fn ble_gattc_read_uuid_err;
static ble_gattc_err_fn ble_gattc_read_long_err;
static ble_gattc_err_fn ble_gattc_read_mult_err;
static ble_gattc_err_fn ble_gattc_read_mult_var_err;
static ble_gattc_err_fn ble_gattc_write_err;
static ble_gattc_err_fn ble_gattc_write_long_err;
static ble_gattc_err_fn ble_gattc_write_reliable_err;
#endif

#if MYNEWT_VAL(BLE_GATTS)
static ble_gattc_err_fn ble_gatts_indicate_err;
#endif

static ble_gattc_err_fn * const ble_gattc_err_dispatch[BLE_GATT_OP_CNT] = {
#if MYNEWT_VAL(BLE_GATTC)
    [BLE_GATT_OP_MTU]               = ble_gattc_mtu_err,
    [BLE_GATT_OP_DISC_ALL_SVCS]     = ble_gattc_disc_all_svcs_err,
    [BLE_GATT_OP_DISC_SVC_UUID]     = ble_gattc_disc_svc_uuid_err,
    [BLE_GATT_OP_FIND_INC_SVCS]     = ble_gattc_find_inc_svcs_err,
    [BLE_GATT_OP_DISC_ALL_CHRS]     = ble_gattc_disc_all_chrs_err,
    [BLE_GATT_OP_DISC_CHR_UUID]     = ble_gattc_disc_chr_uuid_err,
    [BLE_GATT_OP_DISC_ALL_DSCS]     = ble_gattc_disc_all_dscs_err,
    [BLE_GATT_OP_READ]              = ble_gattc_read_err,
    [BLE_GATT_OP_READ_UUID]         = ble_gattc_read_uuid_err,
    [BLE_GATT_OP_READ_LONG]         = ble_gattc_read_long_err,
    [BLE_GATT_OP_READ_MULT]         = ble_gattc_read_mult_err,
    [BLE_GATT_OP_READ_MULT_VAR]     = ble_gattc_read_mult_var_err,
    [BLE_GATT_OP_WRITE]             = ble_gattc_write_err,
    [BLE_GATT_OP_WRITE_LONG]        = ble_gattc_write_long_err,
    [BLE_GATT_OP_WRITE_RELIABLE]    = ble_gattc_write_reliable_err,
#endif
#if MYNEWT_VAL(BLE_GATTS)
    [BLE_GATT_OP_INDICATE]          = ble_gatts_indicate_err,
#endif
};

#if MYNEWT_VAL(BLE_GATTC)
/**
 * Resume functions - these handle periodic retries of procedures that have
 * stalled due to memory exhaustion.
 */
typedef int ble_gattc_resume_fn(struct ble_gattc_proc *proc);

static ble_gattc_resume_fn ble_gattc_disc_all_svcs_resume;
static ble_gattc_resume_fn ble_gattc_disc_svc_uuid_resume;
static ble_gattc_resume_fn ble_gattc_find_inc_svcs_resume;
static ble_gattc_resume_fn ble_gattc_disc_all_chrs_resume;
static ble_gattc_resume_fn ble_gattc_disc_chr_uuid_resume;
static ble_gattc_resume_fn ble_gattc_disc_all_dscs_resume;
static ble_gattc_resume_fn ble_gattc_read_long_resume;
static ble_gattc_resume_fn ble_gattc_write_long_resume;
static ble_gattc_resume_fn ble_gattc_write_reliable_resume;

static ble_gattc_resume_fn * const
ble_gattc_resume_dispatch[BLE_GATT_OP_CNT] = {
    [BLE_GATT_OP_MTU]               = NULL,
    [BLE_GATT_OP_DISC_ALL_SVCS]     = ble_gattc_disc_all_svcs_resume,
    [BLE_GATT_OP_DISC_SVC_UUID]     = ble_gattc_disc_svc_uuid_resume,
    [BLE_GATT_OP_FIND_INC_SVCS]     = ble_gattc_find_inc_svcs_resume,
    [BLE_GATT_OP_DISC_ALL_CHRS]     = ble_gattc_disc_all_chrs_resume,
    [BLE_GATT_OP_DISC_CHR_UUID]     = ble_gattc_disc_chr_uuid_resume,
    [BLE_GATT_OP_DISC_ALL_DSCS]     = ble_gattc_disc_all_dscs_resume,
    [BLE_GATT_OP_READ]              = NULL,
    [BLE_GATT_OP_READ_UUID]         = NULL,
    [BLE_GATT_OP_READ_LONG]         = ble_gattc_read_long_resume,
    [BLE_GATT_OP_READ_MULT]         = NULL,
    [BLE_GATT_OP_READ_MULT_VAR]     = NULL,
    [BLE_GATT_OP_WRITE]             = NULL,
    [BLE_GATT_OP_WRITE_LONG]        = ble_gattc_write_long_resume,
    [BLE_GATT_OP_WRITE_RELIABLE]    = ble_gattc_write_reliable_resume,
    [BLE_GATT_OP_INDICATE]          = NULL,
};

/**
 * Timeout functions - these notify the application that a GATT procedure has
 * timed out while waiting for a response.
 */
typedef void ble_gattc_tmo_fn(struct ble_gattc_proc *proc);

static ble_gattc_tmo_fn ble_gattc_mtu_tmo;
static ble_gattc_tmo_fn ble_gattc_disc_all_svcs_tmo;
static ble_gattc_tmo_fn ble_gattc_disc_svc_uuid_tmo;
static ble_gattc_tmo_fn ble_gattc_find_inc_svcs_tmo;
static ble_gattc_tmo_fn ble_gattc_disc_all_chrs_tmo;
static ble_gattc_tmo_fn ble_gattc_disc_chr_uuid_tmo;
static ble_gattc_tmo_fn ble_gattc_disc_all_dscs_tmo;
static ble_gattc_tmo_fn ble_gattc_read_tmo;
static ble_gattc_tmo_fn ble_gattc_read_uuid_tmo;
static ble_gattc_tmo_fn ble_gattc_read_long_tmo;
static ble_gattc_tmo_fn ble_gattc_read_mult_tmo;
static ble_gattc_tmo_fn ble_gattc_read_mult_var_tmo;
static ble_gattc_tmo_fn ble_gattc_write_tmo;
static ble_gattc_tmo_fn ble_gattc_write_long_tmo;
static ble_gattc_tmo_fn ble_gattc_write_reliable_tmo;
static ble_gattc_tmo_fn ble_gatts_indicate_tmo;

static ble_gattc_tmo_fn * const
ble_gattc_tmo_dispatch[BLE_GATT_OP_CNT] = {
    [BLE_GATT_OP_MTU]               = ble_gattc_mtu_tmo,
    [BLE_GATT_OP_DISC_ALL_SVCS]     = ble_gattc_disc_all_svcs_tmo,
    [BLE_GATT_OP_DISC_SVC_UUID]     = ble_gattc_disc_svc_uuid_tmo,
    [BLE_GATT_OP_FIND_INC_SVCS]     = ble_gattc_find_inc_svcs_tmo,
    [BLE_GATT_OP_DISC_ALL_CHRS]     = ble_gattc_disc_all_chrs_tmo,
    [BLE_GATT_OP_DISC_CHR_UUID]     = ble_gattc_disc_chr_uuid_tmo,
    [BLE_GATT_OP_DISC_ALL_DSCS]     = ble_gattc_disc_all_dscs_tmo,
    [BLE_GATT_OP_READ]              = ble_gattc_read_tmo,
    [BLE_GATT_OP_READ_UUID]         = ble_gattc_read_uuid_tmo,
    [BLE_GATT_OP_READ_LONG]         = ble_gattc_read_long_tmo,
    [BLE_GATT_OP_READ_MULT]         = ble_gattc_read_mult_tmo,
    [BLE_GATT_OP_READ_MULT_VAR]     = ble_gattc_read_mult_var_tmo,
    [BLE_GATT_OP_WRITE]             = ble_gattc_write_tmo,
    [BLE_GATT_OP_WRITE_LONG]        = ble_gattc_write_long_tmo,
    [BLE_GATT_OP_WRITE_RELIABLE]    = ble_gattc_write_reliable_tmo,
    [BLE_GATT_OP_INDICATE]          = ble_gatts_indicate_tmo,
};

/**
 * Receive functions - these handle specific incoming responses and apply them
 * to the appropriate active GATT procedure.
 */
typedef int ble_gattc_rx_adata_fn(struct ble_gattc_proc *proc,
                                  struct ble_att_read_type_adata *adata);

typedef int ble_gattc_rx_prep_fn(struct ble_gattc_proc *proc, int status,
                                 uint16_t handle, uint16_t offset,
                                 struct os_mbuf **om);

typedef int ble_gattc_rx_attr_fn(struct ble_gattc_proc *proc, int status,
                                 struct os_mbuf **om);

typedef int ble_gattc_rx_complete_fn(struct ble_gattc_proc *proc, int status);
typedef int ble_gattc_rx_exec_fn(struct ble_gattc_proc *proc, int status);

static ble_gattc_rx_adata_fn ble_gattc_find_inc_svcs_rx_adata;
static ble_gattc_rx_complete_fn ble_gattc_find_inc_svcs_rx_complete;
static ble_gattc_rx_attr_fn ble_gattc_find_inc_svcs_rx_read_rsp;
static ble_gattc_rx_adata_fn ble_gattc_disc_all_chrs_rx_adata;
static ble_gattc_rx_complete_fn ble_gattc_disc_all_chrs_rx_complete;
static ble_gattc_rx_adata_fn ble_gattc_disc_chr_uuid_rx_adata;
static ble_gattc_rx_complete_fn ble_gattc_disc_chr_uuid_rx_complete;
static ble_gattc_rx_attr_fn ble_gattc_read_rx_read_rsp;
static ble_gattc_rx_attr_fn ble_gattc_read_long_rx_read_rsp;
static ble_gattc_rx_adata_fn ble_gattc_read_uuid_rx_adata;
static ble_gattc_rx_complete_fn ble_gattc_read_uuid_rx_complete;
static ble_gattc_rx_prep_fn ble_gattc_write_long_rx_prep;
static ble_gattc_rx_exec_fn ble_gattc_write_long_rx_exec;
static ble_gattc_rx_prep_fn ble_gattc_write_reliable_rx_prep;
static ble_gattc_rx_exec_fn ble_gattc_write_reliable_rx_exec;

static const struct ble_gattc_rx_adata_entry {
    uint8_t op;
    ble_gattc_rx_adata_fn *cb;
} ble_gattc_rx_read_type_elem_entries[] = {
    { BLE_GATT_OP_FIND_INC_SVCS,    ble_gattc_find_inc_svcs_rx_adata },
    { BLE_GATT_OP_DISC_ALL_CHRS,    ble_gattc_disc_all_chrs_rx_adata },
    { BLE_GATT_OP_DISC_CHR_UUID,    ble_gattc_disc_chr_uuid_rx_adata },
    { BLE_GATT_OP_READ_UUID,        ble_gattc_read_uuid_rx_adata },
};

static const struct ble_gattc_rx_complete_entry {
    uint8_t op;
    ble_gattc_rx_complete_fn *cb;
} ble_gattc_rx_read_type_complete_entries[] = {
    { BLE_GATT_OP_FIND_INC_SVCS,    ble_gattc_find_inc_svcs_rx_complete },
    { BLE_GATT_OP_DISC_ALL_CHRS,    ble_gattc_disc_all_chrs_rx_complete },
    { BLE_GATT_OP_DISC_CHR_UUID,    ble_gattc_disc_chr_uuid_rx_complete },
    { BLE_GATT_OP_READ_UUID,        ble_gattc_read_uuid_rx_complete },
};

static const struct ble_gattc_rx_attr_entry {
    uint8_t op;
    ble_gattc_rx_attr_fn *cb;
} ble_gattc_rx_read_rsp_entries[] = {
    { BLE_GATT_OP_READ,             ble_gattc_read_rx_read_rsp },
    { BLE_GATT_OP_READ_LONG,        ble_gattc_read_long_rx_read_rsp },
    { BLE_GATT_OP_FIND_INC_SVCS,    ble_gattc_find_inc_svcs_rx_read_rsp },
};

static const struct ble_gattc_rx_prep_entry {
    uint8_t op;
    ble_gattc_rx_prep_fn *cb;
} ble_gattc_rx_prep_entries[] = {
    { BLE_GATT_OP_WRITE_LONG,       ble_gattc_write_long_rx_prep },
    { BLE_GATT_OP_WRITE_RELIABLE,   ble_gattc_write_reliable_rx_prep },
};

static const struct ble_gattc_rx_exec_entry {
    uint8_t op;
    ble_gattc_rx_exec_fn *cb;
} ble_gattc_rx_exec_entries[] = {
    { BLE_GATT_OP_WRITE_LONG,       ble_gattc_write_long_rx_exec },
    { BLE_GATT_OP_WRITE_RELIABLE,   ble_gattc_write_reliable_rx_exec },
};

#endif
static os_membuf_t ble_gattc_proc_mem[
    OS_MEMPOOL_SIZE(MYNEWT_VAL(BLE_GATT_MAX_PROCS),
                    sizeof (struct ble_gattc_proc))
];

static struct os_mempool ble_gattc_proc_pool;

/* The list of active GATT client procedures. */
static struct ble_gattc_proc_list ble_gattc_procs;

#if MYNEWT_VAL(BLE_GATTC_AUTO_PAIR)
/** Retry procedure after encryption response. */
static struct ble_gattc_proc_list ble_gattc_cached_procs;
#endif

#if MYNEWT_VAL(BLE_GATTC)
/* The time when we should attempt to resume stalled procedures, in OS ticks.
 * A value of 0 indicates no stalled procedures.
 */
static ble_npl_time_t ble_gattc_resume_at;

/* Statistics. */
STATS_SECT_DECL(ble_gattc_stats) ble_gattc_stats;
STATS_NAME_START(ble_gattc_stats)
    STATS_NAME(ble_gattc_stats, mtu)
    STATS_NAME(ble_gattc_stats, mtu_fail)
    STATS_NAME(ble_gattc_stats, disc_all_svcs)
    STATS_NAME(ble_gattc_stats, disc_all_svcs_fail)
    STATS_NAME(ble_gattc_stats, disc_svc_uuid)
    STATS_NAME(ble_gattc_stats, disc_svc_uuid_fail)
    STATS_NAME(ble_gattc_stats, find_inc_svcs)
    STATS_NAME(ble_gattc_stats, find_inc_svcs_fail)
    STATS_NAME(ble_gattc_stats, disc_all_chrs)
    STATS_NAME(ble_gattc_stats, disc_all_chrs_fail)
    STATS_NAME(ble_gattc_stats, disc_chrs_uuid)
    STATS_NAME(ble_gattc_stats, disc_chrs_uuid_fail)
    STATS_NAME(ble_gattc_stats, disc_all_dscs)
    STATS_NAME(ble_gattc_stats, disc_all_dscs_fail)
    STATS_NAME(ble_gattc_stats, read)
    STATS_NAME(ble_gattc_stats, read_fail)
    STATS_NAME(ble_gattc_stats, read_uuid)
    STATS_NAME(ble_gattc_stats, read_uuid_fail)
    STATS_NAME(ble_gattc_stats, read_long)
    STATS_NAME(ble_gattc_stats, read_long_fail)
    STATS_NAME(ble_gattc_stats, read_mult)
    STATS_NAME(ble_gattc_stats, read_mult_fail)
    STATS_NAME(ble_gattc_stats, signed_write)
    STATS_NAME(ble_gattc_stats, signed_write_fail)
    STATS_NAME(ble_gattc_stats, write_no_rsp)
    STATS_NAME(ble_gattc_stats, write_no_rsp_fail)
    STATS_NAME(ble_gattc_stats, write)
    STATS_NAME(ble_gattc_stats, write_fail)
    STATS_NAME(ble_gattc_stats, write_long)
    STATS_NAME(ble_gattc_stats, write_long_fail)
    STATS_NAME(ble_gattc_stats, write_reliable)
    STATS_NAME(ble_gattc_stats, write_reliable_fail)
    STATS_NAME(ble_gattc_stats, notify)
    STATS_NAME(ble_gattc_stats, notify_fail)
    STATS_NAME(ble_gattc_stats, multi_notify)
    STATS_NAME(ble_gattc_stats, multi_notify_fail)
    STATS_NAME(ble_gattc_stats, indicate)
    STATS_NAME(ble_gattc_stats, indicate_fail)
    STATS_NAME(ble_gattc_stats, proc_timeout)
STATS_NAME_END(ble_gattc_stats)

/*****************************************************************************
 * $debug                                                                    *
 *****************************************************************************/
#endif

static void
ble_gattc_dbg_assert_proc_not_inserted(struct ble_gattc_proc *proc)
{
#if MYNEWT_VAL(BLE_HS_DEBUG)
    struct ble_gattc_proc *cur;

    ble_hs_lock();

    STAILQ_FOREACH(cur, &ble_gattc_procs, next) {
        BLE_HS_DBG_ASSERT(cur != proc);
    }

    ble_hs_unlock();
#endif
}


/*****************************************************************************
 * $log                                                                      *
 *****************************************************************************/

static void
ble_gattc_log_proc_init(const char *name)
{
    BLE_HS_LOG(INFO, "GATT procedure initiated: %s", name);
}

#if MYNEWT_VAL(BLE_GATTC)
static void
ble_gattc_log_uuid(const ble_uuid_t *uuid)
{
    char buf[BLE_UUID_STR_LEN];

    ble_uuid_to_str(uuid, buf);

    BLE_HS_LOG(INFO, "%s", buf);
}

static void
ble_gattc_log_disc_svc_uuid(struct ble_gattc_proc *proc)
{
    ble_gattc_log_proc_init("discover service by uuid; uuid=");
    ble_gattc_log_uuid(&proc->disc_svc_uuid.service_uuid.u);
    BLE_HS_LOG(INFO, "\n");
}

static void
ble_gattc_log_find_inc_svcs(struct ble_gattc_proc *proc)
{
    ble_gattc_log_proc_init("find included services; ");
    BLE_HS_LOG(INFO, "start_handle=%d end_handle=%d\n",
               proc->find_inc_svcs.prev_handle + 1,
               proc->find_inc_svcs.end_handle);
}

static void
ble_gattc_log_disc_all_chrs(struct ble_gattc_proc *proc)
{
    ble_gattc_log_proc_init("discover all characteristics; ");
    BLE_HS_LOG(INFO, "start_handle=%d end_handle=%d\n",
               proc->disc_all_chrs.prev_handle + 1,
               proc->disc_all_chrs.end_handle);
}

static void
ble_gattc_log_disc_chr_uuid(struct ble_gattc_proc *proc)
{
    ble_gattc_log_proc_init("discover characteristics by uuid; ");
    BLE_HS_LOG(INFO, "start_handle=%d end_handle=%d uuid=",
               proc->disc_chr_uuid.prev_handle + 1,
               proc->disc_chr_uuid.end_handle);
    ble_gattc_log_uuid(&proc->disc_chr_uuid.chr_uuid.u);
    BLE_HS_LOG(INFO, "\n");
}

static void
ble_gattc_log_disc_all_dscs(struct ble_gattc_proc *proc)
{
    ble_gattc_log_proc_init("discover all descriptors; ");
    BLE_HS_LOG(INFO, "chr_val_handle=%d end_handle=%d\n",
               proc->disc_all_dscs.chr_val_handle,
               proc->disc_all_dscs.end_handle);
}

static void
ble_gattc_log_read(uint16_t att_handle)
{
    ble_gattc_log_proc_init("read; ");
    BLE_HS_LOG(INFO, "att_handle=%d\n", att_handle);
}

static void
ble_gattc_log_read_uuid(uint16_t start_handle, uint16_t end_handle,
                        const ble_uuid_t *uuid)
{
    ble_gattc_log_proc_init("read by uuid; ");
    BLE_HS_LOG(INFO, "start_handle=%d end_handle=%d uuid=",
               start_handle, end_handle);
    ble_gattc_log_uuid(uuid);
    BLE_HS_LOG(INFO, "\n");
}

static void
ble_gattc_log_read_long(struct ble_gattc_proc *proc)
{
    ble_gattc_log_proc_init("read long; ");
    BLE_HS_LOG(INFO, "att_handle=%d\n", proc->read_long.handle);
}

static void
ble_gattc_log_read_mult(const uint16_t *handles, uint8_t num_handles, bool variable)
{
    int i;

    ble_gattc_log_proc_init("read multiple; ");
    BLE_HS_LOG(INFO, "att_handles=");
    for (i = 0; i < num_handles; i++) {
        BLE_HS_LOG(INFO, "%s%d", i != 0 ? "," : "", handles[i]);
    }
    BLE_HS_LOG(INFO, "\n");
}

static void
ble_gattc_log_write(uint16_t att_handle, uint16_t len, int expecting_rsp)
{
    const char *name;

    if (expecting_rsp) {
        name = "write; ";
    } else {
        name = "write no rsp; ";
    }

    ble_gattc_log_proc_init(name);
    BLE_HS_LOG(INFO, "att_handle=%d len=%d\n", att_handle, len);
}

static void
ble_gattc_log_signed_write(uint16_t att_handle, uint16_t len)
{
    ble_gattc_log_proc_init("signed write; ");
    BLE_HS_LOG(INFO, "att_handle=%d len=%d\n", att_handle, len);
}

static void
ble_gattc_log_write_long(struct ble_gattc_proc *proc)
{
    ble_gattc_log_proc_init("write long; ");
    BLE_HS_LOG(INFO, "att_handle=%d len=%d\n",
               proc->write_long.attr.handle,
               OS_MBUF_PKTLEN(proc->write_long.attr.om));
}

static void
ble_gattc_log_write_reliable(struct ble_gattc_proc *proc)
{
    int i;

    ble_gattc_log_proc_init("write reliable; ");
    BLE_HS_LOG(INFO, "att_handles=");
    for (i = 0; i < proc->write_reliable.num_attrs; i++) {
        BLE_HS_LOG(INFO, "%s%d", i != 0 ? "," : "",
                   proc->write_reliable.attrs[i].handle);
    }
    BLE_HS_LOG(INFO, "\n");
}
#endif

static void
ble_gattc_log_notify(uint16_t att_handle)
{
    ble_gattc_log_proc_init("notify; ");
    BLE_HS_LOG(INFO, "att_handle=%d\n", att_handle);
}


#if MYNEWT_VAL(BLE_GATTC)
static void
ble_gattc_log_multi_notify(struct ble_gatt_notif * tuples, uint16_t num)
{
    ble_gattc_log_proc_init("multi handle notify; ");
    for (int i = 0; i < num; i++) {
        BLE_HS_LOG(INFO, "att_handle=%d", tuples[i].handle);
    }
}

#endif
static void
ble_gattc_log_indicate(uint16_t att_handle)
{
    ble_gattc_log_proc_init("indicate; ");
    BLE_HS_LOG(INFO, "att_handle=%d\n", att_handle);
}

#if MYNEWT_VAL(BLE_GATTC)
/*****************************************************************************
 * $rx entry                                                                 *
 *****************************************************************************/

static const void *
ble_gattc_rx_entry_find(uint8_t op, const void *rx_entries, int num_entries)
{
    struct gen_entry {
        uint8_t op;
        void (*cb)(void);
    };

    const struct gen_entry *entries;
    int i;

    entries = rx_entries;
    for (i = 0; i < num_entries; i++) {
        if (entries[i].op == op) {
            return entries + i;
        }
    }

    return NULL;
}

/*****************************************************************************
 * $proc                                                                    *
 *****************************************************************************/
#endif
/**
 * Allocates a proc entry.
 *
 * @return                      An entry on success; null on failure.
 */
static struct ble_gattc_proc *
ble_gattc_proc_alloc(void)
{
    struct ble_gattc_proc *proc;

    proc = os_memblock_get(&ble_gattc_proc_pool);
    if (proc != NULL) {
        memset(proc, 0, sizeof *proc);
    }

    return proc;
}


static void
ble_gattc_proc_prepare(struct ble_gattc_proc *proc, uint16_t conn_handle, uint8_t op)
{
    proc->conn_handle = conn_handle;
    proc->op = op;
    proc->cid = ble_eatt_get_available_chan_cid(conn_handle, op);
}

/**
 * Frees the specified proc entry.  No-op if passed a null pointer.
 */
static void
ble_gattc_proc_free(struct ble_gattc_proc *proc)
{
    int rc;
    int i;

    if (proc != NULL) {
        ble_gattc_dbg_assert_proc_not_inserted(proc);

        switch (proc->op) {
#if MYNEWT_VAL(BLE_GATTC_AUTO_PAIR)
        case BLE_GATT_OP_WRITE:
            if (MYNEWT_VAL(BLE_GATT_WRITE)) {
                os_mbuf_free_chain(proc->write.om);
            }
            break;
#endif
        case BLE_GATT_OP_WRITE_LONG:
            if (MYNEWT_VAL(BLE_GATT_WRITE_LONG)) {
                os_mbuf_free_chain(proc->write_long.attr.om);
            }
            break;

        case BLE_GATT_OP_WRITE_RELIABLE:
            if (MYNEWT_VAL(BLE_GATT_WRITE_RELIABLE)) {
                for (i = 0; i < proc->write_reliable.num_attrs; i++) {
                    os_mbuf_free_chain(proc->write_reliable.attrs[i].om);
                }
            }
            break;

        default:
            break;
        }

#if MYNEWT_VAL(BLE_EATT_CHAN_NUM) > 0
        if (ble_hs_cfg.eatt && proc->cid != BLE_L2CAP_CID_ATT) {
            ble_eatt_release_chan(proc->conn_handle, proc->op);
        }
#endif

#if MYNEWT_VAL(BLE_HS_DEBUG)
        memset(proc, 0xff, sizeof *proc);
#endif
        rc = os_memblock_put(&ble_gattc_proc_pool, proc);
        BLE_HS_DBG_ASSERT_EVAL(rc == 0);
    }
}

static void
ble_gattc_proc_insert(struct ble_gattc_proc *proc)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_hs_lock();
    STAILQ_INSERT_TAIL(&ble_gattc_procs, proc, next);
    ble_hs_unlock();
}

static void
ble_gattc_proc_set_exp_timer(struct ble_gattc_proc *proc)
{
    proc->exp_os_ticks = ble_npl_time_get() +
                         ble_npl_time_ms_to_ticks32(BLE_GATTC_UNRESPONSIVE_TIMEOUT_MS);
}

#if MYNEWT_VAL(BLE_GATTC)
static void
ble_gattc_proc_set_resume_timer(struct ble_gattc_proc *proc)
{
    proc->flags |= BLE_GATTC_PROC_F_STALLED;

    /* Don't overwrite resume time if it is already set; piggyback on it
     * instead.
     */
    if (ble_gattc_resume_at == 0) {
        ble_gattc_resume_at = ble_npl_time_get() +
                              ble_npl_time_ms_to_ticks32(MYNEWT_VAL(BLE_GATT_RESUME_RATE));

        /* A value of 0 indicates the timer is unset.  Disambiguate this. */
        if (ble_gattc_resume_at == 0) {
            ble_gattc_resume_at++;
        }
    }
}

#endif

static void
ble_gattc_process_status(struct ble_gattc_proc *proc, int status)
{
    switch (status) {
    case 0:
        if (!(proc->flags & BLE_GATTC_PROC_F_STALLED)) {
            ble_gattc_proc_set_exp_timer(proc);
        }

        ble_gattc_proc_insert(proc);
        ble_hs_timer_resched();
        break;

    default:
        ble_gattc_proc_free(proc);
        break;
    }
}

#if MYNEWT_VAL(BLE_GATTC)
/**
 * Processes the return code that results from an attempt to resume a
 * procedure.  If the resume attempt failed due to memory exhaustion at a lower
 * layer, the procedure is marked as stalled but still in progress.  Otherwise,
 * the resume error code is unmodified.
 */
static int
ble_gattc_process_resume_status(struct ble_gattc_proc *proc, int status)
{
    switch (status) {
    case 0:
        return 0;

    case BLE_HS_ENOMEM:
        ble_gattc_proc_set_resume_timer(proc);
        return 0;

    default:
        return status;
    }
}

/*****************************************************************************
 * $util                                                                     *
 *****************************************************************************/
#endif

/**
 * Retrieves the error dispatch entry with the specified op code.
 */
static ble_gattc_err_fn *
ble_gattc_err_dispatch_get(uint8_t op)
{
    BLE_HS_DBG_ASSERT(op < BLE_GATT_OP_CNT);
    return ble_gattc_err_dispatch[op];
}

#if MYNEWT_VAL(BLE_GATTC)
/**
 * Retrieves the error dispatch entry with the specified op code.
 */
static ble_gattc_resume_fn *
ble_gattc_resume_dispatch_get(uint8_t op)
{
    BLE_HS_DBG_ASSERT(op < BLE_GATT_OP_CNT);
    return ble_gattc_resume_dispatch[op];
}

static ble_gattc_tmo_fn *
ble_gattc_tmo_dispatch_get(uint8_t op)
{
    BLE_HS_DBG_ASSERT(op < BLE_GATT_OP_CNT);
    return ble_gattc_tmo_dispatch[op];
}

#endif
typedef int ble_gattc_match_fn(struct ble_gattc_proc *proc, void *arg);

struct ble_gattc_criteria_conn_op {
    uint16_t conn_handle;
    uint16_t psm;
    uint8_t op;
};

/**
 * Tests if a proc entry fits the specified criteria.
 *
 * @param proc                  The procedure to test.
 * @param conn_handle           The connection handle to match against.
 * @param op                    The op code to match against, or
 *                                  BLE_GATT_OP_NONE to ignore this criterion.
 *
 * @return                      1 if the proc matches; 0 otherwise.
 */
static int
ble_gattc_proc_matches_conn_op(struct ble_gattc_proc *proc, void *arg)
{
    const struct ble_gattc_criteria_conn_op *criteria;

    criteria = arg;

    if (criteria->conn_handle != proc->conn_handle) {
        return 0;
    }

    if (criteria->op != proc->op && criteria->op != BLE_GATT_OP_NONE) {
        return 0;
    }

    return 1;
}

#if MYNEWT_VAL(BLE_GATTC)
static int
ble_gattc_proc_matches_conn_cid_op(struct ble_gattc_proc *proc, void *arg)
{
    const struct ble_gattc_criteria_conn_op *criteria;

    criteria = arg;

    if (criteria->conn_handle != proc->conn_handle) {
        return 0;
    }

    if (criteria->psm != proc->cid) {
        return 0;
    }

    if (criteria->op != proc->op && criteria->op != BLE_GATT_OP_NONE) {
        return 0;
    }

    return 1;
}

struct ble_gattc_criteria_exp {
    ble_npl_time_t now;
    int32_t next_exp_in;
};

static int
ble_gattc_proc_matches_expired(struct ble_gattc_proc *proc, void *arg)
{
    struct ble_gattc_criteria_exp *criteria;
    int32_t time_diff;

    criteria = arg;

    time_diff = proc->exp_os_ticks - criteria->now;

    if (time_diff <= 0) {
        /* Procedure is expired. */
        return 1;
    }

    /* Procedure isn't expired; determine if it is the next to expire. */
    if (time_diff < criteria->next_exp_in) {
        criteria->next_exp_in = time_diff;
    }
    return 0;
}

struct ble_gattc_criteria_conn_rx_entry {
    uint16_t conn_handle;
    uint16_t cid;
    const void *rx_entries;
    int num_rx_entries;
    const void *matching_rx_entry;
};

static int
ble_gattc_proc_matches_conn_rx_entry(struct ble_gattc_proc *proc, void *arg)
{
    struct ble_gattc_criteria_conn_rx_entry *criteria;

    criteria = arg;

    if (criteria->conn_handle != BLE_HS_CONN_HANDLE_NONE &&
        (criteria->conn_handle != proc->conn_handle ||
         criteria->cid != proc->cid)) {

        return 0;
    }

    /* Entry matches; indicate corresponding rx entry. */
    criteria->matching_rx_entry = ble_gattc_rx_entry_find(
        proc->op, criteria->rx_entries, criteria->num_rx_entries);

    return (criteria->matching_rx_entry != NULL);
}

#endif

static void
ble_gattc_extract(ble_gattc_match_fn *cb, void *arg, int max_procs,
                  struct ble_gattc_proc_list *dst_list)
{
    struct ble_gattc_proc *proc;
    struct ble_gattc_proc *prev;
    struct ble_gattc_proc *next;
#if MYNEWT_VAL(BLE_GATTC_PROC_PREEMPTION_PROTECT)
    struct ble_gattc_proc *cur;
    uint8_t flag = 0;
#endif
    int num_extracted;

    /* Only the parent task is allowed to remove entries from the list. */
    BLE_HS_DBG_ASSERT(ble_hs_is_parent_task());

    STAILQ_INIT(dst_list);
    num_extracted = 0;

    ble_hs_lock();

    prev = NULL;

#if MYNEWT_VAL(BLE_GATTC_PROC_PREEMPTION_PROTECT)
    proc = STAILQ_FIRST(&temp_proc_list);
    while (proc != NULL) {
        next = STAILQ_NEXT(proc, next);
        STAILQ_FOREACH(cur, &ble_gattc_procs, next) {
            if (proc == cur) {
                flag = 1;
                break;
            }
        }
        if (!flag) {
        /* Detected a preemption case */
            STAILQ_INSERT_TAIL(&ble_gattc_procs, proc, next);
        }
        flag = 0;
        proc = next;
    }
    /* Clear the temp proc list */
    STAILQ_INIT(&temp_proc_list);
#endif

    proc = STAILQ_FIRST(&ble_gattc_procs);
    while (proc != NULL) {
        next = STAILQ_NEXT(proc, next);

        if (cb(proc, arg)) {
            if (prev == NULL) {
                STAILQ_REMOVE_HEAD(&ble_gattc_procs, next);
            } else {
                STAILQ_REMOVE_AFTER(&ble_gattc_procs, prev, next);
            }
            STAILQ_INSERT_TAIL(dst_list, proc, next);

            if (max_procs > 0) {
                num_extracted++;
                if (num_extracted >= max_procs) {
                    break;
                }
            }
        } else {
            prev = proc;
        }

        proc = next;
    }

    ble_hs_unlock();
}

#if MYNEWT_VAL(BLE_GATTC)
static struct ble_gattc_proc *
ble_gattc_extract_one(ble_gattc_match_fn *cb, void *arg)
{
    struct ble_gattc_proc_list dst_list;

    ble_gattc_extract(cb, arg, 1, &dst_list);
    return STAILQ_FIRST(&dst_list);
}

#endif

static void
ble_gattc_extract_by_conn_op(uint16_t conn_handle, uint8_t op, int max_procs,
                             struct ble_gattc_proc_list *dst_list)
{
    struct ble_gattc_criteria_conn_op criteria;

    criteria.conn_handle = conn_handle;
    criteria.op = op;

    ble_gattc_extract(ble_gattc_proc_matches_conn_op, &criteria, max_procs, dst_list);
}


#if MYNEWT_VAL(BLE_GATTC)
static void
ble_gattc_extract_by_conn_cid_op(uint16_t conn_handle, uint16_t psm, uint8_t op,
                                 int max_procs,
                                 struct ble_gattc_proc_list *dst_list)
{
    struct ble_gattc_criteria_conn_op criteria;

    criteria.conn_handle = conn_handle;
    criteria.op = op;
    criteria.psm = psm;

    ble_gattc_extract(ble_gattc_proc_matches_conn_cid_op, &criteria, max_procs, dst_list);
}

static struct ble_gattc_proc *
ble_gattc_extract_first_by_conn_cid_op(uint16_t conn_handle, uint16_t cid, uint8_t op)
{
    struct ble_gattc_proc_list dst_list;

    ble_gattc_extract_by_conn_cid_op(conn_handle, cid, op, 1, &dst_list);
    return STAILQ_FIRST(&dst_list);
}

static int
ble_gattc_proc_matches_stalled(struct ble_gattc_proc *proc, void *unused)
{
    return proc->flags & BLE_GATTC_PROC_F_STALLED;
}

static void
ble_gattc_extract_stalled(struct ble_gattc_proc_list *dst_list)
{
    ble_gattc_extract(ble_gattc_proc_matches_stalled, NULL, 0, dst_list);
}

/**
 * @return                      The number of ticks until the next expiration
 *                                  occurs.
 */
static int32_t
ble_gattc_extract_expired(struct ble_gattc_proc_list *dst_list)
{
    struct ble_gattc_criteria_exp criteria;

    criteria.now = ble_npl_time_get();
    criteria.next_exp_in = BLE_HS_FOREVER;

    STAILQ_INIT(dst_list);
    ble_gattc_extract(ble_gattc_proc_matches_expired, &criteria, 0, dst_list);

    return criteria.next_exp_in;
}

static struct ble_gattc_proc *
ble_gattc_extract_with_rx_entry(uint16_t conn_handle, uint16_t cid,
                                const void *rx_entries, int num_rx_entries,
                                const void **out_rx_entry)
{
    struct ble_gattc_criteria_conn_rx_entry criteria;
    struct ble_gattc_proc *proc;

    criteria.conn_handle = conn_handle;
    criteria.cid = cid;
    criteria.rx_entries = rx_entries;
    criteria.num_rx_entries = num_rx_entries;
    criteria.matching_rx_entry = NULL;

    proc = ble_gattc_extract_one(ble_gattc_proc_matches_conn_rx_entry,
                                 &criteria);
    *out_rx_entry = criteria.matching_rx_entry;

    return proc;
}

/**
 * Searches the main proc list for an entry whose connection handle and op code
 * match those specified.  If a matching entry is found, it is removed from the
 * list and returned.
 *
 * @param conn_handle           The connection handle to match against.
 * @param cid                   Source CID of L2CAP channel used
 * @param rx_entries            The array of rx entries corresponding to the
 *                                  op code of the incoming response.
 * @param out_rx_entry          On success, the address of the matching rx
 *                                  entry is written to this pointer.
 *
 * @return                      The matching proc entry on success;
 *                                  null on failure.
 */
#define BLE_GATTC_RX_EXTRACT_RX_ENTRY(conn_handle, cid, rx_entries, out_rx_entry)  \
    ble_gattc_extract_with_rx_entry(                                          \
        (conn_handle), (cid), (rx_entries),                                   \
        sizeof (rx_entries) / sizeof (rx_entries)[0],                         \
        (const void **)(out_rx_entry))
#endif

/**
 * Causes all GATT procedures matching the specified criteria to fail with the
 * specified status code.
 */
static void
ble_gattc_fail_procs(uint16_t conn_handle, uint8_t op, int status)
{
    struct ble_gattc_proc_list temp_list;
    struct ble_gattc_proc *proc;
    ble_gattc_err_fn *err_cb;

    /* Remove all procs with the specified conn handle-op-pair and insert them
     * into the temporary list.
     */
    ble_gattc_extract_by_conn_op(conn_handle, op, 0, &temp_list);

    /* Notify application of failed procedures and free the corresponding proc
     * entries.
     */
    while ((proc = STAILQ_FIRST(&temp_list)) != NULL) {
        err_cb = ble_gattc_err_dispatch_get(proc->op);
        err_cb(proc, status, 0);

        STAILQ_REMOVE_HEAD(&temp_list, next);
        ble_gattc_proc_free(proc);
    }
}

#if MYNEWT_VAL(BLE_GATTC)
static void
ble_gattc_resume_procs(void)
{
    struct ble_gattc_proc_list stall_list;
    struct ble_gattc_proc *proc;
    ble_gattc_resume_fn *resume_cb;
    int rc;

    /* Cancel resume timer since it is being serviced. */
    ble_gattc_resume_at = 0;

    ble_gattc_extract_stalled(&stall_list);

    STAILQ_FOREACH(proc, &stall_list, next) {
        resume_cb = ble_gattc_resume_dispatch_get(proc->op);
        BLE_HS_DBG_ASSERT(resume_cb != NULL);

        proc->flags &= ~BLE_GATTC_PROC_F_STALLED;
        rc = resume_cb(proc);
        ble_gattc_process_status(proc, rc);
    }
}

static int32_t
ble_gattc_ticks_until_resume(void)
{
    ble_npl_time_t now;
    int32_t diff;

    /* Resume timer not set. */
    if (ble_gattc_resume_at == 0) {
        return BLE_HS_FOREVER;
    }

    now = ble_npl_time_get();
    diff = ble_gattc_resume_at - now;
    if (diff <= 0) {
        /* Timer already expired; resume immediately. */
        return 0;
    }

    return diff;
}

static void
ble_gattc_proc_timeout(struct ble_gattc_proc *proc)
{
    ble_gattc_tmo_fn *cb;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    cb = ble_gattc_tmo_dispatch_get(proc->op);
    if (cb != NULL) {
        cb(proc);
    }
}

/**
 * Times out expired GATT client procedures.
 *
 * @return                      The number of ticks until this function should
 *                                  be called again.
 */
int32_t
ble_gattc_timer(void)
{
    struct ble_gattc_proc_list exp_list;
    struct ble_gattc_proc *proc;
    int32_t ticks_until_resume;
    int32_t ticks_until_exp;

    /* Remove timed-out procedures from the main list and insert them into a
     * temporary list.  This function also calculates the number of ticks until
     * the next expiration will occur.
     */
    ticks_until_exp = ble_gattc_extract_expired(&exp_list);

    /* Terminate the connection associated with each timed-out procedure. */
    while ((proc = STAILQ_FIRST(&exp_list)) != NULL) {
        STATS_INC(ble_gattc_stats, proc_timeout);

        ble_gattc_proc_timeout(proc);

        ble_gap_terminate(proc->conn_handle, BLE_ERR_REM_USER_CONN_TERM);

        STAILQ_REMOVE_HEAD(&exp_list, next);
        ble_gattc_proc_free(proc);
    }

    /* If there are stalled procedures, the GATT client will need to wake up to
     * resume them.
     */
    ticks_until_resume = ble_gattc_ticks_until_resume();
    if (ticks_until_resume == 0) {
        ble_gattc_resume_procs();
        ticks_until_resume = ble_gattc_ticks_until_resume();
    }

    return min(ticks_until_exp, ticks_until_resume);
}

/**
 * Returns a pointer to a GATT error object with the specified fields.  The
 * returned object is statically allocated, so this function is not reentrant.
 * This function should only ever be called by the ble_hs task.
 */
static struct ble_gatt_error *
ble_gattc_error(int status, uint16_t att_handle)
{
    static struct ble_gatt_error error;

    /* For consistency, always indicate a handle of 0 on success. */
    if (status == 0 || status == BLE_HS_EDONE) {
        att_handle = 0;
    }

    error.status = status;
    error.att_handle = att_handle;
    return &error;
}

#if MYNEWT_VAL(BLE_GATTC_AUTO_PAIR)
void
ble_gattc_recover_gatt_proc(uint16_t conn_handle, int enc_status)
{
    struct ble_gattc_proc * prev;
    struct ble_gattc_proc * proc;
    struct ble_gattc_proc * next;
    struct os_mbuf * om;
    struct ble_gatt_attr attrs[MYNEWT_VAL(BLE_GATT_WRITE_MAX_ATTRS)];
    ble_gattc_err_fn *err_cb;

    prev = NULL;
    proc = STAILQ_FIRST(&ble_gattc_cached_procs);
    while (proc != NULL) {
        next = STAILQ_NEXT(proc, next);

        if (proc->conn_handle == conn_handle) {
            if (enc_status == 0) {
                switch (proc->op) {
                case BLE_GATT_OP_READ:
                    ble_gattc_read(conn_handle, proc->read.handle,
                                  proc->read.cb, proc->read.cb_arg);
                    break;
                case BLE_GATT_OP_READ_UUID:
                    ble_gattc_read_by_uuid(conn_handle, proc->read_uuid.start_handle,
                                        proc->read_uuid.end_handle, &proc->read_uuid.chr_uuid.u,
                                        proc->read_uuid.cb, proc->read_uuid.cb_arg);
                    break;
                case BLE_GATT_OP_READ_LONG:
                    ble_gattc_read_long(conn_handle, proc->read_long.handle,
                                        proc->read_long.offset, proc->read_long.cb,
                                        proc->read_long.cb_arg);
                    break;
                case BLE_GATT_OP_READ_MULT:
                    ble_gattc_read_mult(conn_handle, proc->read_mult.handles,
                                        proc->read_mult.num_handles, proc->read_mult.cb,
                                        proc->read_mult.cb_arg);
                    break;
                case BLE_GATT_OP_READ_MULT_VAR:
                    ble_gattc_read_mult_var(conn_handle, proc->read_mult.handles,
                                            proc->read_mult.num_handles, proc->read_mult.cb_mult,
                                            proc->read_mult.cb_arg);
                    break;
                case BLE_GATT_OP_WRITE:
                    /* Mbuf will be consumed by the API. */
                    om = os_mbuf_dup(proc->write.om);
                    ble_gattc_write(conn_handle, proc->write.att_handle,
                                    om, proc->write.cb, proc->write.cb_arg);
                    break;
                case BLE_GATT_OP_WRITE_LONG:
                    /* Mbuf will be consumed by the API. */
                    om = os_mbuf_dup(proc->write_long.attr.om);
                    ble_gattc_write_long(conn_handle, proc->write_long.attr.handle,
                                         proc->write_long.attr.offset, om,
                                         proc->write_long.cb, proc->write_long.cb_arg);
                case BLE_GATT_OP_WRITE_RELIABLE:
                    for (int i = 0; i < proc->write_reliable.num_attrs; i++) {
                        attrs[i].handle = proc->write_reliable.attrs[i].handle;
                        attrs[i].offset = 0;
                        attrs[i].om = os_mbuf_dup(proc->write_reliable.attrs[i].om);
                    }
                    ble_gattc_write_reliable(conn_handle, attrs,
                                             proc->write_reliable.num_attrs,
                                             proc->write_reliable.cb, proc->write_reliable.cb_arg);
                }
            } else {
                err_cb = ble_gattc_err_dispatch_get(proc->op);
                if (err_cb != NULL) {
                    err_cb(proc, BLE_HS_ERR_ATT_BASE + proc->error.status, proc->error.att_handle);
                }
            }

            if (!prev) {
                STAILQ_REMOVE_HEAD(&ble_gattc_cached_procs, next);
            } else {
                STAILQ_REMOVE_AFTER(&ble_gattc_cached_procs, prev, next);
            }

            ble_gattc_proc_free(proc);
            proc = prev;
        }

        prev = proc;
        proc = next;
    }
}
#endif /* MYNEWT_VAL(BLE_GATTC_AUTO_PAIR) */

/*****************************************************************************
 * $mtu                                                                      *
 *****************************************************************************/

/**
 * Calls an mtu-exchange proc's callback with the specified parameters.  If the
 * proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
static int
ble_gattc_mtu_cb(struct ble_gattc_proc *proc, int status, uint16_t att_handle,
                 uint16_t mtu)
{
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, mtu_fail);
    }

    if (proc->mtu.cb == NULL) {
        rc = 0;
    } else {
        rc = proc->mtu.cb(proc->conn_handle,
                          ble_gattc_error(status, att_handle),
                          mtu, proc->mtu.cb_arg);
    }

    return rc;
}

static void
ble_gattc_mtu_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_mtu_cb(proc, BLE_HS_ETIMEOUT, 0, 0);
}

/**
 * Handles an incoming ATT error response for the specified mtu-exchange proc.
 */
static void
ble_gattc_mtu_err(struct ble_gattc_proc *proc, int status, uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);
    ble_gattc_mtu_cb(proc, status, att_handle, 0);
}

static int
ble_gattc_mtu_tx(struct ble_gattc_proc *proc)
{
    struct ble_l2cap_chan *chan;
    struct ble_hs_conn *conn;
    uint16_t mtu;
    int rc;

    ble_hs_lock();
    rc = ble_att_conn_chan_find(proc->conn_handle, proc->cid, &conn, &chan);
    if (rc == 0) {
        mtu = chan->my_mtu;
    }
    ble_hs_unlock();

    if (rc == 0) {
        rc = ble_att_clt_tx_mtu(proc->conn_handle, mtu);
    }

    return rc;
}

int
ble_gattc_exchange_mtu(uint16_t conn_handle, ble_gatt_mtu_fn *cb, void *cb_arg)
{
    struct ble_gattc_proc *proc;
    int rc;

    STATS_INC(ble_gattc_stats, mtu);

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    proc->op = BLE_GATT_OP_MTU;
    proc->conn_handle = conn_handle;
    proc->cid = BLE_L2CAP_CID_ATT;
    proc->mtu.cb = cb;
    proc->mtu.cb_arg = cb_arg;

    ble_gattc_log_proc_init("exchange mtu\n");

    rc = ble_gattc_mtu_tx(proc);
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, mtu_fail);
    }

    ble_gattc_process_status(proc, rc);
    return rc;
}

/*****************************************************************************
 * $discover all services                                                    *
 *****************************************************************************/

/**
 * Calls a discover-all-services proc's callback with the specified parameters.
 * If the proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
static int
ble_gattc_disc_all_svcs_cb(struct ble_gattc_proc *proc,
                           uint16_t status, uint16_t att_handle,
                           struct ble_gatt_svc *service)
{
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    BLE_HS_DBG_ASSERT(service != NULL || status != 0);
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, disc_all_svcs_fail);
    }

    if (proc->disc_all_svcs.cb == NULL) {
        rc = 0;
    } else {
        rc = proc->disc_all_svcs.cb(proc->conn_handle,
                                    ble_gattc_error(status, att_handle),
                                    service, proc->disc_all_svcs.cb_arg);
    }

    return rc;
}

static void
ble_gattc_disc_all_svcs_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_disc_all_svcs_cb(proc, BLE_HS_ETIMEOUT, 0, 0);
}

/**
 * Triggers a pending transmit for the specified discover-all-services proc.
 */
static int
ble_gattc_disc_all_svcs_tx(struct ble_gattc_proc *proc)
{
    ble_uuid16_t uuid = BLE_UUID16_INIT(BLE_ATT_UUID_PRIMARY_SERVICE);
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    rc = ble_att_clt_tx_read_group_type(proc->conn_handle, proc->cid,
                                        proc->disc_all_svcs.prev_handle + 1,
                                        0xffff, &uuid.u);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_gattc_disc_all_svcs_resume(struct ble_gattc_proc *proc)
{
    int status;
    int rc;

    status = ble_gattc_disc_all_svcs_tx(proc);
    rc = ble_gattc_process_resume_status(proc, status);
    if (rc != 0) {
        ble_gattc_disc_all_svcs_cb(proc, rc, 0, NULL);
        return rc;
    }

    return 0;
}

/**
 * Handles an incoming ATT error response for the specified
 * discover-all-services proc.
 */
static void
ble_gattc_disc_all_svcs_err(struct ble_gattc_proc *proc, int status,
                            uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_FOUND)) {
        /* Discovery is complete. */
        status = BLE_HS_EDONE;
    }

    ble_gattc_disc_all_svcs_cb(proc, status, att_handle, NULL);
}

/**
 * Handles an incoming attribute data entry from a read-group-type response for
 * the specified discover-all-services proc.
 */
static int
ble_gattc_disc_all_svcs_rx_adata(struct ble_gattc_proc *proc,
                                 struct ble_att_read_group_type_adata *adata)
{
    struct ble_gatt_svc service;
    int cbrc;
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    switch (adata->value_len) {
    case 2:
    case 16:
        rc = ble_uuid_init_from_att_buf(&service.uuid, adata->value,
                                        adata->value_len);
        if (rc != 0) {
            rc = BLE_HS_EBADDATA;
            goto done;
        }
        break;

    default:
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    if (adata->end_group_handle <= proc->disc_all_svcs.prev_handle) {
        /* Peer sent services out of order; terminate procedure. */
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    proc->disc_all_svcs.prev_handle = adata->end_group_handle;

    service.start_handle = adata->att_handle;
    service.end_handle = adata->end_group_handle;

    rc = 0;

done:
    cbrc = ble_gattc_disc_all_svcs_cb(proc, rc, 0, &service);
    if (rc != 0 || cbrc != 0) {
        return BLE_HS_EDONE;
    } else {
        return 0;
    }
}

/**
 * Handles a notification that an incoming read-group-type response has been
 * fully processed.
 */
static int
ble_gattc_disc_all_svcs_rx_complete(struct ble_gattc_proc *proc, int status)
{
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0) {
        ble_gattc_disc_all_svcs_cb(proc, status, 0, NULL);
        return BLE_HS_EDONE;
    }

    if (proc->disc_all_svcs.prev_handle == 0xffff) {
        /* Service discovery complete. */
        ble_gattc_disc_all_svcs_cb(proc, BLE_HS_EDONE, 0, NULL);
        return BLE_HS_EDONE;
    }

    /* Send follow-up request. */
    rc = ble_gattc_disc_all_svcs_resume(proc);
    if (rc != 0) {
        return BLE_HS_EDONE;
    }

    return 0;
}

int
ble_gattc_disc_all_svcs(uint16_t conn_handle, ble_gatt_disc_svc_fn *cb,
                        void *cb_arg)
{
#if !MYNEWT_VAL(BLE_GATT_DISC_ALL_SVCS)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    STATS_INC(ble_gattc_stats, disc_all_svcs);

#if MYNEWT_VAL(BLE_GATT_CACHING)
    rc = ble_gattc_cache_conn_search_all_svcs(conn_handle, cb, cb_arg);
    if(rc == 0) {
        return rc;
    }
#endif

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle, BLE_GATT_OP_DISC_ALL_SVCS);

    proc->disc_all_svcs.prev_handle = 0x0000;
    proc->disc_all_svcs.cb = cb;
    proc->disc_all_svcs.cb_arg = cb_arg;

#if MYNEWT_VAL(BLE_GATTC_PROC_PREEMPTION_PROTECT)
    ble_hs_lock();
    STAILQ_INSERT_TAIL(&temp_proc_list, proc, next);
    ble_hs_unlock();
#endif
    ble_gattc_log_proc_init("discover all services\n");

    rc = ble_gattc_disc_all_svcs_tx(proc);
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, disc_all_svcs_fail);
#if MYNEWT_VAL(BLE_GATTC_PROC_PREEMPTION_PROTECT)
        ble_hs_lock();
        STAILQ_REMOVE(&temp_proc_list,proc,ble_gattc_proc, next);
        ble_hs_unlock();
#endif
    }

    ble_gattc_process_status(proc, rc);
    return rc;
}

/*****************************************************************************
 * $discover service by uuid                                                 *
 *****************************************************************************/

/**
 * Calls a discover-service-by-uuid proc's callback with the specified
 * parameters.  If the proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
static int
ble_gattc_disc_svc_uuid_cb(struct ble_gattc_proc *proc, int status,
                           uint16_t att_handle,
                           struct ble_gatt_svc *service)
{
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    BLE_HS_DBG_ASSERT(service != NULL || status != 0);
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, disc_svc_uuid_fail);
    }

    if (proc->disc_svc_uuid.cb == NULL) {
        rc = 0;
    } else {
        rc = proc->disc_svc_uuid.cb(proc->conn_handle,
                                    ble_gattc_error(status, att_handle),
                                    service, proc->disc_svc_uuid.cb_arg);
    }

    return rc;
}

static void
ble_gattc_disc_svc_uuid_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_disc_svc_uuid_cb(proc, BLE_HS_ETIMEOUT, 0, 0);
}

/**
 * Triggers a pending transmit for the specified discover-service-by-uuid proc.
 */
static int
ble_gattc_disc_svc_uuid_tx(struct ble_gattc_proc *proc)
{
    uint8_t val[16];
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_uuid_flat(&proc->disc_svc_uuid.service_uuid.u, val);
    rc = ble_att_clt_tx_find_type_value(proc->conn_handle, proc->cid,
                                        proc->disc_svc_uuid.prev_handle + 1,
                                        0xffff, BLE_ATT_UUID_PRIMARY_SERVICE,
                                        val,
                                        ble_uuid_length(&proc->disc_svc_uuid.service_uuid.u));
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_gattc_disc_svc_uuid_resume(struct ble_gattc_proc *proc)
{
    int status;
    int rc;

    status = ble_gattc_disc_svc_uuid_tx(proc);
    rc = ble_gattc_process_resume_status(proc, status);
    if (rc != 0) {
        ble_gattc_disc_svc_uuid_cb(proc, rc, 0, NULL);
        return rc;
    }

    return 0;
}

/**
 * Handles an incoming ATT error response for the specified
 * discover-service-by-uuid proc.
 */
static void
ble_gattc_disc_svc_uuid_err(struct ble_gattc_proc *proc, int status,
                            uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_FOUND)) {
        /* Discovery is complete. */
        status = BLE_HS_EDONE;
    }

    ble_gattc_disc_svc_uuid_cb(proc, status, att_handle, NULL);
}

/**
 * Handles an incoming "handles info" entry from a find-type-value response for
 * the specified discover-service-by-uuid proc.
 */
static int
ble_gattc_disc_svc_uuid_rx_hinfo(struct ble_gattc_proc *proc,
                                 struct ble_att_find_type_value_hinfo *hinfo)
{
    struct ble_gatt_svc service;
    int cbrc;
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (hinfo->group_end_handle <= proc->disc_svc_uuid.prev_handle) {
        /* Peer sent services out of order; terminate procedure. */
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    proc->disc_svc_uuid.prev_handle = hinfo->group_end_handle;

    service.start_handle = hinfo->attr_handle;
    service.end_handle = hinfo->group_end_handle;
    service.uuid = proc->disc_svc_uuid.service_uuid;

    rc = 0;

done:
    cbrc = ble_gattc_disc_svc_uuid_cb(proc, rc, 0, &service);
    if (rc != 0 || cbrc != 0) {
        return BLE_HS_EDONE;
    } else {
        return 0;
    }
}

/**
 * Handles a notification that a find-type-value response has been fully
 * processed for the specified discover-service-by-uuid proc.
 */
static int
ble_gattc_disc_svc_uuid_rx_complete(struct ble_gattc_proc *proc, int status)
{
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0) {
        ble_gattc_disc_svc_uuid_cb(proc, status, 0, NULL);
        return BLE_HS_EDONE;
    }

    if (proc->disc_svc_uuid.prev_handle == 0xffff) {
        /* Service discovery complete. */
        ble_gattc_disc_svc_uuid_cb(proc, BLE_HS_EDONE, 0, NULL);
        return BLE_HS_EDONE;
    }

    /* Send follow-up request. */
    rc = ble_gattc_disc_svc_uuid_resume(proc);
    if (rc != 0) {
        return BLE_HS_EDONE;
    }

    return 0;
}

int
ble_gattc_disc_svc_by_uuid(uint16_t conn_handle, const ble_uuid_t *uuid,
                           ble_gatt_disc_svc_fn *cb, void *cb_arg)
{
#if !MYNEWT_VAL(BLE_GATT_DISC_SVC_UUID)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    int rc;

#if MYNEWT_VAL(BLE_GATT_CACHING)
    rc = ble_gattc_cache_conn_search_svc_by_uuid(conn_handle, uuid, cb, cb_arg);
    if(rc == 0) {
        return rc;
    }
#endif
    STATS_INC(ble_gattc_stats, disc_svc_uuid);

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle, BLE_GATT_OP_DISC_SVC_UUID);

    ble_uuid_to_any(uuid, &proc->disc_svc_uuid.service_uuid);
    proc->disc_svc_uuid.prev_handle = 0x0000;
    proc->disc_svc_uuid.cb = cb;
    proc->disc_svc_uuid.cb_arg = cb_arg;

    ble_gattc_log_disc_svc_uuid(proc);

    rc = ble_gattc_disc_svc_uuid_tx(proc);
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, disc_svc_uuid_fail);
    }

    ble_gattc_process_status(proc, rc);
    return rc;
}

/*****************************************************************************
 * $find included svcs                                                       *
 *****************************************************************************/

/**
 * Calls a find-included-services proc's callback with the specified
 * parameters.  If the proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
#if (MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES))
static int
ble_gattc_find_inc_svcs_cb(struct ble_gattc_proc *proc, int status,
                           uint16_t att_handle,
                           struct ble_gatt_incl_svc *incl_svc)
#else
static int
ble_gattc_find_inc_svcs_cb(struct ble_gattc_proc *proc, int status,
                           uint16_t att_handle,
                           struct ble_gatt_svc *incl_svc)
#endif
{
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    BLE_HS_DBG_ASSERT(incl_svc != NULL || status != 0);
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, find_inc_svcs_fail);
    }

    if (proc->find_inc_svcs.cb == NULL) {
        rc = 0;
    } else {
        rc = proc->find_inc_svcs.cb(proc->conn_handle,
                                    ble_gattc_error(status, att_handle),
                                    incl_svc, proc->find_inc_svcs.cb_arg);
    }

    return rc;
}

static void
ble_gattc_find_inc_svcs_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_find_inc_svcs_cb(proc, BLE_HS_ETIMEOUT, 0, 0);
}

/**
 * Triggers a pending transmit for the specified find-included-services proc.
 */
static int
ble_gattc_find_inc_svcs_tx(struct ble_gattc_proc *proc)
{
    ble_uuid16_t uuid = BLE_UUID16_INIT(BLE_ATT_UUID_INCLUDE);
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (proc->find_inc_svcs.cur_start == 0) {
        /* Find the next included service. */
        rc = ble_att_clt_tx_read_type(proc->conn_handle, proc->cid,
                                      proc->find_inc_svcs.prev_handle + 1,
                                      proc->find_inc_svcs.end_handle, &uuid.u);
        if (rc != 0) {
            return rc;
        }
    } else {
        /* Read the UUID of the previously found service. */
        rc = ble_att_clt_tx_read(proc->conn_handle, proc->cid,
                                 proc->find_inc_svcs.cur_start);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

static int
ble_gattc_find_inc_svcs_resume(struct ble_gattc_proc *proc)
{
    int status;
    int rc;

    status = ble_gattc_find_inc_svcs_tx(proc);
    rc = ble_gattc_process_resume_status(proc, status);
    if (rc != 0) {
        ble_gattc_find_inc_svcs_cb(proc, rc, 0, NULL);
        return rc;
    }

    return 0;
}

/**
 * Handles an incoming ATT error response for the specified
 * find-included-services proc.
 */
static void
ble_gattc_find_inc_svcs_err(struct ble_gattc_proc *proc, int status,
                            uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (proc->find_inc_svcs.cur_start == 0 &&
        status == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_FOUND)) {

        /* Discovery is complete. */
        status = BLE_HS_EDONE;
    }

    ble_gattc_find_inc_svcs_cb(proc, status, att_handle, NULL);
}

/**
 * Handles an incoming read-response for the specified find-included-services
 * proc.
 */
static int
ble_gattc_find_inc_svcs_rx_read_rsp(struct ble_gattc_proc *proc, int status,
                                    struct os_mbuf **om)
{
#if (MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES))
    struct ble_gatt_incl_svc service;
#else
    struct ble_gatt_svc service;
#endif
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    rc = ble_uuid_init_from_att_mbuf(&service.uuid, *om, 0, 16);
    os_mbuf_free_chain(*om);
    *om = NULL;

    if (rc != 0) {
        /* Invalid UUID. */
        rc = BLE_HS_EBADDATA;
        goto err;
    }

    if (proc->find_inc_svcs.cur_start == 0) {
        /* Unexpected read response; terminate procedure. */
        rc = BLE_HS_EBADDATA;
        goto err;
    }

    if (status != 0) {
        rc = status;
        goto err;
    }

    /* Report discovered service to application. */
    service.start_handle = proc->find_inc_svcs.cur_start;
    service.end_handle = proc->find_inc_svcs.cur_end;

    rc = ble_gattc_find_inc_svcs_cb(proc, 0, 0, &service);
    if (rc != 0) {
        /* Application has indicated that the procedure should be aborted. */
        return BLE_HS_EDONE;
    }

    /* Proceed to the next service. */
    proc->find_inc_svcs.cur_start = 0;
    proc->find_inc_svcs.cur_end = 0;
    rc = ble_gattc_find_inc_svcs_resume(proc);
    if (rc != 0) {
        goto err;
    }

    return 0;

err:
    ble_gattc_find_inc_svcs_cb(proc, rc, 0, NULL);
    return BLE_HS_EDONE;
}

/**
 * Handles an incoming "attribute data" entry from a read-by-type response for
 * the specified find-included-services proc.
 */
static int
ble_gattc_find_inc_svcs_rx_adata(struct ble_gattc_proc *proc,
                                 struct ble_att_read_type_adata *adata)
{
#if (MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES))
    struct ble_gatt_incl_svc service = {0};
#else
    struct ble_gatt_svc service = {0};
#endif
    int call_cb;
    int cbrc;
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (proc->find_inc_svcs.cur_start != 0) {
        /* We only read one 128-bit UUID service at a time.  Ignore the
         * additional services in the response.
         */
        return 0;
    }

    call_cb = 1;

    if (adata->att_handle <= proc->find_inc_svcs.prev_handle) {
        /* Peer sent services out of order; terminate procedure. */
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    proc->find_inc_svcs.prev_handle = adata->att_handle;

    rc = 0;

    switch (adata->value_len) {
    case BLE_GATTS_INC_SVC_LEN_NO_UUID:
        proc->find_inc_svcs.cur_start = get_le16(adata->value + 0);
        proc->find_inc_svcs.cur_end = get_le16(adata->value + 2);
        call_cb = 0;
        break;

    case BLE_GATTS_INC_SVC_LEN_UUID:
#if (MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES))
        service.handle = adata->att_handle;
#endif
        service.start_handle = get_le16(adata->value + 0);
        service.end_handle = get_le16(adata->value + 2);
        rc = ble_uuid_init_from_att_buf(&service.uuid, adata->value + 4, 2);
        if (rc != 0) {
            rc = BLE_HS_EBADDATA;
        }
        break;

    default:
        rc = BLE_HS_EBADDATA;
        break;
    }

done:
    if (call_cb) {
        cbrc = ble_gattc_find_inc_svcs_cb(proc, 0, 0, &service);
        if (rc != 0) {
            rc = cbrc;
        }
    } else {
        cbrc = 0;
    }

    if (rc != 0 || cbrc != 0) {
        return BLE_HS_EDONE;
    } else {
        return 0;
    }
}

/**
 * Handles a notification that a read-by-type response has been fully
 * processed for the specified find-included-services proc.
 */
static int
ble_gattc_find_inc_svcs_rx_complete(struct ble_gattc_proc *proc, int status)
{
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0) {
        ble_gattc_find_inc_svcs_cb(proc, status, 0, NULL);
        return BLE_HS_EDONE;
    }

    if (proc->find_inc_svcs.prev_handle == 0xffff) {
        /* Procedure complete. */
        ble_gattc_find_inc_svcs_cb(proc, BLE_HS_EDONE, 0, NULL);
        return BLE_HS_EDONE;
    }

    /* Send follow-up request. */
    rc = ble_gattc_find_inc_svcs_resume(proc);
    if (rc != 0) {
        return BLE_HS_EDONE;
    }
    return 0;
}

#if (MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES))
int
ble_gattc_find_inc_svcs(uint16_t conn_handle, uint16_t start_handle,
                        uint16_t end_handle,
                        ble_gatt_disc_incl_svc_fn *cb, void *cb_arg)
#else
int
ble_gattc_find_inc_svcs(uint16_t conn_handle, uint16_t start_handle,
                        uint16_t end_handle,
                        ble_gatt_disc_svc_fn *cb, void *cb_arg)
#endif
{
#if !MYNEWT_VAL(BLE_GATT_FIND_INC_SVCS)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    int rc;

#if MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
    rc = ble_gattc_cache_conn_search_inc_svcs(conn_handle, start_handle, end_handle, cb, cb_arg);
    if (rc == 0) {
        return rc;
    }
#endif
    STATS_INC(ble_gattc_stats, find_inc_svcs);

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle, BLE_GATT_OP_FIND_INC_SVCS);

    proc->find_inc_svcs.prev_handle = start_handle - 1;
    proc->find_inc_svcs.end_handle = end_handle;
    proc->find_inc_svcs.cb = cb;
    proc->find_inc_svcs.cb_arg = cb_arg;

    ble_gattc_log_find_inc_svcs(proc);

    rc = ble_gattc_find_inc_svcs_tx(proc);
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, find_inc_svcs_fail);
    }

    ble_gattc_process_status(proc, rc);
    return rc;
}

/*****************************************************************************
 * $discover all characteristics                                             *
 *****************************************************************************/

/**
 * Calls a discover-all-characteristics proc's callback with the specified
 * parameters.  If the proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
static int
ble_gattc_disc_all_chrs_cb(struct ble_gattc_proc *proc, int status,
                           uint16_t att_handle, struct ble_gatt_chr *chr)
{
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    BLE_HS_DBG_ASSERT(chr != NULL || status != 0);
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, disc_all_chrs_fail);
    }

    if (proc->disc_all_chrs.cb == NULL) {
        rc = 0;
    } else {
        rc = proc->disc_all_chrs.cb(proc->conn_handle,
                                    ble_gattc_error(status, att_handle), chr,
                                    proc->disc_all_chrs.cb_arg);
    }

    return rc;
}

static void
ble_gattc_disc_all_chrs_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_disc_all_chrs_cb(proc, BLE_HS_ETIMEOUT, 0, NULL);
}

/**
 * Triggers a pending transmit for the specified discover-all-characteristics
 * proc.
 */
static int
ble_gattc_disc_all_chrs_tx(struct ble_gattc_proc *proc)
{
    ble_uuid16_t uuid = BLE_UUID16_INIT(BLE_ATT_UUID_CHARACTERISTIC);
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    rc = ble_att_clt_tx_read_type(proc->conn_handle, proc->cid,
                                  proc->disc_all_chrs.prev_handle + 1,
                                  proc->disc_all_chrs.end_handle, &uuid.u);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_gattc_disc_all_chrs_resume(struct ble_gattc_proc *proc)
{
    int status;
    int rc;

    status = ble_gattc_disc_all_chrs_tx(proc);
    rc = ble_gattc_process_resume_status(proc, status);
    if (rc != 0) {
        ble_gattc_disc_all_chrs_cb(proc, rc, 0, NULL);
        return rc;
    }

    return 0;
}

/**
 * Handles an incoming ATT error response for the specified
 * discover-all-characteristics proc.
 */
static void
ble_gattc_disc_all_chrs_err(struct ble_gattc_proc *proc, int status,
                            uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_FOUND)) {
        /* Discovery is complete. */
        status = BLE_HS_EDONE;
    }

    ble_gattc_disc_all_chrs_cb(proc, status, att_handle, NULL);
}

/**
 * Handles an incoming "attribute data" entry from a read-by-type response for
 * the specified discover-all-characteristics proc.
 */
static int
ble_gattc_disc_all_chrs_rx_adata(struct ble_gattc_proc *proc,
                                 struct ble_att_read_type_adata *adata)
{
    struct ble_gatt_chr chr;
    int cbrc;
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    memset(&chr, 0, sizeof chr);
    chr.def_handle = adata->att_handle;

    switch (adata->value_len) {
    case BLE_GATT_CHR_DECL_SZ_16:
    case BLE_GATT_CHR_DECL_SZ_128:
        rc = ble_uuid_init_from_att_buf(&chr.uuid, adata->value + 3,
                                        adata->value_len - 3);
        if (rc != 0) {
            rc = BLE_HS_EBADDATA;
            goto done;
        }
        break;

    default:
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    chr.properties = adata->value[0];
    chr.val_handle = get_le16(adata->value + 1);

    if (adata->att_handle <= proc->disc_all_chrs.prev_handle) {
        /* Peer sent characteristics out of order; terminate procedure. */
        rc = BLE_HS_EBADDATA;
        goto done;
    }
    proc->disc_all_chrs.prev_handle = adata->att_handle;

    rc = 0;

done:
    cbrc = ble_gattc_disc_all_chrs_cb(proc, rc, 0, &chr);
    if (rc != 0 || cbrc != 0) {
        return BLE_HS_EDONE;
    } else {
        return 0;
    }
}

/**
 * Handles a notification that a read-by-type response has been fully
 * processed for the specified discover-all-characteristics proc.
 */
static int
ble_gattc_disc_all_chrs_rx_complete(struct ble_gattc_proc *proc, int status)
{
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0) {
        ble_gattc_disc_all_chrs_cb(proc, status, 0, NULL);
        return BLE_HS_EDONE;
    }

    if (proc->disc_all_chrs.prev_handle == proc->disc_all_chrs.end_handle) {
        /* Characteristic discovery complete. */
        ble_gattc_disc_all_chrs_cb(proc, BLE_HS_EDONE, 0, NULL);
        return BLE_HS_EDONE;
    }

    /* Send follow-up request. */
    rc = ble_gattc_disc_all_chrs_resume(proc);
    if (rc != 0) {
        return BLE_HS_EDONE;
    }
    return 0;
}

int
ble_gattc_disc_all_chrs(uint16_t conn_handle, uint16_t start_handle,
                        uint16_t end_handle, ble_gatt_chr_fn *cb,
                        void *cb_arg)
{
#if !MYNEWT_VAL(BLE_GATT_DISC_ALL_CHRS)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    int rc;

#if MYNEWT_VAL(BLE_GATT_CACHING)
    rc = ble_gattc_cache_conn_search_all_chrs(conn_handle, start_handle, end_handle, cb, cb_arg);
    if(rc == 0) {
        return rc;
    }
#endif
    STATS_INC(ble_gattc_stats, disc_all_chrs);

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle, BLE_GATT_OP_DISC_ALL_CHRS);

    proc->disc_all_chrs.prev_handle = start_handle - 1;
    proc->disc_all_chrs.end_handle = end_handle;
    proc->disc_all_chrs.cb = cb;
    proc->disc_all_chrs.cb_arg = cb_arg;

#if MYNEWT_VAL(BLE_GATTC_PROC_PREEMPTION_PROTECT)
    ble_hs_lock();
    STAILQ_INSERT_TAIL(&temp_proc_list, proc, next);
    ble_hs_unlock();
#endif
    ble_gattc_log_disc_all_chrs(proc);

    rc = ble_gattc_disc_all_chrs_tx(proc);
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, disc_all_chrs_fail);
#if MYNEWT_VAL(BLE_GATTC_PROC_PREEMPTION_PROTECT)
        ble_hs_lock();
        STAILQ_REMOVE(&temp_proc_list,proc,ble_gattc_proc, next);
        ble_hs_unlock();
#endif
    }

    ble_gattc_process_status(proc, rc);
    return rc;
}

/*****************************************************************************
 * $discover characteristic by uuid                                          *
 *****************************************************************************/

/**
 * Calls a discover-characteristic-by-uuid proc's callback with the specified
 * parameters.  If the proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
static int
ble_gattc_disc_chr_uuid_cb(struct ble_gattc_proc *proc, int status,
                           uint16_t att_handle, struct ble_gatt_chr *chr)
{
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    BLE_HS_DBG_ASSERT(chr != NULL || status != 0);
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, disc_chrs_uuid_fail);
    }

    if (proc->disc_chr_uuid.cb == NULL) {
        rc = 0;
    } else {
        rc = proc->disc_chr_uuid.cb(proc->conn_handle,
                                    ble_gattc_error(status, att_handle), chr,
                                    proc->disc_chr_uuid.cb_arg);
    }

    return rc;
}

static void
ble_gattc_disc_chr_uuid_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_disc_chr_uuid_cb(proc, BLE_HS_ETIMEOUT, 0, NULL);
}

/**
 * Triggers a pending transmit for the specified
 * discover-characteristic-by-uuid proc.
 */
static int
ble_gattc_disc_chr_uuid_tx(struct ble_gattc_proc *proc)
{
    ble_uuid16_t uuid = BLE_UUID16_INIT(BLE_ATT_UUID_CHARACTERISTIC);
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    rc = ble_att_clt_tx_read_type(proc->conn_handle, proc->cid,
                                  proc->disc_chr_uuid.prev_handle + 1,
                                  proc->disc_chr_uuid.end_handle, &uuid.u);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_gattc_disc_chr_uuid_resume(struct ble_gattc_proc *proc)
{
    int status;
    int rc;

    status = ble_gattc_disc_chr_uuid_tx(proc);
    rc = ble_gattc_process_resume_status(proc, status);
    if (rc != 0) {
        ble_gattc_disc_chr_uuid_cb(proc, rc, 0, NULL);
        return rc;
    }

    return 0;
}

/**
 * Handles an incoming ATT error response for the specified
 * discover-characteristic-by-uuid proc.
 */
static void
ble_gattc_disc_chr_uuid_err(struct ble_gattc_proc *proc, int status,
                            uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_FOUND)) {
        /* Discovery is complete. */
        status = BLE_HS_EDONE;
    }

    ble_gattc_disc_chr_uuid_cb(proc, status, att_handle, NULL);
}

/**
 * Handles an incoming "attribute data" entry from a read-by-type response for
 * the specified discover-characteristics-by-uuid proc.
 */
static int
ble_gattc_disc_chr_uuid_rx_adata(struct ble_gattc_proc *proc,
                                 struct ble_att_read_type_adata *adata)
{
    struct ble_gatt_chr chr;
    int cbrc;
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    memset(&chr, 0, sizeof chr);
    chr.def_handle = adata->att_handle;

    switch (adata->value_len) {
    case BLE_GATT_CHR_DECL_SZ_16:
    case BLE_GATT_CHR_DECL_SZ_128:
        rc = ble_uuid_init_from_att_buf(&chr.uuid, adata->value + 3,
                                        adata->value_len - 3);
        if (rc != 0) {
            rc = BLE_HS_EBADDATA;
            goto done;
        }
        break;

    default:
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    chr.properties = adata->value[0];
    chr.val_handle = get_le16(adata->value + 1);

    if (adata->att_handle <= proc->disc_chr_uuid.prev_handle) {
        /* Peer sent characteristics out of order; terminate procedure. */
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    proc->disc_chr_uuid.prev_handle = adata->att_handle;

    rc = 0;

done:
    if (rc != 0) {
        /* Failure. */
        cbrc = ble_gattc_disc_chr_uuid_cb(proc, rc, 0, NULL);
    } else if (ble_uuid_cmp(&chr.uuid.u, &proc->disc_chr_uuid.chr_uuid.u) == 0) {
        /* Requested characteristic discovered. */
        cbrc = ble_gattc_disc_chr_uuid_cb(proc, 0, 0, &chr);
    } else {
        /* Uninteresting characteristic; ignore. */
        cbrc = 0;
    }

    if (rc != 0 || cbrc != 0) {
        return BLE_HS_EDONE;
    } else {
        return 0;
    }
}

/**
 * Handles a notification that a read-by-type response has been fully
 * processed for the specified discover-characteristics-by-uuid proc.
 */
static int
ble_gattc_disc_chr_uuid_rx_complete(struct ble_gattc_proc *proc, int status)
{
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0) {
        ble_gattc_disc_chr_uuid_cb(proc, status, 0, NULL);
        return BLE_HS_EDONE;
    }

    if (proc->disc_chr_uuid.prev_handle == proc->disc_chr_uuid.end_handle) {
        /* Characteristic discovery complete. */
        ble_gattc_disc_chr_uuid_cb(proc, BLE_HS_EDONE, 0, NULL);
        return BLE_HS_EDONE;
    }

    /* Send follow-up request. */
    rc = ble_gattc_disc_chr_uuid_resume(proc);
    if (rc != 0) {
        return BLE_HS_EDONE;
    }
    return 0;
}

int
ble_gattc_disc_chrs_by_uuid(uint16_t conn_handle, uint16_t start_handle,
                            uint16_t end_handle, const ble_uuid_t *uuid,
                            ble_gatt_chr_fn *cb, void *cb_arg)
{
#if !MYNEWT_VAL(BLE_GATT_DISC_CHR_UUID)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    int rc;

#if MYNEWT_VAL(BLE_GATT_CACHING)
    rc = ble_gattc_cache_conn_search_chrs_by_uuid(conn_handle, start_handle, end_handle, uuid, cb, cb_arg);
    if(rc == 0) {
        return rc;
    }
#endif
    STATS_INC(ble_gattc_stats, disc_chrs_uuid);

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle, BLE_GATT_OP_DISC_CHR_UUID);

    ble_uuid_to_any(uuid, &proc->disc_chr_uuid.chr_uuid);
    proc->disc_chr_uuid.prev_handle = start_handle - 1;
    proc->disc_chr_uuid.end_handle = end_handle;
    proc->disc_chr_uuid.cb = cb;
    proc->disc_chr_uuid.cb_arg = cb_arg;

    ble_gattc_log_disc_chr_uuid(proc);

    rc = ble_gattc_disc_chr_uuid_tx(proc);
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, disc_chrs_uuid_fail);
    }

    ble_gattc_process_status(proc, rc);
    return rc;
}

/*****************************************************************************
 * $discover all characteristic descriptors                                  *
 *****************************************************************************/

/**
 * Calls a discover-all-descriptors proc's callback with the specified
 * parameters.  If the proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
static int
ble_gattc_disc_all_dscs_cb(struct ble_gattc_proc *proc, int status,
                           uint16_t att_handle, struct ble_gatt_dsc *dsc)
{
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    BLE_HS_DBG_ASSERT(dsc != NULL || status != 0);
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, disc_all_dscs_fail);
    }

    if (proc->disc_all_dscs.cb == NULL) {
        rc = 0;
    } else {
        rc = proc->disc_all_dscs.cb(proc->conn_handle,
                                    ble_gattc_error(status, att_handle),
                                    proc->disc_all_dscs.chr_val_handle,
                                    dsc, proc->disc_all_dscs.cb_arg);
    }

    return rc;
}

static void
ble_gattc_disc_all_dscs_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_disc_all_dscs_cb(proc, BLE_HS_ETIMEOUT, 0, NULL);
}

/**
 * Triggers a pending transmit for the specified discover-all-descriptors proc.
 */
static int
ble_gattc_disc_all_dscs_tx(struct ble_gattc_proc *proc)
{
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    rc = ble_att_clt_tx_find_info(proc->conn_handle, proc->cid,
                                  proc->disc_all_dscs.prev_handle + 1,
                                  proc->disc_all_dscs.end_handle);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_gattc_disc_all_dscs_resume(struct ble_gattc_proc *proc)
{
    int status;
    int rc;

    status = ble_gattc_disc_all_dscs_tx(proc);
    rc = ble_gattc_process_resume_status(proc, status);
    if (rc != 0) {
        ble_gattc_disc_all_dscs_cb(proc, rc, 0, NULL);
        return rc;
    }

    return 0;
}

/**
 * Handles an incoming ATT error response for the specified
 * discover-all-descriptors proc.
 */
static void
ble_gattc_disc_all_dscs_err(struct ble_gattc_proc *proc, int status,
                            uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_FOUND)) {
        /* Discovery is complete. */
        status = BLE_HS_EDONE;
    }

    ble_gattc_disc_all_dscs_cb(proc, status, att_handle, NULL);
}

/**
 * Handles an incoming "information data" entry from a find-information
 * response for the specified discover-all-descriptors proc.
 */
static int
ble_gattc_disc_all_dscs_rx_idata(struct ble_gattc_proc *proc,
                                 struct ble_att_find_info_idata *idata)
{
    struct ble_gatt_dsc dsc;
    int cbrc;
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (idata->attr_handle <= proc->disc_all_dscs.prev_handle) {
        /* Peer sent descriptors out of order; terminate procedure. */
        rc = BLE_HS_EBADDATA;
        goto done;
    }
    proc->disc_all_dscs.prev_handle = idata->attr_handle;

    rc = 0;

done:
    dsc.handle = idata->attr_handle;
    dsc.uuid = idata->uuid;

    cbrc = ble_gattc_disc_all_dscs_cb(proc, rc, 0, &dsc);
    if (rc != 0 || cbrc != 0) {
        return BLE_HS_EDONE;
    } else {
        return 0;
    }
}

/**
 * Handles a notification that a find-information response has been fully
 * processed for the specified discover-all-descriptors proc.
 */
static int
ble_gattc_disc_all_dscs_rx_complete(struct ble_gattc_proc *proc, int status)
{
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0) {
        ble_gattc_disc_all_dscs_cb(proc, status, 0, NULL);
        return BLE_HS_EDONE;
    }

    if (proc->disc_all_dscs.prev_handle == proc->disc_all_dscs.end_handle) {
        /* All descriptors discovered. */
        ble_gattc_disc_all_dscs_cb(proc, BLE_HS_EDONE, 0, NULL);
        return BLE_HS_EDONE;
    }

    /* Send follow-up request. */
    rc = ble_gattc_disc_all_dscs_resume(proc);
    if (rc != 0) {
        return BLE_HS_EDONE;
    }

    return 0;
}

int
ble_gattc_disc_all_dscs(uint16_t conn_handle, uint16_t start_handle,
                        uint16_t end_handle,
                        ble_gatt_dsc_fn *cb, void *cb_arg)
{
#if !MYNEWT_VAL(BLE_GATT_DISC_ALL_DSCS)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    int rc;

#if MYNEWT_VAL(BLE_GATT_CACHING)
    rc = ble_gattc_cache_conn_search_all_dscs(conn_handle, start_handle, end_handle, cb, cb_arg);
    if(rc == 0) {
        return rc;
    }
#endif
    STATS_INC(ble_gattc_stats, disc_all_dscs);

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle, BLE_GATT_OP_DISC_ALL_DSCS);

    proc->disc_all_dscs.chr_val_handle = start_handle;
    proc->disc_all_dscs.prev_handle = start_handle;
    proc->disc_all_dscs.end_handle = end_handle;
    proc->disc_all_dscs.cb = cb;
    proc->disc_all_dscs.cb_arg = cb_arg;

#if MYNEWT_VAL(BLE_GATTC_PROC_PREEMPTION_PROTECT)
    ble_hs_lock();
    STAILQ_INSERT_TAIL(&temp_proc_list, proc, next);
    ble_hs_unlock();
#endif
    ble_gattc_log_disc_all_dscs(proc);

    rc = ble_gattc_disc_all_dscs_tx(proc);
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, disc_all_dscs_fail);
#if MYNEWT_VAL(BLE_GATTC_PROC_PREEMPTION_PROTECT)
        ble_hs_lock();
        STAILQ_REMOVE(&temp_proc_list,proc,ble_gattc_proc, next);
        ble_hs_unlock();
#endif
    }

    ble_gattc_process_status(proc, rc);
    return rc;
}

#if MYNEWT_VAL(BLE_GATT_CACHING)
int ble_gattc_check_valid_param(uint16_t num, uint16_t offset)
{
    if (num == 0) {
        return BLE_ATT_ERR_ATTR_NOT_FOUND;
    } else if (offset >= num) {
        return BLE_ATT_ERR_INVALID_OFFSET;
    }

    return 0; // Success
}

static void ble_gattc_fill_gatt_db_conversion(uint16_t *count, uint16_t num, esp_ble_gatt_db_attr_type_t type,
                                              uint16_t offset, void *result, ble_gattc_db_elem_t *db)
{
    uint16_t db_size = ((*count + offset) > num) ? (num - offset) : *count;
    switch (type) {
        case ESP_BLE_GATT_DB_PRIMARY_SERVICE:
        case ESP_BLE_GATT_DB_SECONDARY_SERVICE: {
            ble_gattc_service_elem_t *svc_result = (ble_gattc_service_elem_t *)result;
            for (int i = 0; i < db_size; i++) {
                svc_result->is_primary = (db[offset + i].type == (int)BLE_GATT_DB_PRIMARY_SERVICE);
                svc_result->start_handle = db[offset + i].start_handle;
                svc_result->end_handle = db[offset + i].end_handle;
                svc_result->uuid = db[offset + i].uuid;
                svc_result++;
            }
            break;
        }
        case ESP_BLE_GATT_DB_CHARACTERISTIC: {
            ble_gattc_char_elem_t *char_result = (ble_gattc_char_elem_t *)result;
            for (int i = 0; i < db_size; i++) {
                char_result->char_handle = db[offset + i].handle;
                char_result->properties = db[offset + i].properties;
                char_result->uuid = db[offset + i].uuid;
                char_result++;
            }
            break;
        }
        case ESP_BLE_GATT_DB_DESCRIPTOR: {
            ble_gattc_descr_elem_t *descr_result = (ble_gattc_descr_elem_t *)result;
            for (int i = 0; i < db_size; i++) {
                descr_result->handle = db[offset + i].handle;
                descr_result->uuid = db[offset + i].uuid;
                descr_result++;
            }
            break;
        }
        case BLE_GATT_DB_INCLUDED_SERVICE: {
            ble_gattc_included_svc_elem_t *incl_result = (ble_gattc_included_svc_elem_t *)result;
            for (int i = 0; i < db_size; i++) {
                incl_result->handle = db[offset + i].handle;
                incl_result->incl_svc_s_handle = db[offset + i].start_handle;
                incl_result->incl_svc_e_handle = db[offset + i].end_handle;
                incl_result->uuid = db[offset + i].uuid;
                incl_result++;
              }
            break;
        }
        default:
            BLE_HS_LOG(DEBUG,"%s(), Not support type(%d)", __func__, type);
            break;
    }
}

void ble_gattc_get_cached_service_by_uuid_db(uint16_t conn_id,
                                             ble_uuid_t *svc_uuid,
                                             ble_gattc_db_elem_t **db,
                                             uint16_t *count)
{
    ble_gattc_get_service_with_uuid(conn_id, svc_uuid, db, count);
}

void ble_gattc_get_cached_all_char_db(uint16_t conn_id, uint16_t start_handle,
                                      uint16_t end_handle, ble_gattc_db_elem_t **db,
                                      uint16_t *count)
{
    ble_gattc_get_db_with_operation(conn_id, BLE_GATT_OP_GET_ALL_CHAR,
                                    0, NULL, NULL, NULL,
                                    start_handle, end_handle, db, count);

}

void ble_gattc_get_cached_all_descriptor_db(uint16_t conn_id, uint16_t char_handle,
                                            ble_gattc_db_elem_t **db, uint16_t *count)
{
    ble_gattc_get_db_with_operation(conn_id, BLE_GATT_OP_GET_ALL_DESC,
                                    char_handle, NULL, NULL, NULL,
                                    0, 0xFFFF, db, count);

}

void ble_gattc_get_cached_char_by_uuid_db(uint16_t conn_id, uint16_t start_handle,
                                          uint16_t end_handle, ble_uuid_t *char_uuid,
                                          ble_gattc_db_elem_t **db, uint16_t *count)
{
    ble_gattc_get_db_with_operation(conn_id, BLE_GATT_OP_GET_CHAR_BY_UUID,
                                    0, char_uuid, NULL, NULL,
                                    start_handle, end_handle, db, count);

}

void ble_gatt_get_cached_descr_by_uuid_db(uint16_t conn_id, uint16_t start_handle, uint16_t end_handle,
                                          ble_uuid_t *char_uuid, ble_uuid_t *descr_uuid,
                                          ble_gattc_db_elem_t **db, uint16_t *count)
{
    ble_gattc_get_db_with_operation(conn_id, BLE_GATT_OP_GET_DESC_BY_UUID,
                                    0, char_uuid, descr_uuid, NULL,
                                    start_handle, end_handle, db, count);
}

void ble_gattc_get_cached_descr_by_char_handle_db(uint16_t conn_id, uint16_t char_handle,
                                                  ble_uuid_t *descr_uuid,
                                                  ble_gattc_db_elem_t **db, uint16_t *count)
{
    ble_gattc_get_db_with_operation(conn_id, BLE_GATT_OP_GET_DESC_BY_HANDLE,
                                    char_handle, NULL, descr_uuid, NULL,
                                    0, 0xFFFF, db, count);
}

#if MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
void ble_gattc_get_cached_include_service_db(uint16_t conn_id, uint16_t start_handle, uint16_t end_handle,
                                             ble_uuid_t *incl_uuid,
                                             ble_gattc_db_elem_t **db, uint16_t *count)
{
    ble_gattc_get_db_with_operation(conn_id, BLE_GATT_OP_GET_INCLUDE_SVC,
                                    0, NULL, NULL, incl_uuid,
                                    start_handle, end_handle, db, count);
}
#endif

void ble_gattc_get_db_size(uint16_t conn_handle, uint16_t start_handle,
                           uint16_t end_handle, uint16_t *count)
{
    ble_gattc_get_db_size_handle(conn_handle, start_handle, end_handle, count);
}

void ble_gattc_get_db_size_by_type(uint16_t conn_handle, ble_gattc_db_attr_type type,
                                   uint16_t start_handle, uint16_t end_handle,
                                   uint16_t char_handle, uint16_t *count)
{
    ble_gattc_get_db_size_with_type_handle(conn_handle, type, start_handle, end_handle, char_handle, count);
}

void ble_gattc_get_cached_gatt_db(uint16_t conn_handle,
                                  uint16_t start_handle,
                                  uint16_t end_handle,
                                  ble_gattc_db_elem_t **db,
                                  uint16_t *count, uint16_t *db_num)
{
    ble_gattc_get_gatt_db(conn_handle, start_handle, end_handle, db,count, db_num);
}

int ble_gattc_get_service(uint16_t conn_handle,
                          ble_uuid_t *svc_uuid,
                          ble_gattc_service_elem_t *result,
                          uint16_t *count, uint16_t offset)
{
      int rc;
      ble_gattc_db_elem_t *db = NULL;
      uint16_t svc_num = 0;

      ble_gattc_get_cached_service_by_uuid_db(conn_handle, svc_uuid, &db, &svc_num);
      rc = ble_gattc_check_valid_param(svc_num, offset);
      if (rc != 0) {
          if (db) {
              free(db);
          }
          *count = 0;
          return rc;
      } else {
          ble_gattc_fill_gatt_db_conversion(count, svc_num, ESP_BLE_GATT_DB_PRIMARY_SERVICE, offset, (void *)result, db);
      }

      *count = svc_num;
      //free the db buffer after used.
      if (db) {
          free(db);
      }
      return 0;
}

int ble_gattc_get_all_char(uint16_t conn_handle,
                           uint16_t start_handle,
                           uint16_t end_handle,
                           ble_gattc_char_elem_t *result,
                           uint16_t *count, uint16_t offset)
{
      int rc;
      ble_gattc_db_elem_t *db = NULL;
      uint16_t char_num = 0;

      ble_gattc_get_cached_all_char_db(conn_handle, start_handle, end_handle, &db, &char_num);
      rc = ble_gattc_check_valid_param(char_num, offset);
      if (rc != 0) {
          if (db) {
              free(db);
          }
          *count = 0;
          return rc;
      } else {
          ble_gattc_fill_gatt_db_conversion(count, char_num, ESP_BLE_GATT_DB_CHARACTERISTIC, offset, (void *)result, db);
      }

      *count = char_num;
      //free the db buffer after used.
      if (db) {
          free(db);
      }
      return 0;
}

int ble_gattc_get_all_descr(uint16_t conn_handle,
                            uint16_t char_handle,
                            ble_gattc_descr_elem_t *result,
                            uint16_t *count, uint16_t offset)
{
      int rc;
      ble_gattc_db_elem_t *db = NULL;
      uint16_t descr_num = 0;

      ble_gattc_get_cached_all_descriptor_db(conn_handle, char_handle, &db, &descr_num);
      rc = ble_gattc_check_valid_param(descr_num, offset);
      if (rc != 0) {
          if (db) {
              free(db);
          }
          *count = 0;
          return rc;
      } else {
          ble_gattc_fill_gatt_db_conversion(count, descr_num, ESP_BLE_GATT_DB_DESCRIPTOR, offset, (void *)result, db);
      }

      *count = descr_num;
      // free the db buffer after used.
      if (db) {
          free(db);
      }
      return 0;
}

int ble_gattc_get_char_by_uuid(uint16_t conn_handle,
                               uint16_t start_handle,
                               uint16_t end_handle,
                               ble_uuid_t *char_uuid,
                               ble_gattc_char_elem_t *result, uint16_t *count)
{
      int rc;
      ble_gattc_db_elem_t *db = NULL;
      uint16_t char_num = 0;

      ble_gattc_get_cached_char_by_uuid_db(conn_handle, start_handle, end_handle, char_uuid, &db, &char_num);

      rc = ble_gattc_check_valid_param(char_num, 0);
      if (rc != 0) {
          if (db) {
            free(db);
          }
          *count = 0;
          return rc;
      } else {
          ble_gattc_fill_gatt_db_conversion(count, char_num, ESP_BLE_GATT_DB_CHARACTERISTIC, 0, (void *)result, db);
      }

      *count = char_num;
      // free the db buffer after used.
      if (db) {
          free(db);
      }
      return 0;

}

int ble_gattc_get_descr_by_uuid(uint16_t conn_handle,
                                uint16_t start_handle,
                                uint16_t end_handle,
                                ble_uuid_t *char_uuid,
                                ble_uuid_t *descr_uuid,
                                ble_gattc_descr_elem_t *result, uint16_t *count)
{
      int rc;
      ble_gattc_db_elem_t *db = NULL;
      uint16_t descr_num = 0;

      ble_gatt_get_cached_descr_by_uuid_db(conn_handle, start_handle, end_handle, char_uuid, descr_uuid, &db, &descr_num);

      rc = ble_gattc_check_valid_param(descr_num, 0);
      if (rc != 0) {
          if (db) {
              free(db);
          }
          *count = 0;
          return rc;
      } else {
             ble_gattc_fill_gatt_db_conversion(count, descr_num, ESP_BLE_GATT_DB_DESCRIPTOR, 0, (void *)result, db);
      }

      *count = descr_num;
      // free the db buffer after used.
      if (db) {
          free(db);
      }
      return 0;
}

int ble_gattc_get_descr_by_char_handle(uint16_t conn_handle,
                                       uint16_t char_handle,
                                       ble_uuid_t *descr_uuid,
                                       ble_gattc_descr_elem_t *result, uint16_t *count)
{
      int rc;
      ble_gattc_db_elem_t *db = NULL;
      uint16_t descr_num = 0;

      ble_gattc_get_cached_descr_by_char_handle_db(conn_handle, char_handle, descr_uuid, &db, &descr_num);

      rc = ble_gattc_check_valid_param(descr_num, 0);
      if (rc != 0) {
          if (db) {
              free(db);
          }
          *count = 0;
          return rc;
      } else {
          ble_gattc_fill_gatt_db_conversion(count, descr_num, ESP_BLE_GATT_DB_DESCRIPTOR, 0, (void *)result, db);
      }

      *count = descr_num;
      // free the db buffer after used.
      if (db) {
          free(db);
      }
      return 0;

}

#if MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
int ble_gattc_get_include_service(uint16_t conn_handle,
                                  uint16_t start_handle,
                                  uint16_t end_handle,
                                  ble_uuid_t *incl_uuid,
                                  ble_gattc_included_svc_elem_t *result, uint16_t *count)
{
    int rc;
    ble_gattc_db_elem_t *db = NULL;
    uint16_t incl_num = 0;

    ble_gattc_get_cached_include_service_db(conn_handle, start_handle, end_handle, incl_uuid, &db, &incl_num);

    rc = ble_gattc_check_valid_param(incl_num,0);
    if (rc != 0) {
        if (db) {
            free(db);
        }
        *count = 0;
        return rc;
    } else {
        ble_gattc_fill_gatt_db_conversion(count, incl_num, ESP_BLE_GATT_DB_INCLUDED_SERVICE, 0, (void *)result, db);
    }

    *count = incl_num;
    //free the db buffer after used.
    if (db) {
        free(db);
    }
    return 0;

}
#endif

int ble_gattc_get_attr_count(uint16_t conn_handle, esp_ble_gatt_db_attr_type_t type,
                             uint16_t start_handle, uint16_t end_handle,
                             uint16_t char_handle, uint16_t *count)
{
    if (type == ESP_BLE_GATT_DB_ALL) {
        ble_gattc_get_db_size(conn_handle, start_handle, end_handle, count);
    } else {
        ble_gattc_get_db_size_by_type(conn_handle, type, start_handle, end_handle, char_handle, count);
    }
    return 0;
}

int ble_gattc_get_db(uint16_t conn_handle,
                     uint16_t start_handle, uint16_t end_handle,
                     ble_gattc_db_elem_t *result, uint16_t *count)
{
    uint16_t num = 0;
    ble_gattc_db_elem_t *db = NULL;

    ble_gattc_get_cached_gatt_db(conn_handle, start_handle, end_handle, &db, &num, count);

    if (num == 0) {
        return BLE_ATT_ERR_ATTR_NOT_FOUND;
    }
    if (db) {
        memcpy(result, db, num * sizeof(ble_gattc_db_elem_t));  // Copy data
        free(db);  // Free allocated memory
    }

    return 0;
}

#endif

/*****************************************************************************
 * $read                                                                     *
 *****************************************************************************/

/**
 * Calls a read-characteristic proc's callback with the specified parameters.
 * If the proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
static int
ble_gattc_read_cb(struct ble_gattc_proc *proc, int status,
                  uint16_t att_handle, struct ble_gatt_attr *attr)
{
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    BLE_HS_DBG_ASSERT(attr != NULL || status != 0);
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, read_fail);
    }

#if MYNEWT_VAL(BLE_GATT_CACHING)
    if (status == BLE_HS_ATT_ERR(BLE_ATT_ERR_DB_OUT_OF_SYNC)) {
        ble_gattc_cache_conn_update(proc->conn_handle, 0, 0xFFFF);
    }
#endif
    if (proc->read.cb == NULL) {
        rc = 0;
    } else {
        rc = proc->read.cb(proc->conn_handle,
                           ble_gattc_error(status, att_handle), attr,
                           proc->read.cb_arg);
    }

    return rc;
}

static void
ble_gattc_read_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_read_cb(proc, BLE_HS_ETIMEOUT, 0, NULL);
}

/**
 * Handles an incoming ATT error response for the specified
 * read-characteristic-value proc.
 */
static void
ble_gattc_read_err(struct ble_gattc_proc *proc, int status,
                   uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);
    ble_gattc_read_cb(proc, status, att_handle, NULL);
}

/**
 * Handles an incoming read-response for the specified
 * read-characteristic-value proc.
 */
static int
ble_gattc_read_rx_read_rsp(struct ble_gattc_proc *proc, int status,
                           struct os_mbuf **om)
{
    struct ble_gatt_attr attr;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    attr.handle = proc->read.handle;
    attr.offset = 0;
    attr.om = *om;

    ble_gattc_read_cb(proc, status, 0, &attr);

    /* Indicate to the caller whether the application consumed the mbuf. */
    *om = attr.om;

    /* The read operation only has a single request / response exchange. */
    return BLE_HS_EDONE;
}

static int
ble_gattc_read_tx(struct ble_gattc_proc *proc)
{
    int rc;

    rc = ble_att_clt_tx_read(proc->conn_handle, proc->cid, proc->read.handle);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

int
ble_gattc_read(uint16_t conn_handle, uint16_t attr_handle,
               ble_gatt_attr_fn *cb, void *cb_arg)
{
#if !MYNEWT_VAL(BLE_GATT_READ)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    STATS_INC(ble_gattc_stats, read);

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle, BLE_GATT_OP_READ);

    proc->read.handle = attr_handle;
    proc->read.cb = cb;
    proc->read.cb_arg = cb_arg;

    ble_gattc_log_read(attr_handle);
    rc = ble_gattc_read_tx(proc);
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, read_fail);
    }

    ble_gattc_process_status(proc, rc);
    return rc;
}

/*****************************************************************************
 * $read by uuid                                                             *
 *****************************************************************************/

/**
 * Calls a read-using-characteristic-uuid proc's callback with the specified
 * parameters.  If the proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
static int
ble_gattc_read_uuid_cb(struct ble_gattc_proc *proc, int status,
                       uint16_t att_handle, struct ble_gatt_attr *attr)
{
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    BLE_HS_DBG_ASSERT(attr != NULL || status != 0);
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, read_uuid_fail);
    }

    if (proc->read_uuid.cb == NULL) {
        rc = 0;
    } else {
        rc = proc->read_uuid.cb(proc->conn_handle,
                                ble_gattc_error(status, att_handle), attr,
                                proc->read_uuid.cb_arg);
    }

    return rc;
}

static void
ble_gattc_read_uuid_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_read_uuid_cb(proc, BLE_HS_ETIMEOUT, 0, NULL);
}

/**
 * Handles an incoming ATT error response for the specified
 * read-using-characteristic-uuid proc.
 */
static void
ble_gattc_read_uuid_err(struct ble_gattc_proc *proc, int status,
                        uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_read_uuid_cb(proc, status, att_handle, NULL);
}

/**
 * Handles an incoming "attribute data" entry from a read-by-type response for
 * the specified read-using-characteristic-uuid proc.
 */
static int
ble_gattc_read_uuid_rx_adata(struct ble_gattc_proc *proc,
                             struct ble_att_read_type_adata *adata)
{
    struct ble_gatt_attr attr;
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    attr.handle = adata->att_handle;
    attr.offset = 0;
    attr.om = ble_hs_mbuf_from_flat(adata->value, adata->value_len);
    if (attr.om == NULL) {
        rc = BLE_HS_ENOMEM;
    } else {
        rc = 0;
    }
    rc = ble_gattc_read_uuid_cb(proc, rc, 0, &attr);

    /* Free the attribute mbuf if the application has not consumed it. */
    os_mbuf_free_chain(attr.om);

    if (rc != 0) {
        return BLE_HS_EDONE;
    }

    return 0;
}

/**
 * Handles a notification that a read-by-type response has been fully
 * processed for the specified read-using-characteristic-uuid proc.
 */
static int
ble_gattc_read_uuid_rx_complete(struct ble_gattc_proc *proc, int status)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0) {
        ble_gattc_read_uuid_cb(proc, status, 0, NULL);
        return BLE_HS_EDONE;
    }

    /* XXX: We may need to send a follow-up request to address the possibility
     * of multiple characteristics with identical UUIDs.
     */
    ble_gattc_read_uuid_cb(proc, BLE_HS_EDONE, 0, NULL);
    return BLE_HS_EDONE;
}

static int
ble_gattc_read_uuid_tx(struct ble_gattc_proc *proc)
{
    return ble_att_clt_tx_read_type(proc->conn_handle, proc->cid,
                                    proc->read_uuid.start_handle,
                                    proc->read_uuid.end_handle,
                                    &proc->read_uuid.chr_uuid.u);
}

int
ble_gattc_read_by_uuid(uint16_t conn_handle, uint16_t start_handle,
                       uint16_t end_handle, const ble_uuid_t *uuid,
                       ble_gatt_attr_fn *cb, void *cb_arg)
{
#if !MYNEWT_VAL(BLE_GATT_READ_UUID)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    STATS_INC(ble_gattc_stats, read_uuid);

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle, BLE_GATT_OP_READ_UUID);

    ble_uuid_to_any(uuid, &proc->read_uuid.chr_uuid);
    proc->read_uuid.start_handle = start_handle;
    proc->read_uuid.end_handle = end_handle;
    proc->read_uuid.cb = cb;
    proc->read_uuid.cb_arg = cb_arg;

    ble_gattc_log_read_uuid(start_handle, end_handle, uuid);
    rc = ble_gattc_read_uuid_tx(proc);
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, read_uuid_fail);
    }

    ble_gattc_process_status(proc, rc);
    return rc;
}

/*****************************************************************************
 * $read long                                                                *
 *****************************************************************************/

/**
 * Calls a read-long-characteristic proc's callback with the specified
 * parameters.  If the proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
static int
ble_gattc_read_long_cb(struct ble_gattc_proc *proc, int status,
                       uint16_t att_handle, struct ble_gatt_attr *attr)
{
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    BLE_HS_DBG_ASSERT(attr != NULL || status != 0);
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, read_long_fail);
    }

#if MYNEWT_VAL(BLE_GATT_CACHING)
    if (status == BLE_HS_ATT_ERR(BLE_ATT_ERR_DB_OUT_OF_SYNC)) {
        ble_gattc_cache_conn_update(proc->conn_handle, 0, 0xFFFF);
    }
#endif
    if (proc->read_long.cb == NULL) {
        rc = 0;
    } else {
        rc = proc->read_long.cb(proc->conn_handle,
                                ble_gattc_error(status, att_handle), attr,
                                proc->read_long.cb_arg);
    }

    return rc;
}

static void
ble_gattc_read_long_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_read_long_cb(proc, BLE_HS_ETIMEOUT, 0, NULL);
}

/**
 * Triggers a pending transmit for the specified read-long-characteristic proc.
 */
static int
ble_gattc_read_long_tx(struct ble_gattc_proc *proc)
{
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (proc->read_long.offset == 0) {
        rc = ble_att_clt_tx_read(proc->conn_handle, proc->cid, proc->read_long.handle);
        if (rc != 0) {
            return rc;
        }
    } else {
        rc = ble_att_clt_tx_read_blob(proc->conn_handle, proc->cid,
                                      proc->read_long.handle,
                                      proc->read_long.offset);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

static int
ble_gattc_read_long_resume(struct ble_gattc_proc *proc)
{
    int status;
    int rc;

    status = ble_gattc_read_long_tx(proc);
    rc = ble_gattc_process_resume_status(proc, status);
    if (rc != 0) {
        ble_gattc_read_long_cb(proc, rc, 0, NULL);
        return rc;
    }

    return 0;
}

/**
 * Handles an incoming ATT error response for the specified
 * read-long-characteristic proc.
 */
static void
ble_gattc_read_long_err(struct ble_gattc_proc *proc, int status,
                        uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);
    ble_gattc_read_long_cb(proc, status, att_handle, NULL);
}

/**
 * Handles an incoming read-response for the specified
 * read-long-characteristic-values proc.
 */
static int
ble_gattc_read_long_rx_read_rsp(struct ble_gattc_proc *proc, int status,
                                struct os_mbuf **om)
{
    struct ble_gatt_attr attr;
    uint16_t data_len;
    uint16_t mtu;
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    data_len = OS_MBUF_PKTLEN(*om);

    attr.handle = proc->read_long.handle;
    attr.offset = proc->read_long.offset;
    attr.om = *om;

    /* Report partial payload to application. */
    rc = ble_gattc_read_long_cb(proc, status, 0, &attr);

    /* Indicate to the caller whether the application consumed the mbuf. */
    *om = attr.om;

    if (rc != 0 || status != 0) {
        return BLE_HS_EDONE;
    }

    /* Determine if this is the end of the attribute value. */
    mtu = ble_att_mtu_by_cid(proc->conn_handle, proc->cid);
    if (mtu == 0) {
        /* No longer connected. */
        return BLE_HS_EDONE;
    }

    if (data_len < mtu - 1) {
        /* Response shorter than maximum allowed; read complete. */
        ble_gattc_read_long_cb(proc, BLE_HS_EDONE, 0, NULL);
        return BLE_HS_EDONE;
    }

    /* Send follow-up request. */
    proc->read_long.offset += data_len;
    rc = ble_gattc_read_long_resume(proc);
    if (rc != 0) {
        return BLE_HS_EDONE;
    }

    return 0;
}

int
ble_gattc_read_long(uint16_t conn_handle, uint16_t handle, uint16_t offset,
                    ble_gatt_attr_fn *cb, void *cb_arg)
{
#if !MYNEWT_VAL(BLE_GATT_READ_LONG)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    STATS_INC(ble_gattc_stats, read_long);

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle, BLE_GATT_OP_READ_LONG);

    proc->read_long.handle = handle;
    proc->read_long.offset = offset;
    proc->read_long.cb = cb;
    proc->read_long.cb_arg = cb_arg;

    ble_gattc_log_read_long(proc);

    rc = ble_gattc_read_long_tx(proc);
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, read_long_fail);
    }

    ble_gattc_process_status(proc, rc);
    return rc;
}

/*****************************************************************************
 * $read multiple                                                            *
 *****************************************************************************/

static int
ble_gattc_read_mult_cb_var(struct ble_gattc_proc *proc, int status,
                           uint16_t att_handle, struct os_mbuf **om)
{
    struct ble_gatt_attr attr[proc->read_mult.num_handles];
    int rc;
    int i;
    uint16_t attr_len;

    if (proc->read_mult.cb_mult == NULL) {
        return 0;
    }

    memset(attr, 0, sizeof(attr));

    for (i = 0; i < proc->read_mult.num_handles; i++) {
        attr[i].handle = proc->read_mult.handles[i];
        attr[i].offset = 0;
        if (om == NULL || OS_MBUF_PKTLEN(*om) == 0) {
            continue;
        }

        *om = os_mbuf_pullup(*om, 2);
        assert(*om);

        attr_len = get_le16((*om)->om_data);

        os_mbuf_adj(*om, 2);

        if (attr_len > BLE_ATT_ATTR_MAX_LEN) {
            /*TODO Figure out what to do here */
            break;
        }

        attr[i].om = os_msys_get_pkthdr(attr_len, 0);
        if (!attr[i].om) {
            /*TODO Figure out what to do here */
            break;
        }

        rc = os_mbuf_appendfrom(attr[i].om, *om, 0, attr_len);
        if (rc) {
            /*TODO Figure out what to do here */
            break;
        }

        os_mbuf_adj(*om, attr_len);
    }

    /*FIXME Testing assert */
    assert(i == proc->read_mult.num_handles);

    proc->read_mult.cb_mult(proc->conn_handle,
                    ble_gattc_error(status, att_handle), &attr[0],
                    i,
                    proc->read_mult.cb_arg);

    for (i = 0; i < proc->read_mult.num_handles; i++) {
        if (attr[i].om != NULL) {
            os_mbuf_free_chain(attr[i].om);
        }
    }

    return 0;
}


/**
 * Calls a read-multiple-characteristics proc's callback with the specified
 * parameters.  If the proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
static int
ble_gattc_read_mult_cb(struct ble_gattc_proc *proc, int status,
                       uint16_t att_handle, struct os_mbuf **om)
{
    struct ble_gatt_attr attr;
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    BLE_HS_DBG_ASSERT(om != NULL || status != 0);
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, read_mult_fail);
    }

    if (proc->read_mult.variable) {
        return ble_gattc_read_mult_cb_var(proc, status, att_handle, om);
    }

    attr.handle = 0;
    attr.offset = 0;
    if (om == NULL) {
        attr.om = NULL;
    } else {
        attr.om = *om;
    }

#if MYNEWT_VAL(BLE_GATT_CACHING)
    if (status == BLE_HS_ATT_ERR(BLE_ATT_ERR_DB_OUT_OF_SYNC)) {
        ble_gattc_cache_conn_update(proc->conn_handle, 0, 0xFFFF);
    }
#endif
    if (proc->read_mult.cb == NULL) {
        rc = 0;
    } else {
        rc = proc->read_mult.cb(proc->conn_handle,
                                ble_gattc_error(status, att_handle), &attr,
                                proc->read_mult.cb_arg);
    }

    /* Indicate to the caller whether the application consumed the mbuf. */
    if (om != NULL) {
        *om = attr.om;
    }

    return rc;
}

static void
ble_gattc_read_mult_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_read_mult_cb(proc, BLE_HS_ETIMEOUT, 0, 0);
}

static void
ble_gattc_read_mult_var_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_read_mult_cb_var(proc, BLE_HS_ETIMEOUT, 0, 0);
}

/**
 * Handles an incoming ATT error response for the specified
 * read-multiple-characteristics proc.
 */
static void
ble_gattc_read_mult_err(struct ble_gattc_proc *proc, int status,
                        uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);
    ble_gattc_read_mult_cb(proc, status, att_handle, NULL);
}

/**
 * Handles an incoming ATT error response for the specified
 * read-multiple-variable-lengthcharacteristics proc.
 */
static void
ble_gattc_read_mult_var_err(struct ble_gattc_proc *proc, int status,
                            uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);
    ble_gattc_read_mult_cb_var(proc, status, att_handle, NULL);
}

static int
ble_gattc_read_mult_tx(struct ble_gattc_proc *proc)
{
    int rc;

    rc = ble_att_clt_tx_read_mult(proc->conn_handle, proc->cid, proc->read_mult.handles,
                                  proc->read_mult.num_handles, proc->read_mult.variable);
    if (rc != 0) {
        return rc;
    }

    return 0;
}


static int
ble_gattc_read_mult_internal(uint16_t conn_handle, const uint16_t *handles,
                             uint8_t num_handles, bool variable, ble_gatt_attr_fn *cb,
                             ble_gatt_attr_mult_fn *cb_mult,
                             void *cb_arg)
{
#if !MYNEWT_VAL(BLE_GATT_READ_MULT)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    proc = NULL;

    STATS_INC(ble_gattc_stats, read_mult);

    if (num_handles > MYNEWT_VAL(BLE_GATT_READ_MAX_ATTRS)) {
        rc = BLE_HS_EINVAL;
        goto done;
    }

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle,
                           variable ? BLE_GATT_OP_READ_MULT_VAR : BLE_GATT_OP_READ_MULT);

    memcpy(proc->read_mult.handles, handles, num_handles * sizeof *handles);
    proc->read_mult.num_handles = num_handles;
    proc->read_mult.variable = variable;
    proc->read_mult.cb = cb;
    proc->read_mult.cb_mult = cb_mult;
    proc->read_mult.cb_arg = cb_arg;

    ble_gattc_log_read_mult(handles, num_handles, variable);
    rc = ble_gattc_read_mult_tx(proc);
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, read_mult_fail);
    }

    ble_gattc_process_status(proc, rc);
    return rc;
}

int
ble_gattc_read_mult(uint16_t conn_handle, const uint16_t *handles,
                    uint8_t num_handles, ble_gatt_attr_fn *cb,
                    void *cb_arg)
{
    return ble_gattc_read_mult_internal(conn_handle, handles,
                                        num_handles, false, cb, NULL, cb_arg);
}

int
ble_gattc_read_mult_var(uint16_t conn_handle, const uint16_t *handles,
                        uint8_t num_handles, ble_gatt_attr_mult_fn *cb,
                        void *cb_arg)
{
#if MYNEWT_VAL(BLE_GATT_READ_MULT_VAR)
    return ble_gattc_read_mult_internal(conn_handle, handles, num_handles,
                                        true, NULL, cb, cb_arg);
#else
    return BLE_HS_ENOTSUP;
#endif
}

/*****************************************************************************
 * $write no response                                                        *
 *****************************************************************************/

int
ble_gattc_write_no_rsp(uint16_t conn_handle, uint16_t attr_handle,
                       struct os_mbuf *txom)
{
#if !MYNEWT_VAL(BLE_GATT_WRITE_NO_RSP)
    return BLE_HS_ENOTSUP;
#endif

    int rc;
    uint16_t cid;

    STATS_INC(ble_gattc_stats, write_no_rsp);

    ble_gattc_log_write(attr_handle, OS_MBUF_PKTLEN(txom), 0);

    cid = ble_eatt_get_available_chan_cid(conn_handle, BLE_GATT_OP_DUMMY);
    rc = ble_att_clt_tx_write_cmd(conn_handle, cid, attr_handle, txom);
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, write);
    }
    ble_eatt_release_chan(conn_handle, BLE_GATT_OP_DUMMY);

    return rc;
}

int
ble_gattc_write_no_rsp_flat(uint16_t conn_handle, uint16_t attr_handle,
                            const void *data, uint16_t data_len)
{
    struct os_mbuf *om;
    int rc;

    om = ble_hs_mbuf_from_flat(data, data_len);
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }

    rc = ble_gattc_write_no_rsp(conn_handle, attr_handle, om);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

/*****************************************************************************
 * $signed write                                                             *
 ****************************************************************************/

int
ble_gattc_signed_write(uint16_t conn_handle, uint16_t attr_handle,
                       struct os_mbuf *txom)
{
#if !MYNEWT_VAL(BLE_GATT_SIGNED_WRITE)
    return BLE_HS_ENOTSUP;
#endif

    int rc;
    struct ble_store_value_sec value_sec;
    struct ble_store_key_sec key_sec;
    struct ble_gap_conn_desc desc;
    uint8_t csrk[16];

    STATS_INC(ble_gattc_stats, signed_write);

    ble_gattc_log_signed_write(attr_handle, OS_MBUF_PKTLEN(txom));

    rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        goto err;
    }
    if (desc.sec_state.encrypted == 1) {
        rc = BLE_HS_EENCRYPT;
        goto err;
    }

    memset(&key_sec, 0, sizeof key_sec);
    key_sec.peer_addr = desc.peer_id_addr;

    /* Getting the CSRK for signing */
    rc = ble_store_read_our_sec(&key_sec, &value_sec);
    if (rc != 0) {
        goto err;
    }
    if (value_sec.csrk_present != 1) {
        rc = BLE_HS_EAUTHEN;
        goto err;
    }

    /* Converting the csrk to little endian */
    swap_buf(csrk, value_sec.csrk, 16);

    rc = ble_att_clt_tx_signed_write_cmd(conn_handle, BLE_L2CAP_CID_ATT, attr_handle,
                                         csrk, value_sec.sign_counter, txom);
    if (rc != 0) {
        goto err;
    }

    return 0;
err:
    STATS_INC(ble_gattc_stats, signed_write_fail);
    os_mbuf_free_chain(txom);
    return rc;
}

/*****************************************************************************
 * $write                                                                    *
 *****************************************************************************/

/**
 * Calls a write-characteristic-value proc's callback with the specified
 * parameters.  If the proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
static int
ble_gattc_write_cb(struct ble_gattc_proc *proc, int status,
                   uint16_t att_handle)
{
    struct ble_gatt_attr attr;
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, write_fail);
    }

#if MYNEWT_VAL(BLE_GATT_CACHING)
    if (status == BLE_HS_ATT_ERR(BLE_ATT_ERR_DB_OUT_OF_SYNC)) {
        ble_gattc_cache_conn_update(proc->conn_handle, 0, 0xFFFF);
    }
#endif
    if (proc->write.cb == NULL) {
        rc = 0;
    } else {
        memset(&attr, 0, sizeof attr);
        attr.handle = proc->write.att_handle;
        rc = proc->write.cb(proc->conn_handle,
                            ble_gattc_error(status, att_handle),
                            &attr, proc->write.cb_arg);
    }

    return rc;
}

static void
ble_gattc_write_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_write_cb(proc, BLE_HS_ETIMEOUT, 0);
}

/**
 * Handles an incoming ATT error response for the specified
 * write-characteristic-value proc.
 */
static void
ble_gattc_write_err(struct ble_gattc_proc *proc, int status,
                    uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);
    ble_gattc_write_cb(proc, status, att_handle);
}

int
ble_gattc_write(uint16_t conn_handle, uint16_t attr_handle,
                struct os_mbuf *txom, ble_gatt_attr_fn *cb, void *cb_arg)
{
#if !MYNEWT_VAL(BLE_GATT_WRITE)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    STATS_INC(ble_gattc_stats, write);

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle, BLE_GATT_OP_WRITE);

    proc->write.att_handle = attr_handle;
    proc->write.cb = cb;
    proc->write.cb_arg = cb_arg;
#if MYNEWT_VAL(BLE_GATTC_AUTO_PAIR)
    proc->write.om = os_mbuf_dup(txom);
#endif

    ble_gattc_log_write(attr_handle, OS_MBUF_PKTLEN(txom), 1);

    rc = ble_att_clt_tx_write_req(conn_handle, proc->cid, attr_handle, txom);
    txom = NULL;
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, write_fail);
    }

    /* Free the mbuf in case the send failed. */
    os_mbuf_free_chain(txom);

    ble_gattc_process_status(proc, rc);
    return rc;
}

int
ble_gattc_write_flat(uint16_t conn_handle, uint16_t attr_handle,
                     const void *data, uint16_t data_len,
                     ble_gatt_attr_fn *cb, void *cb_arg)
{
    struct os_mbuf *om;
    int rc;

    om = ble_hs_mbuf_from_flat(data, data_len);
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }

    rc = ble_gattc_write(conn_handle, attr_handle, om, cb, cb_arg);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

/*****************************************************************************
 * $write long                                                               *
 *****************************************************************************/

/**
 * Calls a write-long-characteristic-value proc's callback with the specified
 * parameters.  If the proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
static int
ble_gattc_write_long_cb(struct ble_gattc_proc *proc, int status,
                        uint16_t att_handle)
{
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, write_long_fail);
    }

#if MYNEWT_VAL(BLE_GATT_CACHING)
    if (status == BLE_HS_ATT_ERR(BLE_ATT_ERR_DB_OUT_OF_SYNC)) {
        ble_gattc_cache_conn_update(proc->conn_handle, 0, 0xFFFF);
    }
#endif
    if (proc->write_long.cb == NULL) {
        rc = 0;
    } else {
        rc = proc->write_long.cb(proc->conn_handle,
                                 ble_gattc_error(status, att_handle),
                                 &proc->write_long.attr,
                                 proc->write_long.cb_arg);
    }

    return rc;
}

static void
ble_gattc_write_long_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_write_long_cb(proc, BLE_HS_ETIMEOUT, 0);
}

/**
 * Triggers a pending transmit for the specified
 * write-long-characteristic-value proc.
 */
static int
ble_gattc_write_long_tx(struct ble_gattc_proc *proc)
{
    struct os_mbuf *om;
    int write_len;
    int max_sz;
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    om = NULL;

    max_sz = ble_att_mtu_by_cid(proc->conn_handle, proc->cid) - BLE_ATT_PREP_WRITE_CMD_BASE_SZ;
    if (max_sz <= 0) {
        /* Not connected. */
        rc = BLE_HS_ENOTCONN;
        goto done;
    }

    write_len = min(max_sz,
                    OS_MBUF_PKTLEN(proc->write_long.attr.om) -
                        proc->write_long.attr.offset);

    if (write_len <= 0) {
        rc = ble_att_clt_tx_exec_write(proc->conn_handle, proc->cid,
                                       BLE_ATT_EXEC_WRITE_F_EXECUTE);
        goto done;
    }

    proc->write_long.length = write_len;
    om = ble_hs_mbuf_att_pkt();
    if (om == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    rc = os_mbuf_appendfrom(om, proc->write_long.attr.om,
                            proc->write_long.attr.offset,
                            proc->write_long.length);
    if (rc != 0) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    rc = ble_att_clt_tx_prep_write(proc->conn_handle, proc->cid,
                                   proc->write_long.attr.handle,
                                   proc->write_long.attr.offset, om);
    om = NULL;
    if (rc != 0) {
        goto done;
    }

done:
    os_mbuf_free_chain(om);
    return rc;
}

static int
ble_gattc_write_long_resume(struct ble_gattc_proc *proc)
{
    int status;
    int rc;

    status = ble_gattc_write_long_tx(proc);
    rc = ble_gattc_process_resume_status(proc, status);
    if (rc != 0) {
        ble_gattc_write_long_cb(proc, rc, 0);
        return rc;
    }

    return 0;
}

/**
 * Handles an incoming ATT error response for the specified
 * write-long-characteristic-value proc.
 */
static void
ble_gattc_write_long_err(struct ble_gattc_proc *proc, int status,
                         uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    /* If we have successfully queued any data, and the failure occurred before
     * we could send the execute write command, then erase all queued data.
     */
    if (proc->write_long.attr.offset > 0 &&
        proc->write_long.attr.offset <
        OS_MBUF_PKTLEN(proc->write_long.attr.om)) {

        ble_att_clt_tx_exec_write(proc->conn_handle, proc->cid,
                                  BLE_ATT_EXEC_WRITE_F_CANCEL);
    }

    /* Report failure. */
    ble_gattc_write_long_cb(proc, status, att_handle);
}

/**
 * Handles an incoming prepare-write-response for the specified
 * write-long-cahracteristic-values proc.
 */
static int
ble_gattc_write_long_rx_prep(struct ble_gattc_proc *proc,
                             int status,
                             uint16_t handle, uint16_t offset,
                             struct os_mbuf **rxom)
{
    struct os_mbuf *om;
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    /* Let the caller free the mbuf. */
    om = *rxom;

    if (status != 0) {
        rc = status;
        goto err;
    }

    /* Verify the response. */
    if (proc->write_long.attr.offset >=
        OS_MBUF_PKTLEN(proc->write_long.attr.om)) {

        /* Expecting a prepare write response, not an execute write
         * response.
         */
        rc = BLE_HS_EBADDATA;
        goto err;
    }
    if (handle != proc->write_long.attr.handle) {
        rc = BLE_HS_EBADDATA;
        goto err;
    }
    if (offset != proc->write_long.attr.offset) {
        rc = BLE_HS_EBADDATA;
        goto err;
    }
    if (offset + OS_MBUF_PKTLEN(om) >
        OS_MBUF_PKTLEN(proc->write_long.attr.om)) {

        rc = BLE_HS_EBADDATA;
        goto err;
    }
    if (OS_MBUF_PKTLEN(om) != proc->write_long.length) {
        rc = BLE_HS_EBADDATA;
        goto err;
    }
    if (os_mbuf_cmpm(om, 0,
                     proc->write_long.attr.om, offset,
                     proc->write_long.length) != 0) {

        rc = BLE_HS_EBADDATA;

        /* if data doesn't match up send cancel write */
        ble_att_clt_tx_exec_write(proc->conn_handle, proc->cid,
                                  BLE_ATT_EXEC_WRITE_F_CANCEL);
        goto err;
    } else {
        /* Send follow-up request. */
        proc->write_long.attr.offset += OS_MBUF_PKTLEN(om);
        rc = ble_gattc_write_long_resume(proc);
        if (rc != 0) {
            goto err;
        }

        return 0;
    }

err:
    /* XXX: Might need to cancel pending writes. */
    ble_gattc_write_long_cb(proc, rc, 0);
    return BLE_HS_EDONE;
}

/**
 * Handles an incoming execute-write-response for the specified
 * write-long-characteristic-values proc.
 */
static int
ble_gattc_write_long_rx_exec(struct ble_gattc_proc *proc, int status)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (proc->write_long.attr.offset <
        OS_MBUF_PKTLEN(proc->write_long.attr.om)) {

        /* Expecting an execute write response, not a prepare write
         * response.
         */
        return BLE_HS_EBADDATA;
    }

    ble_gattc_write_long_cb(proc, status, 0);
    return BLE_HS_EDONE;
}

int
ble_gattc_write_long(uint16_t conn_handle, uint16_t attr_handle,
                     uint16_t offset, struct os_mbuf *txom,
                     ble_gatt_attr_fn *cb, void *cb_arg)
{
#if !MYNEWT_VAL(BLE_GATT_WRITE_LONG)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    STATS_INC(ble_gattc_stats, write_long);

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle, BLE_GATT_OP_WRITE_LONG);

    proc->write_long.attr.handle = attr_handle;
    proc->write_long.attr.offset = offset;
    proc->write_long.attr.om = txom;
    proc->write_long.cb = cb;
    proc->write_long.cb_arg = cb_arg;

    /* The mbuf is consumed by the procedure. */
    txom = NULL;

    if (proc->write_long.attr.offset > OS_MBUF_PKTLEN(proc->write_long.attr.om)) {
        rc = BLE_ATT_ERR_INVALID_OFFSET;
        goto done;
    }

    ble_gattc_log_write_long(proc);

    rc = ble_gattc_write_long_tx(proc);
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, write_long_fail);
    }

    /* Free the mbuf in case of failure. */
    os_mbuf_free_chain(txom);

    ble_gattc_process_status(proc, rc);
    return rc;
}

/*****************************************************************************
 * $write reliable                                                           *
 *****************************************************************************/

/**
 * Calls a write-long-characteristic-value proc's callback with the specified
 * parameters.  If the proc has no callback, this function is a no-op.
 *
 * @return                      The return code of the callback (or 0 if there
 *                                  is no callback).
 */
static int
ble_gattc_write_reliable_cb(struct ble_gattc_proc *proc, int status,
                            uint16_t att_handle)
{
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != 0 && status != BLE_HS_EDONE) {
        STATS_INC(ble_gattc_stats, write_reliable_fail);
    }

#if MYNEWT_VAL(BLE_GATT_CACHING)
    if (status == BLE_HS_ATT_ERR(BLE_ATT_ERR_DB_OUT_OF_SYNC)) {
        ble_gattc_cache_conn_update(proc->conn_handle, 0, 0xFFFF);
    }
#endif
    if (proc->write_reliable.cb == NULL) {
        rc = 0;
    } else {
        rc = proc->write_reliable.cb(proc->conn_handle,
                                     ble_gattc_error(status, att_handle),
                                     proc->write_reliable.attrs,
                                     proc->write_reliable.num_attrs,
                                     proc->write_reliable.cb_arg);
    }

    return rc;
}

static void
ble_gattc_write_reliable_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gattc_write_reliable_cb(proc, BLE_HS_ETIMEOUT, 0);
}

/**
 * Triggers a pending transmit for the specified
 * write-reliable-characteristic-value proc.
 */
static int
ble_gattc_write_reliable_tx(struct ble_gattc_proc *proc)
{
    struct ble_gatt_attr *attr;
    struct os_mbuf *om;
    uint16_t max_sz;
    int attr_idx;
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    om = NULL;

    attr_idx = proc->write_reliable.cur_attr;

    if (attr_idx >= proc->write_reliable.num_attrs) {
        rc = ble_att_clt_tx_exec_write(proc->conn_handle, proc->cid,
                                       BLE_ATT_EXEC_WRITE_F_EXECUTE);
        goto done;
    }

    attr = proc->write_reliable.attrs + attr_idx;

    max_sz = ble_att_mtu_by_cid(proc->conn_handle, proc->cid) - BLE_ATT_PREP_WRITE_CMD_BASE_SZ;
    if (max_sz <= 0) {
        /* Not connected. */
        rc = BLE_HS_ENOTCONN;
        goto done;
    }

    proc->write_reliable.length =
        min(max_sz, OS_MBUF_PKTLEN(attr->om) - attr->offset);

    om = ble_hs_mbuf_att_pkt();
    if (om == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    rc = os_mbuf_appendfrom(om, attr->om, attr->offset,
                            proc->write_reliable.length);
    if (rc != 0) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    rc = ble_att_clt_tx_prep_write(proc->conn_handle, proc->cid,
                                   attr->handle, attr->offset, om);
    om = NULL;
    if (rc != 0) {
        goto done;
    }

done:
    os_mbuf_free_chain(om);
    return rc;
}

static int
ble_gattc_write_reliable_resume(struct ble_gattc_proc *proc)
{
    int status;
    int rc;

    status = ble_gattc_write_reliable_tx(proc);
    rc = ble_gattc_process_resume_status(proc, status);
    if (rc != 0) {
        ble_gattc_write_reliable_cb(proc, rc, 0);
        return rc;
    }

    return 0;
}

/**
 * Handles an incoming ATT error response for the specified
 * write-reliable-characteristic-value proc.
 */
static void
ble_gattc_write_reliable_err(struct ble_gattc_proc *proc, int status,
                             uint16_t att_handle)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);
    ble_gattc_write_reliable_cb(proc, status, att_handle);

    /* If we have successfully queued any data, and the failure occurred before
     * we could send the execute write command, then erase all queued data.
     */
    if (proc->write_reliable.cur_attr < proc->write_reliable.num_attrs) {

        ble_att_clt_tx_exec_write(proc->conn_handle, proc->cid,
                                  BLE_ATT_EXEC_WRITE_F_CANCEL);
    }
}

/**
 * Handles an incoming prepare-write-response for the specified
 * write-reliable-cahracteristic-values proc.
 */
static int
ble_gattc_write_reliable_rx_prep(struct ble_gattc_proc *proc,
                                 int status,
                                 uint16_t handle, uint16_t offset,
                                 struct os_mbuf **rxom)
{
    struct ble_gatt_attr *attr;
    struct os_mbuf *om;
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    /* Let the caller free the mbuf. */
    om = *rxom;

    if (status != 0) {
        rc = status;
        goto err;
    }

    if (proc->write_reliable.cur_attr >= proc->write_reliable.num_attrs) {
        /* Expecting an execute write response, not a prepare write
         * response.
         */
        rc = BLE_HS_EBADDATA;
        goto err;
    }
    attr = proc->write_reliable.attrs + proc->write_reliable.cur_attr;

    /* Verify the response. */
    if (handle != attr->handle) {
        rc = BLE_HS_EBADDATA;
        goto err;
    }
    if (offset != attr->offset) {
        rc = BLE_HS_EBADDATA;
        goto err;
    }
    if (os_mbuf_cmpm(attr->om, offset, om, 0,
                     proc->write_reliable.length) != 0) {

        rc = BLE_HS_EBADDATA;
        goto err;
    }

    /* Send follow-up request. */
    attr->offset += proc->write_reliable.length;
    if (attr->offset >= OS_MBUF_PKTLEN(attr->om)) {
        attr->offset = 0;
        proc->write_reliable.cur_attr++;
    }
    rc = ble_gattc_write_reliable_resume(proc);
    if (rc != 0) {
        goto err;
    }

    return 0;

err:
    ble_gattc_write_reliable_err(proc, rc, 0);
    return BLE_HS_EDONE;
}

/**
 * Handles an incoming execute-write-response for the specified
 * write-reliable-characteristic-values proc.
 */
static int
ble_gattc_write_reliable_rx_exec(struct ble_gattc_proc *proc, int status)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);
    ble_gattc_write_reliable_cb(proc, status, 0);
    return BLE_HS_EDONE;
}

int
ble_gattc_write_reliable(uint16_t conn_handle,
                         struct ble_gatt_attr *attrs,
                         int num_attrs,
                         ble_gatt_reliable_attr_fn *cb, void *cb_arg)
{
#if !MYNEWT_VAL(BLE_GATT_WRITE_RELIABLE)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    int rc;
    int i;

    proc = NULL;

    STATS_INC(ble_gattc_stats, write_reliable);

    if (num_attrs > MYNEWT_VAL(BLE_GATT_WRITE_MAX_ATTRS)) {
        rc = BLE_HS_EINVAL;
        goto done;
    }

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle, BLE_GATT_OP_WRITE_RELIABLE);

    proc->write_reliable.num_attrs = num_attrs;
    proc->write_reliable.cur_attr = 0;
    proc->write_reliable.cb = cb;
    proc->write_reliable.cb_arg = cb_arg;

    for (i = 0; i < num_attrs; i++) {
        proc->write_reliable.attrs[i] = attrs[i];
        proc->write_reliable.attrs[i].offset = 0;

        /* Consume mbuf from caller. */
        attrs[i].om = NULL;
    }

    ble_gattc_log_write_reliable(proc);
    rc = ble_gattc_write_reliable_tx(proc);
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, write_reliable_fail);
    }

    /* Free supplied mbufs in case something failed. */
    for (i = 0; i < num_attrs; i++) {
        os_mbuf_free_chain(attrs[i].om);
        attrs[i].om = NULL;
    }

    ble_gattc_process_status(proc, rc);
    return rc;
}

/*****************************************************************************
 * $notify                                                                   *
 *****************************************************************************/

#if MYNEWT_VAL(BLE_GATT_CACHING)
static int ble_gatts_check_conn_aware(uint16_t conn_handle, bool *aware) {
    struct ble_hs_conn *conn;
    conn = ble_hs_conn_find(conn_handle);
    if(conn == NULL) {
        return BLE_HS_ENOTCONN;
    }
    *aware = conn->bhc_gatt_svr.aware_state;
    return 0;
}
#endif

#endif

int
ble_gatts_notify_custom(uint16_t conn_handle, uint16_t chr_val_handle,
                        struct os_mbuf *txom)
{
#if !MYNEWT_VAL(BLE_GATT_NOTIFY)
    return BLE_HS_ENOTSUP;
#endif
#if MYNEWT_VAL(BLE_GATT_CACHING)
    bool aware;
#endif

    int rc;

    STATS_INC(ble_gattc_stats, notify);

    ble_gattc_log_notify(chr_val_handle);

#if MYNEWT_VAL(BLE_GATT_CACHING)
    ble_hs_lock();
    rc = ble_gatts_check_conn_aware(conn_handle, &aware);
    ble_hs_unlock();
    if(rc != 0) {
        goto done;
    }
    if(!aware) {
        rc = BLE_HS_EREJECT; /* TODO correct error code ?*/
        goto done;
    }
#endif
    if (txom == NULL) {
        /* No custom attribute data; read the value from the specified
         * attribute.
         */
        txom = ble_hs_mbuf_att_pkt();
        if (txom == NULL) {
            rc = BLE_HS_ENOMEM;
            goto done;
        }
        rc = ble_att_svr_read_handle(BLE_HS_CONN_HANDLE_NONE,
                                     chr_val_handle, 0, txom, NULL);
        if (rc != 0) {
            /* Fatal error; application disallowed attribute read. */
            rc = BLE_HS_EAPP;
            goto done;
        }
    }

    rc = ble_att_clt_tx_notify(conn_handle, chr_val_handle, txom);
    txom = NULL;
    if (rc != 0) {
        goto done;
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, notify_fail);
    }

    /* Tell the application that a notification transmission was attempted. */
    ble_gap_notify_tx_event(rc, conn_handle, chr_val_handle, 0);

    os_mbuf_free_chain(txom);

    return rc;
}

#if MYNEWT_VAL(BLE_GATTC)
int
ble_gatts_notify_multiple_custom(uint16_t conn_handle,
                                 size_t chr_count,
                                 struct ble_gatt_notif *tuples)
{
#if !MYNEWT_VAL(BLE_GATT_NOTIFY_MULTIPLE)
    return BLE_HS_ENOTSUP;
#endif

    int rc = 0;
    int i = 0;
    uint16_t cur_chr_cnt = 0;
    /* mtu = MTU - 1 octet (OP code) */
    uint16_t mtu = ble_att_mtu(conn_handle) - 1;
    struct os_mbuf *txom;
    struct ble_hs_conn *conn;

    txom = ble_hs_mbuf_att_pkt();
    if (txom == NULL) {
        return BLE_HS_ENOMEM;
    }

    conn = ble_hs_conn_find(conn_handle);
    if (conn == NULL) {
        return ENOTCONN;
    }

    STATS_INC(ble_gattc_stats, multi_notify);
    ble_gattc_log_multi_notify(tuples, chr_count);

    /* Read missing values */
    for (i = 0; i < chr_count; i++) {
        if (tuples->handle == 0) {
            rc = BLE_HS_EINVAL;
            goto done;
        }
        if (tuples[i].value == NULL) {
            /* No custom attribute data; read the value from the specified
             * attribute
             */
            rc = ble_att_svr_read_local(tuples[i].handle, &tuples[i].value);
            if (rc != 0) {
                BLE_HS_LOG(ERROR, "Attribute read failed (err=0x%02x)", rc);
                goto done;
            }
        }
    }

    /* If peer does not support fall back to multiple single value
     * Notifications */
    if ((conn->bhc_gatt_svr.peer_cl_sup_feat[0] & 0x04) == 0) {
        for (i = 0; i < chr_count; i++) {
            rc = ble_att_clt_tx_notify(conn_handle, tuples[i].handle, tuples[i].value);
            if (rc != 0) {
                goto done;
            }
        }
        goto done;
    }

    for (i = 0; i < chr_count; i++) {
        if (OS_MBUF_PKTLEN(txom) + OS_MBUF_PKTLEN(tuples[i].value) > mtu && cur_chr_cnt < 2) {
            rc = ble_att_clt_tx_notify(conn_handle, tuples[i].handle,
                                       tuples[i].value);
            if (rc != 0) {
                goto done;
            }
            continue;
        } else if (OS_MBUF_PKTLEN(txom) + OS_MBUF_PKTLEN(tuples[i].value) > mtu) {
            rc = ble_att_clt_tx_notify_mult(conn_handle, txom);
            if (rc != 0) {
                goto done;
            }
            cur_chr_cnt = 0;
            /* buffer was consumed, allocate new one */
            txom = ble_hs_mbuf_att_pkt();
            if (txom == NULL) {
                return BLE_HS_ENOMEM;
            }
        }

        /* Handle */
        os_mbuf_append(txom, &tuples[i].handle, sizeof(uint16_t));

        /* Length */
        os_mbuf_append(txom, &OS_MBUF_PKTLEN(tuples[i].value),
                       sizeof(uint16_t));

        /* Value */
        os_mbuf_concat(txom, tuples[i].value);
        cur_chr_cnt++;
    }

    if (cur_chr_cnt == 1) {
        rc = ble_att_clt_tx_notify(conn_handle, tuples[chr_count].handle,
                                   tuples[chr_count].value);
    } else {
        rc = ble_att_clt_tx_notify_mult(conn_handle, txom);
    }

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, multi_notify_fail);
    }

    /* Tell the application that multiple notification transmissions were attempted. */
    for (i = 0; i < chr_count; i++) {
        ble_gap_notify_tx_event(rc, conn_handle, tuples[i].handle, 0);
    }
    return rc;
}

/**
 * Deprecated. Should not be used. Use ble_gatts_notify_custom instead.
 */
int
ble_gattc_notify_custom(uint16_t conn_handle, uint16_t chr_val_handle,
                        struct os_mbuf *txom)
{
    return ble_gatts_notify_custom(conn_handle, chr_val_handle, txom);
}
#endif

int
ble_gatts_notify(uint16_t conn_handle, uint16_t chr_val_handle)
{
#if !MYNEWT_VAL(BLE_GATT_NOTIFY)
    return BLE_HS_ENOTSUP;
#endif

    int rc;

    rc = ble_gatts_notify_custom(conn_handle, chr_val_handle, NULL);

    return rc;
}

#if MYNEWT_VAL(BLE_GATTC)
/**
 * Deprecated. Should not be used. Use ble_gatts_notify instead.
 */
int
ble_gattc_notify(uint16_t conn_handle, uint16_t chr_val_handle)
{
    return ble_gatts_notify(conn_handle, chr_val_handle);
}

#endif

/*****************************************************************************
 * $indicate                                                                 *
 *****************************************************************************/
#if MYNEWT_VAL(BLE_GATTS)
/**
 * Handles an incoming ATT error response for the specified indication proc.
 * A device should never send an error in response to an indication.  If this
 * happens, we treat it like a confirmation (indication ack), but report the
 * error status to the application.
 */
static void
ble_gatts_indicate_err(struct ble_gattc_proc *proc, int status,
                       uint16_t att_handle)
{
    int rc;

    ble_gattc_dbg_assert_proc_not_inserted(proc);

    if (status != BLE_HS_ENOTCONN) {
        rc = ble_gatts_rx_indicate_ack(proc->conn_handle,
                                       proc->indicate.chr_val_handle);
        if (rc != 0) {
            return;
        }
    }

    /* Tell the application about the received acknowledgment. */
    ble_gap_notify_tx_event(status, proc->conn_handle,
                            proc->indicate.chr_val_handle, 1);

    /* Send the next indication if one is pending. */
    ble_gatts_send_next_indicate(proc->conn_handle);
}
#endif

#if MYNEWT_VAL(BLE_GATTC)
static void
ble_gatts_indicate_tmo(struct ble_gattc_proc *proc)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    ble_gattc_dbg_assert_proc_not_inserted(proc);

    ble_gap_notify_tx_event(BLE_HS_ETIMEOUT, proc->conn_handle,
                            proc->indicate.chr_val_handle, 1);
}

/**
 * Handles an incoming handle-value-confirmation for the specified indication
 * proc.
 */
static void
ble_gatts_indicate_rx_rsp(struct ble_gattc_proc *proc)
{
    ble_gattc_dbg_assert_proc_not_inserted(proc);
#if MYNEWT_VAL(BLE_GATTS)
    int rc;

    rc = ble_gatts_rx_indicate_ack(proc->conn_handle,
                                   proc->indicate.chr_val_handle);
    if (rc != 0) {
        return;
    }
#endif

    /* Tell the application about the received acknowledgment. */
    ble_gap_notify_tx_event(BLE_HS_EDONE, proc->conn_handle,
                            proc->indicate.chr_val_handle, 1);
#if MYNEWT_VAL(BLE_GATTS)
    /* Send the next indication if one is pending. */
    ble_gatts_send_next_indicate(proc->conn_handle);
#endif
}

#endif

/**
 * Causes the indication in progress for the specified connection (if any) to
 * fail with a status code of BLE_HS_ENOTCONN;
 */
void
ble_gatts_indicate_fail_notconn(uint16_t conn_handle)
{
    ble_gattc_fail_procs(conn_handle, BLE_GATT_OP_INDICATE, BLE_HS_ENOTCONN);
}

int
ble_gatts_indicate_custom(uint16_t conn_handle, uint16_t chr_val_handle,
                          struct os_mbuf *txom)
{
#if !MYNEWT_VAL(BLE_GATT_INDICATE)
    return BLE_HS_ENOTSUP;
#endif

    struct ble_gattc_proc *proc;
    struct ble_hs_conn *conn;
    int rc;
#if MYNEWT_VAL(BLE_GATT_CACHING)
    bool aware;
#endif

    STATS_INC(ble_gattc_stats, indicate);

    proc = ble_gattc_proc_alloc();
    if (proc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_gattc_proc_prepare(proc, conn_handle, BLE_GATT_OP_INDICATE);

    proc->indicate.chr_val_handle = chr_val_handle;

    ble_gattc_log_indicate(chr_val_handle);

#if MYNEWT_VAL(BLE_GATT_CACHING)
    if(chr_val_handle != ble_svc_gatt_changed_handle()) {
        ble_hs_lock();
        rc = ble_gatts_check_conn_aware(conn_handle, &aware);
        ble_hs_unlock();
        if(rc != 0) {
            goto done;
        }
        if(!aware) {
            rc = BLE_HS_EREJECT;
            goto done;
        }
    }
#endif
    if (txom == NULL) {
        /* No custom attribute data; read the value from the specified
         * attribute.
         */
        txom = ble_hs_mbuf_att_pkt();
        if (txom == NULL) {
            rc = BLE_HS_ENOMEM;
            goto done;
        }

        rc = ble_att_svr_read_handle(BLE_HS_CONN_HANDLE_NONE, chr_val_handle,
                                     0, txom, NULL);
        if (rc != 0) {
            /* Fatal error; application disallowed attribute read. */
            BLE_HS_DBG_ASSERT(0);
            rc = BLE_HS_EAPP;
            goto done;
        }
    }

    rc = ble_att_clt_tx_indicate(conn_handle, proc->cid, chr_val_handle, txom);
    txom = NULL;
    if (rc != 0) {
        goto done;
    }

    ble_hs_lock();
    conn = ble_hs_conn_find(conn_handle);
    if (conn != NULL) {
        BLE_HS_DBG_ASSERT(conn->bhc_gatt_svr.indicate_val_handle == 0);
        conn->bhc_gatt_svr.indicate_val_handle = chr_val_handle;
    }
    ble_hs_unlock();

done:
    if (rc != 0) {
        STATS_INC(ble_gattc_stats, indicate_fail);
    }

    /* Tell the application that an indication transmission was attempted. */
    ble_gap_notify_tx_event(rc, conn_handle, chr_val_handle, 1);

    ble_gattc_process_status(proc, rc);
    os_mbuf_free_chain(txom);
    return rc;
}

#if MYNEWT_VAL(BLE_GATTC)
/**
 * Deprecated. Should not be used. Use ble_gatts_indicate_custom instead.
 */
int
ble_gattc_indicate_custom(uint16_t conn_handle, uint16_t chr_val_handle,
                          struct os_mbuf *txom)
{
    return ble_gatts_indicate_custom(conn_handle, chr_val_handle, txom);
}
#endif


int
ble_gatts_indicate(uint16_t conn_handle, uint16_t chr_val_handle)
{
    return ble_gatts_indicate_custom(conn_handle, chr_val_handle, NULL);
}


#if MYNEWT_VAL(BLE_GATTC)
/**
 * Deprecated. Should not be used. Use ble_gatts_indicate instead.
 */
int
ble_gattc_indicate(uint16_t conn_handle, uint16_t chr_val_handle)
{
    return ble_gatts_indicate(conn_handle, chr_val_handle);
}

/*****************************************************************************
 * $rx                                                                       *
 *****************************************************************************/

/**
 * Dispatches an incoming ATT error-response to the appropriate active GATT
 * procedure.
 */
void
ble_gattc_rx_err(uint16_t conn_handle, uint16_t cid, uint16_t handle, uint16_t status)
{
    struct ble_gattc_proc *proc;
    ble_gattc_err_fn *err_cb;
#if MYNEWT_VAL(BLE_GATTC_AUTO_PAIR)
    struct ble_gap_conn_desc desc;
    int rc;
#endif

    proc = ble_gattc_extract_first_by_conn_cid_op(conn_handle, cid, BLE_GATT_OP_NONE);
#if MYNEWT_VAL(BLE_GATTC_AUTO_PAIR)
    ble_gap_conn_find(conn_handle, &desc);
    proc->error.att_handle = handle;
    proc->error.status = status;
#endif

    if (proc != NULL) {
#if MYNEWT_VAL(BLE_GATTC_AUTO_PAIR)
        if (desc.sec_state.encrypted == 0 &&
            (status == BLE_ATT_ERR_INSUFFICIENT_ENC ||
             status == BLE_ATT_ERR_INSUFFICIENT_AUTHEN)) {
            rc = ble_gap_security_initiate(conn_handle);
            if (rc == 0) {
                STAILQ_INSERT_TAIL(&ble_gattc_cached_procs, proc, next);
                return;
            }
        }
#endif
        err_cb = ble_gattc_err_dispatch_get(proc->op);
        if (err_cb != NULL) {
            err_cb(proc, BLE_HS_ERR_ATT_BASE + status, handle);
        }
        ble_gattc_proc_free(proc);
    }
}

/**
 * Dispatches an incoming ATT exchange-mtu-response to the appropriate active
 * GATT procedure.
 */
void
ble_gattc_rx_mtu(uint16_t conn_handle, uint16_t cid, int status, uint16_t chan_mtu)
{
    struct ble_gattc_proc *proc;

    assert(cid == BLE_L2CAP_CID_ATT);

    proc = ble_gattc_extract_first_by_conn_cid_op(conn_handle, BLE_L2CAP_CID_ATT, BLE_GATT_OP_MTU);
    if (proc != NULL) {
        ble_gattc_mtu_cb(proc, status, 0, chan_mtu);
        ble_gattc_process_status(proc, BLE_HS_EDONE);
    }
}

/**
 * Dispatches an incoming "information data" entry from a
 * find-information-response to the appropriate active GATT procedure.
 */
void
ble_gattc_rx_find_info_idata(uint16_t conn_handle, uint16_t cid,
                             struct ble_att_find_info_idata *idata)
{
#if !NIMBLE_BLE_ATT_CLT_FIND_INFO
    return;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    proc = ble_gattc_extract_first_by_conn_cid_op(conn_handle, cid,
                                              BLE_GATT_OP_DISC_ALL_DSCS);
    if (proc != NULL) {
        rc = ble_gattc_disc_all_dscs_rx_idata(proc, idata);
        ble_gattc_process_status(proc, rc);
    }
}

/**
 * Dispatches an incoming notification of the end of a
 * find-information-response to the appropriate active GATT procedure.
 */
void
ble_gattc_rx_find_info_complete(uint16_t conn_handle, uint16_t cid, int status)
{
#if !NIMBLE_BLE_ATT_CLT_FIND_INFO
    return;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    proc = ble_gattc_extract_first_by_conn_cid_op(conn_handle, cid,
                                                  BLE_GATT_OP_DISC_ALL_DSCS);
    if (proc != NULL) {
        rc = ble_gattc_disc_all_dscs_rx_complete(proc, status);
        ble_gattc_process_status(proc, rc);
    }
}

/**
 * Dispatches an incoming "handles info" entry from a
 * find-by-type-value-response to the appropriate active GATT procedure.
 */
void
ble_gattc_rx_find_type_value_hinfo(uint16_t conn_handle, uint16_t cid,
                                   struct ble_att_find_type_value_hinfo *hinfo)
{
#if !NIMBLE_BLE_ATT_CLT_FIND_TYPE
    return;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    proc = ble_gattc_extract_first_by_conn_cid_op(conn_handle, cid,
                                              BLE_GATT_OP_DISC_SVC_UUID);
    if (proc != NULL) {
        rc = ble_gattc_disc_svc_uuid_rx_hinfo(proc, hinfo);
        ble_gattc_process_status(proc, rc);
    }
}

/**
 * Dispatches an incoming notification of the end of a
 * find-by-type-value-response to the appropriate active GATT procedure.
 */
void
ble_gattc_rx_find_type_value_complete(uint16_t conn_handle, uint16_t cid, int status)
{
#if !NIMBLE_BLE_ATT_CLT_FIND_TYPE
    return;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    proc = ble_gattc_extract_first_by_conn_cid_op(conn_handle, cid,
                                                  BLE_GATT_OP_DISC_SVC_UUID);
    if (proc != NULL) {
        rc = ble_gattc_disc_svc_uuid_rx_complete(proc, status);
        ble_gattc_process_status(proc, rc);
    }
}

/**
 * Dispatches an incoming "attribute data" entry from a read-by-type-response
 * to the appropriate active GATT procedure.
 */
void
ble_gattc_rx_read_type_adata(uint16_t conn_handle, uint16_t cid,
                             struct ble_att_read_type_adata *adata)
{
#if !NIMBLE_BLE_ATT_CLT_READ_TYPE
    return;
#endif

    const struct ble_gattc_rx_adata_entry *rx_entry;
    struct ble_gattc_proc *proc;
    int rc;

    proc = BLE_GATTC_RX_EXTRACT_RX_ENTRY(conn_handle, cid,
                                         ble_gattc_rx_read_type_elem_entries,
                                         &rx_entry);
    if (proc != NULL) {
        rc = rx_entry->cb(proc, adata);
        ble_gattc_process_status(proc, rc);
    }
}

/**
 * Dispatches an incoming notification of the end of a read-by-type-response to
 * the appropriate active GATT procedure.
 */
void
ble_gattc_rx_read_type_complete(uint16_t conn_handle, uint16_t cid, int status)
{
#if !NIMBLE_BLE_ATT_CLT_READ_TYPE
    return;
#endif

    const struct ble_gattc_rx_complete_entry *rx_entry;
    struct ble_gattc_proc *proc;
    int rc;

    proc = BLE_GATTC_RX_EXTRACT_RX_ENTRY(
        conn_handle, cid, ble_gattc_rx_read_type_complete_entries,
        &rx_entry);
    if (proc != NULL) {
        rc = rx_entry->cb(proc, status);
        ble_gattc_process_status(proc, rc);
    }
}

/**
 * Dispatches an incoming "attribute data" entry from a
 * read-by-group-type-response to the appropriate active GATT procedure.
 */
void
ble_gattc_rx_read_group_type_adata(uint16_t conn_handle, uint16_t cid,
                                   struct ble_att_read_group_type_adata *adata)
{
#if !NIMBLE_BLE_ATT_CLT_READ_GROUP_TYPE
    return;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    proc = ble_gattc_extract_first_by_conn_cid_op(conn_handle, cid,
                                              BLE_GATT_OP_DISC_ALL_SVCS);
    if (proc != NULL) {
        rc = ble_gattc_disc_all_svcs_rx_adata(proc, adata);
        ble_gattc_process_status(proc, rc);
    }
}

/**
 * Dispatches an incoming notification of the end of a
 * read-by-group-type-response to the appropriate active GATT procedure.
 */
void
ble_gattc_rx_read_group_type_complete(uint16_t conn_handle, uint16_t cid, int status)
{
#if !NIMBLE_BLE_ATT_CLT_READ_GROUP_TYPE
    return;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    proc = ble_gattc_extract_first_by_conn_cid_op(conn_handle, cid,
                                                  BLE_GATT_OP_DISC_ALL_SVCS);
    if (proc != NULL) {
        rc = ble_gattc_disc_all_svcs_rx_complete(proc, status);
        ble_gattc_process_status(proc, rc);
    }
}

/**
 * Dispatches an incoming ATT read-response to the appropriate active GATT
 * procedure.
 */
void
ble_gattc_rx_read_rsp(uint16_t conn_handle, uint16_t cid, int status, struct os_mbuf **om)
{
#if !NIMBLE_BLE_ATT_CLT_READ
    return;
#endif

    const struct ble_gattc_rx_attr_entry *rx_entry;
    struct ble_gattc_proc *proc;
    int rc;

    proc = BLE_GATTC_RX_EXTRACT_RX_ENTRY(conn_handle, cid,
                                         ble_gattc_rx_read_rsp_entries,
                                         &rx_entry);
    if (proc != NULL) {
        rc = rx_entry->cb(proc, status, om);
        ble_gattc_process_status(proc, rc);
    }
}

/**
 * Dispatches an incoming ATT read-blob-response to the appropriate active GATT
 * procedure.
 */
void
ble_gattc_rx_read_blob_rsp(uint16_t conn_handle, uint16_t cid, int status,
                           struct os_mbuf **om)
{
#if !NIMBLE_BLE_ATT_CLT_READ_BLOB
    return;
#endif

    struct ble_gattc_proc *proc;
    int rc;

    proc = ble_gattc_extract_first_by_conn_cid_op(conn_handle, cid,
                                              BLE_GATT_OP_READ_LONG);
    if (proc != NULL) {
        rc = ble_gattc_read_long_rx_read_rsp(proc, status, om);
        ble_gattc_process_status(proc, rc);
    }
}

/**
 * Dispatches an incoming ATT read-multiple-response to the appropriate active
 * GATT procedure.
 */
void
ble_gattc_rx_read_mult_rsp(uint16_t conn_handle, uint16_t cid, int status,
                           struct os_mbuf **om, bool variable)
{
#if !NIMBLE_BLE_ATT_CLT_READ_MULT
    return;
#endif

    struct ble_gattc_proc *proc;
    uint8_t op;

    op = variable ? BLE_GATT_OP_READ_MULT_VAR : BLE_GATT_OP_READ_MULT;

    proc = ble_gattc_extract_first_by_conn_cid_op(conn_handle, cid, op);
    if (proc != NULL) {
        ble_gattc_read_mult_cb(proc, status, 0, om);
        ble_gattc_process_status(proc, BLE_HS_EDONE);
    }
}

/**
 * Dispatches an incoming ATT write-response to the appropriate active GATT
 * procedure.
 */
void
ble_gattc_rx_write_rsp(uint16_t conn_handle, uint16_t cid)
{
#if !NIMBLE_BLE_ATT_CLT_WRITE
    return;
#endif

    struct ble_gattc_proc *proc;

    proc = ble_gattc_extract_first_by_conn_cid_op(conn_handle, cid,
                                              BLE_GATT_OP_WRITE);
    if (proc != NULL) {
        ble_gattc_write_cb(proc, 0, 0);
        ble_gattc_process_status(proc, BLE_HS_EDONE);
    }
}

/**
 * Dispatches an incoming ATT prepare-write-response to the appropriate active
 * GATT procedure.
 */
void
ble_gattc_rx_prep_write_rsp(uint16_t conn_handle, uint16_t cid, int status,
                            uint16_t handle, uint16_t offset,
                            struct os_mbuf **om)
{
#if !NIMBLE_BLE_ATT_CLT_PREP_WRITE
    return;
#endif

    const struct ble_gattc_rx_prep_entry *rx_entry;
    struct ble_gattc_proc *proc;
    int rc;

    proc = BLE_GATTC_RX_EXTRACT_RX_ENTRY(conn_handle, cid,
                                         ble_gattc_rx_prep_entries,
                                         &rx_entry);
    if (proc != NULL) {
        rc = rx_entry->cb(proc, status, handle, offset, om);
        ble_gattc_process_status(proc, rc);
    }
}

/**
 * Dispatches an incoming ATT execute-write-response to the appropriate active
 * GATT procedure.
 */
void
ble_gattc_rx_exec_write_rsp(uint16_t conn_handle, uint16_t cid, int status)
{
#if !NIMBLE_BLE_ATT_CLT_EXEC_WRITE
    return;
#endif

    const struct ble_gattc_rx_exec_entry *rx_entry;
    struct ble_gattc_proc *proc;
    int rc;

    proc = BLE_GATTC_RX_EXTRACT_RX_ENTRY(conn_handle, cid,
                                         ble_gattc_rx_exec_entries, &rx_entry);
    if (proc != NULL) {
        rc = rx_entry->cb(proc, status);
        ble_gattc_process_status(proc, rc);
    }
}

#if MYNEWT_VAL(BLE_GATTC)
/**
 * Dispatches an incoming ATT handle-value-confirmation to the appropriate
 * active GATT procedure.
 */
void
ble_gatts_rx_indicate_rsp(uint16_t conn_handle, uint16_t cid)
{
#if !NIMBLE_BLE_ATT_CLT_INDICATE
    return;
#endif

    struct ble_gattc_proc *proc;

    proc = ble_gattc_extract_first_by_conn_cid_op(conn_handle, cid,
                                              BLE_GATT_OP_INDICATE);
    if (proc != NULL) {
        ble_gatts_indicate_rx_rsp(proc);
        ble_gattc_process_status(proc, BLE_HS_EDONE);
    }
}
#endif

/*****************************************************************************
 * $misc                                                                     *
 *****************************************************************************/

/**
 * Called when a BLE connection ends.  Frees all GATT resources associated with
 * the connection and cancels all relevant pending and in-progress GATT
 * procedures.
 *
 * @param conn_handle           The handle of the connection that was
 *                                  terminated.
 */
void
ble_gattc_connection_broken(uint16_t conn_handle)
{
    struct ble_hs_conn *conn;
    struct os_mbuf_pkthdr *omp;

    ble_gattc_fail_procs(conn_handle, BLE_GATT_OP_NONE, BLE_HS_ENOTCONN);

    ble_hs_lock();
    conn = ble_hs_conn_find(conn_handle);
    ble_hs_unlock();

    while ((omp = STAILQ_FIRST(&conn->att_tx_q)) != NULL) {
        STAILQ_REMOVE_HEAD(&conn->att_tx_q, omp_next);
        os_mbuf_free_chain(OS_MBUF_PKTHDR_TO_MBUF(omp));
    }
}

/**
 * Indicates whether there are currently any active GATT client procedures.
 */
int
ble_gattc_any_jobs(void)
{
    return !STAILQ_EMPTY(&ble_gattc_procs);
}

#endif

int
ble_gattc_init(void)
{
    int rc;

#if MYNEWT_VAL(BLE_GATTC_PROC_PREEMPTION_PROTECT)
    STAILQ_INIT(&temp_proc_list);
#endif
    STAILQ_INIT(&ble_gattc_procs);
#if MYNEWT_VAL(BLE_GATTC_AUTO_PAIR)
    STAILQ_INIT(&ble_gattc_cached_procs);
#endif

    if (MYNEWT_VAL(BLE_GATT_MAX_PROCS) > 0) {
        rc = os_mempool_init(&ble_gattc_proc_pool,
                             MYNEWT_VAL(BLE_GATT_MAX_PROCS),
                             sizeof (struct ble_gattc_proc),
                             ble_gattc_proc_mem,
                             "ble_gattc_proc_pool");
        if (rc != 0) {
            return rc;
        }
    }

    rc = stats_init_and_reg(
        STATS_HDR(ble_gattc_stats), STATS_SIZE_INIT_PARMS(ble_gattc_stats,
        STATS_SIZE_32), STATS_NAME_INIT_PARMS(ble_gattc_stats), "ble_gattc");
    if (rc != 0) {
        return BLE_HS_EOS;
    }

    return 0;
}

#endif
