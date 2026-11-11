    (() => {
      "use strict";

      const snapshotChannels = new Map();
      const eventHandlers = new Map();
      const eventBindings = new Set();
      const fieldWidgets = new Map();
      const pageLoaders = new Map();
      const pageLeavers = new Map();
      const pageListeners = new Set();
      const pageTitles = new Map();
      const startupHandlers = new Set();
      const actions = new Map();
      const messageTimers = new Map();
      let eventSource = null;
      let actionsStarted = false;
      let activePage = "";
      let activeStorageUpload = null;

      const languageSources = /* UI_LANGUAGES */ {};
      const catalogs = {};
      const catalogLoads = new Map();
      const languageListeners = new Set();
      const textBindings = new WeakMap();
      const numberFormats = new Map();
      const translatedAttributes = {
        "data-i18n": "textContent",
        "data-i18n-title": "title",
        "data-i18n-placeholder": "placeholder",
        "data-i18n-aria-label": "aria-label",
      };
      let languagePreference = "auto";
      let requestedLanguage = "";
      let languageRequest = 0;
      let uiLanguage = "en";
      let pluralRules = new Intl.PluralRules(uiLanguage);
      const englishPluralRules = new Intl.PluralRules("en");

      function resolveLanguage(preference) {
        if (Object.hasOwn(languageSources, preference)) return preference;
        const preferences = navigator.languages?.length ?
          navigator.languages : [navigator.language];
        for (const value of preferences) {
          const tag = String(value || "").toLowerCase();
          const supported = Object.keys(languageSources).find(
            (key) => key.toLowerCase() === tag) || tag.split("-")[0];
          if (Object.hasOwn(languageSources, supported)) return supported;
        }
        return "en";
      }

      function localizedNumber(value, maximumFractionDigits = 2,
                               minimumFractionDigits = 0) {
        const key = maximumFractionDigits + ":" + minimumFractionDigits;
        if (!numberFormats.has(key)) {
          numberFormats.set(key, new Intl.NumberFormat(uiLanguage, {
            maximumFractionDigits, minimumFractionDigits,
          }));
        }
        return numberFormats.get(key).format(value);
      }

      function localizedValue(value) {
        if (Array.isArray(value)) return value.map(localizedValue).join("");
        if (value && typeof value === "object" && value.key) {
          return translate(value.key, value.params, value.fallback);
        }
        if (typeof value === "number") return localizedNumber(value);
        return String(value ?? "");
      }

      function translate(key, params = {}, fallback = key) {
        let value = catalogs[uiLanguage]?.[key];
        let rules = pluralRules;
        if (value === undefined) {
          value = catalogs.en?.[key];
          rules = englishPluralRules;
        }
        if (value === undefined) return String(fallback);
        if (typeof value === "object") {
          value = value[rules.select(params.count)] ?? value.other;
        }
        return value.replace(/\{(\w+)\}/g,
          (_match, name) => localizedValue(params[name]));
      }

      function localizedMessage(key, params = {}, fallback) {
        return {key, params, fallback};
      }

      function localizedError(error) {
        if (error && typeof error === "object" && error.key) return error;
        if (error?.ui) return error.ui;
        const code = error?.message ?? error;
        return Object.hasOwn(catalogs.en || {}, "error." + code) ?
          localizedMessage("error." + code) : String(code ?? "");
      }

      function localizedFailure(key, params = {}) {
        const message = key && typeof key === "object" ? key :
          localizedMessage(key, params);
        return Object.assign(new Error(message.key), {
          ui: message,
        });
      }

      function localizedJoin(values, separator = "") {
        const parts = values.flatMap((value, index) =>
          index ? [separator, value] : [value]);
        return localizedMessage("common.parts", {parts});
      }

      function localizedText(target, value, attribute = "textContent") {
        const element = typeof target === "string" ?
          document.getElementById(target) : target;
        if (!element) return;
        const marker = Object.keys(translatedAttributes).find(
          (key) => translatedAttributes[key] === attribute);
        let bindings = textBindings.get(element);
        if (value && typeof value === "object" && value.key) {
          if (!bindings) {
            bindings = {};
            textBindings.set(element, bindings);
          }
          bindings[attribute] = value;
          element.setAttribute(marker, value.key);
        } else {
          if (bindings) delete bindings[attribute];
          if (marker) element.removeAttribute(marker);
        }
        const text = localizedValue(value);
        if (attribute === "textContent") element.textContent = text;
        else element.setAttribute(attribute, text);
      }

      function translateElements(root = document) {
        const selector = Object.keys(translatedAttributes).map(
          (key) => "[" + key + "]").join(",");
        const elements = [...root.querySelectorAll(selector)];
        if (root.matches?.(selector)) elements.unshift(root);
        for (const element of elements) {
          for (const [marker, attribute] of Object.entries(translatedAttributes)) {
            if (!element.hasAttribute(marker)) continue;
            const binding = textBindings.get(element)?.[attribute];
            const text = binding ? localizedValue(binding) :
              translate(element.getAttribute(marker));
            if (attribute === "textContent") element.textContent = text;
            else element.setAttribute(attribute, text);
          }
        }
      }

      async function loadCatalog(language) {
        if (catalogs[language]) return;
        if (!catalogLoads.has(language)) {
          const pending = requestOk(languageSources[language])
            .then((response) => response.json())
            .then((catalog) => { catalogs[language] = catalog; })
            .finally(() => catalogLoads.delete(language));
          catalogLoads.set(language, pending);
        }
        await catalogLoads.get(language);
      }

      async function applyLanguage(preference) {
        languagePreference = preference;
        let language = resolveLanguage(preference);
        if (language === requestedLanguage) return;
        requestedLanguage = language;
        const serial = ++languageRequest;

        try {
          await loadCatalog("en");
          if (serial !== languageRequest) return;
          try {
            await loadCatalog(language);
          } catch (error) {
            console.warn("Could not load UI language", language, error);
            language = "en";
          }
        } catch (error) {
          console.warn("Could not load English UI catalog", error);
          return;
        }

        if (serial !== languageRequest) return;
        uiLanguage = language;
        pluralRules = new Intl.PluralRules(language);
        numberFormats.clear();
        document.documentElement.lang = language;
        translateElements();
        languageListeners.forEach((listener) => listener(language));
      }

      function onLanguageChange(listener) {
        languageListeners.add(listener);
        return () => languageListeners.delete(listener);
      }

      subscribeSnapshot("status", (data) => {
        if (data.ui_language !== undefined) applyLanguage(data.ui_language);
      }, false);
      window.addEventListener("languagechange", () =>
        applyLanguage(languagePreference));

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
              reject(typeof timeoutMessage === "object" ?
                localizedFailure(timeoutMessage) : new Error(timeoutMessage));
            } else {
              resolve(null);
            }
          }, timeoutMs);
        });
      }

      async function request(url, options) {
        return fetch(url, options);
      }

      async function requestBody(url, options) {
        const response = await request(url, options);
        const body = await response.arrayBuffer();
        return {response, body};
      }

      async function requestOk(url, options) {
        const response = await request(url, options);
        if (!response.ok) throw new Error(await response.text());
        return response;
      }

      async function postJson(url, body) {
        const response = await requestOk(url, {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify(body),
        });

        return await response.json();
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
          localizedValue(title || localizedMessage("storage.rename_prompt")),
          currentName);
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
                  window.confirm(translate("storage.replace_prompt",
                    {name: file.name}));
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

      function definePages(pages) {
        Object.entries(pages).forEach(([id, title]) =>
          pageTitles.set(id, title));
      }

      function showPage(page) {
        if (!pageTitles.has(page)) return false;
        const previous = activePage;
        if (previous && previous !== page) leavePage(previous);

        document.querySelectorAll(".pane").forEach((pane) => {
          pane.classList.toggle("active", pane.id === "p-" + page);
        });
        document.querySelectorAll(".nav").forEach((nav) => {
          nav.classList.toggle("active", nav.dataset.tab === page);
        });

        activePage = page;
        const title = document.getElementById("title");
        if (title) localizedText(title, pageTitles.get(page));
        location.hash = page;
        pageListeners.forEach((listener) => listener(page, previous));
        loadPage(page, false);
        return true;
      }

      function activePageId() {
        return activePage;
      }

      function pageIsActive(page) {
        return activePage === page;
      }

      function onPageChange(listener) {
        pageListeners.add(listener);
        return () => pageListeners.delete(listener);
      }

      function refreshPage() {
        if (activePage) loadPage(activePage, true);
      }

      function registerStartup(handler) {
        startupHandlers.add(handler);
      }

      function runStartup() {
        startupHandlers.forEach((handler) => handler());
      }

      function registerAction(name, handler) {
        if (actions.has(name)) throw new Error("duplicate UI action " + name);
        actions.set(name, handler);
      }

      function registerFieldWidget(key, widget) {
        if (fieldWidgets.has(key)) {
          throw new Error("duplicate config field widget " + key);
        }
        fieldWidgets.set(key, Object.freeze(widget));
      }

      function decorateField(key, control) {
        const widget = fieldWidgets.get(key);
        return widget && widget.decorate ? widget.decorate(control) : control;
      }

      function defaultFieldValue(key, control) {
        const widget = fieldWidgets.get(key);
        if (widget && widget.setDefault) widget.setDefault(control);
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
          localizedText(element,
            value === undefined || value === null || value === "" ? "--" : value);
        }
      }

      function message(id, value, ok, sticky) {
        const element = document.getElementById(id);
        if (!element) return;

        const previous = messageTimers.get(id);
        if (previous) clearTimeout(previous);
        messageTimers.delete(id);

        localizedText(element, localizedError(value));
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
        localizedText(element, "");
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
        const caption = document.createElement("span");
        localizedText(caption, label);
        labelElement.appendChild(caption);
        if (small) {
          const smallElement = document.createElement("small");
          localizedText(smallElement, small);
          labelElement.appendChild(smallElement);
        }
        element.appendChild(labelElement);
        element.appendChild(control);
        return element;
      }

      function valueSpan(value) {
        const element = document.createElement("span");
        element.className = "value";
        localizedText(element,
          value === undefined || value === null || value === "" ? "--" : value);
        return element;
      }

      function wifiSignalLevel(rssi) {
        const value = Number(rssi);
        if (!Number.isFinite(value) || value >= 0) return 0;
        if (value >= -55) return 4;
        if (value >= -65) return 3;
        if (value >= -75) return 2;
        return 1;
      }

      function formatRssi(rssi) {
        const value = Number(rssi);
        return Number.isFinite(value) && value < 0 ?
          Math.round(value) + " dBm" : "--";
      }

      function wifiSignal(rssi, label) {
        const value = document.createElement("span");
        value.className = "value wifi-signal";

        const bars = document.createElement("span");
        bars.className = "wifi-bars";
        bars.title = formatRssi(rssi);
        const level = wifiSignalLevel(rssi);
        for (let index = 1; index <= 4; index++) {
          const bar = document.createElement("span");
          if (index <= level) bar.className = "on";
          bars.appendChild(bar);
        }
        value.appendChild(bars);

        const text = document.createElement("span");
        text.textContent = label || formatRssi(rssi);
        value.appendChild(text);
        return value;
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
        i18n: Object.freeze({
          t: translate,
          format: localizedValue,
          message: localizedMessage,
          text: localizedText,
          error: localizedError,
          failure: localizedFailure,
          join: localizedJoin,
          number: localizedNumber,
          translate: translateElements,
          onChange: onLanguageChange,
          language: () => uiLanguage,
        }),
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
        forms: Object.freeze({
          decorate: decorateField,
          register: registerFieldWidget,
          setDefault: defaultFieldValue,
        }),
        format: Object.freeze({
          bytes: formatBytes,
          duration: formatDuration,
          modified: formatModified,
          pad2,
          rssi: formatRssi,
        }),
        http: Object.freeze({request, requestBody, requestOk, postJson, upload, responseError}),
        pages: Object.freeze({
          active: activePageId,
          define: definePages,
          isActive: pageIsActive,
          load: loadPage,
          leave: leavePage,
          onChange: onPageChange,
          onLoad: registerPageLoader,
          onLeave: registerPageLeaver,
          refresh: refreshPage,
          show: showPage,
        }),
        snapshots: Object.freeze({
          publish: publishSnapshot,
          read: readSnapshot,
          subscribe: subscribeSnapshot,
          wait: waitForSnapshot,
        }),
        time: Object.freeze({delay}),
        startup: Object.freeze({register: registerStartup, run: runStartup}),
        ui: Object.freeze({
          clearMessage,
          message,
          row,
          setControlValue,
          text,
          uploadProgress,
          valueSpan,
          wifiSignal,
        }),
        uploads: Object.freeze({begin: beginStorageUpload}),
      });

      applyLanguage("auto");
    })();
