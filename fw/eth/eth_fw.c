// SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
// SPDX-License-Identifier: Apache-2.0

// Mock Wormhole Ethernet base firmware, run on the erisc cores.
#include <stdint.h>

#define ETH_HEARTBEAT_ADDR 0x1C
#define BASE_FW_HEARTBEAT_SIGNATURE 0xABCDu

#define PHYS_RD32(addr) (*(volatile uint32_t *)(addr))
#define PHYS_WR32(addr, data) do { *(volatile uint32_t *)(addr) = data; } while (0)

// During device init the host posts a go_msg_t to L1 0x2490, with the run signal in the
// top byte, and waits for the base fw to ack. Mimic silicon: ack RUN_MSG_INIT by clearing the signal.
#define ETH_GO_MSG_ADDR 0x2490
#define RUN_MSG_INIT 0x40u

// Cooperative-yield pointer an active-erisc app calls periodically to let the base fw run its
// housekeeping (CallingIntoCustomerCode.md). We point it at eth_yield_entry (the ABI-preserving shim
// in crt0.S, which calls eth_service) so routing keeps running while metal's app holds the core.
#define ETH_APP_YIELD_PTR_ADDR 0x9020
extern void eth_yield_entry(void);

// Legacy remote-queue the host (UMD) uses to reach a remote chip: it writes a command into this eth
// core's L1 and bumps the request write pointer; the base fw drains it. Layout per tile.cpp.
#define REQ_QUEUE_BASE 0x11080
#define REQ_ROUTING_QUEUE_BASE (REQ_QUEUE_BASE + 0x40)
#define REQ_WRPTR (REQ_QUEUE_BASE + 0x20)
#define REQ_RDPTR (REQ_QUEUE_BASE + 0x30)
#define RESP_QUEUE_BASE (REQ_QUEUE_BASE + 2 * 0xC0)
#define RESP_ROUTING_QUEUE_BASE (RESP_QUEUE_BASE + 0x40)
#define RESP_WRPTR (RESP_QUEUE_BASE + 0x20)
#define CMD_BUF_PTR_MASK 7u
#define CMD_BUF_SIZE_MASK 3u
#define CMD_WR_REQ 0x1u // single 32-bit write
#define CMD_RD_REQ 0x4u // single 32-bit read
#define CMD_RD_DATA 0x8u // response flag written back for a read
#define CMD_DATA_BLOCK 0x40u // payload is a byte block (data field is its size)
#define CMD_DATA_BLOCK_DRAM 0x10u // block source/dest is host DRAM
#define CMD_BROADCAST 0x80u // multicast the (host-DRAM) block to the tensix grid of every targeted chip
#define CMD_ORDERED 0x1000u // ordering hint; satisfied by our synchronous, in-order delivery (masked off before matching)
#define BROADCAST_HEADER_SIZE 32 // grid/chip-mask header prefixing the broadcast payload (ignored: we hit all tensix)
// Per-slot L1 block staging the host/fw use for CMD_DATA_BLOCK payloads.
#define ETH_ROUTING_DATA_BUFFER_ADDR 0x12000
#define MAX_L1_BLOCK_SIZE 1024
#define ROUTING_CMD_SRC_ADDR_TAG 28 // routing-cmd offset of the host-DRAM source tag

#define PCIE_COORD 0xC0u // tile P0
#define PCIE_HOST_WINDOW_BASE 0x800000000ull

// NOC0 command buffer 0: the eth core issues NOC transactions by programming these registers. On WH
// the target coordinate lives in the address' bits 47:36 and the sim applies coordinate translation.
#define NOC0_REGS_BASE 0xFFB20000u
#define NOC_TARG_ADDR_LO 0x0
#define NOC_TARG_ADDR_MID 0x4
#define NOC_RET_ADDR_LO 0xC
#define NOC_RET_ADDR_MID 0x10
#define NOC_CTRL 0x1C
#define NOC_AT_LEN_BE 0x20
#define NOC_CMD_CTRL 0x28
#define NOC_NODE_ID 0x2C
#define NOC_CTRL_WR_POSTED 0x2u // bit1 = write request (posted)
#define NOC_CTRL_RD_NONPOSTED 0x10u // bit4 = nonposted; read otherwise (bit1 clear)
#define NOC_CTRL_MCAST 0x122u // bit1 write | bit5 multicast | bit8 path-reserve (posted)
// WH tensix grid rectangle in NOC0 coords, x=1..9 (packed x|y<<6). The multicast skips the non-tensix
// col 5 / row 6 via the eth core's router_cfg, so it lands on exactly the 80 tensix cores.
#define TENSIX_GRID_START (1u | (1u << 6))
#define TENSIX_GRID_END (9u | (11u << 6))
#define NOC_MAX_PACKET_SIZE 8192u  // max bytes per NOC transaction (WH)
#define RISCV_LOCAL_MEM_BASE 0xFFB00000u // targets at/above this are MMIO: NOC reads capped at 4 bytes

