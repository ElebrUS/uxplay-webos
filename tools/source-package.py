#!/usr/bin/env python3
"""Package corresponding source and original dependency sources; exclude device data."""
import argparse
import json
import pathlib
import subprocess
import tarfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--safari', action='store_true')
    args = parser.parse_args()
    lock=json.loads((ROOT/'dependencies.lock.json').read_text())
    version=json.loads((ROOT/'packaging/appinfo.json').read_text())['version']
    variant = 'safari-' if args.safari else ''
    destination=ROOT/'dist'/f'uxplay-webos-{variant}{version}-source.tar.gz'
    destination.parent.mkdir(exist_ok=True)
    subprocess.run(['python3',str(ROOT/'tools/bootstrap.py')],check=True)
    # Reuse the embedded revision in a source delivery. Create it from Git
    # when packaging a checkout.
    upstream = ROOT / 'upstream-uxplay-source.tar'
    if not upstream.is_file():
        upstream = ROOT / '.cache/uxplay-source.tar'
        with upstream.open('wb') as out:
            subprocess.run(['git', '-C', str(ROOT/'vendor/uxplay'), 'archive', '--format=tar',
                            '--prefix=uxplay/', lock['uxplay']['commit']], stdout=out, check=True)
    with tarfile.open(destination,'w:gz') as archive:
        for name in ['src','tools','tests','packaging','docs','CMakeLists.txt','dependencies.lock.json','README.md','CHANGELOG.md','THIRD_PARTY.md','LICENSE','.github','.gitattributes']:
            archive.add(ROOT/name,arcname='uxplay-webos/'+name,filter=lambda info:None if '__pycache__' in info.name or info.name.endswith('.pyc') else info)
        for name in ['openssl','libplist','ffmpeg','jitterentropy']:
            filename=lock[name]['archive']
            archive.add(ROOT/'.cache'/filename,arcname='uxplay-webos/.cache/'+filename)
        archive.add(upstream,arcname='uxplay-webos/upstream-uxplay-source.tar')
    print(destination)

if __name__=='__main__':
    main()
