#!/usr/bin/env python3
"""构建 native、收齐运行时并打包 App。仅在目标平台本机执行，不混用另一平台 SDK。"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import posixpath
import re
import shutil
import subprocess
import sys

APP = Path(__file__).resolve().parents[1]
ROOT = APP.parent
SYSTEM = {'Darwin': 'darwin', 'Linux': 'linux', 'Windows': 'windows'}[platform.system()]
ARCH = {'aarch64': 'arm64', 'arm64': 'arm64', 'x86_64': 'amd64', 'AMD64': 'amd64'}[platform.machine()]
PLATFORM = f'{SYSTEM}-{ARCH}'
EXE = '.exe' if SYSTEM == 'windows' else ''
WINDOWS_DUMPBIN = None


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def output(args, **kwargs):
    kwargs.setdefault('text', True)
    kwargs.setdefault('errors', 'replace')
    return subprocess.check_output([str(a) for a in args], **kwargs)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def parse_otool_dependencies(text):
    return [line.strip().split(' (')[0] for line in text.splitlines()[1:] if line.strip()]


def parse_otool_rpaths(text):
    return re.findall(r'cmd LC_RPATH\s+cmdsize\s+\d+\s+path (.*?) \(offset', text)


def parse_dumpbin_dependencies(text):
    # dumpbin 的 Summary 也可能含十六进制/文件名；只接受单独占一行的 DLL 导入名。
    return [match.group(1).strip() for match in re.finditer(r'^\s+([^:\r\n]+?\.dll)\s*$', text, re.M | re.I)]


def dependencies(path, system=SYSTEM, env=None):
    if system == 'darwin':
        return parse_otool_dependencies(output(['otool', '-L', path], env=env))
    if system == 'linux':
        found = []
        for line in output(['ldd', path], env=env).splitlines():
            if 'not found' in line:
                raise RuntimeError(f'缺少依赖 {path}: {line.strip()}')
            match = re.search(r'=>\s+(/\S+)', line) or re.match(r'\s*(/\S+)', line)
            if match:
                found.append(match.group(1))
        return found
    if WINDOWS_DUMPBIN is None:
        raise RuntimeError('尚未定位 dumpbin；请先执行 Windows SDK 预检')
    return parse_dumpbin_dependencies(output([WINDOWS_DUMPBIN, '/DEPENDENTS', path], env=env))


def is_system(name, system=SYSTEM):
    if system == 'darwin':
        return name.startswith(('/usr/lib/', '/System/Library/'))
    if system == 'linux':
        # glibc/loader 来自目标发行版；OpenGL/X11/libstdc++ 等传递依赖随包收集。
        return bool(re.match(r'^(ld-linux.*|lib(c|m|pthread|dl|rt|resolv|util)\.so.*)$', Path(name).name))
    lowered = name.lower()
    return lowered in {
        'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll', 'shell32.dll', 'ole32.dll',
        'oleaut32.dll', 'comdlg32.dll', 'comctl32.dll', 'ws2_32.dll', 'wsock32.dll', 'rpcrt4.dll',
        'ntdll.dll', 'secur32.dll', 'sspicli.dll', 'bcrypt.dll', 'crypt32.dll', 'cryptbase.dll',
        'winmm.dll', 'version.dll', 'opengl32.dll', 'shlwapi.dll', 'ucrtbase.dll', 'normaliz.dll',
        'setupapi.dll', 'imm32.dll', 'dwmapi.dll', 'psapi.dll', 'dbghelp.dll', 'shcore.dll',
        'powrprof.dll', 'msimg32.dll', 'userenv.dll', 'iphlpapi.dll', 'dnsapi.dll', 'nsi.dll',
        'mswsock.dll', 'wtsapi32.dll', 'wintrust.dll', 'imagehlp.dll', 'cabinet.dll',
    } or lowered.startswith(('api-ms-win-', 'ext-ms-win-'))


def find_sqlite3(sdk, system=SYSTEM):
    executable = 'sqlite3.exe' if system == 'windows' else 'sqlite3'
    candidates = [
        sdk / 'tools' / 'sqlite3' / executable,
        sdk / 'bin' / executable,
        sdk / 'opt' / 'sqlite' / 'bin' / executable,
    ]
    for candidate in candidates:
        if candidate.is_file() and (system == 'windows' or os.access(candidate, os.X_OK)):
            return candidate
    raise RuntimeError(f'SDK 缺少 sqlite3 工具；已检查: {", ".join(str(p) for p in candidates)}')


def sqlite_cmake_prefix(sdk, system=SYSTEM):
    candidate = sdk / 'opt' / 'sqlite'
    return candidate if (candidate / 'bin' / ('sqlite3.exe' if system == 'windows' else 'sqlite3')).is_file() else sdk


def find_windows_dll(name, search_paths):
    # 显式按大小写折叠扫描，使解析测试在非 Windows 宿主上也验证同一合同。
    for folder in search_paths:
        if not folder or not folder.is_dir():
            continue
        try:
            entries = list(folder.iterdir())
        except OSError:
            continue
        matches = [p for p in entries if p.is_file() and p.name.casefold() == Path(name).name.casefold()]
        if len(matches) > 1:
            raise RuntimeError(f'DLL 名称存在大小写冲突: {folder} -> {name}')
        if matches:
            return matches[0]
    return None


def discover_windows_dumpbin(environment=None):
    environment = os.environ if environment is None else environment
    existing = shutil.which('dumpbin', path=environment.get('PATH'))
    if existing:
        return Path(existing)
    host = 'arm64' if ARCH == 'arm64' else 'x64'
    target = 'arm64' if ARCH == 'arm64' else 'x64'
    pattern = f'VC\\Tools\\MSVC\\**\\bin\\Host{host}\\{target}\\dumpbin.exe'
    candidates = []
    if environment.get('ProgramFiles(x86)'):
        candidates.append(Path(environment['ProgramFiles(x86)']) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe')
    if environment.get('ProgramFiles'):
        candidates.append(Path(environment['ProgramFiles']) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe')
    vswhere = next((p for p in candidates if p.is_file()), None)
    if vswhere is None:
        found = shutil.which('vswhere', path=environment.get('PATH'))
        vswhere = Path(found) if found else None
    if vswhere is not None:
        found = output([vswhere, '-latest', '-products', '*', '-find', pattern]).splitlines()
        for line in found:
            candidate = Path(line.strip().lstrip('\ufeff'))
            if candidate.is_file():
                return candidate
    raise RuntimeError('找不到 dumpbin；请在 x64 Native Tools / VS Developer PowerShell 中运行，或安装 MSVC 与 vswhere')


def vc_redist_root(dumpbin, environment=None):
    environment = os.environ if environment is None else environment
    if environment.get('VCToolsRedistDir'):
        return Path(environment['VCToolsRedistDir'])
    for toolset in dumpbin.parents:
        if (toolset.parent.name.casefold() == 'msvc' and toolset.parent.parent.name.casefold() == 'tools'
                and toolset.parent.parent.parent.name.casefold() == 'vc'):
            return toolset.parent.parent.parent / 'Redist' / 'MSVC' / toolset.name
    raise RuntimeError(f'无法从 dumpbin 推导 VC Redist 目录: {dumpbin}')


def find_vc_runtime_dirs(redist_root, architecture):
    target = {'amd64': 'x64', 'arm64': 'arm64'}[architecture]
    candidates = [p for p in redist_root.rglob('Microsoft.VC*.CRT')
                  if p.is_dir() and target in {part.casefold() for part in p.parts}]
    if not candidates:
        return []
    # VCToolsRedistDir 通常已指向一个工具集；若传入 MSVC 总目录，只选最新版本，避免混装 CRT。
    by_toolset = {}
    for candidate in candidates:
        by_toolset.setdefault(candidate.parent.parent, []).append(candidate)

    def version_key(path):
        values = re.findall(r'\d+', path.name)
        return tuple(int(v) for v in values) if values else (0,)

    latest = max(by_toolset, key=version_key)
    return sorted(by_toolset[latest])


def prepare_sdk(sdk):
    global WINDOWS_DUMPBIN
    sqlite = find_sqlite3(sdk)
    search = [sqlite.parent, sdk / 'bin']
    vc_dirs = []
    if SYSTEM == 'windows':
        WINDOWS_DUMPBIN = discover_windows_dumpbin().resolve()
        redist = vc_redist_root(WINDOWS_DUMPBIN)
        vc_dirs = find_vc_runtime_dirs(redist, ARCH)
        if not vc_dirs or not any(p.is_file() for folder in vc_dirs for p in folder.glob('*.dll')):
            raise RuntimeError(f'找不到 {ARCH} Release MSVC CRT: {redist}')
        search += [WINDOWS_DUMPBIN.parent, *vc_dirs]
    os.environ['PATH'] = os.pathsep.join([*(str(p) for p in search), os.environ.get('PATH', '')])
    return sqlite, vc_dirs


def clean_runtime_environment():
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('OSG_', 'PROJ_', 'DYLD_', 'OSGB_TILES_BIN'))
           and key not in {'LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH', 'PYTHONPATH'}}
    if SYSTEM == 'windows':
        env['PATH'] = str(Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32')
    else:
        env['PATH'] = '/usr/bin:/bin'
    return env


def darwin_dependency_candidates(name, original, executable, rpaths):
    def expand(value):
        return value.replace('@loader_path', str(original.parent)).replace('@executable_path', str(executable.parent))

    candidates = []
    if name.startswith('@rpath/'):
        candidates += [Path(expand(rpath)) / name.removeprefix('@rpath/') for rpath in rpaths]
    elif name.startswith(('@loader_path/', '@executable_path/')):
        candidates.append(Path(expand(name)))
    elif Path(name).is_absolute():
        candidates.append(Path(name))
    return candidates


def linux_multiarch(architecture):
    return {'amd64': 'x86_64-linux-gnu', 'arm64': 'aarch64-linux-gnu'}.get(architecture)


def is_osg_plugin(path):
    return path.name.startswith('osgdb_') and (path.suffix in {'.dylib', '.dll'} or '.so' in path.name)


def discover_osg_plugins(sdk, system=SYSTEM, architecture=ARCH):
    folders = [sdk / 'lib', sdk / 'lib64', sdk / 'plugins', sdk / 'bin']
    if system == 'linux' and linux_multiarch(architecture):
        multiarch = sdk / 'lib' / linux_multiarch(architecture)
        folders = [multiarch, sdk / 'lib', sdk / 'lib64', sdk / 'plugins']
    candidates = []
    for folder in folders:
        if not folder.exists():
            continue
        candidates += list(folder.glob('osgPlugins-*/osgdb_*'))
        candidates += list(folder.glob('*/osgPlugins-*/osgdb_*'))
    by_stem = {}
    for path in sorted(candidates, key=lambda p: str(p)):
        if not path.is_file() or not is_osg_plugin(path):
            continue
        stem = path.name.split('.so', 1)[0].rsplit('.', 1)[0]
        previous = by_stem.get(stem)
        if previous is not None and previous.resolve() != path.resolve():
            # 宿主多架构目录优先；其余冲突不能由 glob 顺序静默决定。
            host_folder = linux_multiarch(architecture) if system == 'linux' else None
            if host_folder and host_folder in previous.parts:
                continue
            if host_folder and host_folder in path.parts:
                by_stem[stem] = path
                continue
            raise RuntimeError(f'OSG 插件同名冲突: {previous} / {path}')
        by_stem[stem] = path
    return by_stem


def linux_runtime_rpath(relative):
    relative = PurePosixPath(relative)
    if relative.parts[0] == 'bin':
        return '$ORIGIN/../lib'
    if len(relative.parts) >= 3 and relative.parts[1].startswith('osgPlugins-'):
        return '$ORIGIN/..'
    return '$ORIGIN'


def license_name(path):
    upper = path.name.upper()
    return upper.startswith(('LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT'))


def collect_licenses(sdk, build_dir, destination, copied_sources):
    entries = {}

    def add(source, relative):
        if not source.is_file():
            return
        target = destination / relative
        previous = entries.get(relative)
        if previous is not None and previous.resolve() != source.resolve() and sha(previous) != sha(source):
            raise RuntimeError(f'许可文件同名不同内容: {source} / {previous}')
        entries[relative] = source

    add(ROOT / 'LICENSE', 'licenses/product/LICENSE')
    add(ROOT / 'native/third_party/nlohmann/LICENSE.MIT', 'licenses/bundled/nlohmann/LICENSE.MIT')
    add(ROOT / 'native/third_party/meshoptimizer/LICENSE.md', 'licenses/bundled/meshoptimizer/LICENSE.md')
    add(APP / 'web/vendor/cesium/LICENSE.md', 'licenses/bundled/cesium/LICENSE.md')
    add(APP / 'web/vendor/cesium/SOURCE.json', 'licenses/bundled/cesium/SOURCE.json')
    for name in ['draco', 'basis-universal', 'basis-zstd', 'basis-pvpngreader']:
        add(ROOT / 'native/third_party/codecs' / f'{name}.LICENSE', f'licenses/bundled/{name}/LICENSE')
    add(ROOT / 'native/third_party/codecs/SOURCE.json', 'licenses/bundled/codecs/SOURCE.json')
    add(ROOT / 'native/third_party/codecs/draco.AUTHORS', 'licenses/bundled/draco/AUTHORS')
    proj_source = build_dir / '_deps' / 'proj-src'
    cache = build_dir / 'CMakeCache.txt'
    if cache.is_file():
        override = re.search(r'^FETCHCONTENT_SOURCE_DIR_PROJ:[^=]+=(.+)$', cache.read_text(encoding='utf-8'), re.M)
        if override:
            proj_source = Path(override.group(1).strip())
    proj_license = proj_source / 'COPYING'
    if not proj_license.is_file():
        raise RuntimeError(f'缺少 PROJ 许可: {proj_license}')
    add(proj_license, 'licenses/PROJ/COPYING')

    for source in sorted(sdk.glob('share/*/copyright')):
        add(source, str(PurePosixPath('licenses/vcpkg') / source.parent.name / source.name))

    packages = set()
    cellars = set()
    for item in copied_sources:
        source = Path(item).resolve()
        parts = source.parts
        if 'Cellar' in parts:
            index = parts.index('Cellar')
            if len(parts) > index + 2:
                cellars.add(Path(*parts[:index + 3]))
        if SYSTEM == 'linux' and str(source).startswith(('/usr/', '/lib/')) and shutil.which('dpkg-query'):
            result = subprocess.run(['dpkg-query', '-S', str(source)], text=True, errors='replace',
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            if result.returncode == 0:
                for line in result.stdout.splitlines():
                    if ': ' in line:
                        packages.add(line.split(': ', 1)[0].split(',', 1)[0].split(':', 1)[0])
            elif str(source).startswith('/usr/lib/'):
                raise RuntimeError(f'无法确定依赖许可所属软件包: {source}: {result.stderr.strip()}')

    for cellar in sorted(cellars):
        for source in sorted(cellar.iterdir()):
            if source.is_file() and (license_name(source) or source.name == 'sbom.spdx.json'):
                add(source, str(PurePosixPath('licenses/homebrew') / cellar.parent.name / cellar.name / source.name))
    for package in sorted(packages):
        copyright = Path('/usr/share/doc') / package / 'copyright'
        if not copyright.is_file():
            raise RuntimeError(f'依赖 {package} 缺少随包 copyright: {copyright}')
        add(copyright, str(PurePosixPath('licenses/debian') / package / 'copyright'))

    copied = []
    for relative, source in sorted(entries.items()):
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        copied.append(target)
    return copied


def safe_manifest_path(value):
    if not isinstance(value, str) or not value or '\\' in value:
        raise RuntimeError(f'资源清单路径无效: {value!r}')
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {'', '.', '..'} for part in path.parts) or str(path) != value:
        raise RuntimeError(f'资源清单路径逃逸或不规范: {value!r}')
    return path


def safe_plugin_path(value):
    if not isinstance(value, str) or not value or '\\' in value or PurePosixPath(value).is_absolute():
        raise RuntimeError(f'插件清单路径无效: {value!r}')
    normalized = PurePosixPath(posixpath.normpath(posixpath.join('bin', value)))
    if normalized == PurePosixPath('..') or str(normalized).startswith('../') or normalized == PurePosixPath('.'):
        raise RuntimeError(f'插件清单路径逃逸资源根: {value!r}')
    return normalized


def managed_files(root, manifest_path=None):
    files = {}
    for folder in ['bin', 'lib', 'licenses']:
        base = root / folder
        if not base.exists():
            continue
        for path in base.rglob('*'):
            if path.is_symlink():
                raise RuntimeError(f'受管资源不得使用符号链接: {path}')
            if path.is_file() and path != manifest_path:
                relative = str(path.relative_to(root)).replace(os.sep, '/')
                files[relative] = path
    return files


def write_runtime_manifest(stage_root, platform, plugins, plugin_directories, capabilities):
    for relative in [*plugins, *plugin_directories]:
        safe_plugin_path(relative)
    manifest_path = stage_root / 'bin' / 'native-runtime.json'
    files = managed_files(stage_root, manifest_path)
    manifest = {
        'protocolVersion': 1,
        'platform': platform,
        'plugins': plugins,
        'pluginDirectories': plugin_directories,
        'capabilities': capabilities,
        'licenses': sorted(relative for relative in files if relative.startswith('licenses/')),
        'files': {relative: sha(path) for relative, path in sorted(files.items())},
    }
    if not manifest['licenses']:
        raise RuntimeError('分发包没有第三方许可文件')
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return manifest_path


def verify_resources(package, expected_platform=None):
    package = package.resolve()
    manifest_path = package / 'bin' / 'native-runtime.json'
    if not manifest_path.is_file() and (package / 'Contents/Resources/bin/native-runtime.json').is_file():
        return verify_resources(package / 'Contents/Resources', expected_platform)
    if not manifest_path.is_file():
        raise RuntimeError(f'缺少资源清单: {manifest_path}')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if manifest.get('protocolVersion') != 1:
        raise RuntimeError(f'不支持的资源清单协议: {manifest_path}')
    if expected_platform is not None and manifest.get('platform') != expected_platform:
        raise RuntimeError(f'资源平台不匹配: {manifest.get("platform")} != {expected_platform}')
    if not isinstance(manifest.get('files'), dict) or not manifest['files']:
        raise RuntimeError('资源清单没有文件哈希')
    root = package
    expected = {}
    for relative, digest in manifest['files'].items():
        path = safe_manifest_path(relative)
        if path.parts[0] not in {'bin', 'lib', 'licenses'}:
            raise RuntimeError(f'资源清单路径不在受管目录: {relative}')
        if not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest):
            raise RuntimeError(f'资源 SHA-256 格式无效: {relative}')
        expected[relative] = digest
    actual = managed_files(root, manifest_path)
    missing = sorted(set(expected) - set(actual))
    unlisted = sorted(set(actual) - set(expected))
    if missing:
        raise RuntimeError(f'资源清单所列文件缺失: {missing}')
    if unlisted:
        raise RuntimeError(f'存在未登记的受管资源: {unlisted}')
    for relative, digest in expected.items():
        target = actual[relative]
        if not target.resolve().is_relative_to(root):
            raise RuntimeError(f'资源路径逃逸: {relative}')
        if sha(target) != digest:
            raise RuntimeError(f'资源 SHA-256 不匹配: {relative}')

    platform_name = manifest.get('platform', '')
    native_name = 'osgb-tiles.exe' if platform_name.startswith('windows-') else 'osgb-tiles'
    if f'bin/{native_name}' not in expected:
        raise RuntimeError(f'资源清单缺少随包 native: bin/{native_name}')
    plugin_directories = manifest.get('pluginDirectories')
    plugins = manifest.get('plugins')
    if not isinstance(plugin_directories, list) or not plugin_directories or not isinstance(plugins, list) or not plugins:
        raise RuntimeError('资源清单没有 OSG 插件')
    directories = [safe_plugin_path(value) for value in plugin_directories]
    for directory in directories:
        if not (root / Path(*directory.parts)).is_dir():
            raise RuntimeError(f'OSG 插件目录缺失: {directory}')
    for value in plugins:
        plugin = safe_plugin_path(value)
        if str(plugin) not in expected:
            raise RuntimeError(f'OSG 插件没有文件哈希: {value}')
        if not any(plugin.is_relative_to(directory) for directory in directories):
            raise RuntimeError(f'OSG 插件不在声明目录中: {value}')
    licenses = manifest.get('licenses')
    discovered_licenses = sorted(relative for relative in expected if relative.startswith('licenses/'))
    if not isinstance(licenses, list) or not licenses or sorted(licenses) != discovered_licenses:
        raise RuntimeError('第三方许可清单不完整')
    return manifest


def distribution_packages(path):
    path = path.resolve()
    direct = [path / 'bin/native-runtime.json', path / 'Contents/Resources/bin/native-runtime.json']
    if any(candidate.is_file() for candidate in direct):
        packages = [path]
    else:
        packages = []
        for manifest in sorted(path.rglob('native-runtime.json')):
            if manifest.parent.name != 'bin' or '.building' in manifest.parts:
                continue
            resources = manifest.parent.parent
            package = resources.parent.parent if resources.name == 'Resources' and resources.parent.name == 'Contents' else resources
            packages.append(package)
    if not packages:
        raise RuntimeError(f'没有找到分发资源清单: {path}')
    return packages


def verify_distribution(path, check_signatures=True):
    results = []
    for package in distribution_packages(path):
        manifest = verify_resources(package)
        if check_signatures and SYSTEM == 'darwin' and package.suffix == '.app':
            run(['codesign', '--verify', '--deep', '--strict', package])
        results.append((package, manifest))
    return results


def is_macho(path):
    with path.open('rb') as stream:
        magic = stream.read(4)
    return magic in {bytes.fromhex(value) for value in
                     ['feedface', 'cefaedfe', 'feedfacf', 'cffaedfe', 'cafebabe', 'bebafeca']}


def refresh_signed_manifest(package):
    resources = package / 'Contents' / 'Resources'
    manifest_path = resources / 'bin' / 'native-runtime.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    actual = managed_files(resources, manifest_path)
    expected = manifest.get('files', {})
    if set(actual) != set(expected):
        raise RuntimeError('签名前后受管文件集合不一致，拒绝刷新哈希')
    changed = [relative for relative, path in actual.items() if sha(path) != expected[relative]]
    for relative in changed:
        if not is_macho(actual[relative]):
            raise RuntimeError(f'签名改变了非 Mach-O 资源: {relative}')
        run(['codesign', '--verify', '--strict', actual[relative]], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    manifest['files'] = {relative: sha(path) for relative, path in sorted(actual.items())}
    manifest['licenses'] = sorted(relative for relative in actual if relative.startswith('licenses/'))
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return changed


def finalize_macos_package(package):
    changed = refresh_signed_manifest(package)
    config = json.loads((APP / 'mygo.json').read_text(encoding='utf-8')).get('macos', {})
    identity = config.get('signingIdentity', '-')
    command = ['codesign', '--force', '--sign', identity]
    if identity != '-':
        command += ['--options', 'runtime', '--timestamp']
    if config.get('entitlements'):
        command += ['--entitlements', APP / config['entitlements']]
    # 嵌套 Mach-O 已由 MyGo 深度签名；这里只封存刷新后的清单，避免再次改变其哈希。
    run(command + [package])
    verify_resources(package)
    run(['codesign', '--verify', '--deep', '--strict', package])
    if changed:
        print(f'已刷新签名后的 Mach-O 哈希: {package}（{len(changed)} 个）', flush=True)


def finalize_distribution(path):
    for package in distribution_packages(path):
        if SYSTEM == 'darwin' and package.suffix == '.app':
            finalize_macos_package(package)
    return verify_distribution(path)


def stage(binary, sdk, build_dir, vc_dirs=()):
    destination = APP / 'resources' / PLATFORM
    temporary = destination.with_name(PLATFORM + '.building')
    if temporary.exists():
        shutil.rmtree(temporary)
    bindir = temporary / 'bin'
    libdir = bindir if SYSTEM == 'windows' else temporary / 'lib'
    bindir.mkdir(parents=True)
    if libdir != bindir:
        libdir.mkdir()
    copied = {}
    pending = []
    sqlite_prefix = sqlite_cmake_prefix(sdk)
    search = [*vc_dirs, sdk / 'lib', sdk / 'lib64', sdk / 'bin', sqlite_prefix / 'lib', binary.parent]

    def copy(source, target):
        source = source.resolve()
        if not source.is_file():
            raise RuntimeError(f'运行时源文件不存在: {source}')
        if target in copied:
            if copied[target] != source and sha(target) != sha(source):
                raise RuntimeError(f'同名不同库: {source} / {copied[target]}')
            return
        if target.exists() and sha(target) != sha(source):
            raise RuntimeError(f'目标已有不同文件: {target}')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        target.chmod(target.stat().st_mode | 0o200)
        copied[target] = source
        pending.append(target)

    native = bindir / ('osgb-tiles' + EXE)
    copy(binary, native)
    if SYSTEM == 'windows':
        for folder in vc_dirs:
            for source in sorted(folder.glob('*.dll')):
                copy(source, bindir / source.name)

    by_stem = discover_osg_plugins(sdk)
    required = ['osgdb_osg', 'osgdb_serializers_osg']
    if 'osgdb_png' in by_stem and 'osgdb_jpeg' in by_stem:
        required += ['osgdb_png', 'osgdb_jpeg']
    elif SYSTEM == 'darwin' and 'osgdb_imageio' in by_stem:
        required += ['osgdb_imageio']
    else:
        raise RuntimeError('SDK 缺少 JPEG/PNG OSG 插件')
    selected = []
    for stem in required:
        source = by_stem.get(stem)
        if source is None:
            raise RuntimeError(f'SDK 缺少 {stem}')
        selected.append(source)
    directory_names = {source.parent.name for source in selected}
    if len(directory_names) != 1:
        raise RuntimeError(f'必需 OSG 插件来自不同目录: {sorted(directory_names)}')
    plugindir = libdir / directory_names.pop()
    plugins = []
    for source in selected:
        target = plugindir / source.name
        copy(source, target)
        plugins.append(os.path.relpath(target, bindir).replace(os.sep, '/'))

    links = {}
    for current in pending:
        original = copied[current]
        rpaths = parse_otool_rpaths(output(['otool', '-l', original])) if SYSTEM == 'darwin' else []
        for name in dependencies(original):
            if is_system(name):
                continue
            candidates = []
            if SYSTEM == 'darwin':
                candidates = darwin_dependency_candidates(name, original, binary, rpaths)
                if name.startswith('@@HOMEBREW_PREFIX@@/'):
                    candidates.append(sdk / name.removeprefix('@@HOMEBREW_PREFIX@@/'))
                if name.startswith('@@HOMEBREW_CELLAR@@/'):
                    candidates.append(sdk / 'Cellar' / name.removeprefix('@@HOMEBREW_CELLAR@@/'))
            elif Path(name).is_absolute():
                candidates = [Path(name)]
            basename = Path(name).name
            candidates += [original.parent / basename, *(folder / basename for folder in search)]
            source = next((p for p in candidates if p.is_file()), None)
            if SYSTEM == 'windows':
                source = find_windows_dll(name, [original.parent, *search])
            if source is None or not source.is_file():
                raise RuntimeError(f'无法解析 {original} -> {name}')
            target = libdir / basename
            copy(source, target)
            links.setdefault(current, []).append((name, target))

    for current in pending:
        if SYSTEM == 'darwin':
            for name, target in links.get(current, []):
                relative = os.path.relpath(target, current.parent)
                run(['install_name_tool', '-change', name, '@loader_path/' + relative, current], stdout=subprocess.DEVNULL)
            if current != native:
                run(['install_name_tool', '-id', '@loader_path/' + current.name, current], stdout=subprocess.DEVNULL)
            rpaths = parse_otool_rpaths(output(['otool', '-l', current]))
            for rpath in sorted(set(rpaths)):
                run(['install_name_tool', '-delete_rpath', rpath, current], stdout=subprocess.DEVNULL)
            run(['codesign', '--force', '--sign', '-', current], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            run(['codesign', '--verify', '--strict', current], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        elif SYSTEM == 'linux':
            relative = PurePosixPath(current.relative_to(temporary).as_posix())
            run(['patchelf', '--set-rpath', linux_runtime_rpath(relative), current])

    collect_licenses(sdk, build_dir, temporary, copied.values())
    runtime_env = clean_runtime_environment()
    capabilities = json.loads(output([native, '--capabilities'], env=runtime_env))
    plugin_directories = [os.path.relpath(plugindir, bindir).replace(os.sep, '/')]
    write_runtime_manifest(temporary, PLATFORM, plugins, plugin_directories, capabilities)
    # 清单存在后再次执行，强制 configureRuntime 从包内加载全部声明插件。
    packaged_capabilities = json.loads(output([native, '--capabilities'], env=runtime_env))
    if packaged_capabilities != capabilities:
        raise RuntimeError('包内插件加载前后 native 能力不一致')

    # 最终动态依赖审核；清除 SDK 环境后，非系统依赖只能来自包内。
    for current in pending:
        for name in dependencies(current, env=runtime_env):
            if is_system(name):
                continue
            if SYSTEM == 'darwin':
                if not name.startswith('@loader_path/'):
                    raise RuntimeError(f'未封闭的运行时依赖: {current} -> {name}')
                resolved = (current.parent / name.removeprefix('@loader_path/')).resolve()
                if not resolved.is_file() or not resolved.is_relative_to(temporary.resolve()):
                    raise RuntimeError(f'未封闭的运行时依赖: {current} -> {name}')
            elif SYSTEM == 'linux' and not Path(name).resolve().is_relative_to(temporary.resolve()):
                raise RuntimeError(f'未封闭的运行时依赖: {current} -> {name}')
            elif SYSTEM == 'windows' and find_windows_dll(name, [bindir]) is None:
                raise RuntimeError(f'未封闭的运行时依赖: {current} -> {name}')
    verify_resources(temporary, expected_platform=PLATFORM)
    if destination.exists():
        shutil.rmtree(destination)
    temporary.rename(destination)
    verify_resources(destination, expected_platform=PLATFORM)
    print(f'随包 native: {destination / "bin" / native.name}；{len(pending)} 个二进制/插件/库', flush=True)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-prefix', type=Path, help='该平台 SDK 安装前缀；仅构建阶段使用')
    parser.add_argument('--native-build', type=Path, default=APP / 'build' / PLATFORM)
    parser.add_argument('--proj-source', type=Path, help='可选的 PROJ 9.6.2 已核验源码缓存')
    parser.add_argument('--toolchain', type=Path, help='构建 SDK 的 CMake toolchain，例如 app/sdk/toolchain/scripts/buildsystems/vcpkg.cmake')
    parser.add_argument('--stage-only', action='store_true', help='只重新收集已构建 native 的运行时')
    parser.add_argument('--skip-desktop', action='store_true', help='生成便携 HTTP/浏览器版目录，不生成桌面安装包')
    parser.add_argument('--verify-package', type=Path, help='只核验便携包、.app 或包含这些包的 dist 目录')
    parser.add_argument('--finalize-package', type=Path, help='MyGo 打包后刷新 macOS 签名哈希、封存并核验')
    args = parser.parse_args()
    if args.finalize_package:
        results = finalize_distribution(args.finalize_package)
        for package, manifest in results:
            print(f'分发完成并核验通过: {package}（{len(manifest["files"])} 个文件）', flush=True)
        return
    if args.verify_package:
        results = verify_distribution(args.verify_package)
        for package, manifest in results:
            signature = '与整体签名' if SYSTEM == 'darwin' and package.suffix == '.app' else ''
            print(f'资源哈希{signature}核验通过: {package}（{len(manifest["files"])} 个文件）', flush=True)
        return
    if not args.sdk_prefix:
        parser.error('构建需要 --sdk-prefix；只核验产物时使用 --verify-package')
    sdk = args.sdk_prefix.resolve()
    build = args.native_build.resolve()
    binary = build / ('osgb-tiles' + EXE)
    sqlite, vc_dirs = prepare_sdk(sdk)
    if not args.stage_only:
        sqlite_prefix = sqlite_cmake_prefix(sdk)
        prefixes = [sdk] if sqlite_prefix == sdk else [sdk, sqlite_prefix]
        configure = [
            'cmake', '-S', ROOT / 'native', '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_PREFIX_PATH=' + ';'.join(str(p) for p in prefixes),
            f'-DEXE_SQLITE3={sqlite}', f'-DSQLite3_ROOT={sqlite_prefix}',
        ]
        if args.toolchain:
            configure += [f'-DCMAKE_TOOLCHAIN_FILE={args.toolchain.resolve()}', '-DVCPKG_MANIFEST_MODE=OFF']
            if SYSTEM == 'windows':
                configure += [f'-DVCPKG_TARGET_TRIPLET={"arm64" if ARCH == "arm64" else "x64"}-windows']
        if args.proj_source:
            configure += [f'-DFETCHCONTENT_SOURCE_DIR_PROJ={args.proj_source.resolve()}']
        run(configure)
        run(['cmake', '--build', build, '--parallel', '2'])
        run(['ctest', '--test-dir', build, '--output-on-failure', '--parallel', '2'])
    resources = stage(binary, sdk, build, vc_dirs)
    run(['go', 'test', '-p=2', './...', '-count=1'], cwd=APP)
    env = dict(os.environ, CGO_ENABLED='0', GOMAXPROCS='2')
    if SYSTEM == 'windows':
        # 同一个 console 构建用于便携目录和桌面包的浏览器入口。
        run(['go', 'build', '-p=2', '-o', resources / 'osgb-tiles-browser.exe', './cmd/osgb-tiles-app'], cwd=APP, env=env)
        (resources / 'Start Browser.cmd').write_text(
            '@echo off\ncd /d "%~dp0"\n'
            '"%~dp0osgb-tiles-browser.exe" --browser\n'
            'if errorlevel 1 pause\n', encoding='ascii', newline='\r\n')
        (resources / 'README-Windows.txt').write_text(
            'Windows x64 完整包：请先解压整个目录，保留 bin、插件、DLL 和 licenses。\n'
            '便携桌面包：双击 OSGB 转 3D Tiles.exe；不需要安装本应用。\n'
            '可选个人安装：运行 osgb-tiles-setup-*.exe，默认安装到当前用户的 LocalAppData\\Programs，无需管理员权限。\n'
            '没有 WebView2 或不想安装：运行 Start Browser.cmd；在浏览器手动填写本机路径，按终端 Ctrl+C 停止。\n'
            '若需要桌面窗口，可从微软下载 WebView2 安装器，以普通用户身份运行：\n'
            'https://go.microsoft.com/fwlink/p/?LinkId=2124703\n'
            '企业策略可能限制运行时安装；本包不会自动提权或安装 WebView2。\n', encoding='utf-8-sig')
    if args.skip_desktop:
        dest = APP / 'dist' / PLATFORM / 'portable'
        if dest.exists():
            shutil.rmtree(dest)
        shutil.copytree(resources, dest)
        if SYSTEM == 'windows':
            (dest / 'osgb-tiles-browser.exe').rename(dest / 'osgb-tiles-app.exe')
            launcher = dest / 'Start Browser.cmd'
            launcher.write_bytes(launcher.read_bytes().replace(b'osgb-tiles-browser.exe', b'osgb-tiles-app.exe'))
        else:
            run(['go', 'build', '-p=2', '-o', dest / ('osgb-tiles-app' + EXE), './cmd/osgb-tiles-app'], cwd=APP, env=env)
        (dest / 'README.txt').write_text('运行 osgb-tiles-app --headless；使用终端显示的本机地址。命令行转换使用 bin/osgb-tiles。请整体移动目录。\n', encoding='utf-8')
        verify_resources(dest, expected_platform=PLATFORM)
        print(f'便携包: {dest}', flush=True)
    else:
        env = dict(os.environ, GOMAXPROCS='2')
        run(['go', 'tool', 'mygo', 'build', '--platform', f'{SYSTEM}/{ARCH}'], cwd=APP, env=env)
        results = finalize_distribution(APP / 'dist' / PLATFORM)
        for package, _ in results:
            print(f'桌面包: {package}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (subprocess.CalledProcessError, RuntimeError) as error:
        sys.exit(str(error))
