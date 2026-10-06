"""Stage the candidate without changing installed development classes."""
from pathlib import Path
import hashlib
import json
import re
import shutil
import xml.etree.ElementTree as ET
import zipfile

TOOLS = Path('E:/SkyCraftKCD2Tools')
PORT = Path(__file__).resolve().parents[1]
NAME = 'world-combat-repair-20261007'
CANDIDATE = TOOLS / 'candidates' / NAME
BUILD = CANDIDATE / 'fabric-build'
RELEASE = TOOLS / 'releases' / NAME
FABRIC = TOOLS / 'fabric-src'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()

def main():
    jar = BUILD / 'libs/skycraft-0.1.2.jar'
    checked = 0
    with zipfile.ZipFile(jar) as archive:
        for source_set in ('main', 'client'):
            for file in (BUILD / 'classes/java' / source_set).rglob('*.class'):
                if archive.read(file.relative_to(BUILD / 'classes/java' / source_set).as_posix()) != file.read_bytes():
                    raise RuntimeError(f'JAR mismatch: {file}')
                checked += 1
    suites = [ET.parse(file).getroot() for file in (BUILD / 'test-results/test').glob('TEST-*.xml')]
    tests = sum(int(s.get('tests', 0)) for s in suites)
    if not tests or any(int(s.get('failures', 0)) + int(s.get('errors', 0)) for s in suites):
        raise RuntimeError('Java tests failed or missing')
    combat_log = CANDIDATE / 'runtime-harness/client.out.log'
    shader_log = CANDIDATE / 'shader-test/shader-test.out.log'
    combat_match = re.search(r'COMBAT_RUNTIME_RESULT: PASSED \((\d+) checks\)', combat_log.read_text(encoding='utf-8'))
    if not combat_match or int(combat_match[1]) < 16:
        raise RuntimeError('Runtime combat evidence missing')
    if 'SHADER_RUNTIME_RESULT: PASSED' not in shader_log.read_text(encoding='utf-8'):
        raise RuntimeError('Runtime shader evidence missing')
    RELEASE.mkdir(parents=True, exist_ok=True)
    files = {
        'd3d12.dll': TOOLS / 'KCD2ModLoader-build/Release/d3d12_.dll',
        'SkyCraftKCD2.addon64': TOOLS / 'reshade-runtime/SkyCraftKCD2.addon64',
        'SkyCraftKCD2.fx': PORT / 'passthrough/SkyCraftKCD2.fx',
        'ReShade.ini': PORT / 'passthrough/ReShade.ini',
        jar.name: jar,
        'repair-notes.md': PORT / ('docs/' + NAME + '.md'),
    }
    for name, file in files.items(): shutil.copy2(file, RELEASE / name)
    shutil.copytree(BUILD / 'classes', RELEASE / 'runtime/classes', dirs_exist_ok=True)
    shutil.copytree(BUILD / 'resources', RELEASE / 'runtime/resources', dirs_exist_ok=True)
    evidence = RELEASE / 'verification'
    evidence.mkdir(exist_ok=True)
    shutil.copy2(combat_log, evidence / 'minecraft-combat-runtime.log')
    shutil.copy2(shader_log, evidence / 'reshade-shader-runtime.log')
    shutil.copy2(CANDIDATE / 'shader-test/ReShade.log', evidence / 'ReShade-smoke.log')
    for name in ('fabric-build.log', 'native-build.log', 'addon-build.log'):
        shutil.copy2(CANDIDATE / name, evidence / name)
    shutil.copy2(CANDIDATE / 'contracts.json', evidence / 'contracts.json')
    behavior = TOOLS / 'source-references/behavior-20261007'
    shutil.copytree(behavior, RELEASE / 'references/current-game-api', dirs_exist_ok=True)
    shutil.copytree(BUILD / 'test-results/test', evidence / 'junit', dirs_exist_ok=True)
    refs = TOOLS / 'source-references/occlusion-combat-20261006'
    ref_records = []
    for name, url in (
        ('cheat_core_npc.lua', 'https://raw.githubusercontent.com/Ovo307000/kcd2-mods/main/reference/mod-templates/cheat-mod-unpacked/scripts/cheat/cheat_core_npc.lua'),
        ('generic_depth_addon.cpp', 'https://raw.githubusercontent.com/crosire/reshade/main/examples/09-depth/generic_depth_addon.cpp'),
    ):
        destination = RELEASE / 'references' / name
        destination.parent.mkdir(exist_ok=True)
        shutil.copy2(refs / name, destination)
        ref_records.append({'file': name, 'sourceUrl': url, 'sha256': sha(destination)})
    (RELEASE / 'references/files.json').write_text(json.dumps(ref_records, indent=2)+'\n')
    snapshot = RELEASE / 'sources'
    shutil.copytree(PORT, snapshot / 'kcd2-port', dirs_exist_ok=True, ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    shutil.copytree(FABRIC / 'src', snapshot / 'fabric/src', dirs_exist_ok=True)
    for name in ('build.gradle','gradle.properties','settings.gradle'):
        shutil.copy2(FABRIC / name, snapshot / 'fabric' / name)
    shutil.copy2(PORT.parent / 'source-review/SkyCraft-main/LICENSE', snapshot / 'fabric/LICENSE')
    shutil.copy2(PORT.parent / 'source-review/SkyCraft-main/LICENSE', RELEASE / 'SkyCraft-LICENSE')
    loader = TOOLS / 'KCD2ModLoader-src/WizGery-KCD2ModLoader-a5dd09b'
    for name in ('gui.cpp', 'renderer.cpp'):
        shutil.copy2(loader / 'src/gui' / name, snapshot / ('loader-' + name))
    shutil.copy2(PORT.parent / 'source-review/SkyCraft-main/protocol/skycraft_protocol.h', snapshot / 'skycraft_protocol.h')
    with zipfile.ZipFile(RELEASE / 'sources.zip','w',zipfile.ZIP_DEFLATED) as archive:
        for file in sorted(snapshot.rglob('*')):
            if file.is_file(): archive.write(file,file.relative_to(snapshot).as_posix())
    result = {'candidate':NAME, 'candidateClassesCheckedAgainstJar':checked, 'javaTests':tests,
        'minecraftRuntimeChecks':int(combat_match[1]), 'reshadeRuntimeChecks':5,
        'hostSha256':sha(RELEASE/'d3d12.dll'), 'fabricJarSha256':sha(RELEASE/jar.name),
        'addonSha256':sha(RELEASE/'SkyCraftKCD2.addon64'), 'shaderSha256':sha(RELEASE/'SkyCraftKCD2.fx'),
        'inGameVerified':False}
    (RELEASE / 'build-verification.json').write_text(json.dumps(result,indent=2)+'\n')
    manifest = [{'file':file.relative_to(RELEASE).as_posix(),'sha256':sha(file),'bytes':file.stat().st_size}
        for file in sorted(RELEASE.rglob('*')) if file.is_file() and file.name != 'manifest.json']
    (RELEASE/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(result))

if __name__ == '__main__': main()
