#!/usr/bin/env python3
#
# make.py (https://github.com/mjcraighead/make-py)
# Copyright (c) 2012-2026 Matt Craighead
# SPDX-License-Identifier: MIT
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
# associated documentation files (the "Software"), to deal in the Software without restriction,
# including without limitation the rights to use, copy, modify, merge, publish, distribute,
# sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all copies or
# substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
# OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

import argparse
import ast
import builtins
import contextlib
import functools
import hashlib
import importlib.util
import inspect
import itertools
import os
import pickle
import platform
import queue
import re
import shlex
import shutil
import subprocess
import sys
import threading
from types import MappingProxyType, SimpleNamespace
from typing import Any, Dict, Iterator, List, NoReturn, Optional, Set, Tuple, cast

# Disable creation of __pycache__/.pyc files from rules.py files
sys.dont_write_bytecode = True

tasks: Dict[str, 'Task'] = {}
make_db: Dict[str, Dict[str, Optional[str]]] = {}
task_queue = queue.PriorityQueue()
event_queue = queue.Queue()
priority_queue_counter = itertools.count() # tiebreaker counter to fall back to FIFO when task priorities are the same
any_tasks_failed = False # global failure flag across all tasks in this run
default_subprocess_env = None # default inherited env for subprocess.run

try:
    progress_columns = os.get_terminal_size().columns - 1 # avoid last column to prevent line wrap
except OSError:
    progress_columns = None # stdout is not attached to a terminal

def die(msg: str) -> NoReturn:
    print(msg)
    sys.exit(1)

def expect(cond: bool, path: str, lineno: int, msg: str) -> None:
    if not cond:
        die(f'ERROR: {os.path.relpath(path)}:{lineno}: {msg}')

def get_timestamp_if_exists(path: str) -> float:
    """Return the modification time of 'path', or -1.0 if nonexistent, using only one stat() call."""
    try:
        return os.stat(path).st_mtime
    except FileNotFoundError:
        return -1.0

if os.name == 'nt': # evaluate this condition only once, rather than per call, for performance
    @functools.lru_cache(maxsize=None)
    def normpath(path: str) -> str:
        return os.path.normpath(path).lower().replace('\\', '/')

    def joinpath(cwd: str, path: str) -> str:
        return path if (path[0] == '/' or path[1:2] == ':') else f'{cwd}/{path}'
else:
    @functools.lru_cache(maxsize=None)
    def normpath(path: str) -> str:
        return os.path.normpath(path)

    def joinpath(cwd: str, path: str) -> str:
        return path if path[0] == '/' else f'{cwd}/{path}'

def execute(task: 'Task', verbose: bool) -> None:
    """Run task command, capture/filter its output, update bookkeeping, and log to event_queue."""
    # Output file existence is not checked here; missing outputs are detected in schedule() when they are consumed as inputs.
    try:
        # Historical note: before Python 3.4 on Windows, subprocess.Popen() calls could inherit unrelated file handles
        # from other threads, leading to very strange file locking errors.  Fixed by: https://peps.python.org/pep-0446/
        result = subprocess.run(task.cmd, cwd=task.cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=default_subprocess_env)
        out = result.stdout.decode('utf-8', 'replace') # Assumes UTF-8, but robust if not -- XXX consider changing out to bytes
        code = result.returncode
    except Exception as e:
        out = str(e)
        code = 1
    if task.msvc_show_includes:
        # Parse MSVC /showIncludes output, skipping system headers
        r = re.compile(r'^Note: including file:\s*(.*)$')
        (inputs, new_out) = (set(), [])
        for line in out.rstrip().splitlines(): # XXX .rstrip() is probably not needed here
            m = r.match(line)
            if m:
                input = normpath(m.group(1))
                if not input.startswith('c:/program files'):
                    inputs.add(input)
            else:
                new_out.append(line)
        out = '' if len(new_out) == 1 else '\n'.join(new_out) # drop lone "source.c" line printed by MSVC

        # Write a make-style depfile listing all included headers
        tmp_path = f'{task.depfile}.tmp'
        parts = [f'{task.outputs[0]}:', *sorted(inputs)] # we checked for only 1 output at task declaration time
        with open(tmp_path, 'w', encoding='utf-8') as f:
            f.write(' \\\n  '.join(parts) + '\n') # add line continuations and indentation
        os.replace(tmp_path, task.depfile)
    elif task.output_exclude:
        r = re.compile(task.output_exclude)
        out = '\n'.join(line for line in out.splitlines() if not r.match(line))
    if out and not task.allow_output:
        code = 1 # any output that hasn't been filtered by msvc_show_includes/output_exclude is an error unless explicitly declared otherwise

    built_text = 'Built %s.\n' % '\n  and '.join(repr(output) for output in task.outputs)
    if progress_columns is not None: # need to precede "Built [...]" with erasing the current progress indicator
        built_text = '\r%s\r%s' % (' ' * progress_columns, built_text)

    if verbose or code:
        if os.name == 'nt':
            quoted_cmd = subprocess.list2cmdline(task.cmd)
        else:
            quoted_cmd = ' '.join(shlex.quote(x) for x in task.cmd) # XXX switch to shlex.join once we drop 3.6/3.7 support
        out = f'{quoted_cmd}\n{out}'
    if code:
        global any_tasks_failed
        any_tasks_failed = True
        event_queue.put(('log', f'{built_text}{out.rstrip()}\n\n'))
        for output in task.outputs:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(output)
        return

    local_make_db = make_db[task.cwd]
    signature = task.signature()
    for output in task.outputs:
        assert output in local_make_db, output # make sure slot is already allocated
        local_make_db[output] = signature
    if out:
        event_queue.put(('log', f'{built_text}{out.rstrip()}\n\n'))
    elif progress_columns is None:
        event_queue.put(('log', built_text))

