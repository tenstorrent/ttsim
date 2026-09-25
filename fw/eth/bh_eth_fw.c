// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Mock Blackhole Ethernet base firmware, run on the erisc cores.
#include <stdint.h>

#define PHYS_RD32(addr) (*(volatile uint32_t *)(addr))
#define PHYS_WR32(addr, data) do { *(volatile uint32_t *)(addr) = data; } while (0)
#define PHYS_WR8(addr, data) do { *(volatile uint8_t *)(addr) = data; } while (0)

// boot_results_t at MEM_SYSENG_ETH_RESULTS_BASE_ADDR 0x7CC00 (tt-metal blackhole eth_fw_api.h):
// eth_status @ +0 {postcode @ +0, port_status @ +4, train_status @ +8}, heartbeat @ +0x70,
// eth_live_status @ +512 {rx_link_up @ +4}, eth_fw_ver @ +0x3BC {patch, minor, major},
// local_info @ +0x3C0, remote_info @ +0x3E0 (chip_info_t: asic_location @ +1, eth_id @ +2,
// logical_eth_id @ +3, board_id_hi @ +4, board_id_lo @ +8, asic_id_hi @ +20, asic_id_lo @ +24).
#define POSTCODE_ADDR 0x7CC00
#define TRAIN_STATUS_ADDR 0x7CC08
#define HEARTBEAT_ADDR 0x7CC70
#define HEARTBEAT_SIGNATURE 0xABCD0000u
#define RX_LINK_UP_ADDR 0x7CE04
#define LOCAL_INFO_ADDR 0x7CFC0
#define REMOTE_INFO_ADDR 0x7CFE0
#define POSTCODE_ETH_INIT_PASS 0xC0DEA000u
#define LINK_TRAIN_PASS 2u
// all_eth_mailbox_t at MEM_SYSENG_ETH_MAILBOX_ADDR: four eth_mailbox_t {msg, arg[3]} (eth_fw_api.h).
#define ETH_MAILBOX_ADDR 0x7D000
#define ETH_MSG_CALL_RELEASE_CORE 0xCA110002u // MEM_SYSENG_ETH_MSG_CALL | MEM_SYSENG_ETH_MSG_RELEASE_CORE
#define ETH_MSG_DONE_RELEASE_CORE 0xD0E50002u // MEM_SYSENG_ETH_MSG_DONE | MEM_SYSENG_ETH_MSG_RELEASE_CORE

// ttsim models no link training, so the simulator hands the fw the peered-ness the PHY would have
// reported (1 = this channel has a peer), in the word just below the image. Must match tile.cpp.
#define ETH_FW_PEER_FLAG_ADDR 0x75FFC
// Below it, on peered channels, the chip_info inputs the link handshake would have exchanged, from
// the same peer table the simulator routes on: {asic_location, eth_id, board_id_hi, board_id_lo,
// asic_id_hi, asic_id_lo} for the local then the remote chip, then the remote logical_eth_id.
#define ETH_FW_CHIP_INFO_ADDR 0x75FC8

static void write_chip_info(uint32_t dst, uint32_t src) {
    PHYS_WR8(dst + 1, PHYS_RD32(src + 0)); // asic_location
    PHYS_WR8(dst + 2, PHYS_RD32(src + 4)); // eth_id
    PHYS_WR32(dst + 4, PHYS_RD32(src + 8)); // board_id_hi
    PHYS_WR32(dst + 8, PHYS_RD32(src + 12)); // board_id_lo
    PHYS_WR32(dst + 20, PHYS_RD32(src + 16)); // asic_id_hi
    PHYS_WR32(dst + 24, PHYS_RD32(src + 20)); // asic_id_lo
}

void eth_fw_main(void) {
    uint32_t peered = PHYS_RD32(ETH_FW_PEER_FLAG_ADDR) != 0;
    if (peered) {
        PHYS_WR32(TRAIN_STATUS_ADDR, LINK_TRAIN_PASS);
        write_chip_info(LOCAL_INFO_ADDR, ETH_FW_CHIP_INFO_ADDR);
        write_chip_info(REMOTE_INFO_ADDR, ETH_FW_CHIP_INFO_ADDR + 24);
        PHYS_WR8(REMOTE_INFO_ADDR + 3, PHYS_RD32(ETH_FW_CHIP_INFO_ADDR + 48)); // logical_eth_id
    }
    PHYS_WR32(RX_LINK_UP_ADDR, peered);
    // Last: postcode reports how far eth_init() got, and the host waits for a terminal value.
    PHYS_WR32(POSTCODE_ADDR, POSTCODE_ETH_INIT_PASS);

    for (uint32_t hb = 0;; hb++) {
        PHYS_WR32(HEARTBEAT_ADDR, HEARTBEAT_SIGNATURE | (hb & 0xFFFF));
        // RELEASE_CORE hands this erisc to the code at arg[0], which returns here when it is done.
        // Any other message is left unserviced; the simulator rejects it only when the host posts it (tile.cpp).
        for (uint32_t mailbox = ETH_MAILBOX_ADDR; mailbox < ETH_MAILBOX_ADDR + 64; mailbox += 16) {
            if (PHYS_RD32(mailbox) == ETH_MSG_CALL_RELEASE_CORE) {
                uint32_t entry = PHYS_RD32(mailbox + 4); // arg[0], read before the ack frees the slot
                PHYS_WR32(mailbox, ETH_MSG_DONE_RELEASE_CORE);
                ((void (*)(void))entry)();
            }
        }
    }
}
