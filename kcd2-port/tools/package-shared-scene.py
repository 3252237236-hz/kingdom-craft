"""Stage the shared-scene build and its actual verification evidence on E:."""
from pathlib import Path
import argparse
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
    global NAME, CANDIDATE, BUILD, RELEASE
    parser = argparse.ArgumentParser()
    parser.add_argument('--candidate', default=NAME)
    parser.add_argument('--release-name')
    parser.add_argument('--preserved-minecraft-baseline', type=Path)
    parser.add_argument('--native-dll', type=Path)
    parser.add_argument('--preserved-baseline', type=Path)
    parser.add_argument('--minimum-combat-checks', type=int, default=20)
    parser.add_argument('--require-attack-approval-checks', action='store_true')
    parser.add_argument('--native-mob-profile', type=Path)
    parser.add_argument('--mob-installation-evidence', type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9-]+', args.candidate):
        raise RuntimeError('Invalid candidate name')
    NAME = args.candidate
    CANDIDATE = TOOLS / 'candidates' / NAME
    BUILD = CANDIDATE / 'fabric-build'
    RELEASE = TOOLS / 'releases' / NAME
    if args.release_name:
        if not re.fullmatch(r'[a-z0-9-]+', args.release_name):
            raise RuntimeError('Invalid release name')
        NAME = args.release_name
        RELEASE = TOOLS / 'releases' / NAME
    baseline = args.preserved_baseline
    attack_evidence = None
    if args.require_attack_approval_checks:
        attack_evidence = json.loads((CANDIDATE / 'attack-contract-evidence.json').read_text(encoding='utf-8-sig'))
        if attack_evidence['checks'] < 555 or 'native attack approval contracts passed' not in \
                (CANDIDATE / 'attack-contract.log').read_text(encoding='utf-8-sig'):
            raise RuntimeError('Native attack approval contracts missing')
        for name, fingerprint in attack_evidence['sourceSha256'].items():
            if sha(REPO / name) != fingerprint:
                raise RuntimeError(f'Native attack contract source changed: {name}')
        if sha(CANDIDATE / 'approval-contract-build/Release/attack_contract.exe') != attack_evidence['executableSha256']:
            raise RuntimeError('Native attack contract executable changed')
    preserved = {}
    if baseline:
        for name in (
            'kcd2-port/passthrough/SkyCraftKCD2.fx',
            'kcd2-port/host/skycraft_host.cpp',
            'kcd2-port/host/skycraft_scene.hpp',
            'kcd2-port/host/skycraft_scene.cpp',
            'kcd2-port/host/skycraft_scene_geometry.hpp',
            'fabric/src/client/java/dev/skycraft/client/mixin/GameRendererMixin.java',
            'fabric/src/client/java/dev/skycraft/client/mixin/CameraMixin.java',
            'fabric/src/main/java/dev/skycraft/world/NativeScene.java',
            'fabric/src/main/java/dev/skycraft/world/SkyRay.java',
        ):
            if sha(REPO / name) != sha(baseline / 'sources' / name):
                raise RuntimeError(f'Preserved baseline source changed: {name}')
            preserved[name] = sha(REPO / name)
    jar = BUILD / 'libs/skycraft-0.1.2.jar'
    minecraft_preserved = {}
    if args.preserved_minecraft_baseline:
        previous = args.preserved_minecraft_baseline
        if sha(jar) != sha(previous / jar.name):
            raise RuntimeError('Minecraft baseline JAR changed')
        for file in (REPO / 'fabric/src').rglob('*'):
            if file.is_file():
                relative = file.relative_to(REPO).as_posix()
                if sha(file) != sha(previous / 'sources' / relative):
                    raise RuntimeError(f'Minecraft baseline source changed: {relative}')
                minecraft_preserved[relative] = sha(file)
    checked = 0
    with zipfile.ZipFile(jar) as archive:
        if any(name.startswith('skycraft/test/') or 'harness' in name.lower() for name in archive.namelist()):
            raise RuntimeError('Test harness leaked into production JAR')
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
    required = [('SCENE', 13), ('ENV', 12), ('COMBAT', args.minimum_combat_checks), ('CAMERA', 6)]
    if args.native_mob_profile:
        required.append(('MOB', 18))
    for name, minimum in required:
        match = re.search(rf'{name}_RUNTIME_RESULT: PASSED \((\d+) checks\)', runtime)
        if not match or int(match[1]) < minimum or '_RUNTIME_RESULT: FAILED' in runtime:
            raise RuntimeError(f'Runtime {name} evidence missing or failed')
        counts[name.lower()] = int(match[1])
    mob_evidence = None
    install_evidence = None
    if args.native_mob_profile:
        profile = args.native_mob_profile.resolve()
        if profile.drive.upper() != 'E:':
            raise RuntimeError('Native profile must be staged on E:')
        mob_evidence = json.loads((profile/'build-verification.json').read_text(encoding='utf-8-sig'))
        if mob_evidence['checks'] < 282 or sha(profile/'Data/kingdomcraft_mobs.pak') != mob_evidence['profilePakSha256'] \
                or sha(profile/'mod.manifest') != mob_evidence['modManifestSha256']:
            raise RuntimeError('Native profile evidence mismatch')
        for name, fingerprint in mob_evidence['sourceSha256'].items():
            if sha(REPO/name) != fingerprint:
                raise RuntimeError(f'Native mob source changed: {name}')
        with zipfile.ZipFile(profile/'Data/kingdomcraft_mobs.pak') as archive:
            expected = {p.relative_to(REPO/'kcd2-port/native-mobs').as_posix():p.read_bytes()
                        for p in (REPO/'kcd2-port/native-mobs').rglob('*')
                        if p.is_file() and p.name != 'mod.manifest'}
            if set(archive.namelist()) != set(expected) or any(archive.read(n) != b for n,b in expected.items()):
                raise RuntimeError('Native profile archive differs from authored sources')
        if not args.mob_installation_evidence:
            raise RuntimeError('Native mob installation/rollback evidence required')
        install_evidence = json.loads(args.mob_installation_evidence.read_text(encoding='utf-8-sig'))
        if install_evidence['checks'] < 60:
            raise RuntimeError('Native mob installation checks missing')
        for name, fingerprint in install_evidence['sourceSha256'].items():
            if sha(REPO/name) != fingerprint:
                raise RuntimeError(f'Installation contract source changed: {name}')
        if sha(args.native_dll or CANDIDATE/'native-build/Release/d3d12_.dll') != install_evidence['hostSha256'] \
            or sha(jar) != install_evidence['fabricJarSha256'] \
            or mob_evidence['profilePakSha256'] != install_evidence['nativeMobProfilePakSha256']:
            raise RuntimeError('Installation fixtures used different production artifacts')
    contracts = {'host-lua-contract.log': CANDIDATE / 'host-lua-contract.log',
        'scene-contract.log': baseline / 'verification/scene-contract.log' if baseline else CANDIDATE / 'scene-contract.log'}
    for name, marker in (('host-lua-contract.log', 'host Lua contracts passed'),
                         ('scene-contract.log', 'shared scene geometry contracts passed')):
        if marker not in contracts[name].read_text(encoding='utf-8-sig'):
            raise RuntimeError(f'Contract evidence missing: {name}')
    original = json.loads((BASE / 'build-verification.json').read_text(encoding='utf-8-sig'))
    for name, key in (('SkyCraftKCD2.addon64', 'addonSha256'),):
        if sha(BASE / name) != original[key]:
            raise RuntimeError(f'Unchanged compositor fingerprint mismatch: {name}')
    shader = REPO / 'kcd2-port/passthrough/SkyCraftKCD2.fx'
    shader_log = baseline / 'verification/reshade-shader-runtime.log' if baseline else CANDIDATE / 'shader-test/shader-test.out.log'
    shader_result = shader_log.read_text(encoding='utf-8-sig')
    shader_reference = baseline / 'SkyCraftKCD2.fx' if baseline else CANDIDATE / 'shader-test/reshade-shaders/Shaders/SkyCraftKCD2.fx'
    if sha(shader) != sha(shader_reference) \
            or 'SHADER_RUNTIME_RESULT: PASSED' not in shader_result \
            or 'SHADER_RUNTIME_CHECKS: 13' not in shader_result or 'SHADER_RUNTIME_FAIL' in shader_result:
        raise RuntimeError('Current shader runtime evidence missing or failed')
    if 'harness.jar' in ''.join(str(p) for p in BUILD.rglob('*')):
        raise RuntimeError('Test mod leaked into production build')
    RELEASE.mkdir(parents=True, exist_ok=False)
    files = {
        'd3d12.dll': args.native_dll or CANDIDATE / 'native-build/Release/d3d12_.dll',
        jar.name: jar,
        'SkyCraftKCD2.addon64': BASE / 'SkyCraftKCD2.addon64',
        'SkyCraftKCD2.fx': shader,
        'repair-notes.md': REPO / f'kcd2-port/docs/{args.candidate}.md',
        'SkyCraft-LICENSE': REPO / 'fabric/LICENSE',
    }
    for name, source in files.items():
        shutil.copy2(source, RELEASE / name)
    if mob_evidence:
        mod = RELEASE/'native-mod/kingdomcraft_mobs'
        (mod/'Data').mkdir(parents=True)
        shutil.copy2(args.native_mob_profile/'mod.manifest', mod/'mod.manifest')
        shutil.copy2(args.native_mob_profile/'Data/kingdomcraft_mobs.pak', mod/'Data/kingdomcraft_mobs.pak')
    for folder in ('classes', 'resources'):
        for source_set in ('main', 'client'):
            relative = Path('java') / source_set if folder == 'classes' else Path(source_set)
            shutil.copytree(BUILD / folder / relative, RELEASE / 'runtime' / folder / relative)
    shutil.copytree(REPO / 'fabric/src', RELEASE / 'runtime/src')
    evidence = RELEASE / 'verification'
    evidence.mkdir()
    if mob_evidence:
        shutil.copy2(args.native_mob_profile/'build-verification.json', evidence/'native-mob-profile.json')
        shutil.copy2(args.native_mob_profile/'controller-contract.log', evidence/'native-mob-controller.log')
        shutil.copy2(args.mob_installation_evidence, evidence/'native-mob-installation.json')
    for name in ('fabric-build.log', 'native-build.log'):
        shutil.copy2(CANDIDATE / name, evidence / name)
    if attack_evidence:
        for name in ('attack-contract.log', 'attack-contract-build.log', 'attack-contract-evidence.json'):
            shutil.copy2(CANDIDATE / name, evidence / name)
    for name, source in contracts.items():
        shutil.copy2(source, evidence / name)
    shutil.copy2(log, evidence / 'minecraft-runtime.log')
    shutil.copy2(CANDIDATE / 'runtime-harness/client.err.log', evidence / 'minecraft-stderr.log')
    for file in ('camera-pass-test-aborted.out.log', 'camera-pass-test-aborted.err.log'):
        source = CANDIDATE / 'runtime-harness' / file
        if source.is_file(): shutil.copy2(source, evidence / file)
    for file in (CANDIDATE / 'runtime-harness').glob('*failed.*.log'):
        shutil.copy2(file, evidence / file.name)
    shutil.copytree(BUILD / 'test-results/test', evidence / 'junit')
    shutil.copy2(shader_log, evidence / 'reshade-shader-runtime.log')
    shutil.copy2(baseline / 'verification/reshade-runtime.log' if baseline else CANDIDATE / 'shader-test/ReShade.log',
                 evidence / 'reshade-runtime.log')
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
        'nativeSoulAttackerIdentityGameplayVerified': False,
        'nativeCameraDisplacementVerified': False,
        'inGameVerified': False,
    }
    if baseline:
        result.update({'preservedVisualSceneBaseline': baseline.name,
            'preservedSourceSha256': preserved, 'shaderChecksReused': True,
            'sceneGeometryContractReused': True,
            'nativeEquippedBoundsGameplayVerified': False})
    if attack_evidence:
        result.update({'nativeAttackApprovalContractChecks': attack_evidence['checks'],
            'nativeAttackApprovalSourceSha256': attack_evidence['sourceSha256'],
            'combatWaitBudgetMsPerServerTick': 50,
            'nativeAttackHandshakeGameplayVerified': False})
    if minecraft_preserved:
        result.update({'minecraftRuntimeChecksReusedFrom': args.preserved_minecraft_baseline.name,
            'preservedMinecraftSourceSha256': minecraft_preserved})
    if mob_evidence:
        result.update({'nativeMobProfilePakSha256': mob_evidence['profilePakSha256'],
            'nativeMobManifestSha256': mob_evidence['modManifestSha256'],
            'nativeMobControllerChecks': mob_evidence['checks'],
            'nativeMobTransportImplemented': True, 'nativeMobTransportIsolatedRuntimeVerified': True,
            'nativeMobProfileLoadingVerified': False, 'nativeMobPerceptionVerified': False,
            'nativeMobBodyDimensionsApplied': False, 'exactNativeMobHitEventsCaptured': False})
        result['nativeMobInstallationRollbackChecks'] = install_evidence['checks']
    (RELEASE / 'build-verification.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    manifest = [{'file': file.relative_to(RELEASE).as_posix(), 'sha256': sha(file), 'bytes': file.stat().st_size}
                for file in sorted(RELEASE.rglob('*')) if file.is_file() and file.name != 'manifest.json']
    (RELEASE / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
