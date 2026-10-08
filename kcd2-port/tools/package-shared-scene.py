"""Stage the shared-scene build and its actual verification evidence on E:."""
from pathlib import Path
import hashlib
import json
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET
import zipfile

TOOLS = Path('E:/SkyCraftKCD2Tools')
REPO = Path(__file__).resolve().parents[2]
NAME = 'shared-scene-20261009'
CANDIDATE = TOOLS / 'candidates' / NAME
BUILD = CANDIDATE / 'fabric-build'
RELEASE = TOOLS / 'releases' / NAME
BASE = TOOLS / 'releases/world-combat-repair-20261007'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def main():
    jar = BUILD / 'libs/skycraft-0.1.2.jar'
    checked = 0
    with zipfile.ZipFile(jar) as archive:
        for source_set in ('main', 'client'):
            root = BUILD / 'classes/java' / source_set
            for file in root.rglob('*.class'):
                if archive.read(file.relative_to(root).as_posix()) != file.read_bytes():
                    raise RuntimeError(f'Class/JAR mismatch: {file}')
                checked += 1
    suites = [ET.parse(file).getroot() for file in (BUILD / 'test-results/test').glob('TEST-*.xml')]
    tests = sum(int(suite.get('tests', 0)) for suite in suites)
    if tests < 38 or any(int(s.get('failures', 0)) + int(s.get('errors', 0)) for s in suites):
        raise RuntimeError('Java verification missing or failed')
    log = CANDIDATE / 'runtime-harness/client.out.log'
    runtime = log.read_text(encoding='utf-8')
    counts = {}
    for name, minimum in (('SCENE', 13), ('ENV', 12), ('COMBAT', 20), ('CAMERA', 6)):
        match = re.search(rf'{name}_RUNTIME_RESULT: PASSED \((\d+) checks\)', runtime)
        if not match or int(match[1]) < minimum or '_RUNTIME_RESULT: FAILED' in runtime:
            raise RuntimeError(f'Runtime {name} evidence missing or failed')
        counts[name.lower()] = int(match[1])
    for name, marker in (('host-lua-contract.log', 'host Lua contracts passed'),
                         ('scene-contract.log', 'shared scene geometry contracts passed')):
        if marker not in (CANDIDATE / name).read_text(encoding='utf-8-sig'):
            raise RuntimeError(f'Contract evidence missing: {name}')
    original = json.loads((BASE / 'build-verification.json').read_text(encoding='utf-8-sig'))
    for name, key in (('SkyCraftKCD2.addon64', 'addonSha256'),):
        if sha(BASE / name) != original[key]:
            raise RuntimeError(f'Unchanged compositor fingerprint mismatch: {name}')
    shader = REPO / 'kcd2-port/passthrough/SkyCraftKCD2.fx'
    shader_log = CANDIDATE / 'shader-test/shader-test.out.log'
    shader_result = shader_log.read_text(encoding='utf-8-sig')
    if sha(shader) != sha(CANDIDATE / 'shader-test/reshade-shaders/Shaders/SkyCraftKCD2.fx') \
            or 'SHADER_RUNTIME_RESULT: PASSED' not in shader_result \
            or 'SHADER_RUNTIME_CHECKS: 13' not in shader_result or 'SHADER_RUNTIME_FAIL' in shader_result:
        raise RuntimeError('Current shader runtime evidence missing or failed')
    if 'harness.jar' in ''.join(str(p) for p in BUILD.rglob('*')):
        raise RuntimeError('Test mod leaked into production build')
    RELEASE.mkdir(parents=True, exist_ok=False)
    files = {
        'd3d12.dll': CANDIDATE / 'native-build/Release/d3d12_.dll',
        jar.name: jar,
        'SkyCraftKCD2.addon64': BASE / 'SkyCraftKCD2.addon64',
        'SkyCraftKCD2.fx': shader,
        'repair-notes.md': REPO / f'kcd2-port/docs/{NAME}.md',
        'SkyCraft-LICENSE': REPO / 'fabric/LICENSE',
    }
    for name, source in files.items():
        shutil.copy2(source, RELEASE / name)
    for folder in ('classes', 'resources'):
        for source_set in ('main', 'client'):
            relative = Path('java') / source_set if folder == 'classes' else Path(source_set)
            shutil.copytree(BUILD / folder / relative, RELEASE / 'runtime' / folder / relative)
    shutil.copytree(REPO / 'fabric/src', RELEASE / 'runtime/src')
    evidence = RELEASE / 'verification'
    evidence.mkdir()
    for name in ('fabric-build.log', 'native-build.log', 'host-lua-contract.log', 'scene-contract.log'):
        shutil.copy2(CANDIDATE / name, evidence / name)
    shutil.copy2(log, evidence / 'minecraft-runtime.log')
    shutil.copy2(CANDIDATE / 'runtime-harness/client.err.log', evidence / 'minecraft-stderr.log')
    for file in ('camera-pass-test-aborted.out.log', 'camera-pass-test-aborted.err.log'):
        shutil.copy2(CANDIDATE / 'runtime-harness' / file, evidence / file)
    shutil.copytree(BUILD / 'test-results/test', evidence / 'junit')
    shutil.copy2(shader_log, evidence / 'reshade-shader-runtime.log')
    shutil.copy2(CANDIDATE / 'shader-test/ReShade.log', evidence / 'reshade-runtime.log')
    sources = RELEASE / 'sources'
    sources.mkdir()
    tracked = subprocess.check_output(['git', 'ls-files', '-c', '-o', '--exclude-standard'], cwd=REPO).decode('utf-8')
    for name in tracked.splitlines():
        if not name.startswith(('fabric/', 'native-loader/', 'kcd2-port/', 'protocol/')) \
                and name not in ('README.md', 'THIRD-PARTY-NOTICES.md'):
            continue
        destination = sources / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / name, destination)
    with zipfile.ZipFile(RELEASE / 'sources.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for file in sorted(sources.rglob('*')):
            if file.is_file():
                archive.write(file, file.relative_to(sources).as_posix())
    result = {
        'candidate': NAME, 'candidateClassesCheckedAgainstJar': checked,
        'javaTests': tests, 'minecraftRuntimeChecks': sum(counts.values()),
        'minecraftRuntimeChecksBySystem': counts,
        'nativeSceneContractPassed': True, 'hostLuaContractPassed': True,
        'hostSha256': sha(RELEASE / 'd3d12.dll'), 'fabricJarSha256': sha(RELEASE / jar.name),
        'addonSha256': original['addonSha256'], 'shaderSha256': sha(shader),
        'shaderRuntimeChecks': 13, 'reusedAddonFrom': original['candidate'],
        'nativeWeaponClashVerified': False, 'nativeNpcDeathVerified': False,
        'nativeCameraDisplacementVerified': False,
        'inGameVerified': False,
    }
    (RELEASE / 'build-verification.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    manifest = [{'file': file.relative_to(RELEASE).as_posix(), 'sha256': sha(file), 'bytes': file.stat().st_size}
                for file in sorted(RELEASE.rglob('*')) if file.is_file() and file.name != 'manifest.json']
    (RELEASE / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