class WorkerThread(threading.Thread):
    def __init__(self, verbose: bool):
        super().__init__()
        self.verbose = verbose

    def run(self) -> None:
        while not any_tasks_failed:
            (_, _, task) = task_queue.get()
            if task is None:
                break
            if task.cmd is not None:
                event_queue.put(('start', task))
                execute(task, self.verbose)
            event_queue.put(('finish', task))

# Note: external orchestrators may predeclare certain outputs as hermetically clean.
# make.py treats such declarations as axiomatic -- they come from elsewhere.
def schedule(output: str, visited: Set[str], enqueued: Set['Task'], completed: Set[str]) -> None:
    if output in visited or output in completed:
        return
    task = tasks[output]
    visited.update(task.outputs)
    if task in enqueued:
        return

    # Recurse into inputs and order-only inputs and wait for them to complete
    # Never recurse into depfile inputs here, as the .d file could be stale/garbage from a previous run
    for input in itertools.chain(task.inputs, task.order_only_inputs):
        if input in tasks:
            schedule(input, visited, enqueued, completed)
        else:
            visited.add(input)
            completed.add(input)
    if any(input not in completed for input in itertools.chain(task.inputs, task.order_only_inputs)):
        return

    # Error if any of the inputs does not exist -- they should always exist by this point
    input_timestamps = [get_timestamp_if_exists(input) for input in task.inputs]
    for (input, input_timestamp) in zip(task.inputs, input_timestamps):
        if input_timestamp < 0:
            if input not in tasks or tasks[input].cmd is not None: # source file or real (not phony) rule; do not check for phony rules
                global any_tasks_failed
                any_tasks_failed = True
                msg = f"ERROR: input {input!r} of {' '.join(repr(output) for output in task.outputs)} is nonexistent"
                if progress_columns is not None:
                    msg = '\r%s\r%s' % (' ' * progress_columns, msg)
                die(msg)

    if task.cmd is not None:
        # Do all outputs exist, and are all of them at least as new as every single input?
        local_make_db = make_db[task.cwd]
        output_timestamp = min(get_timestamp_if_exists(output) for output in task.outputs) # oldest output timestamp, or -1.0 if any output is nonexistent
        if output_timestamp >= 0 and all(input_timestamp <= output_timestamp for input_timestamp in input_timestamps):
            # Is the task's signature identical to the last time we ran it?
            signature = task.signature()
            if all(local_make_db.get(output) == signature for output in task.outputs):
                # Parse the depfile, if present
                depfile_inputs = []
                if task.depfile:
                    try:
                        with open(task.depfile, encoding='utf-8') as f:
                            depfile_inputs = f.read().replace('\\\n', '')
                        if '\\' in depfile_inputs: # shlex.split is slow, don't use it unless we really need it
                            depfile_inputs = shlex.split(depfile_inputs)[1:]
                        else:
                            depfile_inputs = depfile_inputs.split()[1:]
                        depfile_inputs = [normpath(joinpath(task.cwd, x)) for x in depfile_inputs]
                    except FileNotFoundError:
                        depfile_inputs = None # depfile was expected but missing -- always dirty
                    except Exception: # anything else that went wrong
                        msg = f"WARNING: malformed depfile for {' '.join(repr(output) for output in task.outputs)} (will rebuild)"
                        if progress_columns is not None:
                            msg = '\r%s\r%s' % (' ' * progress_columns, msg)
                        print(msg)
                        depfile_inputs = None

                # Do all depfile_inputs exist, and are all outputs at least as new as every single depfile_input?
                if depfile_inputs is not None and all(0 <= get_timestamp_if_exists(input) <= output_timestamp for input in depfile_inputs):
                    completed.update(task.outputs)
                    return # skip the task

        # Remove stale outputs immediately once this task is marked dirty
        for output in task.outputs:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(output)
            assert output in local_make_db, output # make sure slot is already allocated
            local_make_db[output] = None

        # Ensure outputs' parent directories exist
        for output in task.outputs:
            os.makedirs(os.path.dirname(output), exist_ok=True)

    # Enqueue this task to the worker threads -- note that PriorityQueue needs the sense of priority reversed
    task_queue.put((-task.priority, next(priority_queue_counter), task))
    enqueued.add(task)

