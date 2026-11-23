# SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# make.py build rules for src/: codegen, compile, and link the simulator per chip and config.
CHIPS = [
    (0, 'wh'),
    (1, 'bh'),
]
CONFIGS = [
    # (tt_arch_version, chip, data_chip, num_chips, num_mmio_chips, num_mem_chips)
    (0, 'wh',     'wh',  1,  1, None),
    (0, 'wh_x2',  'wh',  2,  1, None),
    (0, 'wh_x8',  'wh',  8,  4, None), # T3000/LoudBox: 4 n300 cards (4 MMIO + 4 eth-tunneled), 4x2 mesh
    (0, 'wh_x32', 'wh', 32, 32, None), # WH Galaxy (6U): 32 WH chips, 8x4 mesh, all MMIO (BDF)
    (1, 'bh',     'bh',  1,  1, None),
    (1, 'bh_x2',  'bh',  2,  2, None),
    (1, 'bh_x4',  'bh',  4,  4, None), # QuietBox 2: two P300 cards, 2x2 mesh, all MMIO
    (1, 'bh_x32', 'bh', 32, 32, None), # BH Galaxy: 32 BH chips, 8x4 torus xy, all MMIO (BDF)
]

def rules(ctx):
    compile_opts = [
        '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wundef', '-Wcast-qual', '-Werror', '-MMD',
        '-fPIC', '-fvisibility=hidden', '-fno-exceptions', '-fno-rtti',
        '-ffp-contract=off', # for hostfp, do not fuse a*b + c
    ]
    if ctx.host.os != 'darwin':
        compile_opts += ['-fno-semantic-interposition']
    if ctx.env.get('COLOR') == '1':
        compile_opts += ['-fdiagnostics-color=always']
    if ctx.host.arch == 'x86_64':
        # X86_64_V4=1: AVX-512, for hosts that have it (release builds stay on x86-64-v3)
        compile_opts += ['-march=x86-64-v4' if ctx.env.get('X86_64_V4') == '1' else '-march=x86-64-v3']
    # DEBUG=1: debug info, light optimization, and no stripping, in the same output directories
    debug = ctx.env.get('DEBUG') == '1'
    compile_opts += ['-g', '-Og'] if debug else ['-O2']

    # Each chip or system builds into _out/<config>/, plus variants of it in _out/<config><suffix>/
    flavors = [''] # directory suffixes
    so_link_opts = ['-shared']
    if ctx.host.os != 'darwin':
        so_link_opts += ['-Wl,-Bsymbolic', '-Wl,--no-undefined']

    target = '_out/riscv_decode.h'
    script = 'riscv_gen_decode.py'
    ctx.rule(target, script, cmd=['python3', script])
    gen_h_files = [target]

    chip_gen_h_files = {}
    for (tt_arch_version, chip) in CHIPS:
        target = f'_out/{chip}/tensix_decode.h'
        script = 'tensix_gen_decode.py'
        dep = f'../data/{chip}/tensix_isa.json'
        ctx.rule(target, [script, dep], cmd=['python3', script, '--chip', chip, '--out', target])
        chip_gen_h_files[chip] = gen_h_files + [target]

        fw_names = ['eth_fw']
        script = 'gen_fw_blob.py'
        for name in fw_names:
            target = f'_out/{chip}/{name}_blob.h'
            dep = f'../data/{chip}/{name}.bin'
            ctx.rule(target, [script, dep], cmd=['python3', script, '--bin', dep, '--out', target])
            chip_gen_h_files[chip] += [target]

        target = f'_out/{chip}/tensix_regs.h'
        script = 'tensix_gen_regs.py'
        dep = f'../data/{chip}/tensix_regs.json'
        ctx.rule(target, [script, dep], cmd=['python3', script, '--chip', chip, '--out', target])
        chip_gen_h_files[chip] += [target]

        target = f'_out/{chip}/tile_regs.h'
        script = 'tile_gen_regs.py'
        dep = f'../data/{chip}/tile_regs.json'
        ctx.rule(target, [script, dep], cmd=['python3', script, '--chip', chip, '--out', target])
        chip_gen_h_files[chip] += [target]

    # Everything but sim.cpp, which holds the chip count, is built once per chip and shared by its configs.
    shared_o_files = {}
    for (tt_arch_version, chip) in CHIPS:
        for flavor in flavors:
            out_dir = f'_out/{chip}{flavor}'
            gcc_opts = [
                *compile_opts,
                f'-DTT_ARCH_VERSION={tt_arch_version}',
                f'-I_out/{chip}',
            ]

            c_files = ['libttsim.cpp', 'pcie.cpp', 'common.cpp', 'rv32.cpp', 'tensix.cpp', 'tile.cpp']

            o_files = []
            for file in c_files:
                o_file = f'{out_dir}/{file.replace(".cpp", ".o")}'
                d_file = o_file.replace('.o', '.d')
                cmd = ['g++', *gcc_opts, '-c', file, '-o', o_file]
                ctx.rule(o_file, file, cmd=cmd, depfile=d_file, order_only_inputs=chip_gen_h_files[chip])
                o_files += [o_file]
            shared_o_files[chip, flavor] = o_files

    build_targets = []
    old_path_targets = []
    for (tt_arch_version, chip, data_chip, num_chips, num_mmio_chips, num_mem_chips) in CONFIGS:
        if chip == 'bh_x32' and ctx.host.arch != 'x86_64':
            continue # -mcmodel=medium is x86_64 only; bh_x32 BSS of ~6.9GB is too large for aarch64

        for flavor in flavors:
            out_dir = f'_out/{chip}{flavor}'
            gcc_opts = [
                *compile_opts,
                f'-DTT_ARCH_VERSION={tt_arch_version}',
                f'-DNUM_CHIPS={num_chips}',
                f'-DNUM_MMIO_CHIPS={num_mmio_chips}',
                f'-I_out/{data_chip}',
            ]
            if num_mem_chips is not None:
                gcc_opts += [f'-DNUM_MEM_CHIPS={num_mem_chips}']
            if num_chips >= 16 and ctx.host.arch == 'x86_64':
                gcc_opts += ['-mcmodel=medium'] # required for >2GB BSS in very large (Galaxy) sims

            o_file = f'{out_dir}/sim.o'
            d_file = o_file.replace('.o', '.d')
            cmd = ['g++', *gcc_opts, '-c', 'sim.cpp', '-o', o_file]
            ctx.rule(o_file, 'sim.cpp', cmd=cmd, depfile=d_file, order_only_inputs=chip_gen_h_files[data_chip])
            o_files = [*shared_o_files[data_chip, flavor], o_file]

            target = f'{out_dir}/libttsim.so'
            link_so_deps = o_files
            cmd = ['g++', *o_files, *so_link_opts, '-o', target]
            if ctx.host.os != 'darwin':
                link_so_deps += ['libttsim.map']
                cmd += ['-Wl,--version-script=libttsim.map']
                if not debug:
                    cmd += ['-Wl,--strip-all']
            ctx.rule(target, link_so_deps, cmd=cmd)
            build_targets += [target]
            if not flavor:
                old_path_targets += [target]

    # Standalone full-system riscv64 simulator, aka ttsim-riscv64
    for flavor in flavors:
        out_dir = f'_out/riscv64{flavor}'
        gcc_opts = [
            *compile_opts,
            '-DTTSIM_RV64_SYSTEM=1',
            '-frounding-math', # for hostfp, do not assume the default rounding mode
        ]
        c_files = ['main.cpp', 'common.cpp', 'rv64_fpu.cpp', 'rv64_system.cpp']
        o_files = []
        for file in c_files:
            o_file = f'{out_dir}/{file.replace(".cpp", ".o")}'
            d_file = o_file.replace('.o', '.d')
            cmd = ['g++', *gcc_opts, '-c', file, '-o', o_file]
            ctx.rule(o_file, file, cmd=cmd, depfile=d_file, order_only_inputs=gen_h_files)
            o_files += [o_file]
        target = f'{out_dir}/ttsim'
        cmd = ['g++', *o_files, '-o', target]
        ctx.rule(target, o_files, cmd=cmd)
        build_targets += [target]
        if not flavor:
            old_path_targets += [target]

    # The old output paths, _out/release_<config>/, copied from the new ones so that existing scripts and docs keep
    # working for now. To be removed.
    old_flavor = ''
    for target in old_path_targets:
        (_, out_dir, name) = target.split('/')
        src = f'_out/{out_dir}{old_flavor}/{name}'
        old_target = f'_out/release_{out_dir}/{name}'
        ctx.rule(old_target, src, cmd=['cp', src, old_target])
        build_targets += [old_target]

    ctx.rule(':build', build_targets)
