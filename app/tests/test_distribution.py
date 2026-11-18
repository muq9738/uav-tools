#!/usr/bin/env python3
"""分发合同的离线最小测试；不把这些解析检查当作目标平台运行验收。"""
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import tempfile
import unittest

MODULE_PATH = Path(__file__).resolve().parents[1] / 'scripts' / 'build.py'
SPEC = importlib.util.spec_from_file_location('osgb_build', MODULE_PATH)
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)


class DependencyParsingTests(unittest.TestCase):
    def test_dumpbin_imports_ignore_noise_and_keep_unicode(self):
        text = '''
Microsoft (R) COFF/PE Dumper
Dump of file C:\\模型\\osgb-tiles.exe
File Type: EXECUTABLE IMAGE
  Image has the following dependencies:
    KERNEL32.dll
    MSVCP140.dll
    osgDB.dll
    纹理依赖.dll
  Summary
        1000 .data
'''
        self.assertEqual(
            build.parse_dumpbin_dependencies(text),
            ['KERNEL32.dll', 'MSVCP140.dll', 'osgDB.dll', '纹理依赖.dll'],
        )

    def test_vc_runtime_is_payload_not_a_system_dll(self):
        self.assertFalse(build.is_system('VCRUNTIME140.dll', 'windows'))
        self.assertFalse(build.is_system('MSVCP140_1.dll', 'windows'))
        self.assertTrue(build.is_system('kernel32.dll', 'windows'))
        self.assertTrue(build.is_system('api-ms-win-core-file-l1-1-0.dll', 'windows'))

    def test_otool_rpaths_and_relative_dependency_candidates(self):
        command = '''
Load command 25
          cmd LC_RPATH
      cmdsize 48
         path @loader_path/../lib (offset 12)
Load command 26
          cmd LC_RPATH
      cmdsize 40
         path /opt/homebrew/lib (offset 12)
'''
        rpaths = build.parse_otool_rpaths(command)
        self.assertEqual(rpaths, ['@loader_path/../lib', '/opt/homebrew/lib'])
        original = Path('/临时 目录/package/bin/osgb-tiles')
        candidates = build.darwin_dependency_candidates(
            '@rpath/lib示例.dylib', original, original, rpaths)
        self.assertEqual(candidates[0], original.parent / '../lib/lib示例.dylib')
        self.assertIn(Path('/opt/homebrew/lib/lib示例.dylib'), candidates)
        executable_candidates = build.darwin_dependency_candidates(
            '@executable_path/../lib/libosg.dylib', original, original, [])
        self.assertEqual(executable_candidates[0], original.parent / '../lib/libosg.dylib')


