/* AX200 register subset, from OpenBSD if_iwxreg.h (BSD license).
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 * Copyright(c) 2020 - 2024 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name Intel Corporation nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef OSITOK_IWXREG_H
#define OSITOK_IWXREG_H

#define IWX_HW_IF_CONFIG         0x000
#define IWX_INT                  0x008
#define IWX_INT_MASK             0x00c
#define IWX_RESET                0x020
#define IWX_GP_CNTRL             0x024
#define IWX_HW_REV               0x028
#define IWX_GIO                  0x03c
#define IWX_MBOX_SET             0x088
#define IWX_HW_RF_ID             0x09c
#define IWX_GIO_CHICKEN          0x100
#define IWX_DBG_HPET             0x240
#define IWX_DBG_LINK_PWR         0x250
#define IWX_PRPH_WADDR           0x444
#define IWX_PRPH_RADDR           0x448
#define IWX_PRPH_WDAT            0x44c
#define IWX_PRPH_RDAT            0x450
#define IWX_PRPH_ADDR_MASK_22000 0x000fffffU
#define IWX_PRPH_DWORD           (3U << 24)
#define IWX_HPM_DEBUG            0xa03440
#define IWX_PRPH_WPROT           0xa04d00
#define IWX_PERSISTENCE          0x00001000
#define IWX_WFPM_ACCESS          0x00001000
#define IWX_NIC_READY            0x00400000
#define IWX_PREPARE              0x08000000
#define IWX_HAP_WAKE_L1A         0x00080000
#define IWX_LINK_PWR_DISABLED    0x80000000
#define IWX_SW_RESET             0x00000080
#define IWX_MASTER_DISABLED      0x00000100
#define IWX_STOP_MASTER          0x00000200
#define IWX_CLOCK_READY          0x00000001
#define IWX_INIT_DONE            0x00000004
#define IWX_RF_ENABLED           0x08000000
#define IWX_OS_ALIVE             0x00000020
#define IWX_L1A_NO_L0S_RX        0x00800000
#define IWX_L0S_DISABLED         0x00000002
#define IWX_HPET_MAX             0xffff0000

#define IWX_TLV_MAGIC            0x0a4c5749
#define IWX_TLV_SEC_RT           19
#define IWX_TLV_PHY_SKU          23
#define IWX_TLV_NUM_CPU          27
#define IWX_TLV_API_CHANGES      29
#define IWX_TLV_CAPABILITIES     30
#define IWX_TLV_SCAN_CHANNELS    31
#define IWX_TLV_PAGING           32
#define IWX_TLV_FW_VERSION       36
#define IWX_TLV_CMD_VERSIONS     48
#define IWX_CPU_SEPARATOR        0xffffcccc
#define IWX_PAGING_SEPARATOR     0xaaaabbbb
#define IWX_MAX_DRAM_ENTRIES     64
#define IWX_MAX_SECTION_SIZE     32768U

#define IWX_INT_COALESCING        0x004 /* byte access */
#define IWX_INT_PERIODIC          0x005 /* byte access */
#define IWX_CTXT_INFO_BA          0x040
#define IWX_UCODE_GP1_CLR         0x05c
#define IWX_SHADOW_CTRL           0x0a8
#define IWX_LTR_LONG              0x0d4
#define IWX_FH_INT                0x010
#define IWX_RX_WIDX               0x1c80
#define IWX_CPU_INIT_RUN          0xa05c44
#define IWX_MAC_ACCESS            0x00000008
#define IWX_GOING_SLEEP           0x00000010
#define IWX_INT_ALIVE             (1U << 0)
#define IWX_INT_SW_ERR            (1U << 25)
#define IWX_INT_HW_ERR            (1U << 29)
#define IWX_INT_CT_KILL           (1U << 6)
#define IWX_FH_ERROR              (1U << 31)
#define IWX_RX_COUNT              512U
#define IWX_RX_INVALID            0x55550000U
#define IWX_HBUS_TARG_WRPTR        0x460
#define IWX_MAC_OTP0              0x380
#define IWX_MAC_OTP1              0x384
#define IWX_MAC_STRAP0            0x388
#define IWX_MAC_STRAP1            0x38c
#define IWX_INIT_COMPLETE         0x04
#define IWX_SYSTEM_GROUP          0x02
#define IWX_INIT_EXTENDED         0x03
#define IWX_NVM_GROUP             0x0c
#define IWX_NVM_COMPLETE          0x00
#define IWX_NVM_GET               0x02
#define IWX_CMD_FAILED            0x40
#define IWX_CMD_COUNT             256U
#define IWX_FIRST_TB_SIZE          20U

