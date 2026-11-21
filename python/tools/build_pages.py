"""Assemble the project site from README and the initial images of one release."""

import argparse
from html import escape
import json
import os
from pathlib import Path
import shutil
from string import Template
from urllib.parse import urlsplit
import zipfile

from markdown_it import MarkdownIt

from build_release import DEFAULT_ENVIRONMENTS, PROJECT_DIR, manifest_version, validate_esp32_image


PAGES = PROJECT_DIR / "pages"
BOARD_NAMES = {
    "xiao-esp32s3-plus-sdmmc4": "XIAO S3 Plus · 4-bit SDMMC",
    "xiao-esp32s3-plus-spisd": "XIAO S3 Plus · SPI microSD",
    "waveshare-esp32s3-touch-lcd-1-54": "Waveshare S3 Touch LCD 1.54",
    "waveshare-esp32s3-geek": "Waveshare ESP32-S3-GEEK",
}


def home_markdown(readme: str) -> str:
    blocks = []
    start = None
    lines = readme.splitlines()
    for i, line in enumerate(lines):
        if line.strip() == "<!-- pages:home:start -->":
            if start is not None:
                raise ValueError(f"README:{i+1}: nested pages:home:start")
            start = i + 1
        elif line.strip() == "<!-- pages:home:end -->":
            if start is None:
                raise ValueError(f"README:{i+1}: unmatched pages:home:end")
            block = "\n".join(lines[start:i]).strip()
            if not block:
                raise ValueError(f"README:{i+1}: empty pages:home block")
            blocks.append(block)
            start = None

    if start is not None or not blocks:
        raise ValueError("README: missing or unclosed pages:home block")
    return "\n\n".join(blocks)


def render_home(out: Path, repo: str) -> str:
    parser = MarkdownIt("commonmark", {"html": False})
    tokens = parser.parse(home_markdown((PROJECT_DIR / "README.md").read_text()))
    for block in tokens:
        for token in block.children or ():
            if token.type == "link_open":
                href = token.attrGet("href")
                if not urlsplit(href).scheme and not href.startswith(("#", "//")):
                    token.attrSet("href", f"{repo}/blob/HEAD/{href}")
            elif token.type == "image":
                image = (PROJECT_DIR / token.attrGet("src")).resolve()
                relative = image.relative_to((PROJECT_DIR / "docs").resolve())
                dest = out / "assets" / relative
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(image, dest)
                token.attrSet("src", dest.relative_to(out).as_posix())
    return parser.renderer.render(tokens, parser.options, {})


def build(tag: str, release_dir: Path, vendor: Path, out: Path) -> None:
    if manifest_version(tag) == tag:
        raise ValueError(f"expected a release tag, got {tag!r}")
    repo = "https://github.com/" + os.environ.get("GITHUB_REPOSITORY", "m-kozlowski/aircannect")
    release = f"{repo}/releases/tag/{tag}"
    out.mkdir(parents=True, exist_ok=False)
    assets = out / "assets"
    assets.mkdir()
    shutil.copyfile(PAGES / "site.css", assets / "site.css")
    shutil.copyfile(PROJECT_DIR / "www/img/favicon.png", assets / "favicon.png")

    install = out / "install"
    images = install / tag
    images.mkdir(parents=True)
    shutil.copytree(vendor / "dist/web", install / "esp-web-tools")
    shutil.copyfile(vendor / "LICENSE", install / "esp-web-tools/LICENSE")
    shutil.copyfile(PAGES / "install.js", install / "install.js")

    options = []
    for profile in DEFAULT_ENVIRONMENTS:
        name = BOARD_NAMES[profile]
        stem = f"aircannect-{tag}-{profile}-initial"
        archive = release_dir / f"{stem}.zip"
        binary = images / f"{stem}.bin"
        with zipfile.ZipFile(archive) as z:
            # Read the named member to EOF (ZIP CRC); never extract arbitrary paths.
            with z.open(binary.name) as src, binary.open("wb") as dst:
                shutil.copyfileobj(src, dst)
        validate_esp32_image(binary)

        manifest = {
            "name": f"AirCANnect · {name}", "version": tag,
            "new_install_improv_wait_time": 0,
            "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": stem + ".bin", "offset": 0}]}],
        }
        (images / f"{profile}.json").write_text(json.dumps(manifest, indent=2) + "\n")
        zip_url = f"{repo}/releases/download/{tag}/{archive.name}"
        options.append(f'<option value="{tag}/{profile}.json" data-zip="{zip_url}">{escape(name)}</option>')

    template = Template((PAGES / "layout.html").read_text())
    links = {"tag": escape(tag), "repo": escape(repo), "release": escape(release)}
    home = template.substitute(links, title="AirCANnect", content=render_home(out, repo),
                               page="home", root="./", install_current="")
    (out / "index.html").write_text(home)

    content = Template((PAGES / "install.html").read_text()).substitute(links, options="\n".join(options))
    (install / "index.html").write_text(template.substitute(
        links, title="USB installer · AirCANnect", content=content, page="install",
        root="../", install_current='aria-current="page"'))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--release-dir", type=Path, required=True)
    parser.add_argument("--esp-web-tools", type=Path, required=True, help="unpacked npm package")
    parser.add_argument("--output", type=Path, required=True, help="new artifact directory")
    args = parser.parse_args()
    build(args.tag, args.release_dir, args.esp_web_tools, args.output)
