#!/usr/bin/env python3
# SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# Emits _out/{chip}/tile_regs.h from data/{chip}/tile_regs.json.
# Each of its blocks gets its register offsets (an array's as NAME(i), with NAME_INDEX(offset)), its field positions for
# bits<>() and NAME_VAL(x), x placed in the field, and the bits its fields cover as NAME_FIELDS_MASK, a struct of its
# register values, their values at reset as BLOCK_AT_RESET, and case labels for the hand-written 32-bit read and write
# switches, given the switched offset: a register reads as stored, and a write changes its sw = rw bits and sets the
# onwrite = woset bits written 1, and a 32-bit word of a register without fields is unmodeled. A register wider than 64
# bits is held as 32-bit words, its fields positioned within their word NAME_WORD. A hand register gets only its offset
# and field positions: hand-written code holds its state and reads and writes it. A field reset from a signal resets to
# 0 here, its value set where ttsim knows it. A block type's registers are shared by every block of that type, which
# must agree on them; its field positions are those of the fields every such block places alike, for code that reads any
# block of the type.
import argparse
import json
import re
from typing import TextIO

def reg_case_values(module_name: str, reg: dict) -> list[str]:
    reg_name = reg['name']
    prefixed_name = f'{module_name.upper()}_{reg_name.upper()}'
    array_size = reg.get('array_size')
    if array_size is None:
        return [prefixed_name]
    return [f'{prefixed_name}({i})' for i in range(array_size)]

# A block with a word_size is switched on its offset divided by it, so its cases are word indices, and a contiguous array's
# one range
def reg_cases(module: dict, reg: dict) -> list[str]:
    values = reg_case_values(module['name'], reg)
    word_size = module.get('word_size')
    if word_size is None:
        return values
    assert not int(reg['offset'], 0) % word_size, (module['name'], reg)
    if (len(values) > 1) and (int(reg['array_stride'], 0) == word_size):
        return [f'{values[0]} / {word_size} ... {values[-1]} / {word_size}']
    return [f'{value} / {word_size}' for value in values]

REFUSAL_CATEGORIES = {
    'unimplemented': 'UnimplementedFunctionality',
    'unsupported': 'UnsupportedFunctionality',
    'undefined': 'UndefinedBehavior',
}

def write_default_cases_macro(f: TextIO, module: dict, suffix: str, rw_filter: str) -> None:
    module_name = module['name']
    direction = 'read' if rw_filter == 'wo' else 'write' # the access these cases refuse
    case_lines = [f'#define {module_name.upper()}_{suffix}_DEFAULT_CASES()']
    for reg in module['regs']:
        rw = reg['rw']
        if rw is None:
            refusal = 'unsupported' if reg.get('unsupported', False) else 'unimplemented'
        elif rw == rw_filter:
            refusal = reg.get(direction, 'unimplemented')
        else:
            continue
        category = REFUSAL_CATEGORIES[refusal]
        message = reg['name'].upper()
        case_lines += [f'    case {case}:' for case in reg_cases(module, reg)]
        case_lines.append(f'        TTSIM_ERROR({category}, "{message}");')

    f.write(' \\\n'.join(case_lines))
    f.write('\n\n')

def mask(msb: int, lsb: int) -> int:
    return ((1 << (msb + 1)) - 1) & ~((1 << lsb) - 1)

# A register's elements as (case label, member): a dense array as one range of labels, indexed by the switched offset o
def reg_elements(b: str, reg: dict) -> list[tuple[str, str]]:
    r = reg['name']
    name = f'{b.upper()}_{r.upper()}'
    count = reg.get('count', 1)
    if count == 1:
        return [(name, f'(r).{r}')]
    if int(reg['stride'], 0) * 8 == reg['width'] == 32:
        return [(f'{name}(0) ... {name}({count - 1})', f'(r).{r}[{name}_INDEX(o)]')]
    return [(f'{name}({i})', f'(r).{r}[{i}]') for i in range(count)]