// ETH_TXQ queue 0: move local L1 bytes to the link peer's L1 (eth_send_packet). 16-byte aligned.
#define ETH_TXQ_BASE 0xFFB90000u
#define ETH_TXQ_CMD 0x4
#define ETH_TXQ_TRANSFER_START_ADDR 0x14
#define ETH_TXQ_TRANSFER_SIZE_BYTES 0x18
#define ETH_TXQ_DEST_ADDR 0x1C
#define ETH_TXQ_CMD_START_DATA 2u

// Routing scratch in this core's L1 (16-byte aligned; only used under ETH_FW_FEATURE_ROUTING). Each
// core receives forwarded requests/acks from its link peer here and stages its own sends.
#define SEND_SEQ_ADDR 0x28010 // sender: next link sequence number
#define LAST_RECV_SEQ_ADDR 0x28014 // receiver: last processed inbox sequence number
#define REQ_INBOX_ADDR 0x28100 // peer -> me: forwarded remote-queue requests
#define RESP_INBOX_ADDR 0x28200 // peer -> me: acks / read responses
#define OUTBOX_ADDR 0x28300 // me -> peer: staged header packet
#define NOC_STAGE_ADDR 0x28400 // receiver: single-word NOC source, alignment-matched to the target
#define BLOCK_BUF_ADDR 0x30000 // block payload staged for / landed from the link (16-byte aligned)
#define NOC_BLOCK_STAGE_ADDR 0x32000 // receiver: block NOC source/dest, alignment-matched to the target
// Broadcast payload staging. Offset by the 32-byte header so a host-window read keeps (stage & 63) ==
// (host_src & 63) while the multicast keeps (stage & 15) == (local_addr & 15) == 0. Fits the BLOCK_BUF slot.
#define BCAST_STAGE_ADDR (BLOCK_BUF_ADDR + BROADCAST_HEADER_SIZE)
#define BCAST_CHUNK 4096u

// Multi-hop relay. UMD round-robins each remote-queue command over ALL of an MMIO chip's active eth
// cores, so a command whose target chip is peered by a SIBLING eth core (not this one's link peer) is
// forwarded over the on-chip NOC to that egress core; the egress does the single eth hop and returns the
// reply. The sim writes ROUTE_TABLE (per active eth core): count, then {peer chip mesh coord, that core's
// NOC coord} per active local eth core -- used to find the egress core (or confirm I peer the target).
#define ROUTE_TABLE_ADDR 0x28500
// Egress inbox: one request slot per potential ingress core (indexed by the ingress's route-table index).
#define RELAY_REQ_BASE 0x29000
#define RELAY_SLOT_SIZE 0x20
#define RELAY_LAST_SEQ_BASE 0x29400 // egress: per-ingress last-serviced seq (0 until first relay)
#define RELAY_RESP_ADDR 0x29500     // ingress: reply slot the egress writes back {seq, data}
#define RELAY_SEND_SEQ_ADDR 0x29510 // ingress: next relay sequence number (per ingress core)
#define RELAY_WAIT_ADDR 0x29514     // ingress: 1 while a posted relay awaits its reply (non-blocking)
// Ingress landing buffer for a relayed block read. Distinct from BLOCK_BUF because the ingress keeps
// using BLOCK_BUF for its receiver/egress roles while the relay is in flight, and would clobber it.
#define RELAY_RESP_BLOCK_ADDR 0x35000
// Relay request slot (32 bytes). The whole slot lands in one NOC write, so the egress polls RLY_SEQ safely.
#define RLY_OP 4
#define RLY_SYS_LO 8
#define RLY_SYS_HI 12
#define RLY_SIZE 16
#define RLY_DATA 20
#define RLY_INGRESS 24 // ingress core's NOC coord (reply + block-buffer target)
#define RLY_BLOCK 28   // ingress L1 block-buffer addr (block-write source / block-read dest)

// Link packet (32 bytes, 16-aligned). seq is nonzero and monotonically increasing per sender.
#define PKT_SEQ 0
#define PKT_OP 4
#define PKT_ADDR_LO 8 // ret_addr_lo = sys_addr[31:0]
#define PKT_ADDR_MID 12 // ret_addr_mid = sys_addr[47:32] (target coord + addr[35:32])
#define PKT_SIZE 16
#define PKT_DATA 20
#define PKT_BYTES 32
#define OP_WRITE 1u // single-word write; PKT_DATA carries the word
#define OP_READ 2u // single-word read
#define OP_ACK 3u // write/block-write done
#define OP_RESP 4u // single-word read done; PKT_DATA carries the read word
#define OP_BLOCK_WRITE 5u // block write; the block precedes this header at BLOCK_BUF
#define OP_BLOCK_READ 6u // block read; PKT_SIZE bytes wanted
#define OP_BLOCK_RESP 7u // block read done; the block precedes this header at BLOCK_BUF
#define OP_BROADCAST 8u // multicast the accompanying block (staged at BCAST_STAGE) to the receiver's tensix grid