class Task:
    __slots__ = ('outputs', 'inputs', 'cwd', 'cmd', 'depfile', 'order_only_inputs', 'msvc_show_includes', 'allow_output', 'output_exclude',
                 'latency', 'priority', 'path', 'lineno')
    def __init__(self, outputs, inputs, cwd, cmd, depfile, order_only_inputs, msvc_show_includes, allow_output, output_exclude, latency, path, lineno):
        self.outputs = tuple(sorted(outputs)) # freeze lists into canonical tuples for downstream logic
        self.inputs = tuple(sorted(inputs))
        self.cwd = cwd
        self.cmd = tuple(cmd) if cmd is not None else cmd
        self.depfile = depfile
        self.order_only_inputs = tuple(sorted(order_only_inputs))
        self.msvc_show_includes = msvc_show_includes
        self.allow_output = allow_output
        self.output_exclude = output_exclude
        self.latency = latency
        self.priority = -1 # "unvisited" sentinel priority; zero is a valid computed priority
        self.path = path
        self.lineno = lineno

    # latency/priority/path/lineno are excluded from signatures because they do not affect the outputs' content.
    # outputs, inputs, and order_only_inputs are included since they alter DAG structure (and thus execution ordering and correctness).
    def signature(self) -> str:
        info = (self.outputs, self.inputs, self.cwd, self.cmd, self.depfile, self.order_only_inputs, self.msvc_show_includes, self.allow_output, self.output_exclude)
        return hashlib.sha256(pickle.dumps(info, protocol=4)).hexdigest() # XXX bump to protocol=5 once we drop 3.6/3.7 support

# make.py canonicalized host ABI detection: runs on any plausible system in 2026 with Python 3.6+.
# ctx.host.os = normalized OS ABI family (kernel/loader/libc), ctx.host.arch = normalized CPU ISA family.
os_map = {
    'Windows': 'windows', 'Linux': 'linux', 'Darwin': 'darwin',
    'FreeBSD': 'freebsd', 'OpenBSD': 'openbsd', 'NetBSD': 'netbsd',
    'DragonFly': 'dragonflybsd', 'SunOS': 'sunos',
}
arch_map = {
    'AMD64': 'x86_64', 'x86_64': 'x86_64',
    'x86': 'x86_32', 'i686': 'x86_32',
    'ARM64': 'aarch64', 'aarch64': 'aarch64', 'arm64': 'aarch64',
    'ppc64le': 'ppc64le', 'riscv64': 'riscv64', 's390x': 's390x',
}
def detect_host() -> SimpleNamespace:
    (system, machine) = (platform.system(), platform.machine())
    if system not in os_map or machine not in arch_map:
        die(f'ERROR: host detection failed: system={system!r} machine={machine!r}')
    return SimpleNamespace(os=os_map[system], arch=arch_map[machine])

