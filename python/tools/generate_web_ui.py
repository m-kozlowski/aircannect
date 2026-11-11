"""Pre-build script: minify and gzip the WebUI into a C header."""

import base64
import gzip
import hashlib
import importlib.util
import mimetypes
import os
import re
import json
from pathlib import Path


Import("env")


project_dir = env.get("PROJECT_DIR", ".")
rjs_min_path = os.path.abspath(os.path.join(
    project_dir,
    "third_party",
    "rjsmin",
    "rjsmin.py",
))


def load_js_minifier():
    spec = importlib.util.spec_from_file_location(
        "aircannect_rjsmin",
        rjs_min_path,
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load JavaScript minifier: {rjs_min_path}")

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if module.__version__ != "1.2.5":
        raise RuntimeError(
            f"unsupported rjsmin version {module.__version__}; expected 1.2.5"
        )
    return module._make_jsmin(python_only=True)


js_minify = load_js_minifier()

localization_spec = importlib.util.spec_from_file_location(
    "aircannect_web_localization",
    os.path.join(project_dir, "python/tools/web_localization.py"),
)
localization = importlib.util.module_from_spec(localization_spec)
localization_spec.loader.exec_module(localization)


def inline_assets(html, base_dir):
    languages = {}

    def asset_path(path_text):
        if os.path.isabs(path_text) or ".." in path_text.split("/"):
            raise ValueError(f"unsafe inline asset path: {path_text}")
        path = os.path.join(base_dir, path_text)
        return path

    def read_text_asset(path_text):
        path = asset_path(path_text)
        with open(path, "r", encoding="utf-8") as f:
            return f.read()

    def read_data_uri(path_text):
        path = asset_path(path_text)
        mime, _ = mimetypes.guess_type(path)
        if not mime:
            mime = "application/octet-stream"
        with open(path, "rb") as f:
            encoded = base64.b64encode(f.read()).decode("ascii")
        return f"data:{mime};base64,{encoded}"

    def inline_css(match):
        href = match.group("href")
        return "<style>\n" + read_text_asset(href) + "\n</style>"

    def inline_js(match):
        src = match.group("src")
        source = read_text_asset(src)
        if "/* UI_LANGUAGES */ {}" in source:
            root = Path(base_dir).parent
            catalogs = localization.load_catalogs(root)
            localization.check_language_header(root, catalogs)
            localization.check_source_keys(root, catalogs)
            urls = {}
            for tag, catalog in catalogs.items():
                data = json.dumps(
                    catalog, ensure_ascii=False, separators=(",", ":"), sort_keys=True,
                ).encode("utf-8")
                version = hashlib.sha256(data).hexdigest()[:16]
                url = f"/i18n/{tag}.{version}.json"
                urls[tag] = url
                languages[url] = gzip.compress(data, compresslevel=9, mtime=0)

            source = source.replace(
                "/* UI_LANGUAGES */ {}",
                json.dumps(urls, ensure_ascii=True,
                           separators=(",", ":")).replace("<", "\\u003c"),
            )
        return "<script>\n" + source + "\n</script>"

    def inline_link_asset(match):
        tag = match.group(0)
        href = match.group("href")
        return re.sub(
            r"\bhref=[\"'][^\"']+[\"']",
            'href="' + read_data_uri(href) + '"',
            tag,
            count=1,
            flags=re.IGNORECASE,
        )

    def inline_img(match):
        tag = match.group(0)
        src = match.group("src")
        return re.sub(
            r"\bsrc=[\"'][^\"']+[\"']",
            'src="' + read_data_uri(src) + '"',
            tag,
            count=1,
            flags=re.IGNORECASE,
        )

    html = re.sub(
        r"<link\b(?=[^>]*\bdata-inline\b)(?=[^>]*\brel=[\"']stylesheet[\"'])"
        r"(?=[^>]*\bhref=[\"'](?P<href>[^\"']+)[\"'])[^>]*>",
        inline_css,
        html,
        flags=re.IGNORECASE,
    )
    html = re.sub(
        r"<script\b(?=[^>]*\bdata-inline\b)"
        r"(?=[^>]*\bsrc=[\"'](?P<src>[^\"']+)[\"'])[^>]*>\s*</script>",
        inline_js,
        html,
        flags=re.IGNORECASE,
    )
    html = re.sub(
        r"<link\b(?=[^>]*\bdata-inline\b)(?![^>]*\brel=[\"']stylesheet[\"'])"
        r"(?=[^>]*\bhref=[\"'](?P<href>[^\"']+)[\"'])[^>]*>",
        inline_link_asset,
        html,
        flags=re.IGNORECASE,
    )
    html = re.sub(
        r"<img\b(?=[^>]*\bdata-inline\b)"
        r"(?=[^>]*\bsrc=[\"'](?P<src>[^\"']+)[\"'])[^>]*>",
        inline_img,
        html,
        flags=re.IGNORECASE,
    )
    return html, languages


def minify_html(html):
    html = re.sub(r"<!--.*?-->", "", html, flags=re.DOTALL)
    blocks = []

    def stash(block):
        token = f"@@AC_WEB_BLOCK_{len(blocks)}@@"
        blocks.append(block)
        return token

    def minify_css(match):
        css = match.group(1)
        css = re.sub(r"/\*.*?\*/", "", css, flags=re.DOTALL)
        css = re.sub(r"\s*([{}:;,>+~])\s*", r"\1", css)
        css = re.sub(r";\}", "}", css)
        css = re.sub(r"\s+", " ", css).strip()
        return "<style>" + css + "</style>"

    def minify_js(match):
        js = js_minify(match.group(1))
        return "<script>" + js + "</script>"

    html = re.sub(
        r"<style>(.*?)</style>",
        lambda match: stash(minify_css(match)),
        html,
        flags=re.DOTALL,
    )
    html = re.sub(
        r"<script>(.*?)</script>",
        lambda match: stash(minify_js(match)),
        html,
        flags=re.DOTALL,
    )
    html = re.sub(r">\s+<", "><", html)
    html = re.sub(r"\s+", " ", html).strip()
    for index, block in enumerate(blocks):
        html = html.replace(f"@@AC_WEB_BLOCK_{index}@@", block)
    return html


def byte_array(name, data):
    rows = [f"static const uint8_t {name}[] PROGMEM = {{\n"]
    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        rows.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    return "".join(rows) + "};\n"


html_path = os.path.join(project_dir, "www", "index.html")
web_dir = os.path.dirname(html_path)
header_path = os.path.join(project_dir, "src", "web_ui_html.h")

if os.path.exists(html_path):
    with open(html_path, "r", encoding="utf-8") as f:
        raw = f.read()
    inlined, languages = inline_assets(raw, web_dir)
    minified = minify_html(inlined)
    compressed = gzip.compress(minified.encode("utf-8"), compresslevel=9, mtime=0)
    header = (
        "// Auto-generated from www/index.html, do not edit.\n"
        "#pragma once\n#include <Arduino.h>\n#include <stdint.h>\n\n"
        f"#define HTML_PAGE_GZ_SIZE {len(compressed)}\n\n" +
        byte_array("HTML_PAGE_GZ", compressed)
    )
    if languages:
        for index, data in enumerate(languages.values()):
            header += "\n" + byte_array(f"WEB_LANGUAGE_{index}", data)

        header += (
            "\nstruct WebUiLanguageAsset {\n"
            "    const char *url;\n    const uint8_t *data;\n    size_t size;\n"
            "};\nstatic const WebUiLanguageAsset WEB_UI_LANGUAGES[] = {\n"
        )
        for index, url in enumerate(languages):
            header += (f'    {{"{url}", WEB_LANGUAGE_{index}, '
                       f'sizeof(WEB_LANGUAGE_{index})}},\n')
        header += "};\n"

    output = Path(header_path)
    if not output.exists() or output.read_text(encoding="utf-8") != header:
        output.write_text(header, encoding="utf-8")
        print(
            f"[web_ui] {html_path} ({len(raw)} -> "
            f"{len(inlined)} inline -> {len(minified)} minified -> "
            f"{len(compressed)} gz)"
        )
else:
    print(f"[web_ui] WARNING: {html_path} not found")