// Copy `n` bytes (rounded up to a word) between eth L1 addresses; used to realign block payloads.
// not safe if dst is inside [src, src + n)
static void l1_copy(uint32_t dst, uint32_t src, uint32_t n) {
    for (uint32_t i = 0; i < n; i += 4) {
        PHYS_WR32(dst + i, PHYS_RD32(src + i));
    }
}
static inline uint32_t round16(uint32_t n) { return (n + 15u) & ~15u; }

// Trigger the command programmed into NOC command buffer 0 and wait for the NIU to accept it
static void noc_trigger(void) {
    PHYS_WR32(NOC0_REGS_BASE + NOC_CMD_CTRL, 1);
    while (PHYS_RD32(NOC0_REGS_BASE + NOC_CMD_CTRL)) {
    }
}

// Issue a NOC unicast write of `size` bytes from local L1 `src` to the target encoded by
// (ret_lo, ret_mid) on this chip. `src` must satisfy (src & 15) == (ret_lo & 15).
static void noc_write(uint32_t ret_lo, uint32_t ret_mid, uint32_t src, uint32_t size) {
    uint32_t my_coord = PHYS_RD32(NOC0_REGS_BASE + NOC_NODE_ID) & 0xFFF; // physical coord (identity remap on NOC0)
    PHYS_WR32(NOC0_REGS_BASE + NOC_TARG_ADDR_LO, src);
    PHYS_WR32(NOC0_REGS_BASE + NOC_TARG_ADDR_MID, my_coord << 4); // src is my own L1 (< 4 GiB)
    PHYS_WR32(NOC0_REGS_BASE + NOC_RET_ADDR_LO, ret_lo);
    PHYS_WR32(NOC0_REGS_BASE + NOC_RET_ADDR_MID, ret_mid);
    PHYS_WR32(NOC0_REGS_BASE + NOC_CTRL, NOC_CTRL_WR_POSTED);
    PHYS_WR32(NOC0_REGS_BASE + NOC_AT_LEN_BE, size);
    noc_trigger();
}

// Issue a NOC unicast read of `size` bytes from the target encoded by (src_lo, src_mid) on this chip
// into local L1 `dst`. `dst` must satisfy (dst & 63) == (src_lo & 63) to match every target's alignment.
static void noc_read(uint32_t src_lo, uint32_t src_mid, uint32_t dst, uint32_t size) {
    uint32_t my_coord = PHYS_RD32(NOC0_REGS_BASE + NOC_NODE_ID) & 0xFFF;
    PHYS_WR32(NOC0_REGS_BASE + NOC_TARG_ADDR_LO, src_lo);
    PHYS_WR32(NOC0_REGS_BASE + NOC_TARG_ADDR_MID, src_mid);
    PHYS_WR32(NOC0_REGS_BASE + NOC_RET_ADDR_LO, dst);
    PHYS_WR32(NOC0_REGS_BASE + NOC_RET_ADDR_MID, my_coord << 4); // dst is my own L1 (< 4 GiB)
    PHYS_WR32(NOC0_REGS_BASE + NOC_CTRL, NOC_CTRL_RD_NONPOSTED);
    PHYS_WR32(NOC0_REGS_BASE + NOC_AT_LEN_BE, size);
    noc_trigger();
}

// Issue a NOC multicast write of `size` bytes from local L1 `src` to L1 offset `dst` on every tensix in
// the rectangle [start_coord, end_coord] on this chip. `src` must satisfy (src & 15) == (dst & 15).
static void noc_multicast(uint32_t dst, uint32_t start_coord, uint32_t end_coord, uint32_t src, uint32_t size) {
    uint32_t my_coord = PHYS_RD32(NOC0_REGS_BASE + NOC_NODE_ID) & 0xFFF;
    PHYS_WR32(NOC0_REGS_BASE + NOC_TARG_ADDR_LO, src);
    PHYS_WR32(NOC0_REGS_BASE + NOC_TARG_ADDR_MID, my_coord << 4);
    PHYS_WR32(NOC0_REGS_BASE + NOC_RET_ADDR_LO, dst);
    PHYS_WR32(NOC0_REGS_BASE + NOC_RET_ADDR_MID, ((start_coord << 12) | end_coord) << 4); // targ rectangle
    PHYS_WR32(NOC0_REGS_BASE + NOC_CTRL, NOC_CTRL_MCAST);
    PHYS_WR32(NOC0_REGS_BASE + NOC_AT_LEN_BE, size);
    noc_trigger();
}

// Forward `size` bytes staged at local `src` to the link peer's L1 at `dst`.
static void eth_send(uint32_t dst, uint32_t src, uint32_t size) {
    PHYS_WR32(ETH_TXQ_BASE + ETH_TXQ_TRANSFER_START_ADDR, src);
    PHYS_WR32(ETH_TXQ_BASE + ETH_TXQ_TRANSFER_SIZE_BYTES, size);
    PHYS_WR32(ETH_TXQ_BASE + ETH_TXQ_DEST_ADDR, dst);
    PHYS_WR32(ETH_TXQ_BASE + ETH_TXQ_CMD, ETH_TXQ_CMD_START_DATA);
}

