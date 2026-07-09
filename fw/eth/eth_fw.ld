/* SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc. */
/* SPDX-License-Identifier: Apache-2.0 */

/* Link the simulated Wormhole eth base firmware as a flat image based at the eth-L1 load address. */
/* ETH_FW_LOAD_ADDR is supplied at link time (--defsym) from the eth_fw region in the tile address map. */
/* _start (crt0 .start) is placed first so the image's first byte is the entry point. */
ENTRY(_start)

SECTIONS {
    . = ETH_FW_LOAD_ADDR;
    .text : {
        *(.start)
        *(.text*)
        *(.rodata*)
    }
    .data : {
        *(.data*)
        *(.sdata*)
    }
    .bss : {
        *(.bss*)
        *(.sbss*)
        *(COMMON)
    }
    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.eh_frame*) }
}
