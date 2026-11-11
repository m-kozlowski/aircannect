# ESPAsyncWebServer SSE patch

For ESPAsyncWebServer 3.11.0, ported from AirBridge commit `d940af19`.

Finish sending the current event before starting the next one. TCP send space
can grow between writes as LwIP processes ACKs; advancing the queue after a
partial write can otherwise interleave two events.

`python/tools/extend_asynctcp.py` applies the patch to a generated source copy.
The installed library, event queue limits, and ACK accounting are unchanged.
