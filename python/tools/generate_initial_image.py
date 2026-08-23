"""Create the complete first-install image requested by release builds."""

import subprocess


Import("env")


def create_initial_image(source, target, env):
    firmware_path = env.subst("$BUILD_DIR/${PROGNAME}.bin")
    initial_path = str(target[0])
    app_offset = env.subst("$ESP32_APP_OFFSET") or "0x10000"
    mcu = env.BoardConfig().get("build.mcu", "esp32s3")

    command = [
        env.subst("$UPLOADER"),
        "--chip",
        mcu,
        "merge-bin",
        "--output",
        initial_path,
    ]

    for offset, image_path in env.get("FLASH_EXTRA_IMAGES", []):
        command.extend((str(offset), env.subst(image_path)))

    command.extend((app_offset, firmware_path))
    subprocess.run(command, check=True)


firmware_target = env.File("$BUILD_DIR/${PROGNAME}.bin")
initial_sources = [firmware_target]

for _, image_path in env.get("FLASH_EXTRA_IMAGES", []):
    initial_sources.append(env.File(env.subst(image_path)))

initial_target = env.Command(
    "$BUILD_DIR/${PROGNAME}.factory.bin",
    initial_sources,
    env.Action(
        create_initial_image,
        "Building $BUILD_DIR/${PROGNAME}.factory.bin",
    ),
)
env.Alias("initialbin", initial_target)