class EvalContext:
    __slots__ = ('cwd', 'env', 'host', 'path')
    def rule(self, outputs, inputs, *, cmd=None, depfile=None, order_only_inputs=None,
             msvc_show_includes=False, allow_output=False, output_exclude=None, latency=1) -> None:
        frame = inspect.currentframe()
        assert frame is not None
        frame = frame.f_back
        assert frame is not None
        (path, lineno) = (frame.f_code.co_filename, frame.f_lineno)
        cwd = self.cwd
        if not isinstance(outputs, list):
            expect(isinstance(outputs, str), path, lineno, 'outputs must be either a str or a list')
            outputs = [outputs]
        if cmd is None: # phony rule -- not allowed to have commands
            expect(all(o.startswith(':') for o in outputs), path, lineno, 'phony rule outputs must start with :')
            expect(not any('/' in o for o in outputs), path, lineno, 'phony rule outputs must not contain path separators')
            expect(depfile is None, path, lineno, 'phony rules cannot have depfiles')
            expect(order_only_inputs is None, path, lineno, 'phony rules cannot have order_only_inputs')
            expect(msvc_show_includes == False, path, lineno, 'phony rules cannot set msvc_show_includes')
            expect(output_exclude is None, path, lineno, 'phony rules cannot set output_exclude')
            latency = 0 # no command, therefore zero execution latency
        else: # real rule -- must have a command
            expect(all(o.startswith('_out/') for o in outputs), path, lineno, "rule output paths must start with '_out/'")
            expect(isinstance(cmd, list) and all(isinstance(x, str) for x in cmd), path, lineno, 'real rules must set cmd=[argv_list]')
        outputs = [normpath(joinpath(cwd, x)) for x in outputs]
        expect(len(outputs) == len(set(outputs)), path, lineno, 'outputs contains duplicate paths')
        if not isinstance(inputs, list):
            expect(isinstance(inputs, str), path, lineno, 'inputs must be either a str or a list')
            inputs = [inputs]
        inputs = [normpath(joinpath(cwd, x)) for x in inputs]
        expect(len(inputs) == len(set(inputs)), path, lineno, 'inputs contains duplicate paths')
        if depfile is not None:
            expect(isinstance(depfile, str), path, lineno, 'depfile must be either None or a str')
            depfile = normpath(joinpath(cwd, depfile))
        if order_only_inputs is None:
            order_only_inputs = []
        expect(isinstance(order_only_inputs, list), path, lineno, 'order_only_inputs must be either None or a list')
        order_only_inputs = [normpath(joinpath(cwd, x)) for x in order_only_inputs]
        expect(len(order_only_inputs) == len(set(order_only_inputs)), path, lineno, 'order_only_inputs contains duplicate paths')
        expect(output_exclude is None or isinstance(output_exclude, str), path, lineno, 'output_exclude must be either None or a str')
        if msvc_show_includes:
            expect(len(outputs) == 1, path, lineno, 'msvc_show_includes requires only a single output')

        task = Task(outputs, inputs, cwd, cmd, depfile, order_only_inputs, msvc_show_includes, allow_output, output_exclude, latency, path, lineno)
        for output in outputs:
            if output in tasks:
                die(f'ERROR: multiple tasks declare {output!r}:\n'
                    f'  first declared at {os.path.relpath(tasks[output].path)}:{tasks[output].lineno}\n'
                    f'  again declared at {os.path.relpath(path)}:{lineno}')
            tasks[output] = task
            if output not in make_db[cwd]:
                make_db[cwd][output] = None # preallocate a slot for every possible output in the make_db before we launch the WorkerThreads

