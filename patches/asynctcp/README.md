The patches target AsyncTCP 3.4.10. `0001-dispatch-callbacks.patch` adds a
callback event to the existing AsyncTCP task so prepared HTTP responses can
start without waiting for its periodic TCP poll.

`0002-initialize-receive-credit.patch` initializes the deferred receive-credit
counter that upstream 3.4.10 leaves indeterminate. Closing a client credits
only bytes actually deferred by `ackLater()`, and skips the lwIP call when
that count is zero. Real deferred credit is still returned before closure.

`python/tools/extend_asynctcp.py` copies the installed source to staging and
uses `python/tools/library_patches.py` to apply these patches in filename order.
Only the patched copy is compiled; the installed library stays unchanged.
A failed patch stops the build without replacing the last generated source.