class DependencyLocationTests(unittest.TestCase):
    def test_windows_dll_lookup_is_case_insensitive_and_unicode_safe(self):
        with tempfile.TemporaryDirectory(prefix='部署-模型-') as directory:
            root = Path(directory)
            sdk_bin = root / '开发 SDK' / 'bin'
            crt = root / 'VC' / 'x64' / 'Microsoft.VC143.CRT'
            sdk_bin.mkdir(parents=True)
            crt.mkdir(parents=True)
            expected = sdk_bin / 'OsgDB.dll'
            expected.write_bytes(b'osg')
            (crt / 'MSVCP140.dll').write_bytes(b'crt')
            self.assertEqual(
                build.find_windows_dll('osgdb.dll', [crt, sdk_bin]), expected)
            self.assertEqual(
                build.find_windows_dll('msvcp140.dll', [sdk_bin, crt]),
                crt / 'MSVCP140.dll')
            self.assertIsNone(build.find_windows_dll('missing.dll', [sdk_bin]))

    def test_vc_redist_matches_dumpbin_toolset_and_target_architecture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dumpbin = (root / 'VS' / 'VC' / 'Tools' / 'MSVC' / '14.44.35207' /
                       'bin' / 'Hostx64' / 'x64' / 'dumpbin.exe')
            dumpbin.parent.mkdir(parents=True)
            dumpbin.write_bytes(b'tool')
            redist = build.vc_redist_root(dumpbin, {})
            self.assertEqual(
                redist,
                root / 'VS' / 'VC' / 'Redist' / 'MSVC' / '14.44.35207')
            crt = redist / 'x64' / 'Microsoft.VC143.CRT'
            wrong = redist / 'arm64' / 'Microsoft.VC143.CRT'
            crt.mkdir(parents=True)
            wrong.mkdir(parents=True)
            self.assertEqual(build.find_vc_runtime_dirs(redist, 'amd64'), [crt])

    def test_sqlite_tool_comes_from_requested_sdk_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory) / '依赖 SDK'
            windows_tool = sdk / 'tools' / 'sqlite3' / 'sqlite3.exe'
            windows_tool.parent.mkdir(parents=True)
            windows_tool.write_bytes(b'sqlite')
            self.assertEqual(build.find_sqlite3(sdk, 'windows'), windows_tool)
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory)
            tool = sdk / 'opt' / 'sqlite' / 'bin' / 'sqlite3'
            tool.parent.mkdir(parents=True)
            tool.write_bytes(b'sqlite')
            tool.chmod(0o755)
            self.assertEqual(build.find_sqlite3(sdk, 'darwin'), tool)
            self.assertEqual(build.sqlite_cmake_prefix(sdk, 'darwin'), sdk / 'opt' / 'sqlite')

    def test_linux_multiarch_plugins_prefer_host_and_report_conflicts(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory)
            host = sdk / 'lib' / 'aarch64-linux-gnu' / 'osgPlugins-3.6.5'
            other = sdk / 'lib' / 'x86_64-linux-gnu' / 'osgPlugins-3.6.5'
            host.mkdir(parents=True)
            other.mkdir(parents=True)
            for stem in ['osgdb_osg', 'osgdb_serializers_osg', 'osgdb_png', 'osgdb_jpeg']:
                (host / (stem + '.so')).write_bytes(stem.encode())
                (other / (stem + '.so')).write_bytes(b'wrong architecture')
            plugins = build.discover_osg_plugins(sdk, 'linux', 'arm64')
            self.assertEqual(plugins['osgdb_osg'], host / 'osgdb_osg.so')
            self.assertEqual(plugins['osgdb_png'], host / 'osgdb_png.so')

    def test_linux_rpaths_are_relative_to_each_binary(self):
        self.assertEqual(build.linux_runtime_rpath(PurePosixPath('bin/osgb-tiles')), '$ORIGIN/../lib')
        self.assertEqual(build.linux_runtime_rpath(PurePosixPath('lib/libosg.so')), '$ORIGIN')
        self.assertEqual(
            build.linux_runtime_rpath(PurePosixPath('lib/osgPlugins-3.6.5/osgdb_osg.so')),
            '$ORIGIN/..')
        self.assertFalse(build.is_system('libstdc++.so.6', 'linux'))
        self.assertTrue(build.is_system('libc.so.6', 'linux'))


