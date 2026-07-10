#!/usr/bin/env python3
# SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# Generates _out/{chip}/tensix_regs.h from data/{chip}/tensix_regs.json.
import argparse
import json

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--chip', action='store', required=True)
    parser.add_argument('--out', action='store', required=True)
    args = parser.parse_args()

    with open(f'../data/{args.chip}/tensix_regs.json') as f:
        all_regs = json.load(f)

    group_sizes = {'thread_cfg': 16, 'cfg': 32}

    with open(args.out, 'w') as f:
        for (group, regs) in all_regs.items():
            reg_size = group_sizes[group]
            for (addr, fields) in regs.items():
                f.write(f'#define {group.upper()}{addr}_REG_UNION() \\\n')
                f.write('    union { \\\n')
                f.write('        struct { \\\n')
                bit_pos = 0
                used_bits = 0
                for field in fields:
                    (shift, name, size) = (field['shift'], field['name'], field['size'])
                    assert bit_pos <= shift, (bit_pos, field) # fields should never be out of order
                    if bit_pos < shift:
                        f.write(f'            uint32_t : {shift - bit_pos}; \\\n')
                    if size == 32:
                        f.write(f'            uint32_t {name}; \\\n')
                    else:
                        f.write(f'            uint32_t {name} : {size}; \\\n')
                    bit_pos = shift + size
                    mask = ((1 << size) - 1) << shift
                    assert not used_bits & mask, (addr, field)
                    used_bits |= mask
                if bit_pos < reg_size:
                    f.write(f'            uint32_t : {reg_size - bit_pos}; \\\n')
                f.write('        }; \\\n')
                f.write(f'        uint{reg_size}_t {group}{addr}; \\\n')
                f.write('    };\n')
                f.write(f'#define {group.upper()}{addr}_REG_MASK 0x{used_bits:X}\n')
                f.write('\n')

if __name__ == '__main__':
    main()
