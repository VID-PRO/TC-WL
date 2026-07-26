import subprocess
import os
import re

Import("env")


def get_fw_version():
    project_dir = env.subst("$PROJECT_DIR")
    config_h = os.path.join(project_dir, "src", "config.h")
    try:
        with open(config_h) as f:
            for line in f:
                m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', line)
                if m:
                    return m.group(1)
    except OSError:
        pass
    return "?"


def build_android(target, source, env):
    android_dir = os.path.join(env.subst("$PROJECT_DIR"), "android")
    apk_out = os.path.join(android_dir, "app", "build", "outputs", "apk", "release")

    if not os.path.isdir(android_dir):
        print("Android project not found at %s" % android_dir)
        return

    gradlew = os.path.join(android_dir, "gradlew")
    if not os.path.isfile(gradlew):
        print("No gradlew found — run 'gradle wrapper' in the android/ directory first")
        print("  cd android && gradle wrapper")
        return

    print("Building Android app...")

    fw_ver = get_fw_version()
    print("FW_VERSION = %s" % fw_ver)

    java_home = os.environ.get("JAVA_HOME", "")
    gradle_cmd = [gradlew, "assembleRelease", "-PfwVersion=%s" % fw_ver]
    gradle_env = os.environ.copy()
    if java_home:
        gradle_env["JAVA_HOME"] = java_home

    result = subprocess.run(
        gradle_cmd, cwd=android_dir, env=gradle_env,
        capture_output=True, text=True,
    )

    if result.returncode == 0:
        print("Android build succeeded!")
        for root, dirs, files in os.walk(apk_out):
            for f in files:
                if f.endswith(".apk"):
                    print("  APK: %s" % os.path.join(root, f))
    else:
        print("Android build FAILED (exit code %d)" % result.returncode)
        print(result.stdout)
        print(result.stderr)
    return result.returncode


# Phony alias target — SCons won't look for a file named "build_android"
target = env.Alias("build_android", [], [build_android])
env.AlwaysBuild(target)
