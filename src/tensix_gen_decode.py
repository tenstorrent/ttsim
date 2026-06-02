#!/usr/bin/env python3
# SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# Generates _out/{chip}/tensix_decode.h from data/{chip}/tensix_isa.json.
import argparse
import json

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--chip', action='store', required=True)
    parser.add_argument('--out', action='store', required=True)
    args = parser.parse_args()

    with open(f'../data/{args.chip}/tensix_isa.json') as f:
        opcodes = json.load(f)

    with open(args.out, 'w') as f:
        for (name, info) in opcodes.items():
            if name in {'MOP', 'NOP', 'MOP_CFG', 'REPLAY'}:
                continue # skip for now

            if info.get('unsupported'):
                f.write(f'#define TENSIX_DECODER_{name}() \\\n')
                f.write(f'    static bool tensix_decode_{name.lower()}(TensixState *p_tensix, uint32_t pipe, uint32_t inst) {{ \\\n')
                f.write('        TTSIM_ERROR_NOFMT(UnsupportedFunctionality); \\\n')
                f.write('    }\n')
                f.write('\n')
                continue

            unused_arg_bits = 0xFFFFFF
            for (arg_name, bits) in info['args'].items():
                (hi, lo) = tuple(int(x) for x in bits.split(':'))
                bits = ((2 << (hi - lo)) - 1) << lo
                assert (bits & unused_arg_bits) == bits
                unused_arg_bits &= ~bits

            f.write(f'#define TENSIX_EXECUTE_{name}() \\\n')
            arg_list = ['TensixState *p_tensix', 'uint32_t pipe']
            for (arg_name, bits) in info['args'].items():
                arg_list += [f'uint32_t {arg_name}']
            f.write(f'    static bool tensix_{name.lower()}({", ".join(arg_list)})\n')
            f.write('\n')

            f.write(f'#define TENSIX_DECODER_{name}() \\\n')
            f.write(f'    static bool tensix_decode_{name.lower()}(TensixState *p_tensix, uint32_t pipe, uint32_t inst) {{ \\\n')
            if unused_arg_bits:
                if args.chip in {'wh', 'bh'}:
                    f.write(f'        TTSIM_VERIFY(!(inst & 0x{unused_arg_bits:06X}), UndefinedBehavior, "invalid opcode bits set in inst=0x%x", inst); \\\n')
                else:
                    f.write(f'        TTSIM_VERIFY(!(inst & 0x{unused_arg_bits:06X}), UnimplementedFunctionality, "inst=0x%x", inst); \\\n')
            arg_list = ['p_tensix', 'pipe']
            for (arg_name, bits) in info['args'].items():
                (hi, lo) = tuple(int(x) for x in bits.split(':'))
                f.write(f'        uint32_t {arg_name} = bits<{hi},{lo}>(inst); \\\n')
                arg_list += [arg_name]
            f.write(f'        return tensix_{name.lower()}({", ".join(arg_list)}); \\\n')
            f.write('    }\n')
            f.write('\n')

        f.write('#define TENSIX_DECODERS() \\\n')
        for (name, info) in opcodes.items():
            if name in {'MOP', 'NOP', 'MOP_CFG', 'REPLAY'}:
                continue # skip for now
            f.write(f'    TENSIX_DECODER_{name}() \\\n')
        f.write('\n')

        f.write('#define TENSIX_OPCODE_CASES() \\\n')
        for (name, info) in opcodes.items():
            if name in {'MOP', 'NOP', 'MOP_CFG', 'REPLAY'}:
                continue # skip for now
            opcode = info['opcode']
            f.write(f'    case 0x{opcode:02X}: return tensix_decode_{name.lower()}(p_tensix, pipe, inst); \\\n')
        f.write('\n')

if __name__ == '__main__':
    main()
