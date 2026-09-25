# SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# The fw's reserved eth-L1 region: code, scratch, and stack all live in tt-metal's ROUTING_FW_RESERVED
# window (0x11000-0x18000), above UMD's tunnel queues/buffer. Must match the 'eth_fw' region in tile.cpp.
WH_ETH_FW_LOAD_ADDR = 0x13000
WH_ETH_FW_STACK_TOP = 0x18000

# Blackhole has no such window: its fw lives in the syseng-reserved region (MEM_SYSENG_RESERVED_BASE
# 0x70000 up to MEM_SYSENG_ETH_RESULTS_BASE_ADDR 0x7CC00, per tt-metal blackhole dev_mem_map.h). Load
# above the three eth_api_table stubs e_tile_init still pokes (0x71574/0x75414/0x759AC, tile.cpp).
BH_ETH_FW_LOAD_ADDR = 0x76000
BH_ETH_FW_STACK_TOP = 0x7CC00

# (output stem, sources, load address, stack top) per architecture.
IMAGES = [
    ('eth_fw', ['crt0.S', 'eth_fw.c'], WH_ETH_FW_LOAD_ADDR, WH_ETH_FW_STACK_TOP),
    ('bh_eth_fw', ['bh_crt0.S', 'bh_eth_fw.c'], BH_ETH_FW_LOAD_ADDR, BH_ETH_FW_STACK_TOP),
]

# The absolute-address MMIO writes in the fw trip -Warray-bounds, so suppress it.
CC_OPTS = [
    '-march=rv32im', '-mabi=ilp32', '-nostdlib', '-nostartfiles', '-ffreestanding',
    '-fno-pic', '-Os', '-Wall', '-Wextra', '-Werror', '-Wno-array-bounds',
]

def rules(ctx):
    assert ctx.host.os == 'linux', 'eth fw can only be built on Linux (the SFPI cross compiler is Linux-only)'

    sfpi_path = ctx.env.get('SFPI_PATH', ctx.path.expanduser('~/sfpi-7.3.0'))
    sfpi_gcc = f'{sfpi_path}/compiler/bin/riscv-tt-elf-gcc'
    sfpi_objcopy = f'{sfpi_path}/compiler/bin/riscv-tt-elf-objcopy'

    binaries = []
    for (stem, srcs, load_addr, stack_top) in IMAGES:
        elf = f'_out/{stem}.elf'
        cmd = [sfpi_gcc, *CC_OPTS, '-T', 'eth_fw.ld',
               f'-Wl,--defsym=ETH_FW_LOAD_ADDR=0x{load_addr:X}',
               f'-Wl,--defsym=ETH_FW_STACK_TOP=0x{stack_top:X}',
               *srcs, '-o', elf]
        ctx.rule(elf, [*srcs, 'eth_fw.ld'], cmd=cmd)

        binary = f'_out/{stem}.bin'
        ctx.rule(binary, elf, cmd=[sfpi_objcopy, '-O', 'binary', elf, binary])
        binaries += [binary]

    ctx.rule(':build', binaries)