// --- Multi-hop relay: route a command to the local eth core that peers its target chip (see ROUTE_TABLE) ---
static uint32_t my_noc_coord(void) { return PHYS_RD32(NOC0_REGS_BASE + NOC_NODE_ID) & 0xFFF; }

// Mesh coord of the chip THIS eth core's link peers (from its own route-table entry).
static uint32_t route_my_peer(void) {
    uint32_t my = my_noc_coord();
    uint32_t n = PHYS_RD32(ROUTE_TABLE_ADDR);
    for (uint32_t i = 0; i < n; i++) {
        if (PHYS_RD32(ROUTE_TABLE_ADDR + 4 + 8 * i + 4) == my) {
            return PHYS_RD32(ROUTE_TABLE_ADDR + 4 + 8 * i);
        }
    }
    return 0xFFFFFFFF;
}

// This eth core's index in the route table -- also its request-slot index on any egress core.
static uint32_t route_my_index(void) {
    uint32_t my = my_noc_coord();
    uint32_t n = PHYS_RD32(ROUTE_TABLE_ADDR);
    for (uint32_t i = 0; i < n; i++) {
        if (PHYS_RD32(ROUTE_TABLE_ADDR + 4 + 8 * i + 4) == my) {
            return i;
        }
    }
    return 0;
}

// NOC coord of the local eth core that link-peers chip `tc`, or 0xFFFFFFFF if none is on this chip.
static uint32_t route_egress(uint32_t tc) {
    uint32_t n = PHYS_RD32(ROUTE_TABLE_ADDR);
    for (uint32_t i = 0; i < n; i++) {
        if (PHYS_RD32(ROUTE_TABLE_ADDR + 4 + 8 * i) == tc) {
            return PHYS_RD32(ROUTE_TABLE_ADDR + 4 + 8 * i + 4);
        }
    }
    return 0xFFFFFFFF;
}

// Intra-chip NOC copy to/from another eth core `coord`'s L1 (src/dst must be alignment-matched per noc_*).
static void noc_put(uint32_t coord, uint32_t dst, uint32_t src, uint32_t size) { noc_write(dst, coord << 4, src, size); }
static void noc_get(uint32_t coord, uint32_t src, uint32_t dst, uint32_t size) { noc_read(src, coord << 4, dst, size); }

// Do one single-hop remote access to this core's link peer and wait for the reply. For OP_BLOCK_WRITE the
// block must be staged at `block_src` (shipped to the peer's BLOCK_BUF ahead of the header); for
// OP_BLOCK_READ the block lands at BLOCK_BUF. Returns the read word for OP_READ.
static uint32_t single_hop(uint32_t op, uint32_t sys_lo, uint32_t sys_hi, uint32_t size, uint32_t data,
                           uint32_t block_src, uint32_t *send_seq) {
    uint32_t seq = ++(*send_seq);
    if (op == OP_BLOCK_WRITE) {
        eth_send(BLOCK_BUF_ADDR, block_src, round16(size));
    }
    PHYS_WR32(OUTBOX_ADDR + PKT_SEQ, seq);
    PHYS_WR32(OUTBOX_ADDR + PKT_OP, op);
    PHYS_WR32(OUTBOX_ADDR + PKT_ADDR_LO, sys_lo);
    PHYS_WR32(OUTBOX_ADDR + PKT_ADDR_MID, sys_hi & 0xFFFF);
    PHYS_WR32(OUTBOX_ADDR + PKT_SIZE, size);
    PHYS_WR32(OUTBOX_ADDR + PKT_DATA, data);
    eth_send(REQ_INBOX_ADDR, OUTBOX_ADDR, PKT_BYTES);
    while (PHYS_RD32(RESP_INBOX_ADDR + PKT_SEQ) != seq) {
    }
    return PHYS_RD32(RESP_INBOX_ADDR + PKT_DATA);
}

// Ingress role: post a relay to sibling egress core `egress` (which peers the target chip) and return
// WITHOUT waiting. The egress does the single eth hop and writes the reply back into our RELAY_RESP; the
// ingress collects it on a later eth_service tick (see service_request_queue). Not spinning here is what
// lets the egress/receiver roles keep running, so two siblings relaying through each other can't deadlock.
// `ingress_block` is the buffer the egress uses: the block-write source, or the block-read landing buffer.
static void relay_post(uint32_t egress, uint32_t op, uint32_t sys_lo, uint32_t sys_hi, uint32_t size,
                       uint32_t data, uint32_t ingress_block) {
    uint32_t seq = PHYS_RD32(RELAY_SEND_SEQ_ADDR) + 1;
    PHYS_WR32(RELAY_SEND_SEQ_ADDR, seq);
    PHYS_WR32(OUTBOX_ADDR + PKT_SEQ, seq); // RLY_SEQ == PKT_SEQ == 0
    PHYS_WR32(OUTBOX_ADDR + RLY_OP, op);
    PHYS_WR32(OUTBOX_ADDR + RLY_SYS_LO, sys_lo);
    PHYS_WR32(OUTBOX_ADDR + RLY_SYS_HI, sys_hi);
    PHYS_WR32(OUTBOX_ADDR + RLY_SIZE, size);
    PHYS_WR32(OUTBOX_ADDR + RLY_DATA, data);
    PHYS_WR32(OUTBOX_ADDR + RLY_INGRESS, my_noc_coord());
    PHYS_WR32(OUTBOX_ADDR + RLY_BLOCK, ingress_block);
    noc_put(egress, RELAY_REQ_BASE + route_my_index() * RELAY_SLOT_SIZE, OUTBOX_ADDR, RELAY_SLOT_SIZE);
}

