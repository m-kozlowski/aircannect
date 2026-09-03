    function liveViewWanted(tabId) {
      return tabId === "dash" && document.visibilityState !== "hidden";
    }

    function postLiveViewState(active, force) {
      const now = Date.now();
      if (!force && liveViewActiveSent === active) {
        if (!active || now - liveViewLastPostMs < liveViewHeartbeatMs) return;
      }
      liveViewActiveSent = active;
      liveViewLastPostMs = now;
      fetch("/api/live/view?id=" + encodeURIComponent(liveViewClientId) +
        "&active=" + (active ? "1" : "0"), {
        method: "POST",
        cache: "no-store",
        keepalive: true,
      }).catch(() => {
        liveViewActiveSent = null;
      });
    }

    function updateLiveViewState(tabId, force) {
      const activeTab = tabId ||
        (document.querySelector(".pane.active") || {}).id || "p-dash";
      const id = activeTab.replace(/^p-/, "");
      postLiveViewState(liveViewWanted(id), force);
    }

    function loadTabContent(id, refresh) {
      if (id === "dash") loadStatus();
      if (id === "report") {
        if (refresh) {
          refreshReportSummary(true);
        } else {
          loadReportSummary(true);
          scheduleReportDrawAfterReveal();
        }
      }
      if (id === "edf") {
        loadStatus();
        loadEdfOverview();
        loadSmbSyncStatus();
        loadSleepHqSyncStatus();
      }
      if (id === "storage") {
        loadStorageList(refresh);
        loadSmbSyncStatus();
      }
      if (id === "oxi") {
        loadStatus();
        loadOximetrySensors();
      }
      if (id === "clinical") loadSettings(refresh);
      if (id === "wifi") loadWifi();
      if (id === "ota") loadOta();
      if (id === "console") {
        loadConsole();
        if (!refresh) {
          setTimeout(() => document.getElementById("consoleInput").focus(), 0);
        }
      }
      if (id === "config") loadConfig();
    }

    function showTab(id) {
      Object.keys(tabs).forEach((tab) => {
        document.getElementById("p-" + tab).classList.toggle("active", tab === id);
      });
      document.querySelectorAll(".nav").forEach((nav) => {
        nav.classList.toggle("active", nav.dataset.tab === id);
      });

      document.getElementById("title").textContent = tabs[id];
      location.hash = id;
      updateLiveViewState(id);
      if (id !== "report") cancelReportRequests();

      loadTabContent(id, false);
    }

    function clinicalTabActive() {
      const pane = document.getElementById("p-clinical");
      return !!pane && pane.classList.contains("active");
    }

    function reportTabActive() {
      const pane = document.getElementById("p-report");
      return !!pane && pane.classList.contains("active");
    }

    function refreshActive() {
      const pane = document.querySelector(".pane.active").id.replace("p-", "");
      updateLiveViewState(pane);
      loadTabContent(pane, true);
    }

    document.addEventListener("visibilitychange", () => updateLiveViewState());
    window.addEventListener("beforeunload", () => {
      if (navigator.sendBeacon) {
        navigator.sendBeacon("/api/live/view?id=" +
          encodeURIComponent(liveViewClientId) + "&active=0", new Blob([]));
      }
    });

    async function api(url, opt) {
      const response = await fetch(url, opt);
      if (!response.ok) throw new Error(await response.text());
      return response;
    }

    const msgTimers = {};

    function msg(id, text, ok, sticky) {
      const element = document.getElementById(id);
      if (!element) return;
      if (msgTimers[id]) {
        clearTimeout(msgTimers[id]);
        msgTimers[id] = null;
      }
      element.textContent = text;
      element.className = "msg " + (ok ? "ok" : "err");
      if (!sticky) {
        msgTimers[id] = setTimeout(() => element.className = "msg", 5000);
      }
    }

    function setControlValue(id, value) {
      const element = document.getElementById(id);
      if (!element || document.activeElement === element) return;
      element.value = value === undefined || value === null ? "" : String(value);
    }

    function up(id, text) {
      const element = document.getElementById(id);
      if (element) {
        element.textContent =
          text === undefined || text === null || text === "" ? "--" : text;
      }
    }

    function apiError(error) {
      up("title", "API unavailable");
      if (location.protocol === "file:") {
        up("wifiTop", "Open device HTTP UI, not file preview");
      } else {
        up("wifiTop", error && error.message ? error.message : "API error");
      }
    }

    function fmtUp(seconds) {
      const days = (seconds / 86400) | 0;
      const hours = ((seconds % 86400) / 3600) | 0;
      const minutes = ((seconds % 3600) / 60) | 0;
      return (days ? days + "d " : "") + hours + "h " + minutes + "m";
    }

    function fmtBytes(bytes) {
      if (!bytes) return "0 B";

      const units = ["B", "KB", "MB", "GB"];
      let index = 0;
      let value = Number(bytes);
      while (value >= 1024 && index < units.length - 1) {
        value /= 1024;
        index++;
      }
      return (value >= 10 || index === 0 ? value.toFixed(0) : value.toFixed(1)) +
        " " + units[index];
    }
