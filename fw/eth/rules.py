# SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# The fw's reserved eth-L1 region: code, scratch, and stack all live in tt-metal's ROUTING_FW_RESERVED
# window (0x11000-0x18000), above UMD's tunnel queues/buffer. Must match the 'eth_fw' region in tile.cpp.
ETH_FW_LOAD_ADDR = 0x13000
ETH_FW_STACK_TOP = 0x18000

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

    srcs = ['crt0.S', 'eth_fw.c']
    elf = '_out/eth_fw.elf'
    cmd = [sfpi_gcc, *CC_OPTS, '-T', 'eth_fw.ld',
           f'-Wl,--defsym=ETH_FW_LOAD_ADDR=0x{ETH_FW_LOAD_ADDR:X}',
           f'-Wl,--defsym=ETH_FW_STACK_TOP=0x{ETH_FW_STACK_TOP:X}',
           *srcs, '-o', elf]
    ctx.rule(elf, [*srcs, 'eth_fw.ld'], cmd=cmd)

    binary = '_out/eth_fw.bin'
    ctx.rule(binary, elf, cmd=[sfpi_objcopy, '-O', 'binary', elf, binary])

    ctx.rule(':build', [binary])
