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
#define CMD_DATA_BLOCK_DRAM 0x10u // block source/dest is host DRAM (still faked; handled later)
#define CMD_ORDERED 0x1000u // ordering hint; satisfied by our synchronous, in-order delivery (masked off before matching)
// Per-slot L1 block staging the host/fw use for CMD_DATA_BLOCK payloads.
#define ETH_ROUTING_DATA_BUFFER_ADDR 0x12000
#define MAX_L1_BLOCK_SIZE 1024

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
#define NOC_MAX_PACKET_SIZE 8192u // max bytes per NOC transaction (WH)
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
#define REQ_INBOX_ADDR 0x28100 // peer -> me: forwarded remote-queue requests
#define RESP_INBOX_ADDR 0x28200 // peer -> me: acks / read responses
#define OUTBOX_ADDR 0x28300 // me -> peer: staged header packet
#define NOC_STAGE_ADDR 0x28400 // receiver: single-word NOC source, alignment-matched to the target
#define BLOCK_BUF_ADDR 0x30000 // block payload staged for / landed from the link (16-byte aligned)
#define NOC_BLOCK_STAGE_ADDR 0x32000 // receiver: block NOC source/dest, alignment-matched to the target

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

// Forward `size` bytes staged at local `src` to the link peer's L1 at `dst`.
static void eth_send(uint32_t dst, uint32_t src, uint32_t size) {
    PHYS_WR32(ETH_TXQ_BASE + ETH_TXQ_TRANSFER_START_ADDR, src);
    PHYS_WR32(ETH_TXQ_BASE + ETH_TXQ_TRANSFER_SIZE_BYTES, size);
    PHYS_WR32(ETH_TXQ_BASE + ETH_TXQ_DEST_ADDR, dst);
    PHYS_WR32(ETH_TXQ_BASE + ETH_TXQ_CMD, ETH_TXQ_CMD_START_DATA);
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
    }
    *last_seq = seq;
    PHYS_WR32(OUTBOX_ADDR + PKT_SEQ, seq);
    PHYS_WR32(OUTBOX_ADDR + PKT_OP, reply_op);
    PHYS_WR32(OUTBOX_ADDR + PKT_DATA, reply_data);
    eth_send(RESP_INBOX_ADDR, OUTBOX_ADDR, PKT_BYTES);
}

// Sender role: drain the host request queue, forwarding each command to the link peer and waiting for
// its reply before signalling completion / filling the response queue. Handles single-word and L1
// block reads/writes.
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
            uint32_t seq = ++(*send_seq);
            if (is_block && !is_read) { // ship the L1 block to the peer ahead of the header
                eth_send(BLOCK_BUF_ADDR, ETH_ROUTING_DATA_BUFFER_ADDR + cmd_id * MAX_L1_BLOCK_SIZE, round16(size));
            }
            PHYS_WR32(OUTBOX_ADDR + PKT_SEQ, seq);
            PHYS_WR32(OUTBOX_ADDR + PKT_OP, is_block ? (is_read ? OP_BLOCK_READ : OP_BLOCK_WRITE)
                                                : (is_read ? OP_READ : OP_WRITE));
            PHYS_WR32(OUTBOX_ADDR + PKT_ADDR_LO, sys_lo); // ret/targ addr_lo = sys_addr[31:0]
            PHYS_WR32(OUTBOX_ADDR + PKT_ADDR_MID, sys_hi & 0xFFFF); // ret/targ addr_mid = sys_addr[47:32]
            PHYS_WR32(OUTBOX_ADDR + PKT_SIZE, size);
            PHYS_WR32(OUTBOX_ADDR + PKT_DATA, data); // single-word write payload (unused otherwise)
            eth_send(REQ_INBOX_ADDR, OUTBOX_ADDR, PKT_BYTES);
            while (PHYS_RD32(RESP_INBOX_ADDR + PKT_SEQ) != seq) {
                // spin until the peer performed the access and replied (its core is clocked in step)
            }
            if (!is_read) {
                uint32_t wr_req = PHYS_RD32(REQ_QUEUE_BASE + 0);
                PHYS_WR32(REQ_QUEUE_BASE + 0, wr_req + 1); // host polls these for write completion
                PHYS_WR32(REQ_QUEUE_BASE + 4, wr_req + 1);
            } else {
                uint32_t resp_wrptr = PHYS_RD32(RESP_WRPTR) & CMD_BUF_PTR_MASK;
                uint32_t resp_id = resp_wrptr & CMD_BUF_SIZE_MASK;
                uint32_t resp_addr = RESP_ROUTING_QUEUE_BASE + 32 * resp_id;
                if (is_block) { // block landed in BLOCK_BUF ahead of the response header
                    l1_copy(ETH_ROUTING_DATA_BUFFER_ADDR + resp_id * MAX_L1_BLOCK_SIZE, BLOCK_BUF_ADDR, size);
                    PHYS_WR32(resp_addr + 12, CMD_DATA_BLOCK | CMD_RD_DATA);
                } else {
                    PHYS_WR32(resp_addr + 8, PHYS_RD32(RESP_INBOX_ADDR + PKT_DATA)); // read result for the host
                    PHYS_WR32(resp_addr + 12, CMD_RD_DATA);
                }
                PHYS_WR32(RESP_WRPTR, (resp_wrptr + 1) & CMD_BUF_PTR_MASK);
            }
        }
        // an unmodeled command variant -- advancing rdptr without servicing it would stall the
        // host, so anything unexpected here needs handling above.
        PHYS_WR32(REQ_RDPTR, (rdptr + 1) & CMD_BUF_PTR_MASK);
    }
}

void eth_fw_main(void) {
    uint32_t hb = 0;
    uint32_t send_seq = 0;
    uint32_t last_recv_seq = 0;
    for (;;) {
        hb = (hb + 1) & 0xFFFFu;
        PHYS_WR32(ETH_HEARTBEAT_ADDR, (BASE_FW_HEARTBEAT_SIGNATURE << 16) | hb);

        uint32_t go = PHYS_RD32(ETH_GO_MSG_ADDR);
        if ((go >> 24) == RUN_MSG_INIT) {
            PHYS_WR32(ETH_GO_MSG_ADDR, go & 0x00FFFFFFu);
        }

        service_request_queue(&send_seq);
        service_inbox(&last_recv_seq);
    }
}
