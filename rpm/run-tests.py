#!/usr/bin/python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Check the selected FreeTDS driver without requiring a database server."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def select_module(paths):
    modules = [p for root in paths for p in root.glob('freetds-api-*.qmod')]
    if len(modules) != 1:
        raise RuntimeError('Expected exactly one FreeTDS module: ' + repr(modules))
    return modules[0]


def verify_suite(output):
    if not re.search(r'^Ran 4 test cases, 4 succeeded \(14 assertions\)$', output, re.M):
        raise RuntimeError('Incomplete FreeTDS driver coverage')
    if re.search(r'^Skipped:|warning:', output, re.M | re.I):
        raise RuntimeError('Unexpected skipped test or diagnostic')


def run(build=None, compiler=False):
    source = Path(__file__).resolve().parents[1]
    env = os.environ.copy()
    for key in ('QORE_MODULE_DIR', 'QORE_MODULE_DIR_ONLY', 'QORE_INCLUDE_DIR', 'LD_LIBRARY_PATH',
                'LD_PRELOAD', 'FreeTDS_INCLUDE_DIR', 'FreeTDS_LIBS', 'FREETDS', 'FREETDSCONF',
                'TDSVER', 'TDSDUMP', 'TDSDUMPCONFIG', 'SYBASE', 'SYBASE_OCS'):
        env.pop(key, None)
    env.update(LC_ALL='C.UTF-8', TZ='UTC')
    paths = subprocess.check_output(['/usr/bin/qore', '--module-path'], env=env, text=True).strip().split(':')
    module = select_module([build.resolve()] if build else [Path(p) for p in dict.fromkeys(paths)])
    env.update(QORE_MODULE_DIR=':'.join(dict.fromkeys([str(module.parent), *paths])), QORE_MODULE_DIR_ONLY='1')
    with tempfile.TemporaryDirectory(prefix='qore-freetds-rpm-') as directory:
        root = Path(directory)
        suite = (source / 'test/freetds-offline.qtest').read_text()
        (root / 'freetds-offline.qtest').write_text(re.sub(r'^%prepend-module-path .*\n', '', suite, flags=re.M))
        completed = subprocess.run(['/usr/bin/qore', '-b', '--enable-debug', '-l', str(module),
                                    str(root / 'freetds-offline.qtest'), '-v'],
                                   env=env, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, check=True, timeout=90)
        print(completed.stdout, end='', flush=True)
        verify_suite(completed.stdout)
        if compiler:
            env['AUTOPKGTEST_TMP'] = str(root)
            subprocess.run(['/bin/sh', str(source / 'debian/tests/compiler')],
                           env=env, cwd=root, check=True, timeout=120)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build-dir', type=Path)
    mode.add_argument('--installed', action='store_true')
    parser.add_argument('--compiler', action='store_true')
    args = parser.parse_args()
    run(args.build_dir, args.compiler)
