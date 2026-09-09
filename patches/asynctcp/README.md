The patches target AsyncTCP 3.4.10. `0001-dispatch-callbacks.patch` adds a
callback event to the existing AsyncTCP task so prepared HTTP responses can
start without waiting for its periodic TCP poll.

`python/tools/extend_asynctcp.py` copies the installed source to staging and
uses `python/tools/library_patches.py` to apply these patches in filename order.
Only the patched copy is compiled; the installed library stays unchanged.
A failed patch stops the build without replacing the last generated source.
