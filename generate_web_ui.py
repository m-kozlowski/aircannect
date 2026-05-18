"""
Pre-build script: minify + gzip www/index.html -> src/web_ui_html.h.
"""

import gzip
import os
import re

Import("env")


def minify_html(html):
    html = re.sub(r"<!--.*?-->", "", html, flags=re.DOTALL)

    def minify_css(match):
        css = match.group(1)
        css = re.sub(r"/\*.*?\*/", "", css, flags=re.DOTALL)
        css = re.sub(r"\s*([{}:;,>+~])\s*", r"\1", css)
        css = re.sub(r";\}", "}", css)
        css = re.sub(r"\s+", " ", css).strip()
        return "<style>" + css + "</style>"

    def minify_js(match):
        js = match.group(1)
        js = re.sub(r"//[^\n]*", "", js)
        js = re.sub(r"/\*.*?\*/", "", js, flags=re.DOTALL)
        js = "\n".join(line.strip() for line in js.splitlines() if line.strip())
        js = re.sub(r"\s*([{}();,=<>+\-*/?:&|!])\s*", r"\1", js)
        js = re.sub(
            r"\b(const|let|var|return|typeof|new|await|async|function|else)\b(?=[^\s({;,])",
            r"\1 ",
            js,
        )
        return "<script>" + js + "</script>"

    html = re.sub(r"<style>(.*?)</style>", minify_css, html, flags=re.DOTALL)
    html = re.sub(r"<script>(.*?)</script>", minify_js, html, flags=re.DOTALL)
    html = re.sub(r">\s+<", "><", html)
    html = re.sub(r"\s+", " ", html).strip()
    return html


project_dir = env.get("PROJECT_DIR", ".")
html_path = os.path.join(project_dir, "www", "index.html")
header_path = os.path.join(project_dir, "src", "web_ui_html.h")

if os.path.exists(html_path):
    needs_update = (
        not os.path.exists(header_path)
        or os.path.getmtime(html_path) > os.path.getmtime(header_path)
    )
    if needs_update:
        with open(html_path, "r", encoding="utf-8") as f:
            raw = f.read()
        minified = minify_html(raw)
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
            f"{len(minified)} -> {len(compressed)} gz)"
        )
else:
    print(f"[web_ui] WARNING: {html_path} not found")
