"""Exercise the actual installer on disposable E: trees, including rollback.

Only path constants are substituted. The fault case inserts one throw after
the installer's directory changes; no real game, runtime or save is touched.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO/'kcd2-port/tools/install-shared-scene.ps1'
CHECKS = 0


def sha(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def check(value):
    global CHECKS
    assert value, f'installer check {CHECKS+1} failed'
    CHECKS += 1


def stage(release, root, existing):
    tools = root/'tools'
    staged = tools/'releases'/release.name
    for file in release.rglob('*'):
        if file.is_file():
            destination = staged/file.relative_to(release)
            destination.parent.mkdir(parents=True, exist_ok=True)
            # Manifest is changed by the installer; other source files stay read-only.
            if file.name in ('manifest.json', 'installation.json'):
                shutil.copyfile(file, destination)
            else:
                os.link(file, destination)
    game = root/'game'
    game_bin = game/'Bin/Win64MasterMasterSteamPGO'
    runtime = tools/'fabric-src'
    old_files = {
        game_bin/'d3d12.dll':b'previous host',
        game_bin/'reshade-shaders/Shaders/SkyCraftKCD2.fx':b'previous shader',
        runtime/'build/libs/skycraft-0.1.2.jar':b'previous jar',
        runtime/'build/classes/old-class.marker':b'previous classes',
        runtime/'build/resources/old-resource.marker':b'previous resources',
        runtime/'src/old-source.marker':b'previous source',
        runtime/'run/saves/SkyCraft/old-save.marker':b'untouched player world',
        game/'Mods/other_mod/Data/other.pak':b'untouched unrelated mod',
        game/'Mods/mod_order.txt':b'other_mod\r\n# user comment\r\n',
    }
    if existing:
        old_files[game/'Mods/kingdomcraft_mobs/mod.manifest'] = b'previous profile'
        old_files[game/'Mods/kingdomcraft_mobs/Data/kingdomcraft_mobs.pak'] = b'previous pak'
    for file, data in old_files.items():
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(data)
    if json.loads((release/'build-verification.json').read_text(encoding='utf-8-sig')).get('compositorUpdated'):
        old_files[game_bin/'SkyCraftKCD2.addon64']=b'previous compositor'
        (game_bin/'SkyCraftKCD2.addon64').write_bytes(b'previous compositor')
    else:
        shutil.copyfile(release/'SkyCraftKCD2.addon64', game_bin/'SkyCraftKCD2.addon64')
    text = SCRIPT.read_text(encoding='utf-8-sig')
    replacements = {
        "$toolsRoot = 'E:\\SkyCraftKCD2Tools'":f"$toolsRoot = '{tools}'",
        "$gameBin = 'D:\\Steam\\steamapps\\common\\KingdomComeDeliverance2\\Bin\\Win64MasterMasterSteamPGO'":f"$gameBin = '{game_bin}'",
        "$gameRoot = 'D:\\Steam\\steamapps\\common\\KingdomComeDeliverance2'":f"$gameRoot = '{game}'",
    }
    for before, after in replacements.items():
        check(text.count(before)==1)
        text = text.replace(before, after)
    return text, tools, game, runtime, old_files


def run_case(release, base, name, existing, fault=False):
    root = base/name
    root.mkdir(parents=True, exist_ok=False)
    text, tools, game, runtime, previous = stage(release, root, existing)
    if fault:
        needle = "    Assert-RuntimesClosed\n    foreach ($target in $targets) { Copy-Item"
        check(text.count(needle)==1)
        text = text.replace(needle, "    throw 'deliberate installer contract fault'\n"+needle)
    script = root/'installer.ps1'
    script.write_text(text, encoding='utf-8-sig')
    environment = os.environ.copy()
    environment.pop('PSModulePath', None)
    shell = shutil.which('pwsh.exe') or shutil.which('powershell.exe')
    if not shell:
        raise RuntimeError('PowerShell required')
    result = subprocess.run([shell,'-NoProfile','-ExecutionPolicy','Bypass','-File',str(script),
        '-Candidate',release.name,'-MinimumMinecraftChecks','94'],capture_output=True,env=environment)
    (root/'stdout.log').write_bytes(result.stdout)
    (root/'stderr.log').write_bytes(result.stderr)
    check((result.returncode!=0)==fault)
    if fault:
        check(b'deliberate installer contract fault' in result.stderr)
        for file, data in previous.items():
            check(file.read_bytes()==data)
        if not existing:
            check(not (game/'Mods/kingdomcraft_mobs').exists())
        check(not (tools/(release.name+'-installation.json')).exists())
    else:
        check((game/'Mods/other_mod/Data/other.pak').read_bytes()==b'untouched unrelated mod')
        order = (game/'Mods/mod_order.txt').read_bytes().decode('utf-8-sig')
        check(order=='other_mod\r\n# user comment\r\nkingdomcraft_mobs\r\n')
        check((runtime/'run/saves/SkyCraft/old-save.marker').read_bytes()==b'untouched player world')
        record = json.loads((tools/(release.name+'-installation.json')).read_text(encoding='utf-8-sig'))
        backup = Path(record['backup'])
        check(record['inGameVerified'] is False and record['exactNativeMobHitEventsCaptured'] is False)
        check((backup/'mod_order.txt').read_bytes()==b'other_mod\r\n# user comment\r\n')
        if existing:
            check((backup/'native-mob-profile/mod.manifest').read_bytes()==b'previous profile')
        for source, destination in (
            ('d3d12.dll', game/'Bin/Win64MasterMasterSteamPGO/d3d12.dll'),
            ('SkyCraftKCD2.addon64', game/'Bin/Win64MasterMasterSteamPGO/SkyCraftKCD2.addon64'),
            ('native-mod/kingdomcraft_mobs/mod.manifest', game/'Mods/kingdomcraft_mobs/mod.manifest'),
            ('native-mod/kingdomcraft_mobs/Data/kingdomcraft_mobs.pak', game/'Mods/kingdomcraft_mobs/Data/kingdomcraft_mobs.pak'),
        ):
            check(sha(release/source)==sha(destination))
    print(f'installer fixture passed: {name}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--release',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    release, output = args.release.resolve(), args.output.resolve()
    if release.drive.upper()!='E:' or output.drive.upper()!='E:':
        raise RuntimeError('Use E: fixtures only')
    run_case(release, output, 'new-profile', False)
    run_case(release, output, 'updated-profile', True)
    run_case(release, output, 'rollback-new-profile', False, True)
    run_case(release, output, 'rollback-updated-profile', True, True)
    print(f'native mob installation contracts passed ({CHECKS} checks)')
    evidence={'checks':CHECKS,'hostSha256':sha(release/'d3d12.dll').upper(),
        'fabricJarSha256':sha(release/'skycraft-0.1.2.jar').upper(),
        'addonSha256':sha(release/'SkyCraftKCD2.addon64').upper(),
        'nativeMobProfilePakSha256':sha(release/'native-mod/kingdomcraft_mobs/Data/kingdomcraft_mobs.pak').upper(),
        'sourceSha256':{p.relative_to(REPO).as_posix():sha(p).upper() for p in (SCRIPT,Path(__file__).resolve())}}
    (output/'installation-evidence.json').write_text(json.dumps(evidence,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':
    main()