// Egress role: service relay requests siblings forwarded to us (we peer their target chip). For each new
// request, do the single hop to our peer and write the reply back into the ingress core's RELAY_RESP.
static void service_relay(uint32_t *send_seq) {
    uint32_t n = PHYS_RD32(ROUTE_TABLE_ADDR);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = RELAY_REQ_BASE + i * RELAY_SLOT_SIZE;
        uint32_t seq = PHYS_RD32(slot + PKT_SEQ);
        if (seq == PHYS_RD32(RELAY_LAST_SEQ_BASE + 4 * i)) {
            continue;
        }
        uint32_t op = PHYS_RD32(slot + RLY_OP);
        uint32_t size = PHYS_RD32(slot + RLY_SIZE);
        uint32_t ingress = PHYS_RD32(slot + RLY_INGRESS);
        uint32_t ingress_block = PHYS_RD32(slot + RLY_BLOCK);
        if (op == OP_BLOCK_WRITE) {
            noc_get(ingress, ingress_block, BLOCK_BUF_ADDR, round16(size)); // fetch the block to ship
        }
        uint32_t reply = single_hop(op, PHYS_RD32(slot + RLY_SYS_LO), PHYS_RD32(slot + RLY_SYS_HI),
                                    size, PHYS_RD32(slot + RLY_DATA), BLOCK_BUF_ADDR, send_seq);
        if (op == OP_BLOCK_READ) {
            noc_put(ingress, ingress_block, BLOCK_BUF_ADDR, round16(size)); // deposit into ingress's relay buffer
        }
        PHYS_WR32(RELAY_LAST_SEQ_BASE + 4 * i, seq);
        PHYS_WR32(OUTBOX_ADDR + 0, seq);
        PHYS_WR32(OUTBOX_ADDR + 4, reply);
        noc_put(ingress, RELAY_RESP_ADDR, OUTBOX_ADDR, 8);
    }
}

// Receiver role: if the peer forwarded a new request, perform the local NOC access and reply.
static void service_inbox(uint32_t *last_seq) {
    uint32_t seq = PHYS_RD32(REQ_INBOX_ADDR + PKT_SEQ);
    if (seq == *last_seq) {
        return;
    }
    uint32_t op = PHYS_RD32(REQ_INBOX_ADDR + PKT_OP);
    uint32_t addr_lo = PHYS_RD32(REQ_INBOX_ADDR + PKT_ADDR_LO);
    uint32_t addr_mid = PHYS_RD32(REQ_INBOX_ADDR + PKT_ADDR_MID);
    uint32_t size = PHYS_RD32(REQ_INBOX_ADDR + PKT_SIZE);
    uint32_t off = addr_lo & 63u; // NOC needs (staging & 63) == (target & 63) for L1/DRAM/MMIO
    uint32_t reply_op = OP_ACK;
    uint32_t reply_data = 0;
    if (op == OP_WRITE) {
        PHYS_WR32(NOC_STAGE_ADDR + off, PHYS_RD32(REQ_INBOX_ADDR + PKT_DATA));
        noc_write(addr_lo, addr_mid, NOC_STAGE_ADDR + off, size);
    } else if (op == OP_READ) {
        noc_read(addr_lo, addr_mid, NOC_STAGE_ADDR + off, size);
        reply_data = PHYS_RD32(NOC_STAGE_ADDR + off);
        reply_op = OP_RESP;
    } else if (op == OP_BLOCK_WRITE) {
        // The block preceded this header at BLOCK_BUF; realign it and NOC-write it to the target.
        l1_copy(NOC_BLOCK_STAGE_ADDR + off, BLOCK_BUF_ADDR, size);
        noc_write(addr_lo, addr_mid, NOC_BLOCK_STAGE_ADDR + off, size);
    } else if (op == OP_BLOCK_READ) {
        // NOC-read the block, then send it back ahead of the response header. Reads from an MMIO
        // target (e.g. ARC/eth regs) are capped at 4 bytes per NOC transaction, so chunk them.
        uint32_t mmio = (addr_mid & 0xF) || (addr_lo >= RISCV_LOCAL_MEM_BASE);
        uint32_t chunk_max = mmio ? 4u : NOC_MAX_PACKET_SIZE;
        for (uint32_t done = 0; done < size; done += chunk_max) {
            uint32_t chunk = size - done < chunk_max ? size - done : chunk_max;
            noc_read(addr_lo + done, addr_mid, NOC_BLOCK_STAGE_ADDR + off + done, chunk);
        }
        l1_copy(BLOCK_BUF_ADDR, NOC_BLOCK_STAGE_ADDR + off, size);
        eth_send(BLOCK_BUF_ADDR, BLOCK_BUF_ADDR, round16(size));
        reply_op = OP_BLOCK_RESP;
    } else if (op == OP_BROADCAST) {
        // The block preceded this header at BCAST_STAGE; multicast it to this chip's tensix grid.
        noc_multicast(addr_lo, TENSIX_GRID_START, TENSIX_GRID_END, BCAST_STAGE_ADDR, size);
    }
    *last_seq = seq;
    PHYS_WR32(OUTBOX_ADDR + PKT_SEQ, seq);
    PHYS_WR32(OUTBOX_ADDR + PKT_OP, reply_op);
    PHYS_WR32(OUTBOX_ADDR + PKT_DATA, reply_data);
    eth_send(RESP_INBOX_ADDR, OUTBOX_ADDR, PKT_BYTES);
}

