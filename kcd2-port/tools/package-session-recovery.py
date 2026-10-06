"""Stage the reviewed host/client and verify the dev runtime matches the JAR."""
from pathlib import Path
import hashlib
import json
import shutil
import zipfile

TOOLS = Path(r"E:\SkyCraftKCD2Tools")
PORT = Path(__file__).resolve().parents[1]
RELEASE = TOOLS / "releases" / "session-recovery-20261006"
FABRIC = TOOLS / "fabric-src"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def main():
    jar = FABRIC / "build/libs/skycraft-0.1.2.jar"
    checked = 0
    with zipfile.ZipFile(jar) as archive:
        for source_set in ("main", "client"):
            root = FABRIC / "build/classes/java" / source_set
            for file in root.rglob("*.class"):
                relative = file.relative_to(root).as_posix()
                if archive.read(relative) != file.read_bytes():
                    raise RuntimeError(f"Runtime class does not match release JAR: {relative}")
                checked += 1
    if checked == 0:
        raise RuntimeError("No runtime classes checked")
    RELEASE.mkdir(parents=True, exist_ok=True)
    shutil.copy2(TOOLS / "KCD2ModLoader-build/Release/d3d12_.dll", RELEASE / "d3d12.dll")
    shutil.copy2(jar, RELEASE / jar.name)
    shutil.copy2(PORT / "README.md", RELEASE / "README.md")
    shutil.copytree(PORT / "docs", RELEASE / "docs", dirs_exist_ok=True)
    shutil.copy2(PORT / "docs/session-recovery-20261006.md", RELEASE / "repair-notes.md")
    original = PORT.parent / "source-review/SkyCraft-main"
    shutil.copy2(original / "LICENSE", RELEASE / "SkyCraft-LICENSE")
    snapshot = RELEASE / "sources"
    shutil.copytree(PORT, snapshot / "kcd2-port", dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    shutil.copytree(FABRIC / "src", snapshot / "fabric/src", dirs_exist_ok=True)
    for name in ("build.gradle", "gradle.properties", "settings.gradle"):
        shutil.copy2(FABRIC / name, snapshot / "fabric" / name)
    shutil.copy2(original / "LICENSE", snapshot / "fabric/LICENSE")
    shutil.copy2(original / "protocol/skycraft_protocol.h", snapshot / "skycraft_protocol.h")
    renderer = TOOLS / "KCD2ModLoader-src/WizGery-KCD2ModLoader-a5dd09b/src/gui/renderer.cpp"
    shutil.copy2(renderer, snapshot / "loader-renderer.cpp")
    with zipfile.ZipFile(RELEASE / "sources.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for file in sorted(snapshot.rglob("*")):
            if file.is_file():
                archive.write(file, file.relative_to(snapshot).as_posix())
    result = {
        "candidate": RELEASE.name,
        "runtimeClassesCheckedAgainstJar": checked,
        "hostSha256": sha(RELEASE / "d3d12.dll"),
        "fabricJarSha256": sha(RELEASE / jar.name),
        "gameplayVerified": False,
    }
    (RELEASE / "build-verification.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
