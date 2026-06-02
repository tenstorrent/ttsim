#!/usr/bin/env python3
# SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# Generates _out/riscv_decode.h from the RISC-V instruction encoding table defined below.
riscv_decode_table = [
    (32, 0x0003, 0x707F, 'load<int8_t, int_xlen_t>'),    # LB
    (32, 0x1003, 0x707F, 'load<int16_t, int_xlen_t>'),   # LH
    (32, 0x2003, 0x707F, 'load<int32_t, int_xlen_t>'),   # LW
    (64, 0x3003, 0x707F, 'load<uint64_t, uint_xlen_t>'), # LD
    (32, 0x4003, 0x707F, 'load<uint8_t, uint_xlen_t>'),  # LBU
    (32, 0x5003, 0x707F, 'load<uint16_t, uint_xlen_t>'), # LHU
    (64, 0x6003, 0x707F, 'load<uint32_t, uint_xlen_t>'), # LWU

    (32, 0x0007, 0x707F, 'v_load<uint8_t>'),
    (32, 0x1007, 0x707F, 'f_load<uint16_t>'),
    (32, 0x2007, 0x707F, 'f_load<uint32_t>'),
    (32, 0x3007, 0x707F, 'f_load<uint64_t>'),
    (32, 0x4007, 0x707F, 'f_load<unsigned __int128>'),
    (32, 0x5007, 0x707F, 'v_load<uint16_t>'),
    (32, 0x6007, 0x707F, 'v_load<uint32_t>'),
    (32, 0x7007, 0x707F, 'v_load<uint64_t>'),

    (64, 0x000B, 0x007F, 'custom_0'),

    (32, 0x000F, 0x707F, 'fence'),
    (32, 0x100F, 0x707F, 'fence_i'),

    (32, 0x0013, 0x707F, 'alu_imm<0>'),
    (32, 0x1013, 0x707F, 'alu_imm<1>'),
    (32, 0x2013, 0x707F, 'alu_imm<2>'),
    (32, 0x3013, 0x707F, 'alu_imm<3>'),
    (32, 0x4013, 0x707F, 'alu_imm<4>'),
    (32, 0x5013, 0x707F, 'alu_imm<5>'),
    (32, 0x6013, 0x707F, 'alu_imm<6>'),
    (32, 0x7013, 0x707F, 'alu_imm<7>'),

    (32, 0x0017, 0x007F, 'auipc'),

    (64, 0x001B, 0x707F, 'alu_imm_32<0>'),
    (64, 0x101B, 0x707F, 'alu_imm_32<1>'),
    (64, 0x501B, 0x707F, 'alu_imm_32<5>'),

    (32, 0x0023, 0x707F, 'store<uint8_t>'),  # SB
    (32, 0x1023, 0x707F, 'store<uint16_t>'), # SH
    (32, 0x2023, 0x707F, 'store<uint32_t>'), # SW
    (64, 0x3023, 0x707F, 'store<uint64_t>'), # SD

    (32, 0x0027, 0x707F, 'v_store<uint8_t>'),
    (32, 0x1027, 0x707F, 'f_store<uint16_t>'),
    (32, 0x2027, 0x707F, 'f_store<uint32_t>'),
    (32, 0x3027, 0x707F, 'f_store<uint64_t>'),
    (32, 0x4027, 0x707F, 'f_store<unsigned __int128>'),
    (32, 0x5027, 0x707F, 'v_store<uint16_t>'),
    (32, 0x6027, 0x707F, 'v_store<uint32_t>'),
    (32, 0x7027, 0x707F, 'v_store<uint64_t>'),

    (64, 0x002B, 0x007F, 'custom_1'),

    (32, 0x202F, 0x707F, 'atomic<uint32_t>'),
    (64, 0x302F, 0x707F, 'atomic<uint64_t>'),

    (32, 0x0033, 0x707F, 'alu<0>'),
    (32, 0x1033, 0x707F, 'alu<1>'),
    (32, 0x2033, 0x707F, 'alu<2>'),
    (32, 0x3033, 0x707F, 'alu<3>'),
    (32, 0x4033, 0x707F, 'alu<4>'),
    (32, 0x5033, 0x707F, 'alu<5>'),
    (32, 0x6033, 0x707F, 'alu<6>'),
    (32, 0x7033, 0x707F, 'alu<7>'),

    (32, 0x0037, 0x007F, 'lui'),

    (64, 0x003B, 0x707F, 'alu_32<0>'),
    (64, 0x103B, 0x707F, 'alu_32<1>'),
    (64, 0x203B, 0x707F, 'alu_32<2>'),
    (64, 0x403B, 0x707F, 'alu_32<4>'),
    (64, 0x503B, 0x707F, 'alu_32<5>'),
    (64, 0x603B, 0x707F, 'alu_32<6>'),
    (64, 0x703B, 0x707F, 'alu_32<7>'),

    (32, 0x0043, 0x007F, 'f_fma<false, false>'), # FMADD
    (32, 0x0047, 0x007F, 'f_fma<false, true>'), # FMSUB
    (32, 0x004B, 0x007F, 'f_fma<true, false>'), # FNMSUB
    (32, 0x004F, 0x007F, 'f_fma<true, true>'), # FNMADD
    (32, 0x0053, 0x007F, 'f_alu'),
    (32, 0x0057, 0x007F, 'v_alu'),

    (64, 0x005B, 0x007F, 'custom_2'),

    (32, 0x0063, 0x707F, 'branch<branch_op_beq>'),
    (32, 0x1063, 0x707F, 'branch<branch_op_bne>'),
    (32, 0x4063, 0x707F, 'branch<branch_op_blt>'),
    (32, 0x5063, 0x707F, 'branch<branch_op_bge>'),
    (32, 0x6063, 0x707F, 'branch<branch_op_bltu>'),
    (32, 0x7063, 0x707F, 'branch<branch_op_bgeu>'),

    (32, 0x0067, 0x707F, 'jalr'),

    (32, 0x006F, 0x007F, 'jal'),

    (32, 0x0073, 0x707F, 'ecall_ebreak'),
    (32, 0x1073, 0x707F, 'csrrw'),
    (32, 0x2073, 0x707F, 'csrrs'),
    (32, 0x3073, 0x707F, 'csrrc'),
    (32, 0x5073, 0x707F, 'csrrwi'),
    (32, 0x6073, 0x707F, 'csrrsi'),
    (32, 0x7073, 0x707F, 'csrrci'),

    (64, 0x007B, 0x007F, 'custom_3'),
]

table: list = [None] * 256
for (isa, inst, mask, name) in riscv_decode_table:
    assert isa in {32, 64}, isa
    assert inst & ~mask == 0, (inst, mask)
    assert (inst & 3) == 3, inst
    if mask == 0x007F:
        for funct3 in range(8):
            opcode = (inst >> 2) | (funct3 << 5)
            assert table[opcode] is None
            table[opcode] = (isa, name)
    elif mask == 0x707F:
        opcode = ((inst >> 2) & 31) | ((inst & 0x7000) >> 7)
        assert table[opcode] is None
        table[opcode] = (isa, name)
    else:
        assert False, mask

with open('_out/riscv_decode.h', 'w') as f:
    for entry in table:
        if entry is None:
            entry = (32, 'unimplemented')
        (isa, name) = entry
        if isa == 64:
            print('#if XLEN == 64', file=f)
        if '<' in name:
            (name, template_args) = name.split('<', 1)
            print(f'RV_XLEN_PREFIX({name})<{template_args},', file=f)
        else:
            print(f'RV_XLEN_PREFIX({name}),', file=f)
        if isa == 64:
            print('#else', file=f)
            print('RV_XLEN_PREFIX(unimplemented),', file=f)
            print('#endif', file=f)