// Finish a single/L1-block command: signal write completion or fill the host response queue. `block_src`
// is where a block-read result landed -- BLOCK_BUF for a direct hop, RELAY_RESP_BLOCK_ADDR for a relay.
static void complete_single(uint32_t is_read, uint32_t is_block, uint32_t size, uint32_t reply, uint32_t block_src) {
    if (!is_read) {
        uint32_t wr_req = PHYS_RD32(REQ_QUEUE_BASE + 0);
        PHYS_WR32(REQ_QUEUE_BASE + 0, wr_req + 1); // host polls these for write completion
        PHYS_WR32(REQ_QUEUE_BASE + 4, wr_req + 1);
    } else {
        uint32_t resp_wrptr = PHYS_RD32(RESP_WRPTR) & CMD_BUF_PTR_MASK;
        uint32_t resp_id = resp_wrptr & CMD_BUF_SIZE_MASK;
        uint32_t resp_addr = RESP_ROUTING_QUEUE_BASE + 32 * resp_id;
        if (is_block) {
            l1_copy(ETH_ROUTING_DATA_BUFFER_ADDR + resp_id * MAX_L1_BLOCK_SIZE, block_src, size);
            PHYS_WR32(resp_addr + 12, CMD_DATA_BLOCK | CMD_RD_DATA);
        } else {
            PHYS_WR32(resp_addr + 8, reply); // read result for the host
            PHYS_WR32(resp_addr + 12, CMD_RD_DATA);
        }
        PHYS_WR32(RESP_WRPTR, (resp_wrptr + 1) & CMD_BUF_PTR_MASK);
    }
}