# Reject disallowed constructs in rules.py -- a non-Turing-complete Starlark-like DSL
BANNED_AST_NODES = (
    ast.While, ast.Lambda, # prevent infinite loops and infinite recursion
    ast.Import, ast.ImportFrom,
    ast.With, ast.AsyncFunctionDef, ast.AsyncFor, ast.AsyncWith,
    ast.Global, ast.Nonlocal, ast.Delete, ast.ClassDef,
    ast.Try, ast.Raise, ast.Yield, ast.YieldFrom, ast.Await,
    getattr(ast, 'NamedExpr', ()), getattr(ast, 'Match', ()), getattr(ast, 'TryStar', ()), # Python feature additions in 3.8+, 3.10+, 3.11+ respectively
    getattr(ast, 'TypeAlias', ()), getattr(ast, 'TypeVar', ()), getattr(ast, 'ParamSpec', ()), getattr(ast, 'TypeVarTuple', ()), # Python feature additions in 3.12+
    getattr(ast, 'TemplateStr', ()), getattr(ast, 'Interpolation', ()), # Python feature additions in 3.14+
)
BANNED_ATTRS = {'encode', 'translate', 'maketrans', 'to_bytes', 'from_bytes'} # ban attributes of str and int that don't make sense in our limited type system
def validate_rules_py_ast(tree, path: str) -> None:
    for node in ast.walk(tree):
        if isinstance(node, BANNED_AST_NODES):
            expect(False, path, node.lineno, f'{type(node).__name__} not allowed') # type: ignore[attr-defined]
        if isinstance(node, ast.Attribute):
            expect(isinstance(node.ctx, ast.Load), path, node.lineno, 'write access to attributes not allowed')
            expect(node.attr not in BANNED_ATTRS and not node.attr.startswith('__'), path, node.lineno, f"access to '.{node.attr}' attribute not allowed")
        if isinstance(node, ast.BinOp):
            expect(not isinstance(node.op, ast.Div), path, node.lineno, 'float division (/) not allowed -- use // if you really mean integer division')
            expect(not isinstance(node.op, ast.Pow), path, node.lineno, 'exponentiation operator (**) not allowed')
        if isinstance(node, ast.Constant) and isinstance(node.value, (bytes, complex, float)): # note: small loophole on 3.6/3.7, which uses ast.Bytes/Num instead
            expect(False, path, node.lineno, f'{type(node.value).__name__} literal not allowed')

SAFE_BUILTINS = (
    'len', 'range', 'print', 'repr', # essentials and debugging
    'enumerate', 'zip', 'sorted', 'reversed', # common iteration helpers
    'list', 'dict', 'set', 'tuple', 'frozenset', 'str', 'int', 'bool', # basic types/constructors
    'abs', 'min', 'max', 'sum', 'any', 'all', # math and logic
)
safe_builtins = {name: getattr(builtins, name) for name in SAFE_BUILTINS}

def eval_rules_py(ctx: EvalContext, verbose: bool, pathname: str, index: int) -> None:
    if verbose:
        print(f'Parsing {pathname!r}...')
    with open(pathname, encoding='utf-8') as f:
        source = f.read()
    tree = ast.parse(source, filename=pathname)
    validate_rules_py_ast(tree, pathname)

    spec = importlib.util.spec_from_file_location(f'rules{index}', pathname)
    if spec is None or spec.loader is None:
        die(f'ERROR: cannot import {pathname!r}')
    rules_py_module = importlib.util.module_from_spec(spec)
    rules_py_module.__dict__['__builtins__'] = safe_builtins
    spec.loader.exec_module(rules_py_module)

    dirname = os.path.dirname(pathname)
    if dirname not in make_db:
        make_db[dirname] = {}
        with contextlib.suppress(FileNotFoundError):
            with open(f'{dirname}/_out/.make.db', encoding='utf-8') as f:
                make_db[dirname] = cast(Dict[str, Optional[str]], dict(line.rstrip().rsplit(' ', 1) for line in f))
    ctx.cwd = dirname
    rules_py_module.rules(ctx)

def locate_rules_py_dir(path: str) -> Optional[str]:
    for pattern in ('/_out/', '/:'): # look for standard and phony rules
        i = path.rfind(pattern)
        if i >= 0:
            return path[:i] # rules.py lives in the parent directory
    return None

def discover_rules(ctx: EvalContext, verbose: bool, output: str, visited_files: Set[str], visited_dirs: Set[str], _active: Set[str]) -> None:
    if output in _active:
        die(f'ERROR: cycle detected involving {output!r}')
    if output in visited_files:
        return
    visited_files.add(output)

    # Locate and evaluate the rules.py for this output (if we haven't already evaluated it)
    rules_py_dir = locate_rules_py_dir(output)
    if rules_py_dir is None:
        return # this is a source file, not an output file or a phony rule name -- we are done
    if rules_py_dir not in visited_dirs:
        eval_rules_py(ctx, verbose, f'{rules_py_dir}/rules.py', len(visited_dirs))
        visited_dirs.add(rules_py_dir)

    if output not in tasks:
        die(f'ERROR: no rule to make {output!r}')
    task = tasks[output]
    _active.add(output)
    for input in itertools.chain(task.inputs, task.order_only_inputs):
        discover_rules(ctx, verbose, input, visited_files, visited_dirs, _active)
    _active.remove(output)

