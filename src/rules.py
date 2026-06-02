# SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# make.py build rules for src/: codegen, compile, and link the simulator per chip and config.
CHIPS = [
    (0, 'wh'),
    (1, 'bh'),
]

def rules(ctx):
    compile_opts = [
        '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wundef', '-Wcast-qual', '-Werror', '-MMD',
        '-fPIC', '-fvisibility=hidden', '-fno-exceptions', '-fno-rtti',
    ]
    if ctx.host.os != 'darwin':
        compile_opts += ['-fno-semantic-interposition']
    if ctx.env.get('COLOR') == '1':
        compile_opts += ['-fdiagnostics-color=always']
    if ctx.host.arch == 'x86_64':
        compile_opts += ['-march=x86-64-v3']
    config_compile_opts = {
        'debug': ['-g', '-Og', '-DDEBUG'],
        'release': ['-O2'],
    }
    so_link_opts = ['-shared']
    if ctx.host.os != 'darwin':
        so_link_opts += ['-Wl,-Bsymbolic', '-Wl,--no-undefined']

    target = '_out/riscv_decode.h'
    script = 'riscv_gen_decode.py'
    ctx.rule(target, script, cmd=['python3', script])
    gen_h_files = [target]

    build_targets = []
    for (tt_arch_version, chip) in CHIPS:
        target = f'_out/{chip}/tensix_decode.h'
        script = 'tensix_gen_decode.py'
        dep = f'../data/{chip}/tensix_isa.json'
        ctx.rule(target, [script, dep], cmd=['python3', script, '--chip', chip, '--out', target])
        chip_gen_h_files = gen_h_files + [target]

        target = f'_out/{chip}/tensix_regs.h'
        script = 'tensix_gen_regs.py'
        dep = f'../data/{chip}/tensix_regs.json'
        ctx.rule(target, [script, dep], cmd=['python3', script, '--chip', chip, '--out', target])
        chip_gen_h_files += [target]

        target = f'_out/{chip}/tile_regs.h'
        script = 'tile_gen_regs.py'
        dep = f'../data/{chip}/tile_regs.json'
        ctx.rule(target, [script, dep], cmd=['python3', script, '--chip', chip, '--out', target])
        chip_gen_h_files += [target]

        for config in config_compile_opts:
            out_dir = f'_out/{config}_{chip}'
            gcc_opts = [*compile_opts, *config_compile_opts[config], f'-DTT_ARCH_VERSION={tt_arch_version}', f'-I_out/{chip}']

            c_files = ['libttsim.cpp', 'rv32.cpp', 'sim.cpp', 'tensix.cpp', 'tile.cpp', 'fma.cpp']

            o_files = []
            for file in c_files:
                o_file = f'{out_dir}/{file.replace(".cpp", ".o")}'
                d_file = o_file.replace('.o', '.d')
                cmd = ['g++', *gcc_opts, '-c', file, '-o', o_file]
                ctx.rule(o_file, file, cmd=cmd, depfile=d_file, order_only_inputs=chip_gen_h_files)
                o_files += [o_file]

            target = f'{out_dir}/libttsim.so'
            link_so_deps = o_files
            cmd = ['g++', *o_files, *so_link_opts, '-o', target]
            if ctx.host.os != 'darwin':
                link_so_deps += ['libttsim.map']
                cmd += ['-Wl,--version-script=libttsim.map']
                if config != 'debug':
                    cmd += ['-Wl,--strip-all']
            ctx.rule(target, link_so_deps, cmd=cmd)
            build_targets += [target]

    ctx.rule(':build', build_targets)
