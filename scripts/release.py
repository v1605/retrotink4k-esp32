Import("env")

import os
import shutil
import subprocess
import sys
import urllib.error
import urllib.request

from SCons.Script import COMMAND_LINE_TARGETS

FIRMWARE = "$BUILD_DIR/${PROGNAME}.bin"
FS_IMAGE = "$BUILD_DIR/${ESP32_FS_IMAGE_NAME}.bin"
PLATFORM_FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}
PACKAGE_TARGETS = {"release", "ota"}


def fail(env, message):
    sys.stderr.write(message + "\n")
    env.Exit(1)


def build_web(target, source, env):
    if not shutil.which("npm"):
        fail(env, "npm not found on PATH; it's needed to build web/ into the filesystem image")
    subprocess.check_call(["npm", "run", "build"], cwd=os.path.join(env.subst("$PROJECT_DIR"), "web"))


def ota_enabled(env):
    names = [d[0] if isinstance(d, (list, tuple)) else str(d).split("=")[0] for d in env.get("CPPDEFINES", [])]
    return "OTA_ENABLED" in names


# Format parsed by handleCombinedUploadBody() in src/web/ota_endpoints.cpp.
def build_ota_package(env):
    with open(env.subst(FS_IMAGE), "rb") as f:
        filesystem = f.read()
    with open(env.subst(FIRMWARE), "rb") as f:
        firmware = f.read()

    path = env.subst("$BUILD_DIR/${PIOENV}-ota-update.bin")
    with open(path, "wb") as out:
        out.write(b"RT4O")
        out.write(bytes([1]))
        out.write(len(filesystem).to_bytes(4, "little"))
        out.write(len(firmware).to_bytes(4, "little"))
        out.write(filesystem)
        out.write(firmware)
    return path


def merge_bin(env, output, pieces):
    pieces = sorted(pieces)
    cmd = [env.subst("$PYTHONEXE"), "-m", "esptool", "--chip", env.BoardConfig().get("build.mcu"),
           "merge-bin", "-o", output, "--target-offset", hex(pieces[0][0])]
    for offset, path in pieces:
        cmd += [hex(offset), path]
    subprocess.check_call(cmd)
    return pieces[0][0]


def build_release(target, source, env):
    app_offset = int(env.subst("$ESP32_APP_OFFSET"), 0)

    # Bootloader and partition table go in the one-time image; boot_app0
    # (initial otadata) belongs with the app it points at.
    init_pieces = []
    update_pieces = [(app_offset, env.subst(FIRMWARE)), (env["FS_START"], env.subst(FS_IMAGE))]
    for offset, path in env.get("FLASH_EXTRA_IMAGES", []):
        piece = (int(env.subst(offset), 0), env.subst(path))
        (update_pieces if os.path.basename(piece[1]) == "boot_app0.bin" else init_pieces).append(piece)

    init_bin = env.subst("$BUILD_DIR/${PIOENV}-blank-board-init.bin")
    update_bin = env.subst("$BUILD_DIR/${PIOENV}-update.bin")
    init_offset = merge_bin(env, init_bin, init_pieces)
    update_offset = merge_bin(env, update_bin, update_pieces)

    chip = env.BoardConfig().get("build.mcu")
    print("")
    print("release: %s  (updates a running board)" % update_bin)
    print("release: %s  (first flash of a blank board only)" % init_bin)
    print("release: blank board:   esptool --chip %s write-flash %s %s %s %s"
          % (chip, hex(init_offset), init_bin, hex(update_offset), update_bin))
    print("release: running board: esptool --chip %s write-flash %s %s" % (chip, hex(update_offset), update_bin))

    if ota_enabled(env):
        package = build_ota_package(env)
        print("release: %s  (OTA: POST to /api/ota/upload, or `pio run -t ota`)" % package)


def deploy_ota(target, source, env):
    if not ota_enabled(env):
        fail(env, "ota: [env:%s] isn't built with OTA_ENABLED" % env["PIOENV"])

    host = os.environ.get("OTA_HOST") or env.GetProjectOption("custom_ota_host", "")
    if not host:
        fail(env, "ota: set custom_ota_host in platformio.ini, or OTA_HOST")

    package = build_ota_package(env)
    with open(package, "rb") as f:
        data = f.read()

    url = "http://%s/api/ota/upload" % host
    print("ota: uploading %s (%d bytes) to %s" % (package, len(data), url))

    # application/octet-stream is required; see /api/ota/upload in README.md.
    request = urllib.request.Request(url, data=data, method="POST",
                                     headers={"Content-Type": "application/octet-stream"})
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            print("ota: %s %s" % (response.status, response.read().decode(errors="replace")))
    except urllib.error.HTTPError as e:
        fail(env, "ota: upload rejected (HTTP %s): %s" % (e.code, e.read().decode(errors="replace")))
    except urllib.error.URLError as e:
        fail(env, "ota: couldn't reach %s (%s)" % (url, e.reason))

    print("ota: uploaded; the device is flashing and will reboot")


# The platform only defines the filesystem image when one of its own FS
# targets is requested, so define it with the same builder for ours.
requested = set(COMMAND_LINE_TARGETS)
if requested & PACKAGE_TARGETS and not requested & PLATFORM_FS_TARGETS:
    env.AlwaysBuild(env.DataToBin("$BUILD_DIR/${ESP32_FS_IMAGE_NAME}", "$PROJECT_DATA_DIR"))

# Every filesystem image gets a freshly built web UI.
if requested & (PACKAGE_TARGETS | PLATFORM_FS_TARGETS):
    env.AddPreAction(FS_IMAGE, env.VerboseAction(build_web, "Building web UI"))

env.AddCustomTarget(
    name="release",
    dependencies=[FIRMWARE, FS_IMAGE],
    actions=[build_release],
    title="Build release images",
    description="Blank-board and update images for esptool, plus the OTA package when OTA_ENABLED",
)

env.AddCustomTarget(
    name="ota",
    dependencies=[FIRMWARE, FS_IMAGE],
    actions=[deploy_ota],
    title="Deploy over OTA",
    description="Build firmware and web UI and upload them to custom_ota_host",
)
