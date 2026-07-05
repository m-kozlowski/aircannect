"""
Pre-build script: minify + gzip www/index.html -> src/web_ui_html.h.
"""

import gzip
import base64
import mimetypes
import os
import re

Import("env")


def inline_assets(html, base_dir):
    deps = []

    def asset_path(path_text):
        if os.path.isabs(path_text) or ".." in path_text.split("/"):
            raise ValueError(f"unsafe inline asset path: {path_text}")
        path = os.path.join(base_dir, path_text)
        deps.append(path)
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
        return "<script>\n" + read_text_asset(src) + "\n</script>"

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
    return html, deps


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
        js = match.group(1).strip()
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


project_dir = env.get("PROJECT_DIR", ".")
html_path = os.path.join(project_dir, "www", "index.html")
web_dir = os.path.dirname(html_path)
header_path = os.path.join(project_dir, "src", "web_ui_html.h")
generator_path = os.path.join(project_dir, "generate_web_ui.py")

if os.path.exists(html_path):
    with open(html_path, "r", encoding="utf-8") as f:
        raw = f.read()
    inlined, deps = inline_assets(raw, web_dir)
    needs_update = (
        not os.path.exists(header_path)
        or os.path.getmtime(html_path) > os.path.getmtime(header_path)
        or os.path.getmtime(generator_path) > os.path.getmtime(header_path)
        or any(os.path.getmtime(dep) > os.path.getmtime(header_path)
               for dep in deps)
    )
    if needs_update:
        minified = minify_html(inlined)
        compressed = gzip.compress(minified.encode("utf-8"), compresslevel=9)
        with open(header_path, "w", encoding="utf-8") as f:
            f.write("// Auto-generated from www/index.html, do not edit.\n")
            f.write("#pragma once\n")
            f.write("#include <Arduino.h>\n")
            f.write("#include <stdint.h>\n\n")
            f.write(f"#define HTML_PAGE_GZ_SIZE {len(compressed)}\n\n")
            f.write("static const uint8_t HTML_PAGE_GZ[] PROGMEM = {\n")
            for i in range(0, len(compressed), 16):
                chunk = compressed[i : i + 16]
                f.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
            f.write("};\n")
        print(
            f"[web_ui] {html_path} ({len(raw)} -> "
            f"{len(inlined)} inline -> {len(minified)} -> "
            f"{len(compressed)} gz)"
        )
else:
    print(f"[web_ui] WARNING: {html_path} not found")
