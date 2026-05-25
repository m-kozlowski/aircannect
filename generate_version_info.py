"""
Pre-build script: write src/version_info.h without changing global flags.
"""

import os
import subprocess

Import("env")


project_dir = env.get("PROJECT_DIR", ".")
script = os.path.join(project_dir, "version.py")
header = os.path.join(project_dir, "src", "version_info.h")

subprocess.run(["python3", script, "--header", header], check=True)
