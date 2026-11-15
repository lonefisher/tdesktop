"""Exact cache identity and checks for the Windows dependency snapshot.

The published key is ``fishgram-checks-deps-v1-`` plus the SHA-256 of a
canonical JSON document. Restore uses that exact key and no prefix.

The document contains:

- ``preparation``: FishGram's ``deps-windows-v3`` identity. That identity
  hashes ``fishgram.json`` fields ``platform``, ``msvc``, ``windowsSdk``, and
  ``qt``; the runner label; and the bytes of ``prepare-dependencies.ps1``,
  the scripts it runs, and every file under ``Telegram/build/prepare``.
  Those files hold the third-party clone URLs, tags, checkout revisions,
  and patch pins.
- ``runnerLabel``, ``msvcToolset``, ``imageOs``, ``imageVersion``: the
  runner image and the installed MSVC toolset directory, for example
  ``14.44.35207``, not only the recipe's ``14.44`` prefix.
- ``submodulePins``: every gitlink in the tdesktop commit. The tdesktop
  commit itself is not an input, so application-only changes reuse the cache.
- ``lockFiles``: tracked ``vcpkg.json``, ``packages.lock.json``, and
  ``*.lock`` files under ``Telegram/build`` or ``cmake``, except
  ``Telegram/build/docker``. Windows preparation does not read that Linux
  image lock.
- ``cacheScript``: the bytes of this file, so a change to the key or the
  credential guard cannot reuse an older snapshot.

Pull-request jobs restore this key and never save it. The saved object is
the FishGram ``Libraries`` and ``ThirdParty`` archive, which omits ``.git``
and compiler intermediates and refuses to grow past 8 GB.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

KEY_PREFIX = 'fishgram-checks-deps-v1-'
PREPARATION_KEY = re.compile(r'^deps-windows-v3-[0-9a-f]{64}$')
TOOLSET = re.compile(r'^\d+(?:\.\d+){2,3}$')
IMAGE_TOKEN = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,80}$')
GITLINK = re.compile(r'^[0-9a-f]{40}$')
LOCK_ROOTS = ('Telegram/build/', 'cmake/')
LOCK_EXCLUDED = 'Telegram/build/docker/'
DEPENDENCY_ROOTS = ('Libraries', 'ThirdParty')
CREDENTIAL_NAMES = {
    '.git-credentials',
    'id_rsa',
    'id_dsa',
    'id_ecdsa',
    'id_ed25519',
    'fishgram-api.clixml',
}
CREDENTIAL_SUFFIXES = ('.pfx', '.p12', '.snk', '.clixml')
FORBIDDEN_PARTS = {'.private', 'update-trust'}
TOKEN_MARKERS = (b'ghs_', b'github_pat_', b'ghp_', b'gho_')
TOKEN_SKIP_SUFFIXES = {
    '.a', '.dll', '.exe', '.jpg', '.lib', '.nupkg', '.o', '.obj', '.pack',
    '.pdb', '.png', '.pyc', '.rlib', '.ttf', '.woff', '.zip',
}
OMITTED_PARTS = {'.git', '__pycache__'}


def script_digest():
    return hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def _reject(message):
    raise ValueError(message)


def _path_allowed(path):
    if not path or '\\' in path or path.startswith('/'):
        return False
    parts = PurePosixPath(path).parts
    return parts and all(part not in ('', '.', '..') for part in parts)


def make_key(preparation, runner_label, msvc_toolset, image_os, image_version,
             submodule_pins, lock_files, script_hash):
    if not PREPARATION_KEY.fullmatch(preparation):
        _reject('Unexpected preparation identity.')
    if not IMAGE_TOKEN.fullmatch(runner_label or ''):
        _reject('Runner label is required.')
    if not TOOLSET.fullmatch(msvc_toolset or ''):
        _reject('Installed MSVC toolset version is required.')
    if not IMAGE_TOKEN.fullmatch(image_os or '') or not IMAGE_TOKEN.fullmatch(image_version or ''):
        _reject('Runner image OS and version are required.')
    if not re.fullmatch(r'[0-9a-f]{64}', script_hash or ''):
        _reject('Cache script hash is required.')
    pins = []
    seen = set()
    for path, sha in submodule_pins:
        if not _path_allowed(path) or not GITLINK.fullmatch(sha):
            _reject('Invalid submodule pin.')
        if path in seen:
            _reject('Duplicate submodule pin.')
        seen.add(path)
        pins.append({'path': path, 'sha': sha})
    if not pins:
        _reject('Pinned submodule revisions are required.')
    locks = {}
    for path, digest in lock_files.items():
        if not _path_allowed(path) or not re.fullmatch(r'[0-9a-f]{64}', digest):
            _reject('Invalid lock file.')
        locks[path] = digest
    payload = {
        'cacheScript': script_hash,
        'imageOs': image_os,
        'imageVersion': image_version,
        'lockFiles': locks,
        'msvcToolset': msvc_toolset,
        'preparation': preparation,
        'runnerLabel': runner_label,
        'submodulePins': sorted(pins, key=lambda pin: pin['path']),
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(',', ':')).encode('utf-8')
    return KEY_PREFIX + hashlib.sha256(encoded).hexdigest()


def git_output(repo, *args):
    result = subprocess.run(
        ['git', '-c', 'core.quotepath=false', '-C', str(repo), *args],
        check=False, capture_output=True)
    if result.returncode != 0:
        detail = result.stderr.decode('utf-8', 'replace').strip()
        raise SystemExit(detail or 'git failed')
    return result.stdout


def submodule_pins(source):
    pins = []
    for record in git_output(source, 'ls-tree', '-r', '-z', 'HEAD').split(b'\0'):
        if not record:
            continue
        meta, path_bytes = record.split(b'\t', 1)
        mode, kind, sha_bytes = meta.split(b' ')
        if mode != b'160000' or kind != b'commit':
            continue
        pins.append((path_bytes.decode('utf-8'), sha_bytes.decode('ascii').lower()))
    return pins


def _lock_path(path):
    if not path.startswith(LOCK_ROOTS) or path.startswith(LOCK_EXCLUDED):
        return False
    return path.endswith('.lock') or path.endswith('/vcpkg.json') or path.endswith('packages.lock.json')


def lock_files(source):
    files = {}
    for record in git_output(source, 'ls-files', '-z').split(b'\0'):
        if not record:
            continue
        path = record.decode('utf-8')
        if _lock_path(path):
            files[path] = hashlib.sha256((source / path).read_bytes()).hexdigest()
    return files


def load_preparation_key(root, runner_label):
    tools = str(Path(root) / 'tools')
    if tools not in sys.path:
        sys.path.insert(0, tools)
    try:
        import dependency_cache_key
    except ImportError as error:
        raise SystemExit('FishGram dependency cache identity is unavailable.') from error
    return dependency_cache_key.cache_key(Path(root), runner_label)


def _toolset_matches_recipe(root, msvc_toolset):
    recipe_path = Path(root) / 'fishgram.json'
    try:
        recipe = json.loads(recipe_path.read_text(encoding='utf-8-sig'))
        prefix = str(recipe['msvc'])
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit('FishGram recipe does not name an MSVC toolset.') from error
    if not msvc_toolset.startswith(prefix + '.'):
        raise SystemExit('Installed MSVC toolset does not match the recipe.')


def run_key(root, runner_label, msvc_toolset, image_os, image_version,
            preparation_key, github_output=None):
    root = Path(root)
    source = root / 'tdesktop'
    _toolset_matches_recipe(root, msvc_toolset)
    preparation = preparation_key(root, runner_label)
    pins = submodule_pins(source)
    locks = lock_files(source)
    try:
        key = make_key(
            preparation, runner_label, msvc_toolset, image_os, image_version,
            pins, locks, script_digest())
    except ValueError as error:
        raise SystemExit(str(error)) from error
    print(f'runnerLabel={runner_label}', file=sys.stderr)
    print(f'msvcToolset={msvc_toolset}', file=sys.stderr)
    print(f'image={image_os} {image_version}', file=sys.stderr)
    print(f'preparation={preparation}', file=sys.stderr)
    print(f'cacheScript={script_digest()}', file=sys.stderr)
    for path, sha in sorted(pins):
        print(f'submodule {path} {sha}', file=sys.stderr)
    for path in sorted(locks):
        print(f'lock {path} {locks[path]}', file=sys.stderr)
    if not locks:
        print('lockFiles=0', file=sys.stderr)
    print(key)
    if github_output is not None:
        with Path(github_output).open('a', encoding='utf-8', newline='\n') as output:
            output.write(f'key={key}\n')
    return key


def _bucket(relative):
    parts = relative.parts
    if len(parts) >= 3 and parts[0] == 'Libraries' and parts[1] == 'win64':
        return '/'.join(parts[:3])
    if len(parts) >= 2:
        return '/'.join(parts[:2])
    return parts[0]


def directory_report(root, disposable):
    totals = {}
    for name in DEPENDENCY_ROOTS:
        base = Path(root) / name
        if not base.is_dir():
            raise SystemExit('Prepared dependency directory is missing: ' + name)
        for current, directories, files in os.walk(base, followlinks=False):
            current = Path(current)
            relative_dir = current.relative_to(root).parts
            kept_directories = []
            for directory in directories:
                if disposable(relative_dir + (directory,), is_directory=True):
                    continue
                kept_directories.append(directory)
            directories[:] = kept_directories
            for filename in files:
                path = current / filename
                relative = path.relative_to(root)
                try:
                    size = path.lstat().st_size
                except OSError:
                    continue
                bucket = totals.setdefault(_bucket(relative), [0, 0])
                if disposable(relative.parts, is_directory=False):
                    bucket[1] += size
                else:
                    bucket[0] += size
    return totals


def print_report(root):
    tools = str(Path(root) / 'tools')
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import dependency_cache_archive

    totals = directory_report(root, dependency_cache_archive.disposable)
    retained = omitted = 0
    print('directory\tretained_bytes\tomitted_intermediate_bytes\tretained_mib')
    for name, (kept, dropped) in sorted(totals.items(), key=lambda item: -item[1][0]):
        retained += kept
        omitted += dropped
        print(f'{name}\t{kept}\t{dropped}\t{kept / (1024 * 1024):.1f}')
    print(f'TOTAL\t{retained}\t{omitted}\t{retained / (1024 * 1024):.1f}')
    print(
        'Compressed size is the archive_bytes value printed by the pack step. '
        f'Budget: {dependency_cache_archive.MAX_ARCHIVE_BYTES} bytes.')
    return totals


def _omitted_from_archive(parts):
    return any(part.casefold() in OMITTED_PARTS for part in parts)


def credential_hits(root):
    hits = []
    for name in DEPENDENCY_ROOTS:
        base = Path(root) / name
        if not base.exists():
            continue
        for current, directories, files in os.walk(base, followlinks=False):
            current = Path(current)
            directories[:] = [
                directory for directory in directories
                if directory.casefold() not in OMITTED_PARTS]
            for filename in files:
                path = current / filename
                relative = path.relative_to(root)
                parts = relative.parts
                if _omitted_from_archive(parts):
                    continue
                folded = [part.casefold() for part in parts]
                lowered = filename.casefold()
                if (lowered in CREDENTIAL_NAMES or lowered.endswith(CREDENTIAL_SUFFIXES)
                        or any(part in FORBIDDEN_PARTS for part in folded)):
                    hits.append(relative.as_posix() + ' name')
                    continue
                if lowered.endswith(tuple(TOKEN_SKIP_SUFFIXES)):
                    continue
                try:
                    info = path.lstat()
                except OSError:
                    continue
                if info.st_size > 2 * 1024 * 1024 or info.st_size == 0:
                    continue
                with path.open('rb') as handle:
                    sample = handle.read(256 * 1024)
                if any(marker in sample for marker in TOKEN_MARKERS):
                    hits.append(relative.as_posix() + ' token')
    return sorted(hits)


def guard(root):
    missing = [name for name in DEPENDENCY_ROOTS if not (Path(root) / name).is_dir()]
    if missing:
        raise SystemExit('Prepared dependency directory is missing: ' + ', '.join(missing))
    hits = credential_hits(root)
    if hits:
        print('Refusing to cache credential-like files:', file=sys.stderr)
        for hit in hits:
            print(hit, file=sys.stderr)
        raise SystemExit('Dependency tree contains credential-like files.')
    print('No credential-like files in the dependency trees.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    key = commands.add_parser('key')
    key.add_argument('--root', type=Path, required=True)
    key.add_argument('--runner-label', required=True)
    key.add_argument('--msvc-toolset', required=True)
    key.add_argument('--image-version', required=True)
    key.add_argument('--image-os', required=True)
    key.add_argument('--github-output', type=Path)
    report = commands.add_parser('report')
    report.add_argument('--root', type=Path, required=True)
    check = commands.add_parser('guard')
    check.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'key':
        run_key(
            args.root, args.runner_label, args.msvc_toolset, args.image_os,
            args.image_version, load_preparation_key, args.github_output)
    elif args.command == 'report':
        print_report(args.root)
    else:
        guard(args.root)


if __name__ == '__main__':
    main()
