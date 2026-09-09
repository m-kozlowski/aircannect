Place upstream libsmb2 patches here as `*.patch`.

`python/tools/generate_libsmb2.py` uses `python/tools/library_patches.py`
to apply them, in sorted order, to the
generated copy under `.pio/generated-libs/libsmb2`. The
`third_party/libsmb2` submodule stays unmodified.
