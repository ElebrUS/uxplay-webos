#!/usr/bin/env python3
"""Fetch pinned sources and the macOS ARM64 cross-toolchain; verify before extracting."""
import hashlib
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CACHE = ROOT / '.cache'
LOCK = json.loads((ROOT / 'dependencies.lock.json').read_text())
UXPLAY_PATCH = ROOT / 'tools/patches/uxplay-webos14.patch'

def run(*args, **kw):
    subprocess.run(args, check=True, **kw)

def main():
    CACHE.mkdir(exist_ok=True)
    for name, item in LOCK.items():
        if name == 'uxplay':
            target = ROOT / 'vendor' / 'uxplay'
            embedded = ROOT / 'upstream-uxplay-source.tar'
            if embedded.exists() and not (target / '.git').exists():
                if not target.exists():
                    target.parent.mkdir(exist_ok=True)
                    run('tar', '-xf', str(embedded), '-C', str(target.parent))
                check = subprocess.run(['git', 'apply', '--no-index', '--reverse', '--check', str(UXPLAY_PATCH)],
                                       cwd=target, capture_output=True)
                if check.returncode:
                    run('git', 'apply', '--no-index', str(UXPLAY_PATCH), cwd=target)
                continue
            if not (target / '.git').exists():
                target.parent.mkdir(exist_ok=True)
                run('git', 'clone', item['repository'], str(target))
            git = ['git', '-C', str(target)]
            status = subprocess.check_output([*git, 'status', '--porcelain'])
            if status:
                head = subprocess.check_output([*git, 'rev-parse', 'HEAD'], text=True).strip()
                patch = subprocess.check_output([*git, 'diff', '--binary', 'HEAD'])
                if (head != item['commit'] or b'?? ' in status or
                        patch.splitlines() != UXPLAY_PATCH.read_bytes().splitlines()):
                    sys.exit('UxPlay has local modifications; preserve them before bootstrapping.')
            else:
                run(*git, 'checkout', '--detach', item['commit'])
                run(*git, 'apply', str(UXPLAY_PATCH))
            continue
        archive = CACHE / item['archive']
        if not archive.exists():
            run('curl', '-fL', '--retry', '3', item['url'], '-o', str(archive))
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != item['sha256']:
            sys.exit(f'Checksum mismatch: {archive}. Remove this archive and retry.')
        target = CACHE / item['directory']
        if not target.exists():
            run('tar', '-xf', str(archive), '-C', str(CACHE))
            if name == 'toolchain':
                run('bash', str(target / 'relocate-sdk.sh'))
    print('Pinned inputs verified.')

if __name__ == '__main__':
    main()
