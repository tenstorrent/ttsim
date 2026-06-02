#!/usr/bin/env python3
# SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# Emits _out/{chip}/tile_regs.h from data/{chip}/tile_regs.json.
# The JSON files are produced (and maintained) by scripts/parse_tile_regs.py.
import argparse
import json
from typing import TextIO

def reg_case_values(module_name: str, reg: dict) -> list[str]:
    reg_name = reg['name']
    prefixed_name = f'{module_name.upper()}_{reg_name.upper()}'
    array_size = reg.get('array_size')
    if array_size is None:
        return [prefixed_name]
    return [f'{prefixed_name}({i})' for i in range(array_size)]

def write_default_cases_macro(f: TextIO, module_name: str, suffix: str, rw_filter: str, regs: list[dict]) -> None:
    case_lines = [f'#define {module_name.upper()}_{suffix}_DEFAULT_CASES()']
    for reg in regs:
        rw = reg['rw']
        if rw is not None and rw != rw_filter:
            continue
        unsupported = reg.get('unsupported', False)
        category = 'UnsupportedFunctionality' if unsupported else 'UnimplementedFunctionality'
        message = reg['name'].upper()
        case_lines += [f'    case {case}:' for case in reg_case_values(module_name, reg)]
        case_lines.append(f'        TTSIM_ERROR({category}, "{message}");')

    f.write(' \\\n'.join(case_lines))
    f.write('\n\n')

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--chip', action='store', required=True)
    parser.add_argument('--out', action='store', required=True)
    args = parser.parse_args()

    with open(f'../data/{args.chip}/tile_regs.json') as f:
        data = json.load(f)

    with open(args.out, 'w') as f:
        for entry in data['address_map']:
            base = int(entry['base'], 0)
            limit = int(entry['limit'], 0)
            name = entry['name']
            assert base <= limit, (hex(base), hex(limit))
            f.write(f'#define {name.upper()}_BASE 0x{base:08X}\n')
            f.write(f'#define {name.upper()}_LIMIT 0x{limit:08X}\n')
        f.write('\n')

        for module in data['regs']:
            module_name = module['name']
            for reg in module['regs']:
                offset = int(reg['offset'], 0)
                reg_name = reg['name']
                array_size = reg.get('array_size')
                reset_value = int(reg['reset_value'], 0) if reg['reset_value'] is not None else None
                if array_size is not None:
                    array_stride = int(reg['array_stride'], 0)
                    f.write(f'#define {module_name.upper()}_{reg_name.upper()}(i) (0x{offset:X} + {array_stride}*(i))\n')
                else:
                    f.write(f'#define {module_name.upper()}_{reg_name.upper()} 0x{offset:X}\n')
                if reset_value is not None:
                    f.write(f'#define {module_name.upper()}_{reg_name.upper()}_RESET_VALUE 0x{reset_value:X}\n')
            f.write('\n')

        for module in data['regs']:
            module_name = module['name']
            if module_name in {'riscv_tdma_regs', 'riscv_debug_regs'}:
                write_default_cases_macro(f, module_name, 'RD', 'wo', module['regs'])
                write_default_cases_macro(f, module_name, 'WR', 'ro', module['regs'])

if __name__ == '__main__':
    main()