#define IWX_LONG_GROUP            1
#define IWX_PHY_GROUP             4
#define IWX_DATA_GROUP            5
#define IWX_TX_ANT_CFG            0x98
#define IWX_BT_CONFIG             0x9b
#define IWX_SOC_CONFIG            0x01
#define IWX_DQA_ENABLE            0x00
#define IWX_TEMP_THRESHOLDS       0x04
#define IWX_DEVICE_POWER          0x77
#define IWX_LTR_CONFIG            0xee
#define IWX_MCC_UPDATE            0xc8
#define IWX_MCC_CHANGED           0xc9
#define IWX_SCAN_CONFIG           0x0c
#define IWX_SCAN_REQUEST          0x0d
#define IWX_SCAN_ABORT            0x0e
#define IWX_SCAN_COMPLETE_NOTIF   0x0f
#define IWX_BEACON_FILTER         0xd2
#define IWX_RX_MPDU               0xc1
#define IWX_PHY_CONTEXT           0x08
#define IWX_RLC_CONFIG            0x08
#define IWX_MAC_CONTEXT           0x28
#define IWX_BINDING_CONTEXT       0x2b
#define IWX_ADD_STATION           0x18
#define IWX_ADD_STA_KEY           0x17
#define IWX_TX_FRAME              0x1c
#define IWX_SCD_QUEUE_CONFIG      0x17
#define IWX_TX_QUEUE_COUNT        512 /* AX200/22000 scheduler queue IDs. */
#define IWX_TX_SEQUENCE_QUEUE_MASK 0x1f /* Echoed legacy header tag, not a queue ID. */
#define IWX_MAC_GROUP             3
#define IWX_SESSION_PROTECTION    0x05
#define IWX_SESSION_NOTIFICATION  0xfb
#define IWX_API_WIFI_MCC          9
#define IWX_API_REDUCED_SCAN       56
#define IWX_API_EXT_SCAN_CHANNEL   58
#define IWX_CAPA_LAR              1
#define IWX_CAPA_DQA              12
#define IWX_CAPA_UMAC_SCAN        2
#define IWX_CAPA_MULTI_MCC        29
#define IWX_CAPA_FW_CT_KILL       74
#define IWX_CAPA_MCC_11AX         89
#define IWX_CAPA_CDB              40
#define IWX_CAPA_UHB_CHANNEL_ABI  48
#define IWX_CAPA_SESSION          54
#define IWX_CHANNEL_VALID        (1U << 0)
#define IWX_CHANNEL_ACTIVE       (1U << 3)
#define IWX_SCAN_PASS_ALL        (1U << 1)
#define IWX_SCAN_ITER_COMPLETE   (1U << 2)
#define IWX_SCAN_ADAPTIVE_DWELL   (1U << 7)
#define IWX_SCAN_FORCE_PASSIVE   (1U << 11)
#define IWX_SCAN_CHANNEL_ORDER   (1U << 5)

typedef struct __attribute__((packed)) {
    uint16_t length;
    uint64_t address;
} iwx_tfh_tb_t;
typedef struct __attribute__((packed)) {
    uint16_t num_tbs;
    iwx_tfh_tb_t tb[25];
    uint32_t reserved;
} iwx_tfh_tfd_t;
typedef struct __attribute__((packed)) {
    uint8_t opcode, group, index, queue;
    uint16_t length;
    uint8_t reserved, version;
} iwx_cmd_header_t;
_Static_assert(sizeof(iwx_tfh_tfd_t) == 256, "TFH descriptor ABI");
_Static_assert(__builtin_offsetof(iwx_tfh_tfd_t, tb) == 2, "TFH buffers ABI");
_Static_assert(sizeof(iwx_cmd_header_t) == 8, "wide command ABI");

/* Read-only error snapshots: Intel iwl-fh.h / iwl-prph.h, 22000 family. */
#define IWX_RFH_RXF_DMA_CFG        0xa09820
#define IWX_RFH_GEN_CFG            0xa09800
#define IWX_RFH_GEN_STATUS         0xa09808
#define IWX_FH_TX_STATUS           0x001eb0
#define IWX_FH_TX_ERROR            0x001eb8
#define IWX_SB_CPU1_STATUS         0xa01e30
#define IWX_SB_CPU2_STATUS         0xa01e34
#define IWX_UMAC_CPU1_STATUS       0xa038c0
#define IWX_UMAC_CPU2_STATUS       0xa038c4
#define IWX_UCODE_LOAD_STATUS      0xa05c40

/* Intel 22000 context-info ABI, little endian on this x86-only transport. */
typedef struct __attribute__((packed)) {
    uint16_t mac_id, version, size_dwords, reserved_version;
    uint32_t control_flags, reserved_control;
    uint64_t reserved0;
    uint64_t free_rbd, used_rbd, status;
    uint64_t cmd_queue;
    uint8_t cmd_queue_size, reserved_cmd[7];
    uint32_t reserved1[4];
    uint8_t dump[16], early_debug[16], pnvm[16];
    uint32_t reserved2[16];
    uint64_t umac[64], lmac[64], paging[64];
    uint32_t reserved3[16];
} iwx_context_t;
_Static_assert(sizeof(iwx_context_t) == 1792, "context-info size");
_Static_assert(__builtin_offsetof(iwx_context_t, control_flags) == 8, "control ABI");
_Static_assert(__builtin_offsetof(iwx_context_t, free_rbd) == 24, "RX config ABI");
_Static_assert(__builtin_offsetof(iwx_context_t, used_rbd) == 32, "RX completion ABI");
_Static_assert(__builtin_offsetof(iwx_context_t, status) == 40, "RX status ABI");
_Static_assert(__builtin_offsetof(iwx_context_t, cmd_queue) == 48, "command queue ABI");
_Static_assert(__builtin_offsetof(iwx_context_t, cmd_queue_size) == 56, "command size ABI");
_Static_assert(__builtin_offsetof(iwx_context_t, umac) == 192, "DRAM map ABI");
_Static_assert(__builtin_offsetof(iwx_context_t, lmac) == 704, "LMAC map ABI");
_Static_assert(__builtin_offsetof(iwx_context_t, paging) == 1216, "paging map ABI");

#endif
