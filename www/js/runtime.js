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
      let activeStorageUpload = null;

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

      async function storageUploadRequest(path, options) {
        const response = await request(path, options || {cache: "no-store"});
        const text = await response.text();
        let data = {};
        try {
          data = text ? JSON.parse(text) : {};
        } catch (_) {
          data = {error: text || ("HTTP " + response.status)};
        }

        if (!response.ok) {
          const error = new Error(data.error || ("HTTP " + response.status));
          error.status = response.status;
          error.data = data;
          throw error;
        }
        return data;
      }

      async function downloadStoragePath(path) {
        for (let attempt = 0; attempt < 400; attempt++) {
          const response = await request("/api/storage/download?path=" +
            encodeURIComponent(path), {cache: "no-store"});
          const text = await response.text();
          if (response.status === 202) {
            await delay(100);
            continue;
          }
          if (!response.ok) {
            throw new Error(responseError(text, response.status));
          }

          const data = JSON.parse(text);
          if (data.state !== "ready" || !Number(data.id)) {
            throw new Error("download_not_ready");
          }

          const link = document.createElement("a");
          link.href = "/api/storage/download?id=" + encodeURIComponent(data.id);
          link.download = data.filename || path.split("/").pop() || "download";
          document.body.appendChild(link);
          link.click();
          link.remove();
          return;
        }
        throw new Error("download_prepare_timeout");
      }

      async function renameStoragePath(base, currentName, title) {
        const requested = window.prompt(
          title || "Rename storage item", currentName);
        if (requested === null) return "";
        const newName = requested.trim();
        if (!newName || newName === currentName) return "";

        const response = await request("/api/storage/rename", {
          method: "POST",
          cache: "no-store",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({base, name: currentName, new_name: newName}),
        });
        const text = await response.text();
        if (!response.ok) {
          throw new Error(responseError(text, response.status));
        }
        return newName;
      }

      function responseError(text, status) {
        try {
          const data = JSON.parse(text);
          if (data && data.error) return data.error;
        } catch (_) {}
        return text || ("HTTP " + status);
      }

      async function storageUploadStatus(id) {
        return storageUploadRequest("/api/storage/upload/status?id=" +
          encodeURIComponent(id), {cache: "no-store"});
      }

      async function cancelStorageUploadSession(id) {
        if (!id) return;
        try {
          await storageUploadRequest("/api/storage/upload/cancel?id=" +
            encodeURIComponent(id), {method: "POST"});
        } catch (_) {}
      }

      function storageUploadRetryMs(status) {
        const requested = Number(status && status.retry_ms);
        if (!Number.isFinite(requested) || requested <= 0) return 500;
        return Math.min(5000, Math.max(100, requested));
      }

      async function waitForStorageUpload(operation, id, predicate,
                                          initialStatus) {
        let status = initialStatus || null;
        for (;;) {
          if (operation.cancelRequested) throw new Error("upload_cancelled");

          if (status) {
            if (status.state === "error" || status.state === "cancelled") {
              throw new Error(status.error || status.state);
            }
            if (predicate(status)) return status;
            await delay(storageUploadRetryMs(status));
          }

          try {
            status = await storageUploadStatus(id);
          } catch (error) {
            status = null;
            if (error.status === 503 && error.message === "status_busy") {
              await delay(250);
              continue;
            }
            if (Number(error.status) > 0) throw error;
            await delay(1000);
          }
        }
      }

      async function startStorageUpload(file, directory, settings) {
        return storageUploadRequest("/api/storage/upload/start", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({
            directory,
            filename: settings.filename || file.name,
            total_size: file.size,
            conflict: settings.conflict || "fail",
          }),
        });
      }

      async function sendStorageUploadChunk(session, file, offset, size) {
        const chunk = file.slice(offset, offset + size);
        const url = "/api/storage/upload/chunk?id=" +
          encodeURIComponent(session.id) + "&offset=" +
          encodeURIComponent(offset) + "&token=" +
          encodeURIComponent(session.token);
        try {
          return await storageUploadRequest(url, {
            method: "POST",
            headers: {"Content-Type": "application/octet-stream"},
            body: chunk,
          });
        } catch (error) {
          if (error.status !== 409 ||
              !["paused", "chunk_pending", "service_busy"].includes(
                error.message)) {
            throw error;
          }
          return null;
        }
      }

      async function uploadStorageFile(operation, file, directory, options) {
        if (activeStorageUpload !== operation || operation.closed) {
          throw new Error("upload_not_active");
        }

        const settings = options || {};
        const progress = settings.progress || (() => {});
        const fileIndex = Number(settings.fileIndex) || 0;
        const fileCount = Math.max(1, Number(settings.fileCount) || 1);
        let conflict = settings.conflict || "fail";

        for (;;) {
          let session = null;
          try {
            session = await startStorageUpload(file, directory,
              Object.assign({}, settings, {conflict}));
            operation.currentId = Number(session.id) || 0;

            let status = await waitForStorageUpload(
              operation,
              session.id,
              (current) => current.state === "ready" ||
                current.state === "done",
              session);
            let committed = Number(status.committed_bytes) || 0;
            const chunkSize = Math.max(
              1, Number(session.chunk_size) || 262144);
            progress(file.name, committed, file.size, fileIndex, fileCount);

            while (status.state !== "done") {
              let accepted = null;
              try {
                accepted = await sendStorageUploadChunk(
                  session,
                  file,
                  committed,
                  Math.min(chunkSize, file.size - committed));
              } catch (error) {
                if (Number(error.status) > 0) throw error;
                status = await waitForStorageUpload(
                  operation,
                  session.id,
                  (current) => current.state === "done" ||
                    current.state === "ready");
                committed = Number(status.committed_bytes) || committed;
                progress(file.name, committed, file.size,
                  fileIndex, fileCount);
                continue;
              }

              if (!accepted) {
                status = await waitForStorageUpload(
                  operation,
                  session.id,
                  (current) => current.state === "done" ||
                    current.state === "ready");
                committed = Number(status.committed_bytes) || committed;
                continue;
              }

              status = await waitForStorageUpload(
                operation,
                session.id,
                (current) => current.state === "done" ||
                  (current.state === "ready" &&
                   Number(current.committed_bytes) > committed),
                accepted);
              committed = Number(status.committed_bytes) || committed;
              progress(file.name, committed, file.size, fileIndex, fileCount);
            }

            operation.currentId = 0;
            return true;
          } catch (error) {
            await cancelStorageUploadSession(
              session ? Number(session.id) || 0 : 0);
            operation.currentId = 0;

            if (error.message === "destination_exists" &&
                conflict === "fail") {
              const confirmReplace = settings.confirmReplace;
              const replace = confirmReplace === false ? false :
                typeof confirmReplace === "function" ?
                  await confirmReplace(file) :
                  window.confirm(file.name +
                    " already exists. Replace it?");
              if (replace) {
                conflict = "replace";
                continue;
              }
              return false;
            }
            throw error;
          }
        }
      }

      function beginStorageUpload() {
        if (activeStorageUpload) return null;

        const operation = {
          cancelRequested: false,
          closed: false,
          currentId: 0,
        };
        activeStorageUpload = operation;

        return Object.freeze({
          get cancelled() {
            return operation.cancelRequested;
          },
          cancel: async () => {
            operation.cancelRequested = true;
            await cancelStorageUploadSession(operation.currentId);
          },
          close: () => {
            if (activeStorageUpload !== operation) return;
            operation.closed = true;
            operation.currentId = 0;
            activeStorageUpload = null;
          },
          file: (file, directory, options) =>
            uploadStorageFile(operation, file, directory, options),
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

      function uploadProgress(prefix, name, committed, total,
                              fileIndex, fileCount) {
        const safeTotal = Math.max(0, Number(total) || 0);
        const safeCommitted = Math.min(safeTotal,
          Math.max(0, Number(committed) || 0));
        const nameNode = document.getElementById(prefix + "Name");
        const amountNode = document.getElementById(prefix + "Amount");
        const bar = document.getElementById(prefix + "Bar");

        if (nameNode) {
          nameNode.textContent = (fileCount > 1 ?
            (fileIndex + 1) + "/" + fileCount + " " : "") + name;
        }
        if (amountNode) {
          amountNode.textContent = formatBytes(safeCommitted) + " / " +
            formatBytes(safeTotal);
        }
        if (bar) {
          bar.max = Math.max(1, safeTotal);
          bar.value = safeCommitted;
        }
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

      function formatDuration(seconds) {
        const value = Number(seconds);
        if (!Number.isFinite(value) || value < 0) return "";
        if (value < 90) return Math.ceil(value) + "s";
        if (value < 3600) return Math.ceil(value / 60) + "m";
        const hours = Math.floor(value / 3600);
        const minutes = Math.ceil((value - hours * 3600) / 60);
        return hours + "h " + minutes + "m";
      }

      function formatModified(value) {
        const seconds = Number(value);
        if (!Number.isFinite(seconds) || seconds <= 0) return "";
        const date = new Date(seconds * 1000);
        if (Number.isNaN(date.getTime())) return "";
        return date.getFullYear() + "-" + pad2(date.getMonth() + 1) + "-" +
          pad2(date.getDate()) + " " + pad2(date.getHours()) + ":" +
          pad2(date.getMinutes());
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
        files: Object.freeze({
          download: downloadStoragePath,
          rename: renameStoragePath,
        }),
        format: Object.freeze({
          bytes: formatBytes,
          duration: formatDuration,
          modified: formatModified,
          pad2,
        }),
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
          uploadProgress,
          valueSpan,
        }),
        uploads: Object.freeze({begin: beginStorageUpload}),
      });
    })();
