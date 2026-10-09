"""Stage the authored native identity component; does not install or claim playability."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys
import zipfile

REPO = Path(__file__).resolve().parents[2]
SOURCE = REPO / 'kcd2-port/native-mobs'
TOOLS = Path('E:/SkyCraftKCD2Tools')


def sha(file):
    return hashlib.sha256(file.read_bytes()).hexdigest().upper()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--game-tables', type=Path, required=True)
    parser.add_argument('--stage', type=Path, default=TOOLS / 'candidates/native-mob-identity-20261009/profile-stage')
    args = parser.parse_args()
    result = subprocess.run([sys.executable, str(REPO/'kcd2-port/tests/native_mob_controller.py'),
                             '--game-tables', str(args.game_tables)],
                            capture_output=True, text=True, cwd=REPO)
    passed = re.search(r'native mob controller contracts passed \((\d+) checks\)', result.stdout)
    if result.returncode or not passed or int(passed[1]) < 100:
        print(result.stdout, end='');print(result.stderr, end='', file=sys.stderr)
        raise RuntimeError('Native mob profile contracts missing or failed')
    stage = args.stage.resolve()
    if stage.drive.upper() != 'E:':
        raise RuntimeError('Development packages must stay on E:')
    stage.mkdir(parents=True, exist_ok=False)
    manifest = stage / 'mod.manifest'
    manifest.write_bytes((SOURCE/'mod.manifest').read_bytes())
    archive = stage/'Data/kingdomcraft_mobs.pak'
    archive.parent.mkdir()
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as pak:
        for file in sorted(SOURCE.rglob('*')):
            if file.is_file() and file.name != 'mod.manifest':
                relative = file.relative_to(SOURCE).as_posix()
                if file.suffix not in ('.lua', '.xml'):
                    raise RuntimeError(f'Unexpected native profile file: {file}')
                info = zipfile.ZipInfo(relative, date_time=(2026,10,9,0,0,0))
                info.compress_type = zipfile.ZIP_DEFLATED
                pak.writestr(info, file.read_bytes())
    (stage/'controller-contract.log').write_text(result.stdout+result.stderr, encoding='utf-8')
    evidence = dict(status='development-component-not-installed', checks=int(passed[1]),
        profilePakSha256=sha(archive), modManifestSha256=sha(manifest),
        checkedAgainstGameTablesSha256=sha(args.game_tables),
        nativeProfileLoadingVerified=False, minecraftTransportConnected=False,
        npcPerceptionVerified=False, nativeBodyDimensionsApplied=False,
        exactNativeHitDamageCaptured=False, inGameVerified=False,
        sourceSha256={p.relative_to(REPO).as_posix():sha(p)
                      for p in sorted(SOURCE.rglob('*')) if p.is_file()})
    (stage/'build-verification.json').write_text(json.dumps(evidence, indent=2)+'\n', encoding='utf-8')
    print(result.stdout, end='')
    print(stage)


if __name__ == '__main__':
    main()
