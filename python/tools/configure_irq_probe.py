"""Link IRQ timing wrappers only in explicitly enabled diagnostic builds."""


def configure(env):
    definitions = dict(
        item if isinstance(item, (tuple, list)) else (item, 1)
        for item in env["CPPDEFINES"]
    )
    if str(definitions.get("AC_IRQ_BLOCK_PROBE_ENABLED", 0)) != "1":
        return

    env.AppendUnique(LINKFLAGS=[
        f"-Wl,--wrap={symbol}" for symbol in (
            "xPortEnterCriticalTimeout",
            "vPortExitCritical",
            "spi_flash_disable_interrupts_caches_and_other_cpu",
            "spi_flash_enable_interrupts_caches_and_other_cpu",
        )
    ])


if "Import" in globals():
    Import("env")
    configure(env)
