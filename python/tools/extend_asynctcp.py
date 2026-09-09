#!/usr/bin/env python3
"""Build AsyncTCP with a narrow callback event, leaving libdeps unchanged."""

from pathlib import Path


def extend_source(source: str) -> str:
    replacements = (
        ("  LWIP_TCP_DNS\n", "  LWIP_TCP_DNS,\n  AC_ASYNC_CALLBACK\n"),
        ("  AsyncClient *client;\n  union {\n",
         "  AsyncClient *client;\n  union {\n    struct {\n"
         "      void (*run)(void *);\n      void *context;\n"
         "    } callback;\n"),
        ("static inline void _prepend_async_event(",
         '''extern "C" bool ac_async_tcp_dispatch(void (*callback)(void *), void *context) {
  if (!callback || !_async_service_task_handle || !_async_queue_mutex) return false;
  auto *event = new (std::nothrow) lwip_tcp_event_packet_t(AC_ASYNC_CALLBACK, nullptr);
  if (!event) return false;
  event->callback.run = callback;
  event->callback.context = context;
  if (xSemaphoreTake(_async_queue_mutex, 0) != pdTRUE) {
    delete event;
    return false;
  }
  if (_async_queue.size() >= CONFIG_ASYNC_TCP_QUEUE_SIZE) {
    xSemaphoreGive(_async_queue_mutex);
    delete event;
    return false;
  }
  _send_async_event(event);
  xSemaphoreGive(_async_queue_mutex);
  return true;
}

static inline void _prepend_async_event('''),
        ("  if (e->client == NULL) {\n",
         "  if (e->event == AC_ASYNC_CALLBACK) {\n"
         "    e->callback.run(e->callback.context);\n"
         "  } else if (e->client == NULL) {\n"),
    )
    for original, replacement in replacements:
        if source.count(original) != 1:
            raise RuntimeError("AsyncTCP 3.4.10 callback extension no longer matches")
        source = source.replace(original, replacement, 1)
    return "#include <new>\n" + source


def build_source(env, node):
    original = Path(node.srcnode().get_abspath())
    output = Path(env.subst("$BUILD_DIR")) / "generated" / "AsyncTCP.cpp"
    content = extend_source(original.read_text())
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != content:
        output.write_text(content)
    return env.File(str(output))


if "Import" in globals():
    Import("env")
    env.AddBuildMiddleware(build_source, "*/AsyncTCP/src/AsyncTCP.cpp")
