#pragma once

// Queue one callback on the existing AsyncTCP task. The callback owns context
// only after acceptance. A rejected submission leaves ownership with caller.
extern "C" bool ac_async_tcp_dispatch(void (*callback)(void *), void *context);
