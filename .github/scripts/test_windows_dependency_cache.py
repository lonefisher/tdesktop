import hashlib
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import windows_dependency_cache as cache


ROOT = Path(__file__).resolve().parents[2]
PREPARATION = 'deps-windows-v3-' + ('ab' * 32)
SCRIPT = 'cd' * 32


def git(repo, *args):
    subprocess.run(['git', '-C', str(repo), *args], check=True, capture_output=True)


def make_repo():
    temporary = tempfile.TemporaryDirectory()
    repo = Path(temporary.name)
    git(repo, 'init', '-q')
    git(repo, 'config', 'user.email', 'cache-test@example.com')
    git(repo, 'config', 'user.name', 'Cache Test')
    git(repo, 'config', 'commit.gpgsign', 'false')
    (repo / 'README').write_text('source\n', encoding='utf-8')
    git(repo, 'add', 'README')
    git(repo, 'commit', '-q', '-m', 'init')
    return temporary, repo


class CacheKeyTests(unittest.TestCase):
    def key(self, pins=(('Telegram/ThirdParty/GSL', 'a' * 40),), locks=None, **changes):
        values = dict(
            preparation=PREPARATION,
            runner_label='windows-2022',
            msvc_toolset='14.44.35207',
            image_os='win22',
            image_version='20261110.1.1',
            submodule_pins=pins,
            lock_files={} if locks is None else locks,
            script_hash=SCRIPT,
        )
        values.update(changes)
        return cache.make_key(**values)

    def test_exact_inputs_are_stable_and_each_change_invalidates(self):
        original = self.key()
        self.assertEqual(original, self.key())
        self.assertTrue(original.startswith('fishgram-checks-deps-v1-'))
        self.assertLessEqual(len(original), 512)
        variants = [
            {'preparation': 'deps-windows-v3-' + ('cd' * 32)},
            {'runner_label': 'windows-2025'},
            {'msvc_toolset': '14.44.35208'},
            {'image_os': 'win25'},
            {'image_version': '20261111.1.1'},
            {'submodule_pins': (('Telegram/ThirdParty/GSL', 'b' * 40),)},
            {'locks': {'cmake/vcpkg.json': 'ef' * 32}},
            {'script_hash': '12' * 32},
        ]
        for changes in variants:
            with self.subTest(changes=changes):
                self.assertNotEqual(original, self.key(**changes))

    def test_application_commit_is_not_a_key_input(self):
        pins = (('cmake', 'c' * 40), ('Telegram/ThirdParty/GSL', 'a' * 40))
        self.assertEqual(self.key(pins), self.key(tuple(reversed(pins))))
        self.assertNotIn('source-commit', cache.make_key.__code__.co_varnames)

    def test_missing_or_malformed_inputs_are_rejected(self):
        with self.assertRaises(ValueError):
            self.key(submodule_pins=())
        with self.assertRaises(ValueError):
            self.key(msvc_toolset='14.44')
        with self.assertRaises(ValueError):
            self.key(image_version='')
        with self.assertRaises(ValueError):
            self.key(preparation='deps-windows-v3-short')
        with self.assertRaises(ValueError):
            self.key(submodule_pins=(('..\\secret', 'a' * 40),))
        with self.assertRaises(ValueError):
            self.key(submodule_pins=(('cmake', 'a' * 40), ('cmake', 'b' * 40)))

    def test_gitlinks_follow_committed_pins_not_head(self):
        temporary, repo = make_repo()
        self.addCleanup(temporary.cleanup)
        first = 'a' * 40
        git(repo, 'update-index', '--add', '--cacheinfo', f'160000,{first},Telegram/ThirdParty/GSL')
        git(repo, 'commit', '-q', '-m', 'pin GSL')
        pinned = cache.submodule_pins(repo)
        head = git_text(repo, 'rev-parse', 'HEAD')
        self.assertEqual(pinned, [('Telegram/ThirdParty/GSL', first)])
        self.assertNotEqual(head, first)
        (repo / 'README').write_text('application only\n', encoding='utf-8')
        git(repo, 'add', 'README')
        git(repo, 'commit', '-q', '-m', 'change application source')
        self.assertEqual(cache.submodule_pins(repo), pinned)
        updated = 'b' * 40
        git(repo, 'update-index', '--cacheinfo', f'160000,{updated},Telegram/ThirdParty/GSL')
        git(repo, 'commit', '-q', '-m', 'move GSL')
        self.assertEqual(cache.submodule_pins(repo), [('Telegram/ThirdParty/GSL', updated)])

    def test_windows_lock_files_ignore_the_linux_image_lock(self):
        temporary, repo = make_repo()
        self.addCleanup(temporary.cleanup)
        docker_lock = repo / 'Telegram/build/docker/centos_env/poetry.lock'
        prepare_lock = repo / 'Telegram/build/prepare/Cargo.lock'
        cmake_lock = repo / 'cmake/vcpkg.json'
        for path, text in ((docker_lock, 'linux\n'), (prepare_lock, 'windows\n'), (cmake_lock, '{}\n')):
            path.parent.mkdir(parents=True)
            path.write_text(text, encoding='utf-8')
        git(repo, 'add', 'Telegram', 'cmake')
        git(repo, 'commit', '-q', '-m', 'locks')
        found = cache.lock_files(repo)
        self.assertNotIn('Telegram/build/docker/centos_env/poetry.lock', found)
        self.assertEqual(
            found['Telegram/build/prepare/Cargo.lock'],
            hashlib.sha256(b'windows\n').hexdigest())
        self.assertIn('cmake/vcpkg.json', found)

    def test_checkout_pins_exclude_its_own_commit(self):
        if not (ROOT / '.gitmodules').is_file():
            self.skipTest('not a tdesktop checkout')
        pins = cache.submodule_pins(ROOT)
        head = git_text(ROOT, 'rev-parse', 'HEAD')
        paths = {path for path, _sha in pins}
        self.assertIn('Telegram/ThirdParty/GSL', paths)
        self.assertGreaterEqual(len(pins), 30)
        self.assertNotIn(head, {sha for _path, sha in pins})
        locks = cache.lock_files(ROOT)
        self.assertNotIn('Telegram/build/docker/centos_env/poetry.lock', locks)
        for path in locks:
            self.assertFalse(path.startswith('Telegram/build/docker/'))


