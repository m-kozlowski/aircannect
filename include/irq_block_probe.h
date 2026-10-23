#pragma once

#ifndef AC_IRQ_BLOCK_PROBE_ENABLED
#define AC_IRQ_BLOCK_PROBE_ENABLED 0
#endif

#if AC_IRQ_BLOCK_PROBE_ENABLED
void irq_block_probe_begin();
void irq_block_probe_poll();
#else
inline void irq_block_probe_begin() {}
inline void irq_block_probe_poll() {}
#endif
