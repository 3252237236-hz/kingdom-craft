"""Compile the test-only Fabric harness in an isolated E: candidate."""
import argparse
from pathlib import Path
import shutil
import subprocess
import zipfile

REPO = Path(__file__).resolve().parents[2]
TEMPLATE = Path('E:/SkyCraftKCD2Tools/candidates/combat-handshake-20261009')
JAVAC = Path('E:/SkyCraftKCD2Tools/JDK25/jdk-25.0.4.1+1/bin/javac.exe')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--candidate', type=Path, required=True)
    args = parser.parse_args()
    candidate = args.candidate.resolve()
    if candidate.drive.upper() != 'E:' or not (candidate/'fabric-build/classes/java/main').is_dir():
        raise RuntimeError('Require an E: candidate with a built production client')
    root = candidate/'runtime-harness'
    root.mkdir(exist_ok=False)
    old = TEMPLATE/'runtime-harness'
    for name in ('javac.args', 'launch.args', 'launch.cfg', 'harness.mod.json'):
        text = (old/name).read_text(encoding='utf-8-sig').replace(TEMPLATE.name, candidate.name)
        if name == 'javac.args':
            text += '\n"'+str(REPO/'kcd2-port/tests/runtime/NativeMobHarness.java').replace('\\','/')+'"\n'
        (root/name).write_text(text, encoding='utf-8')
    (root/'classes').mkdir()
    shutil.copytree(old/'run', root/'run')
    shutil.copyfile(TEMPLATE/'run-runtime.ps1', candidate/'run-runtime.ps1')
    result = subprocess.run([str(JAVAC), '@'+str(root/'javac.args')], capture_output=True, text=True)
    (root/'javac.log').write_text(result.stdout+result.stderr, encoding='utf-8')
    print(result.stdout+result.stderr, end='')
    if result.returncode:
        raise SystemExit(result.returncode)
    with zipfile.ZipFile(root/'harness.jar', 'w', zipfile.ZIP_DEFLATED) as jar:
        jar.write(root/'harness.mod.json', 'fabric.mod.json')
        for file in sorted((root/'classes').rglob('*.class')):
            jar.write(file, file.relative_to(root/'classes').as_posix())
    print('Test-only harness prepared: '+str(root))


if __name__ == '__main__':
    main()