def propagate_latencies(task: Task, latency: int) -> None:
    latency += task.latency
    if latency <= task.priority:
        return # nothing to do -- we are not increasing the priority of this task
    task.priority = latency # update this task's latency and recurse
    for input in itertools.chain(task.inputs, task.order_only_inputs):
        if input in tasks:
            propagate_latencies(tasks[input], latency)

def drain_event_queue() -> Iterator[Tuple[str, Any]]:
    """Drain and yield all pending event_queue entries; blocks until at least one is available."""
    while True:
        try:
            yield event_queue.get(timeout=0.05 if os.name == 'nt' else None) # note: blocks Ctrl-C for up to 0.05s on Windows
            break
        except queue.Empty:
            continue # keep trying until we get at least one event (only hit on Windows)
    while True:
        try:
            yield event_queue.get_nowait()
        except queue.Empty:
            break

def parse_env_args(env_args: List[str]) -> MappingProxyType:
    if os.name == 'nt': # Windows: inject the smallest viable subset of os.environ needed to execute system tools
        keys = ('ProgramFiles', 'ProgramFiles(x86)', 'CommonProgramFiles', 'CommonProgramFiles(x86)', 'SystemRoot', 'ComSpec',
                'TEMP', 'TMP', 'PATH', 'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE')
        env = {k: os.environ[k] for k in keys if k in os.environ}
    else:
        env = {} # POSIX: no injection; hermetic by default
    for arg in env_args:
        if '=' not in arg:
            die(f'ERROR: invalid --env format (expected key=value): {arg!r}')
        (k, v) = arg.split('=', 1)
        if not k.isidentifier():
            die(f'ERROR: invalid key name for --env: {k!r}')
        env[k] = v
    return MappingProxyType(env)

