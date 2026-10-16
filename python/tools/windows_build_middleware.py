"""Keep PlatformIO middleware semantics with pioarduino 55.03.37 on Windows."""

from platformio.builder.tools.piobuild import AddBuildMiddleware
from platformio.compat import IS_WINDOWS


def register_middleware(env, callback, pattern=None):
    if getattr(callback, "__name__", None) == "integrated_middleware":
        # The upstream wrapper drops filename filters and callback return values.
        # Let PlatformIO run the saved chain, then shorten the resulting source.
        upstream = callback.__globals__
        env["__PIO_BUILD_MIDDLEWARES"] = list(upstream["existing_middlewares"])
        callback = upstream["smart_include_length_shorten"]
        env.RemoveMethod(register_middleware)
        env.AddMethod(AddBuildMiddleware)
        print("[windows_build] Preserving PlatformIO middleware filters and results")

    return AddBuildMiddleware(env, callback, pattern)


if "Import" in globals():
    Import("env")
    if IS_WINDOWS and env.PioPlatform().version == "55.03.37":
        env.AddMethod(register_middleware, "AddBuildMiddleware")