def git_text(repo, *args):
    return subprocess.check_output(
        ['git', '-C', str(repo), *args], text=True).strip()


class CacheGuardTests(unittest.TestCase):
    def test_directory_report_splits_retained_and_omitted_bytes(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        kept = root / 'Libraries/win64/ffmpeg/include/libavcodec/avcodec.h'
        omitted = root / 'Libraries/win64/ffmpeg/build/codec.obj'
        tool = root / 'ThirdParty/jom/jom.exe'
        hidden = root / 'ThirdParty/gyp/.git/config'
        for path, payload in (
                (kept, b'header'), (omitted, b'object-file'),
                (tool, b'tool'), (hidden, b'git-config')):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)

        def disposable(parts, is_directory=False):
            return '.git' in parts or (not is_directory and parts[-1].endswith('.obj'))

        totals = cache.directory_report(root, disposable)
        self.assertEqual(totals['Libraries/win64/ffmpeg'], [len(b'header'), len(b'object-file')])
        self.assertEqual(totals['ThirdParty/jom'], [len(b'tool'), 0])
        self.assertNotIn('ThirdParty/gyp', totals)

    def test_guard_rejects_credentials_and_keeps_dependency_sources(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / 'Libraries').mkdir()
        (root / 'ThirdParty').mkdir()
        innocent = root / 'Libraries/openssl/test/key.pem'
        innocent.parent.mkdir(parents=True)
        innocent.write_text('-----BEGIN PRIVATE KEY-----\nnot our signing key\n', encoding='utf-8')
        git_config = root / 'ThirdParty/gyp/.git/config'
        git_config.parent.mkdir(parents=True)
        git_config.write_text('https://x-access-token:ghs_example@github.com\n', encoding='utf-8')
        self.assertEqual(cache.credential_hits(root), [])
        leaked = root / 'Libraries/win64/id_rsa'
        leaked.parent.mkdir(parents=True)
        leaked.write_bytes(b'private')
        self.assertIn('Libraries/win64/id_rsa name', cache.credential_hits(root))
        leaked.unlink()
        token = root / 'ThirdParty/nuget/nuget.config'
        token.parent.mkdir(parents=True)
        token.write_text('token=ghp_example\n', encoding='utf-8')
        self.assertIn('ThirdParty/nuget/nuget.config token', cache.credential_hits(root))

    def test_workflow_keeps_pull_requests_read_only(self):
        text = (ROOT / '.github/workflows/fishgram-checks.yml').read_text(encoding='utf-8')
        self.assertNotIn('restore-keys:', text)
        compile_job = job_text(text, 'compile')
        warmup = job_text(text, 'warmup')
        self.assertIn('actions/cache/restore@', compile_job)
        self.assertNotIn('actions/cache/save@', compile_job)
        self.assertNotIn('uses: actions/cache@', compile_job)
        self.assertIn('cache-mode: read', compile_job)
        self.assertIn('actions/cache/restore@', warmup)
        self.assertIn('actions/cache/save@', warmup)
        self.assertIn('cache-mode: write', warmup)
        self.assertIn("github.ref == 'refs/heads/custom/main'", warmup)
        self.assertIn('lookup-only: true', warmup)
        self.assertNotIn('lookup-only:', compile_job)
        self.assertNotIn('secrets.', text)


def job_text(text, name):
    marker = f'  {name}:\n'
    start = text.index(marker)
    rest = text[start + len(marker):]
    match = re.search(r'\n  [A-Za-z0-9-]+:\n', rest)
    return rest[:match.start()] if match else rest


if __name__ == '__main__':
    unittest.main()