// Sender role: drain the host request queue, forwarding each command to the link peer and waiting for
// its reply before signalling completion / filling the response queue. Handles single-word, L1-block,
// DRAM-block reads/writes, plus broadcast. A command that must be relayed through a sibling egress core
// is non-blocking: it is posted once and this function returns, resuming from the same queue slot on a
// later tick once the reply arrives, so the egress/receiver roles keep running in between.
static void service_request_queue(uint32_t *send_seq) {
    uint32_t wrptr = PHYS_RD32(REQ_WRPTR) & CMD_BUF_PTR_MASK;
    while ((PHYS_RD32(REQ_RDPTR) & CMD_BUF_PTR_MASK) != wrptr) {
        uint32_t rdptr = PHYS_RD32(REQ_RDPTR) & CMD_BUF_PTR_MASK;
        uint32_t cmd_id = rdptr & CMD_BUF_SIZE_MASK;
        uint32_t cmd_addr = REQ_ROUTING_QUEUE_BASE + 32 * cmd_id;
        uint32_t sys_lo = PHYS_RD32(cmd_addr + 0);
        uint32_t sys_hi = PHYS_RD32(cmd_addr + 4);
        uint32_t data = PHYS_RD32(cmd_addr + 8);
        uint32_t flags = PHYS_RD32(cmd_addr + 12) & ~CMD_ORDERED; // ORDERED is a no-op for us; match the rest
        if (flags == CMD_WR_REQ || flags == CMD_RD_REQ ||
            flags == (CMD_WR_REQ | CMD_DATA_BLOCK) || flags == (CMD_RD_REQ | CMD_DATA_BLOCK)) {
            uint32_t is_block = flags & CMD_DATA_BLOCK;
            uint32_t is_read = flags & CMD_RD_REQ;
            uint32_t size = is_block ? data : 4;
            uint32_t op = is_block ? (is_read ? OP_BLOCK_READ : OP_BLOCK_WRITE) : (is_read ? OP_READ : OP_WRITE);
            uint32_t tc = (sys_hi >> 16) & 0xFFF; // target chip mesh coord
            if (tc == route_my_peer()) { // I link-peer the target: do the single eth hop (blocking on my peer)
                uint32_t block_buf = ETH_ROUTING_DATA_BUFFER_ADDR + cmd_id * MAX_L1_BLOCK_SIZE;
                uint32_t reply = single_hop(op, sys_lo, sys_hi, size, data, block_buf, send_seq);
                complete_single(is_read, is_block, size, reply, BLOCK_BUF_ADDR);
            } else if (!PHYS_RD32(RELAY_WAIT_ADDR)) { // forward to the sibling egress; collect the reply later
                uint32_t ingress_block = is_read ? RELAY_RESP_BLOCK_ADDR
                                                 : ETH_ROUTING_DATA_BUFFER_ADDR + cmd_id * MAX_L1_BLOCK_SIZE;
                relay_post(route_egress(tc), op, sys_lo, sys_hi, size, data, ingress_block);
                PHYS_WR32(RELAY_WAIT_ADDR, 1);
                return; // yield so the egress/receiver roles run; resume this slot on a later tick
            } else if (PHYS_RD32(RELAY_RESP_ADDR) != PHYS_RD32(RELAY_SEND_SEQ_ADDR)) {
                return; // the egress hasn't replied yet; keep yielding
            } else { // reply is in: RELAY_RESP holds the read word, a block read landed in RELAY_RESP_BLOCK
                PHYS_WR32(RELAY_WAIT_ADDR, 0);
                complete_single(is_read, is_block, size, PHYS_RD32(RELAY_RESP_ADDR + 4), RELAY_RESP_BLOCK_ADDR);
            }
        } else if (flags == (CMD_WR_REQ | CMD_DATA_BLOCK | CMD_DATA_BLOCK_DRAM)) {
            // DRAM-source block write: pull the block from the host window (via the local PCIe tile)
            // in NOC-sized chunks and forward each as a block write to the target.
            uint32_t size = data;
            uint32_t tag = PHYS_RD32(cmd_addr + ROUTING_CMD_SRC_ADDR_TAG);
            for (uint32_t done = 0; done < size; done += NOC_MAX_PACKET_SIZE) {
                uint32_t chunk = size - done < NOC_MAX_PACKET_SIZE ? size - done : NOC_MAX_PACKET_SIZE;
                uint64_t src = PCIE_HOST_WINDOW_BASE + tag + done;
                noc_read((uint32_t)src, (PCIE_COORD << 4) | (uint32_t)((src >> 32) & 0xF), BLOCK_BUF_ADDR, chunk);
                uint32_t seq = ++(*send_seq);
                eth_send(BLOCK_BUF_ADDR, BLOCK_BUF_ADDR, round16(chunk));
                PHYS_WR32(OUTBOX_ADDR + PKT_SEQ, seq);
                PHYS_WR32(OUTBOX_ADDR + PKT_OP, OP_BLOCK_WRITE);
                PHYS_WR32(OUTBOX_ADDR + PKT_ADDR_LO, sys_lo + done); // target advances per chunk (no 4 GiB carry expected)
                PHYS_WR32(OUTBOX_ADDR + PKT_ADDR_MID, sys_hi & 0xFFFF);
                PHYS_WR32(OUTBOX_ADDR + PKT_SIZE, chunk);
                eth_send(REQ_INBOX_ADDR, OUTBOX_ADDR, PKT_BYTES);
                while (PHYS_RD32(RESP_INBOX_ADDR + PKT_SEQ) != seq) {
                }
            }
            uint32_t wr_req = PHYS_RD32(REQ_QUEUE_BASE + 0);
            PHYS_WR32(REQ_QUEUE_BASE + 0, wr_req + 1); // host polls these for write completion
            PHYS_WR32(REQ_QUEUE_BASE + 4, wr_req + 1);
        } else if (flags == (CMD_RD_REQ | CMD_DATA_BLOCK | CMD_DATA_BLOCK_DRAM)) {
            // DRAM-dest block read: block-read the remote target in NOC-sized chunks and push each to the
            // host window (via the local PCIe tile).
            uint32_t size = data;
            uint32_t tag = PHYS_RD32(cmd_addr + ROUTING_CMD_SRC_ADDR_TAG);
            for (uint32_t done = 0; done < size; done += NOC_MAX_PACKET_SIZE) {
                uint32_t chunk = size - done < NOC_MAX_PACKET_SIZE ? size - done : NOC_MAX_PACKET_SIZE;
                uint32_t seq = ++(*send_seq);
                PHYS_WR32(OUTBOX_ADDR + PKT_SEQ, seq);
                PHYS_WR32(OUTBOX_ADDR + PKT_OP, OP_BLOCK_READ);
                PHYS_WR32(OUTBOX_ADDR + PKT_ADDR_LO, sys_lo + done);
                PHYS_WR32(OUTBOX_ADDR + PKT_ADDR_MID, sys_hi & 0xFFFF);
                PHYS_WR32(OUTBOX_ADDR + PKT_SIZE, chunk);
                eth_send(REQ_INBOX_ADDR, OUTBOX_ADDR, PKT_BYTES);
                while (PHYS_RD32(RESP_INBOX_ADDR + PKT_SEQ) != seq) {
                }
                uint64_t dst = PCIE_HOST_WINDOW_BASE + tag + done; // the block landed in BLOCK_BUF
                noc_write((uint32_t)dst, (PCIE_COORD << 4) | (uint32_t)((dst >> 32) & 0xF), BLOCK_BUF_ADDR, chunk);
            }
            uint32_t resp_wrptr = PHYS_RD32(RESP_WRPTR) & CMD_BUF_PTR_MASK;
            uint32_t resp_id = resp_wrptr & CMD_BUF_SIZE_MASK;
            uint32_t resp_addr = RESP_ROUTING_QUEUE_BASE + 32 * resp_id;
            PHYS_WR32(resp_addr + 12, CMD_DATA_BLOCK | CMD_DATA_BLOCK_DRAM | CMD_RD_DATA);
            PHYS_WR32(RESP_WRPTR, (resp_wrptr + 1) & CMD_BUF_PTR_MASK);
        } else if (flags == (CMD_WR_REQ | CMD_DATA_BLOCK | CMD_DATA_BLOCK_DRAM | CMD_BROADCAST)) {
            // Eth broadcast: the host-DRAM block is a 32-byte grid header (ignored -- we hit every tensix)
            // followed by the payload. Stream the payload from the host window and, per chunk,
            // multicast it to this chip's tensix grid and forward it to the link peer for its own
            // local multicast. Assumes a 16-byte-aligned local_addr and a 64-byte-aligned host tag.
            uint32_t payload = data - BROADCAST_HEADER_SIZE;
            uint32_t tag = PHYS_RD32(cmd_addr + ROUTING_CMD_SRC_ADDR_TAG);
            for (uint32_t done = 0; done < payload; done += BCAST_CHUNK) {
                uint32_t chunk = payload - done < BCAST_CHUNK ? payload - done : BCAST_CHUNK;
                uint64_t src = PCIE_HOST_WINDOW_BASE + tag + BROADCAST_HEADER_SIZE + done;
                noc_read((uint32_t)src, (PCIE_COORD << 4) | (uint32_t)((src >> 32) & 0xF), BCAST_STAGE_ADDR, chunk);
                noc_multicast(sys_lo + done, TENSIX_GRID_START, TENSIX_GRID_END, BCAST_STAGE_ADDR, chunk);
                uint32_t seq = ++(*send_seq);
                eth_send(BCAST_STAGE_ADDR, BCAST_STAGE_ADDR, round16(chunk));
                PHYS_WR32(OUTBOX_ADDR + PKT_SEQ, seq);
                PHYS_WR32(OUTBOX_ADDR + PKT_OP, OP_BROADCAST);
                PHYS_WR32(OUTBOX_ADDR + PKT_ADDR_LO, sys_lo + done);
                // PKT_ADDR_MID is ignored
                PHYS_WR32(OUTBOX_ADDR + PKT_SIZE, chunk);
                eth_send(REQ_INBOX_ADDR, OUTBOX_ADDR, PKT_BYTES);
                while (PHYS_RD32(RESP_INBOX_ADDR + PKT_SEQ) != seq) {
                    // spin until the peer multicast its copy and replied
                }
            }
            uint32_t wr_req = PHYS_RD32(REQ_QUEUE_BASE + 0);
            PHYS_WR32(REQ_QUEUE_BASE + 0, wr_req + 1); // host polls these for write completion
            PHYS_WR32(REQ_QUEUE_BASE + 4, wr_req + 1);
        }
        // an unmodeled command variant -- advancing rdptr without servicing it would stall the
        // host, so anything unexpected here needs handling above.
        PHYS_WR32(REQ_RDPTR, (rdptr + 1) & CMD_BUF_PTR_MASK);
    }
}

