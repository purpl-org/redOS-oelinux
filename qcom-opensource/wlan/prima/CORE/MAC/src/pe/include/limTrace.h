/*
 * Copyright (c) 2012-2015 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

/*

*  \file  limTrace.h

*  \brief definition for trace related APIs

*  \author Sunit Bhatia


  ========================================================================*/



#ifndef __LIM_TRACE_H
#define __LIM_TRACE_H

#include "limGlobal.h"
#include "macTrace.h"
#include "vos_trace.h"
#ifdef LIM_TRACE_RECORD



#define LIM_TRACE_GET_SSN(data)    (((data) >> 16) & 0xff)
#define LIM_TRACE_GET_SUBTYPE(data)    (data & 0xff)
#define LIM_TRACE_GET_DEFERRED(data) (data & 0x80000000)
#define LIM_TRACE_GET_DEFRD_OR_DROPPED(data) (data & 0xc0000000)

#define LIM_MSG_PROCESSED 0
#define LIM_MSG_DEFERRED   1
#define LIM_MSG_DROPPED     2

#define LIM_TRACE_MAKE_RXMGMT(type, ssn) \
    ((ssn << 16) | (type) )
#define LIM_TRACE_MAKE_RXMSG(msg, action) \
    ((msg) | (action << 30) )




enum {
    TRACE_CODE_MLM_STATE,
    TRACE_CODE_SME_STATE,
    TRACE_CODE_TX_MGMT,
    TRACE_CODE_RX_MGMT,
    TRACE_CODE_RX_MGMT_TSF,
    TRACE_CODE_TX_COMPLETE,
    TRACE_CODE_TX_SME_MSG,
    TRACE_CODE_RX_SME_MSG,
    TRACE_CODE_TX_WDA_MSG,
    TRACE_CODE_RX_WDA_MSG,
    TRACE_CODE_TX_LIM_MSG,
    TRACE_CODE_RX_LIM_MSG,
    TRACE_CODE_TX_CFG_MSG,
    TRACE_CODE_RX_CFG_MSG,
    TRACE_CODE_RX_MGMT_DROP,

    TRACE_CODE_TIMER_ACTIVATE,
    TRACE_CODE_TIMER_DEACTIVATE,
    TRACE_CODE_INFO_LOG
};






void limTraceInit(tpAniSirGlobal pMac);
void lim_register_debug_callback(void);
void limTraceReset(tpAniSirGlobal pMac);
void limTraceUpdateMgmtStat(tpAniSirGlobal pMac, tANI_U8 subtype);
void limTraceDumpMgmtStat(tpAniSirGlobal pMac, tANI_U8 subtype);
tANI_U8* limTraceGetMlmStateString( tANI_U32 mlmState );
tANI_U8* limTraceGetSmeStateString( tANI_U32 smeState );
void limTraceDump(tpAniSirGlobal pMac, tpvosTraceRecord pRecord, tANI_U16 recIndex);
void macTraceMsgTx(tpAniSirGlobal pMac, tANI_U8 session, tANI_U32 data);
void macTraceMsgRx(tpAniSirGlobal pMac, tANI_U8 session, tANI_U32 data);

void macTraceMsgRxNew(tpAniSirGlobal pMac, tANI_U8 module, tANI_U8 session, tANI_U32 data);
void macTraceMsgTxNew(tpAniSirGlobal pMac, tANI_U8 module, tANI_U8 session, tANI_U32 data);
#else

static inline void lim_register_debug_callback(void) {};

#endif //endof LIM_TRACE_RECORD MACRO

#endif

