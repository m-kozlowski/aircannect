    (() => {
      "use strict";

      const snapshotChannels = new Map();
      const eventHandlers = new Map();
      const eventBindings = new Set();
      const pageLoaders = new Map();
      const pageLeavers = new Map();
      const actions = new Map();
      const messageTimers = new Map();
      let eventSource = null;
      let actionsStarted = false;

      function snapshotChannel(name) {
        if (!snapshotChannels.has(name)) {
          snapshotChannels.set(name, {
            data: null,
            serial: 0,
            listeners: new Set(),
          });
        }
        return snapshotChannels.get(name);
      }

      function publishSnapshot(name, data) {
        const channel = snapshotChannel(name);
        channel.data = data;
        channel.serial++;
        channel.listeners.forEach((listener) => listener(data, channel.serial));
        return channel.serial;
      }

      function readSnapshot(name) {
        const channel = snapshotChannel(name);
        return {data: channel.data, serial: channel.serial};
      }

      function subscribeSnapshot(name, listener, emitCurrent) {
        const channel = snapshotChannel(name);
        channel.listeners.add(listener);
        if (emitCurrent && channel.data !== null) {
          listener(channel.data, channel.serial);
        }
        return () => channel.listeners.delete(listener);
      }

      function waitForSnapshot(name, predicate, afterSerial, timeoutMs,
                               timeoutMessage) {
        return new Promise((resolve, reject) => {
          let timeout = null;
          let unsubscribe = null;

          const finish = (data, serial) => {
            if (serial <= afterSerial || data === null || !predicate(data)) {
              return false;
            }

            if (unsubscribe) unsubscribe();
            if (timeout !== null) clearTimeout(timeout);
            resolve(data);
            return true;
          };

          const current = readSnapshot(name);
          if (finish(current.data, current.serial)) return;

          unsubscribe = subscribeSnapshot(name, finish, false);
          timeout = setTimeout(() => {
            unsubscribe();
            if (timeoutMessage) {
              reject(new Error(timeoutMessage));
            } else {
              resolve(null);
            }
          }, timeoutMs);
        });
      }

      async function request(url, options) {
        return fetch(url, options);
      }

      async function requestOk(url, options) {
        const response = await request(url, options);
        if (!response.ok) throw new Error(await response.text());
        return response;
      }

      function upload(url, body, options) {
        const settings = options || {};
        return new Promise((resolve, reject) => {
          const xhr = new XMLHttpRequest();
          if (settings.onProgress) xhr.upload.onprogress = settings.onProgress;
          xhr.onload = () => resolve({
            status: xhr.status,
            statusText: xhr.statusText,
            responseText: xhr.responseText,
          });
          xhr.onerror = () => reject(new Error(settings.error || "Upload error"));
          xhr.onabort = () => reject(new Error(settings.aborted || "Upload aborted"));
          xhr.open(settings.method || "POST", url);
          Object.entries(settings.headers || {}).forEach(([key, value]) =>
            xhr.setRequestHeader(key, value));
          xhr.send(body);
        });
      }

      function bindEvent(name) {
        if (!eventSource || eventBindings.has(name)) return;
        eventBindings.add(name);
        eventSource.addEventListener(name, (event) => {
          const entry = eventHandlers.get(name);
          if (!entry) return;

          try {
            const data = entry.raw ? event.data : JSON.parse(event.data);
            if (!entry.raw) publishSnapshot(name, data);
            entry.listeners.forEach((listener) => listener(data, event));
          } catch (error) {
            console.error("SSE event failed", name, error);
          }
        });
      }

      function subscribeEvent(name, listener, options) {
        if (!eventHandlers.has(name)) {
          eventHandlers.set(name, {
            raw: !!(options && options.raw),
            listeners: new Set(),
          });
        }
        const entry = eventHandlers.get(name);
        if (entry.raw !== !!(options && options.raw)) {
          throw new Error("conflicting SSE event type for " + name);
        }
        entry.listeners.add(listener);
        bindEvent(name);
        return () => entry.listeners.delete(listener);
      }

      function startEvents(url) {
        if (!window.EventSource) return;
        if (eventSource) eventSource.close();
        eventBindings.clear();
        eventSource = new EventSource(url || "/api/events");
        eventHandlers.forEach((_entry, name) => bindEvent(name));
      }

      function stopEvents() {
        if (eventSource) eventSource.close();
        eventSource = null;
        eventBindings.clear();
      }

      function registerPageLoader(page, loader) {
        if (!pageLoaders.has(page)) pageLoaders.set(page, new Set());
        pageLoaders.get(page).add(loader);
      }

      function registerPageLeaver(page, leaver) {
        if (!pageLeavers.has(page)) pageLeavers.set(page, new Set());
        pageLeavers.get(page).add(leaver);
      }

      function loadPage(page, refresh) {
        (pageLoaders.get(page) || []).forEach((loader) => loader(!!refresh));
      }

      function leavePage(page) {
        (pageLeavers.get(page) || []).forEach((leaver) => leaver());
      }

      function registerAction(name, handler) {
        if (actions.has(name)) throw new Error("duplicate UI action " + name);
        actions.set(name, handler);
      }

      function invokeAction(name, event, element) {
        const handler = actions.get(name);
        if (!handler) throw new Error("unknown UI action " + name);
        handler(event, element);
      }

      function startActions() {
        if (actionsStarted) return;
        actionsStarted = true;

        document.addEventListener("click", (event) => {
          const element = event.target.closest("[data-action]");
          if (element) invokeAction(element.dataset.action, event, element);
        });
        document.addEventListener("change", (event) => {
          const element = event.target.closest("[data-change-action]");
          if (element) {
            invokeAction(element.dataset.changeAction, event, element);
          }
        });
        document.addEventListener("input", (event) => {
          const element = event.target.closest("[data-input-action]");
          if (element) {
            invokeAction(element.dataset.inputAction, event, element);
          }
        });
        document.addEventListener("keydown", (event) => {
          if (event.key !== "Enter") return;
          const element = event.target.closest("[data-enter-action]");
          if (element) {
            invokeAction(element.dataset.enterAction, event, element);
          }
        });
      }

      function text(id, value) {
        const element = document.getElementById(id);
        if (element) {
          element.textContent =
            value === undefined || value === null || value === "" ? "--" : value;
        }
      }

      function message(id, value, ok, sticky) {
        const element = document.getElementById(id);
        if (!element) return;

        const previous = messageTimers.get(id);
        if (previous) clearTimeout(previous);
        messageTimers.delete(id);

        element.textContent = value;
        element.className = "msg " + (ok ? "ok" : "err");
        if (!sticky) {
          messageTimers.set(id, setTimeout(() => {
            element.className = "msg";
            messageTimers.delete(id);
          }, 5000));
        }
      }

      function clearMessage(id) {
        const element = document.getElementById(id);
        if (!element) return;

        const previous = messageTimers.get(id);
        if (previous) clearTimeout(previous);
        messageTimers.delete(id);
        element.textContent = "";
        element.className = "msg";
      }

      function setControlValue(id, value) {
        const element = document.getElementById(id);
        if (!element || document.activeElement === element) return;
        element.value = value === undefined || value === null ? "" : String(value);
      }

      function row(label, control, small) {
        const element = document.createElement("div");
        element.className = "row";
        const labelElement = document.createElement("label");
        labelElement.textContent = label;
        if (small) {
          const smallElement = document.createElement("small");
          smallElement.textContent = small;
          labelElement.appendChild(smallElement);
        }
        element.appendChild(labelElement);
        element.appendChild(control);
        return element;
      }

      function valueSpan(value) {
        const element = document.createElement("span");
        element.className = "value";
        element.textContent =
          value === undefined || value === null || value === "" ? "--" : value;
        return element;
      }

      function formatBytes(bytes) {
        const value = Number(bytes);
        if (!Number.isFinite(value) || value < 0) return "--";
        if (value < 1024) return Math.round(value) + " B";
        if (value < 1024 * 1024) return (value / 1024).toFixed(1) + " KiB";
        if (value < 1024 * 1024 * 1024) {
          return (value / (1024 * 1024)).toFixed(1) + " MiB";
        }
        return (value / (1024 * 1024 * 1024)).toFixed(2) + " GiB";
      }

      function pad2(value) {
        return String(value).padStart(2, "0");
      }

      function delay(ms) {
        return new Promise((resolve) => setTimeout(resolve, ms));
      }

      window.AirCANnect = Object.freeze({
        actions: Object.freeze({register: registerAction, start: startActions}),
        events: Object.freeze({
          start: startEvents,
          stop: stopEvents,
          subscribe: subscribeEvent,
        }),
        format: Object.freeze({bytes: formatBytes, pad2}),
        http: Object.freeze({request, requestOk, upload}),
        pages: Object.freeze({
          load: loadPage,
          leave: leavePage,
          onLoad: registerPageLoader,
          onLeave: registerPageLeaver,
        }),
        snapshots: Object.freeze({
          publish: publishSnapshot,
          read: readSnapshot,
          subscribe: subscribeSnapshot,
          wait: waitForSnapshot,
        }),
        time: Object.freeze({delay}),
        ui: Object.freeze({
          clearMessage,
          message,
          row,
          setControlValue,
          text,
          valueSpan,
        }),
      });
    })();