// Drain the remote-routing queue and service the link inbox. Called both from the main loop and,
// crucially, from the active-erisc app's cooperative yield (eth_yield_entry) so routing continues
// while metal's app holds the core. State lives in L1 so it survives across those two call sites.
// Non-static: referenced by the crt0 yield shim.
void eth_service(void) {
    uint32_t send_seq = PHYS_RD32(SEND_SEQ_ADDR);
    service_request_queue(&send_seq); // ingress: my host queue (single-hop, or relay to a sibling egress)
    uint32_t last_recv_seq = PHYS_RD32(LAST_RECV_SEQ_ADDR);
    service_inbox(&last_recv_seq); // receiver: a peer forwarded a request to me
    PHYS_WR32(LAST_RECV_SEQ_ADDR, last_recv_seq);
    service_relay(&send_seq); // egress: a sibling forwarded a relay to me; do its single hop
    PHYS_WR32(SEND_SEQ_ADDR, send_seq);
}

void eth_fw_main(void) {
    uint32_t hb = 0;

    // Route the app's periodic yield through the base fw's routing service.
    PHYS_WR32(ETH_APP_YIELD_PTR_ADDR, (uint32_t)&eth_yield_entry);

    for (;;) {
        hb = (hb + 1) & 0xFFFFu;
        PHYS_WR32(ETH_HEARTBEAT_ADDR, (BASE_FW_HEARTBEAT_SIGNATURE << 16) | hb);

        uint32_t go = PHYS_RD32(ETH_GO_MSG_ADDR);
        if ((go >> 24) == RUN_MSG_INIT) {
            PHYS_WR32(ETH_GO_MSG_ADDR, go & 0x00FFFFFFu);
        }

        eth_service();
    }
}
