"""
Pre-build script: generate version metadata separately for each environment.
"""

import os
import subprocess

Import("env")


project_dir = env.get("PROJECT_DIR", ".")
script = os.path.join(project_dir, "python", "tools", "version.py")
output_dir = os.path.join(env.subst("$BUILD_DIR"), "generated")
header = os.path.join(output_dir, "version_info_generated.h")

subprocess.run(["python3", script, "--header", header], check=True)
env.Append(CPPPATH=[output_dir])