def minimal_env(ctx: EvalContext) -> Dict[str, str]:
    if ctx.host.os == 'windows': # currently identical to parse_env_args above, but may diverge if more are needed
        keys = ('ProgramFiles', 'ProgramFiles(x86)', 'CommonProgramFiles', 'CommonProgramFiles(x86)', 'SystemRoot', 'ComSpec',
                'TEMP', 'TMP', 'PATH', 'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE')
        return {k: os.environ[k] for k in keys if k in os.environ}
    else:
        path = '/usr/local/bin:/usr/bin:/bin'
        if ctx.host.os == 'sunos':
            path = '/usr/xpg4/bin:' + path
        return {
            'PATH': path,
            'USER': 'nobody',
            'SHELL': '/bin/sh',
            'LC_ALL': 'C.UTF-8',
            'LANG': 'C.UTF-8',
            'TZ': 'UTC',
            'SOURCE_DATE_EPOCH': '0',
            **{k: os.environ[k] for k in ('HOME', 'TMPDIR') if k in os.environ}
        }

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('-c', '--clean', action='store_true', help='clean _out directories first')
    parser.add_argument('-j', '--jobs', action='store', type=int, help='specify the number of parallel jobs (defaults to one per CPU)')
    parser.add_argument('-v', '--verbose', action='store_true', help='print verbose output')
    parser.add_argument('--env', action='append', default=[], help="set ctx.env['KEY'] to VALUE in rules.py evaluation environment", metavar='KEY=VALUE')
    parser.add_argument('--inherit-env', action='store_true', help='explicitly inherit full host environment in subprocesses')
    parser.add_argument('outputs', nargs='+', help='outputs to make')
    args = parser.parse_args()
    if args.jobs is not None and args.jobs < 1:
        parser.error("--jobs must be >= 1")
    jobs: int = args.jobs or os.cpu_count() or 1 # default to one job per CPU
    cwd = os.getcwd()
    outputs = [normpath(joinpath(cwd, x)) for x in args.outputs]

    # Set up EvalContext and task DB, reading in .make.db files as we go
    ctx = EvalContext()
    ctx.host = detect_host()
    ctx.env = parse_env_args(args.env)
    ctx.path = SimpleNamespace(expanduser=os.path.expanduser) # XXX temporary hole permitted in our sandbox to allow tasks to access ~
    global default_subprocess_env
    default_subprocess_env = os.environ.copy() if args.inherit_env else minimal_env(ctx) # use hermetic environment unless overridden by --inherit-env
    (visited_files, visited_dirs) = (set(), set())
    for output in outputs:
        discover_rules(ctx, args.verbose, output, visited_files, visited_dirs, set())
    for output in outputs:
        if output not in tasks:
            die(f'ERROR: no rule to make {output!r}')
        propagate_latencies(tasks[output], 0)

    # Clean up stale outputs from previous runs that no longer have tasks; also do an explicitly requested clean
    for (cwd, db) in make_db.items():
        if args.clean:
            dirname = f'{cwd}/_out'
            if os.path.exists(dirname):
                print(f'Cleaning {dirname!r}...')
                shutil.rmtree(dirname)
            for output in db:
                db[output] = None
        for (output, signature) in list(db.items()):
            if output not in tasks and signature is not None:
                with contextlib.suppress(FileNotFoundError):
                    os.unlink(output)
                    print(f'Deleted stale output {output!r}.')
                del db[output]

    # Create and start worker threads
    threads = [WorkerThread(args.verbose) for i in range(jobs)]
    for t in threads:
        t.daemon = True # Design note: daemon threads act as a safety net to guarantee process exit if shutdown/join fails.
        t.start()

    # Main loop: schedule/execute tasks, report progress, and shut down as cleanly as possible if we get a Ctrl-C
    try:
        (enqueued, completed, running) = (set(), set(), set())
        while not all(output in completed for output in outputs):
            # Enqueue tasks to the workers
            visited = set()
            for output in outputs:
                schedule(output, visited, enqueued, completed)
            if all(output in completed for output in outputs):
                break # schedule() may have marked more tasks completed

            # Handle events from worker threads, then show progress update and exit if done
            # Be careful about iterating over data structures being edited concurrently by the WorkerThreads
            for (status, payload) in drain_event_queue():
                if status == 'start':
                    running.add(payload)
                elif status == 'finish':
                    running.discard(payload)
                    completed.update(payload.outputs)
                else:
                    assert status == 'log', status
                    sys.stdout.write(payload)
                    sys.stdout.flush()
            if any_tasks_failed:
                break
            if progress_columns is not None:
                remaining_count = len((visited - completed) & tasks.keys())
                if remaining_count:
                    def format_task_outputs(task):
                        outputs = [output.rsplit('/', 1)[-1] for output in task.outputs]
                        return outputs[0] if len(outputs) == 1 else f"[{' '.join(sorted(outputs))}]"
                    names = ' '.join(sorted(format_task_outputs(task) for task in running))
                    progress = f'make.py: {remaining_count} left, building: {names}'
                else:
                    progress = ''
                if len(progress) < progress_columns:
                    pad = progress_columns - len(progress)
                    progress += ' ' * pad # erase old contents
                    progress += '\b' * pad # put cursor back at end of line
                else:
                    progress = progress[:progress_columns]
                sys.stdout.write('\r' + progress)
                sys.stdout.flush()
    finally:
        # Shut down the system by sending sentinel tokens to all the threads
        for i in range(jobs):
            task_queue.put((1000000, 0, None)) # lower priority than any real task
        for t in threads:
            t.join()

        # Write out the final .make.db files (checkpoint at shutdown)
        # Design note: an append-only journal would be an interesting alternative to improve crash resilience.
        for (cwd, db) in make_db.items():
            db = {output: signature for (output, signature) in db.items() if signature is not None} # remove None tombstones
            if db:
                with contextlib.suppress(FileExistsError):
                    os.mkdir(f'{cwd}/_out')
                tmp_path = f'{cwd}/_out/.make.db.tmp'
                with open(tmp_path, 'w', encoding='utf-8') as f:
                    f.write(''.join(f'{output} {signature}\n' for (output, signature) in db.items()))
                os.replace(tmp_path, f'{cwd}/_out/.make.db')
            else:
                with contextlib.suppress(FileNotFoundError):
                    os.unlink(f'{cwd}/_out/.make.db')

    if any_tasks_failed:
        sys.exit(1)

if __name__ == '__main__':
    main()
# To those who understand what this really is: you already know where to look.
