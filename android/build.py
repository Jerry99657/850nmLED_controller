"""
打包 850nm 蓝牙控制 App（不依赖 Gradle / Android Studio）

    python android/build.py

工具链（已下载到 D:/Tools/android）：
    jdk-17/            Temurin JDK 17（d8、apksigner 需要 JDK ≥ 11）
    build-tools-34/    aapt2 / d8 / zipalign / apksigner
    platform-34/       android.jar（API 34）

流程：aapt2 编译资源并链接出基础 APK → javac 编译 → d8 转 dex → 加入 APK → zipalign → apksigner 签名。
界面 assets/index.html 每次从 tools/ble_control.html 复制，保持浏览器版与 App 版一致。

签名密钥 android/keystore/release.p12 首次构建时生成。更新 App 必须用同一密钥签名，
否则手机会拒绝覆盖安装，请妥善保存该文件（仅用于自用侧载，不是应用商店密钥）。
"""
import os
import pathlib
import shutil
import subprocess
import sys
import zipfile

VERSION_NAME = "0.1.0"
VERSION_CODE = 1
MIN_SDK = 26        # Android 8.0
TARGET_SDK = 34

TOOLS = pathlib.Path(os.environ.get("ANDROID_TOOLS", "D:/Tools/android"))
JDK = TOOLS / "jdk-17"
BT = TOOLS / "build-tools-34"
ANDROID_JAR = TOOLS / "platform-34" / "android.jar"

ROOT = pathlib.Path(__file__).resolve().parent
PROJECT = ROOT.parent
BUILD = ROOT / "build"
OUT = ROOT / "out" / f"850nm-ble-controller-{VERSION_NAME}.apk"
KEYSTORE = ROOT / "keystore" / "release.p12"
KS_PASS = "850nm-ble"
KEY_ALIAS = "nm850"

ENV = dict(os.environ, JAVA_HOME=str(JDK), PATH=str(JDK / "bin") + os.pathsep + os.environ.get("PATH", ""))


def run(*cmd):
    cmd = [str(c) for c in cmd]
    print(">", " ".join(pathlib.Path(cmd[0]).name if i == 0 else c for i, c in enumerate(cmd)))
    r = subprocess.run(cmd, env=ENV, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if out:
        print(out)
    if r.returncode != 0:
        sys.exit(f"failed ({r.returncode})")


def main():
    for p in (JDK / "bin" / "javac.exe", BT / "aapt2.exe", BT / "d8.bat", ANDROID_JAR):
        if not p.exists():
            sys.exit(f"missing tool: {p}")

    if BUILD.exists():
        shutil.rmtree(BUILD)
    (BUILD / "res").mkdir(parents=True)
    (BUILD / "classes").mkdir()
    (BUILD / "dex").mkdir()
    assets = BUILD / "assets"
    assets.mkdir()
    shutil.copy(PROJECT / "tools" / "ble_control.html", assets / "index.html")

    # 1. 资源
    run(BT / "aapt2.exe", "compile", "--dir", ROOT / "res", "-o", BUILD / "res" / "compiled.zip")
    base = BUILD / "base.apk"
    run(BT / "aapt2.exe", "link", "-o", base,
        "-I", ANDROID_JAR,
        "--manifest", ROOT / "AndroidManifest.xml",
        "--min-sdk-version", MIN_SDK, "--target-sdk-version", TARGET_SDK,
        "--version-code", VERSION_CODE, "--version-name", VERSION_NAME,
        "-A", assets,
        BUILD / "res" / "compiled.zip")

    # 2. Java → class → dex
    srcs = sorted(str(p) for p in (ROOT / "src").rglob("*.java"))
    run(JDK / "bin" / "javac.exe", "-encoding", "UTF-8", "-source", "8", "-target", "8",
        "-Xlint:-options", "-bootclasspath", ANDROID_JAR,
        "-d", BUILD / "classes", *srcs)
    classes = sorted(str(p) for p in (BUILD / "classes").rglob("*.class"))
    run(BT / "d8.bat", "--release", "--min-api", MIN_SDK, "--lib", ANDROID_JAR,
        "--output", BUILD / "dex", *classes)

    # 3. 把 classes.dex 加进 APK（保留原条目的压缩方式，resources.arsc 必须不压缩）
    unsigned = BUILD / "unsigned.apk"
    with zipfile.ZipFile(base) as zin, zipfile.ZipFile(unsigned, "w") as zout:
        for item in zin.infolist():
            zout.writestr(item, zin.read(item.filename), compress_type=item.compress_type)
        zout.write(BUILD / "dex" / "classes.dex", "classes.dex", compress_type=zipfile.ZIP_DEFLATED)

    aligned = BUILD / "aligned.apk"
    run(BT / "zipalign.exe", "-f", "-p", "4", unsigned, aligned)

    # 4. 签名
    if not KEYSTORE.exists():
        KEYSTORE.parent.mkdir(parents=True, exist_ok=True)
        run(JDK / "bin" / "keytool.exe", "-genkeypair", "-keystore", KEYSTORE, "-storetype", "PKCS12",
            "-storepass", KS_PASS, "-keypass", KS_PASS, "-alias", KEY_ALIAS,
            "-keyalg", "RSA", "-keysize", "2048", "-validity", "36500",
            "-dname", "CN=850nm LED Controller, O=Personal, C=CN")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    run(BT / "apksigner.bat", "sign", "--ks", KEYSTORE, "--ks-pass", f"pass:{KS_PASS}",
        "--ks-key-alias", KEY_ALIAS, "--v4-signing-enabled", "false", "--out", OUT, aligned)
    run(BT / "apksigner.bat", "verify", "--min-sdk-version", MIN_SDK, OUT)

    print(f"\nAPK: {OUT}  ({OUT.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