class LicenseAndManifestTests(unittest.TestCase):
    def test_license_collection_covers_sdk_proj_and_bundled_sources(self):
        with tempfile.TemporaryDirectory(prefix='许可-') as directory:
            root = Path(directory)
            sdk = root / 'sdk'
            copyright = sdk / 'share' / 'osg' / 'copyright'
            copyright.parent.mkdir(parents=True)
            copyright.write_text('OSG license', encoding='utf-8')
            proj = root / 'build' / '_deps' / 'proj-src' / 'COPYING'
            proj.parent.mkdir(parents=True)
            proj.write_text('PROJ license', encoding='utf-8')
            stage = root / 'stage'
            copied = build.collect_licenses(sdk, root / 'build', stage, [])
            relative = {str(path.relative_to(stage)).replace(os.sep, '/') for path in copied}
            self.assertIn('licenses/vcpkg/osg/copyright', relative)
            self.assertIn('licenses/PROJ/COPYING', relative)
            self.assertIn('licenses/bundled/nlohmann/LICENSE.MIT', relative)
            self.assertIn('licenses/bundled/meshoptimizer/LICENSE.md', relative)
            self.assertIn('licenses/product/LICENSE', relative)

    def test_license_collection_uses_declared_proj_source(self):
        with tempfile.TemporaryDirectory(prefix='PROJ缓存-') as directory:
            root = Path(directory)
            sdk, build_dir, external = root / 'sdk', root / 'build', root / '共享源码'
            sdk.mkdir()
            external.mkdir()
            fallback = build_dir / '_deps' / 'proj-src'
            fallback.mkdir(parents=True)
            (fallback / 'COPYING').write_text('wrong source')
            (external / 'COPYING').write_text('actual PROJ source')
            (build_dir / 'CMakeCache.txt').write_text('FETCHCONTENT_SOURCE_DIR_PROJ:PATH=' + str(external) + '\n', encoding='utf-8')
            build.collect_licenses(sdk, build_dir, root / 'stage', [])
            self.assertEqual((root / 'stage/licenses/PROJ/COPYING').read_text(), 'actual PROJ source')
            (external / 'COPYING').unlink()
            with self.assertRaisesRegex(RuntimeError, 'PROJ'):
                build.collect_licenses(sdk, build_dir, root / 'invalid-stage', [])

    def make_package(self, root):
        bindir = root / 'bin'
        plugin = bindir / 'osgPlugins-3.6.5' / 'osgdb_osg.dll'
        plugin.parent.mkdir(parents=True)
        (bindir / 'osgb-tiles.exe').write_bytes(b'native')
        plugin.write_bytes(b'plugin')
        license = root / 'licenses' / 'example' / 'COPYING'
        license.parent.mkdir(parents=True)
        license.write_text('license', encoding='utf-8')
        build.write_runtime_manifest(
            root,
            platform='windows-amd64',
            plugins=['osgPlugins-3.6.5/osgdb_osg.dll'],
            plugin_directories=['osgPlugins-3.6.5'],
            capabilities={'protocolVersion': 1},
        )
        return license

    def test_manifest_hashes_include_licenses_and_detect_tampering(self):
        with tempfile.TemporaryDirectory(prefix='包-') as directory:
            root = Path(directory)
            license = self.make_package(root)
            manifest = build.verify_resources(root, expected_platform='windows-amd64')
            self.assertIn('licenses/example/COPYING', manifest['files'])
            self.assertEqual(manifest['licenses'], ['licenses/example/COPYING'])
            license.write_text('changed', encoding='utf-8')
            with self.assertRaisesRegex(RuntimeError, 'SHA-256'):
                build.verify_resources(root, expected_platform='windows-amd64')

    def test_unix_plugin_paths_may_leave_bin_but_not_resource_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'bin').mkdir(parents=True)
            (root / 'bin' / 'osgb-tiles').write_bytes(b'native')
            plugin = root / 'lib' / 'osgPlugins-3.6.5' / 'osgdb_osg.so'
            plugin.parent.mkdir(parents=True)
            plugin.write_bytes(b'plugin')
            license = root / 'licenses' / 'osg' / 'LICENSE'
            license.parent.mkdir(parents=True)
            license.write_text('license', encoding='utf-8')
            build.write_runtime_manifest(
                root, platform='linux-arm64',
                plugins=['../lib/osgPlugins-3.6.5/osgdb_osg.so'],
                plugin_directories=['../lib/osgPlugins-3.6.5'],
                capabilities={'protocolVersion': 1})
            build.verify_resources(root, expected_platform='linux-arm64')
            self.assertEqual(
                build.safe_plugin_path('../lib/osgPlugins-3.6.5/osgdb_osg.so'),
                PurePosixPath('lib/osgPlugins-3.6.5/osgdb_osg.so'))
            with self.assertRaisesRegex(RuntimeError, '逃逸'):
                build.safe_plugin_path('../../outside/osgdb_osg.so')

    def test_signed_manifest_refresh_rejects_changed_license(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / 'Example.app'
            resources = app / 'Contents' / 'Resources'
            license = self.make_package(resources)
            license.write_text('tampered after signing', encoding='utf-8')
            with self.assertRaisesRegex(RuntimeError, '非 Mach-O'):
                build.refresh_signed_manifest(app)

    def test_manifest_rejects_unlisted_files_and_path_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_package(root)
            (root / 'lib').mkdir()
            (root / 'lib' / 'unlisted.dll').write_bytes(b'unlisted')
            with self.assertRaisesRegex(RuntimeError, '未登记'):
                build.verify_resources(root)
            (root / 'lib' / 'unlisted.dll').unlink()
            manifest_path = root / 'bin' / 'native-runtime.json'
            manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
            manifest['files']['../outside'] = '0' * 64
            manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
            with self.assertRaisesRegex(RuntimeError, '路径'):
                build.verify_resources(root)


if __name__ == '__main__':
    unittest.main()
