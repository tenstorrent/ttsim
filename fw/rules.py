# SPDX-FileCopyrightText: (c) 2025-2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# Provides a top level fw/:build target.
def rules(ctx):
    ctx.rule(':build', [
        'eth/:build',
    ])
