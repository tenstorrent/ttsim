# SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# Top-level make.py rules: aggregates each subdirectory's :build into the repo-wide :build.
def rules(ctx):
    ctx.rule(':build', [
        'src/:build',
    ])
