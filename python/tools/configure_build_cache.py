"""Cache intermediate objects, not complete programs and firmware images."""

Import("env")

env.NoCache([
    env.subst("$PROGPATH"),
    env.subst("$BUILD_DIR/${PROGNAME}.bin"),
    env.subst("$BUILD_DIR/${PROGNAME}.factory.bin"),
])
