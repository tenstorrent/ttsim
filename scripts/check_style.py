#!/usr/bin/env python3
# SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# Repo-wide style checker for tracked source files (line length, whitespace, ASCII, etc.).
import argparse
import os

# Only files that come from external teams/orgs should go in this list
skip_files = {
    'data/wh/eth_fw.bin',
    'src/fma.cpp', # copied exactly from external repo
}

n_errors = 0

def report_error(path: str, line_num: int, msg: str) -> None:
    global n_errors
    n_errors += 1
    print(f'{path}:{line_num}: {msg}')

def check_file(path: str) -> None:
    with open(path, 'rb') as f:
        last_line_empty = True
        line_num = 0
        for (line_num, line) in enumerate(f, 1):
            try:
                line = line.decode('ascii')
            except UnicodeDecodeError as e:
                report_error(path, line_num, 'non-ASCII character found')
                last_line_empty = False
                continue
            if len(line) > 170 and not path.endswith('.md'): # minor: this is before we chop off trailing newline
                report_error(path, line_num, 'line over 170 characters')

            for c in [*range(32), 127]: # all non-printable ASCII chars
                c = chr(c)
                if c != '\n' and c in line:
                    report_error(path, line_num, f'{c!r} character found')
            if line.endswith('\n'):
                line = line[:-1]
            else:
                report_error(path, line_num, 'line does not end with a newline')
            if line.endswith(' '):
                report_error(path, line_num, 'trailing whitespace found')
            if not line:
                if last_line_empty:
                    if line_num == 1:
                        report_error(path, line_num, 'extra blank line at start of file')
                    else:
                        report_error(path, line_num, 'two blank lines in a row')
                last_line_empty = True
            else:
                last_line_empty = False
        if last_line_empty:
            report_error(path, line_num, 'extra blank line at end of file')

def main() -> None:
    parser = argparse.ArgumentParser(
        description='Check style of all tracked files (ASCII only, no trailing whitespace, no extra '
                    'blank lines, no tabs, 170-char max). Run from the repo root; takes no arguments.')
    parser.parse_args()

    for (root, dirs, files) in os.walk('.'):
        for name in ['.git', '.vscode', '_out', '__pycache__']:
            if name in dirs:
                dirs.remove(name)
        for name in files:
            path = f'{root}/{name}'
            assert path.startswith('./'), path
            path = path[2:]
            if path not in skip_files:
                check_file(path)

    if n_errors:
        print(f'\n{n_errors} errors found')
        exit(1)

if __name__ == '__main__':
    main()
