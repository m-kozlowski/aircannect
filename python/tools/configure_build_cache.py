"""Share cached objects, but keep dependency state local to each build."""

import os
import sys

Import("env")

# Run before dependency scanning, and again after platform target naming.
# SConsign stores relative paths and cannot describe two checkouts at once.
env.SConsignFile(os.path.join(
    env.subst("$BUILD_DIR"),
    ".sconsign%d%d" % (sys.version_info[0], sys.version_info[1]),
))

env.NoCache([
    env.subst("$PROGPATH"),
    env.subst("$BUILD_DIR/${PROGNAME}.bin"),
    env.subst("$BUILD_DIR/${PROGNAME}.factory.bin"),
])