def write_block(f: TextIO, block: dict) -> None:
    b = block['name']
    for reg in block['regs']:
        r = reg['name']
        width = reg['width']
        assert (width in {32, 64}) or (not (width % 32) and (reg.get('count', 1) == 1)), (b, reg)
        offset = int(reg['offset'], 0)
        if reg.get('count', 1) > 1:
            stride = int(reg['stride'], 0)
            f.write(f'#define {b.upper()}_{r.upper()}(i) (0x{offset:X} + 0x{stride:X} * (i))\n')
            f.write(f'#define {b.upper()}_{r.upper()}_INDEX(offset) (((offset) - 0x{offset:X}) / 0x{stride:X})\n')
        else:
            f.write(f'#define {b.upper()}_{r.upper()} 0x{offset:X}\n')
        used = 0
        for field in reg['fields']:
            m = mask(field['msb'], field['lsb'])
            assert not (used & m) and (field['msb'] < width), (b, r, field)
            used |= m
            name = f'{b.upper()}_{r.upper()}_{field["name"].upper()}'
            if width > 64: # within its 32-bit word
                assert (field['msb'] // 32) == (field['lsb'] // 32), (b, r, field)
                f.write(f'#define {name}_WORD {field["lsb"] // 32}\n')
            (msb, lsb) = ((field['msb'] % 32, field['lsb'] % 32) if (width > 64) else (field['msb'], field['lsb']))
            (t, suffix) = ('uint64_t', 'ull') if (width == 64) else ('uint32_t', 'u')
            f.write(f'#define {name} {msb}, {lsb}\n')
            f.write(f'#define {name}_VAL(x) (({t}(x) << {lsb}) & 0x{mask(msb, lsb):X}{suffix})\n')
        if width <= 64:
            assert 'fields_mask' not in {field['name'] for field in reg['fields']}, (b, r)
            f.write(f'#define {b.upper()}_{r.upper()}_FIELDS_MASK 0x{used:X}{"ull" if (width == 64) else "u"}\n')
    f.write('\n')

    t = block['struct']
    resets = []
    for reg in block['regs']:
        if reg.get('hand'):
            continue
        reset = sum(int(field['reset'], 0) << field['lsb'] for field in reg['fields'])
        count = reg.get('count', 1)
        if not reset:
            resets.append(f'.{reg["name"]} = ' + ('0x0' if ((count == 1) and (reg['width'] <= 64)) else '{}'))
        elif reg['width'] > 64:
            resets.append(f'.{reg["name"]} = {{{", ".join(f"0x{(reset >> (32 * w)) & 0xFFFFFFFF:X}" for w in range(reg["width"] // 32))}}}')
        elif count == 1:
            resets.append(f'.{reg["name"]} = 0x{reset:X}')
        else:
            resets.append(f'.{reg["name"]} = {{{", ".join([f"0x{reset:X}"] * count)}}}')
    assert 'at_reset' not in {reg['name'] for reg in block['regs']}, b
    f.write(f'static constexpr {t} {b.upper()}_AT_RESET = {{{", ".join(resets)}}};\n\n')

    rd_cases = []
    wr_cases = []
    for reg in block['regs']:
        if reg.get('hand'):
            continue
        r = reg['name']
        wide = reg['width'] > 64
        for half in range(reg['width'] // 32):
            shift = 32 * half
            half_mask = mask(shift + 31, shift)
            fields = [field for field in reg['fields'] if mask(field['msb'], field['lsb']) & half_mask]
            if not fields:
                continue
            assert all(field['sw'] in {'r', 'rw'} for field in fields), (b, reg) # reads of write-only fields: not yet needed
            assert not any(field.get('singlepulse') or field.get('onread') for field in fields), (b, reg) # not plain storage: a hand register
            rw = 0
            woset = 0
            for field in fields:
                m = mask(field['msb'], field['lsb']) & half_mask
                onwrite = field.get('onwrite')
                if field['sw'] == 'r':
                    continue
                elif onwrite is None:
                    rw |= m
                elif onwrite == 'woset':
                    woset |= m
                else:
                    raise NotImplementedError(f'{b}.{r}.{field["name"]}: onwrite = {onwrite}')
            if wide: # held as 32-bit words
                (rw, woset) = (rw >> shift, woset >> shift)
            t_data = f'(uint64_t(data) << {shift})' if (shift and not wide) else ('uint64_t(data)' if (reg['width'] == 64) else 'data')
            suffix = 'ull' if (reg['width'] == 64) else 'u'
            for (label, member) in reg_elements(b, reg):
                label += f' + {4 * half}' if half else ''
                if wide:
                    member += f'[{half}]'
                rd_cases.append(f'    case {label}: return uint32_t({member}' + (f' >> {shift});' if (shift and not wide) else ');'))
                statements = [] # none for a read-only word, which ignores the write
                if rw:
                    statements.append(f'{member} = ({member} & ~0x{rw:X}{suffix}) | ({t_data} & 0x{rw:X}{suffix});')
                if woset:
                    statements.append(f'{member} |= {t_data} & 0x{woset:X}{suffix};')
                wr_cases.append(f'    case {label}: {" ".join(statements)} break;')

    f.write(' \\\n'.join([f'#define {b.upper()}_RD32_CASES(r, o)'] + rd_cases) + '\n')
    f.write(' \\\n'.join([f'#define {b.upper()}_WR32_CASES(r, o, data)'] + wr_cases) + '\n\n')

# The last byte of a block's registers
def block_extent(block: dict) -> int:
    return max(int(reg['offset'], 0) + ((reg.get('count', 1) - 1) * int(reg.get('stride', '0'), 0)) + (reg['width'] // 8) - 1
               for reg in block['regs'])

# Each map's instances, as MAP_NAME_BASE and MAP_NAME_LIMIT (an array's as MAP_NAME_BASE(i) and MAP_NAME_LIMIT(i), with
# MAP_NAME_INDEX(addr)), MAP_NAME_OFFSET(addr), the offset within the instance, and MAP_NAME_CASES(), the case labels of
# its addresses. Instances, and the registers of a map named after a block, must not overlap, and a nested map or a block
# must fit in the instance that holds it.
def write_maps(f: TextIO, data: dict) -> None:
    blocks = {b['name']: b for b in data['blocks']}
    maps = {m['name']: m['instances'] for m in data['maps']}
    for (name, instances) in maps.items():
        spans = []
        if name in blocks:
            spans += [(int(reg['offset'], 0), block_extent({'regs': [reg]}), reg['name']) for reg in blocks[name]['regs']]
        for x in instances:
            (base, size, count) = (int(x['base'], 0), int(x['size'], 0), x.get('count', 1))
            stride = int(x.get('stride', '0'), 0)
            assert (count == 1) or (stride >= size), (name, x)
            spans += [(base + (i * stride), base + (i * stride) + size - 1, f'{x["name"]}[{i}]') for i in range(count)]
            contents = x.get('contents')
            if contents in maps:
                assert max(int(y['base'], 0) + ((y.get('count', 1) - 1) * int(y.get('stride', '0'), 0)) + int(y['size'], 0)
                           for y in maps[contents]) <= size, (name, x)
            if contents in blocks:
                assert block_extent(blocks[contents]) < size, (name, x)
            assert (contents is None) or (contents in maps) or (contents in blocks), (name, x)
        spans.sort()
        for (a, b) in zip(spans, spans[1:]):
            assert a[1] < b[0], (name, a, b) # an overlap

        for x in instances:
            n = f'{name.upper()}_{x["name"].upper()}'
            (base, size, count) = (int(x['base'], 0), int(x['size'], 0), x.get('count', 1))
            if count == 1:
                f.write(f'#define {n}_BASE 0x{base:X}\n')
                f.write(f'#define {n}_LIMIT 0x{base + size - 1:X}\n')
                f.write(f'#define {n}_OFFSET(addr) ((addr) - {n}_BASE)\n')
                f.write(f'#define {n}_CASES() case {n}_BASE ... {n}_LIMIT:\n')
                continue
            stride = int(x['stride'], 0)
            f.write(f'#define {n}_BASE(i) (0x{base:X} + 0x{stride:X} * (i))\n')
            f.write(f'#define {n}_LIMIT(i) ({n}_BASE(i) + 0x{size - 1:X})\n')
            f.write(f'#define {n}_INDEX(addr) (((addr) - 0x{base:X}) / 0x{stride:X})\n')
            f.write(f'#define {n}_OFFSET(addr) ((addr) - {n}_BASE({n}_INDEX(addr)))\n')
            if stride == size:
                f.write(f'#define {n}_CASES() case {n}_BASE(0) ... {n}_LIMIT({count - 1}):\n')
            else:
                cases = [f'    case {n}_BASE({i}) ... {n}_LIMIT({i}):' for i in range(count)]
                f.write(' \\\n'.join([f'#define {n}_CASES()'] + cases) + '\n')
        f.write('\n')

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
            if 'word_size' in module:
                f.write(f'#define {module_name.upper()}_SWITCH(offset) switch ((offset) / {module["word_size"]})\n')
            for reg in module['regs']:
                offset = int(reg['offset'], 0)
                reg_name = reg['name']
                array_size = reg.get('array_size')
                reset_value = int(reg['reset_value'], 0) if reg['reset_value'] is not None else None
                if array_size is not None:
                    array_stride = int(reg['array_stride'], 0)
                    f.write(f'#define {module_name.upper()}_{reg_name.upper()}(i) (0x{offset:X} + {array_stride}*(i))\n')
                    f.write(f'#define {module_name.upper()}_{reg_name.upper()}_INDEX(offset) (((offset) - 0x{offset:X}) / {array_stride})\n')
                else:
                    f.write(f'#define {module_name.upper()}_{reg_name.upper()} 0x{offset:X}\n')
                if (array_size is not None) or ('word_size' in module):
                    case_lines = [f'#define {module_name.upper()}_{reg_name.upper()}_CASES()'] # any element
                    case_lines += [f'    case {case}:' for case in reg_cases(module, reg)]
                    f.write(' \\\n'.join(case_lines) + '\n')
                if reset_value is not None:
                    f.write(f'#define {module_name.upper()}_{reg_name.upper()}_RESET_VALUE 0x{reset_value:X}\n')
            f.write('\n')

        for module in data['regs']:
            module_name = module['name']
            if module_name in {'riscv_tdma_regs', 'riscv_debug_regs'}:
                write_default_cases_macro(f, module, 'RD', 'wo')
                write_default_cases_macro(f, module, 'WR', 'ro')

        structs = {}
        type_fields = {}
        for block in data['blocks']:
            # a type named after one of its blocks is that block alone, which gives its own field positions
            assert (block['type'] == block['name']) or (block['type'] not in {b['name'] for b in data['blocks']}), block['type']
            block['struct'] = ''.join(word.capitalize() for word in block['type'].split('_'))
            layout = [(reg['name'], reg['offset'], reg['width'], reg.get('count', 1), reg.get('stride')) for reg in block['regs']
                      if not reg.get('hand')]
            assert structs.setdefault(block['struct'], layout) == layout, block['name']
            fields = [(reg['name'], field['name'], field['msb'], field['lsb']) for reg in block['regs'] for field in reg['fields']]
            type_fields[block['type']] = [x for x in type_fields.get(block['type'], fields) if x in fields]
        for (t, layout) in structs.items():
            f.write(f'struct {t} {{\n')
            for (name, _, width, count, _) in layout:
                if width > 64: # as 32-bit words
                    f.write(f'    uint32_t {name}[{width // 32}];\n')
                else:
                    f.write(f'    uint{width}_t {name}' + (f'[{count}];\n' if (count > 1) else ';\n'))
            f.write('};\n\n')
        for (t, fields) in type_fields.items():
            if t in {b['name'] for b in data['blocks']}:
                assert [b['type'] for b in data['blocks']].count(t) == 1, t
                continue
            for (r, name, msb, lsb) in fields:
                f.write(f'#define {t.upper()}_{r.upper()}_{name.upper()} {msb}, {lsb}\n')
            f.write('\n')
        for block in data['blocks']:
            write_block(f, block)
        write_maps(f, data)

    with open(args.out) as f:
        names = re.findall(r'^#define (\w+)', f.read(), re.MULTILINE)
    assert len(names) == len(set(names)), sorted({n for n in names if names.count(n) > 1})

if __name__ == '__main__':
    main()
