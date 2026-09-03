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
      AirCANnect.http.request("/api/live/view?id=" + encodeURIComponent(liveViewClientId) +
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

    function apiError(error) {
      AirCANnect.ui.text("title", "API unavailable");
      if (location.protocol === "file:") {
        AirCANnect.ui.text("wifiTop", "Open device HTTP UI, not file preview");
      } else {
        AirCANnect.ui.text(
          "wifiTop", error && error.message ? error.message : "API error");
      }
    }

    function fmtUp(seconds) {
      const days = (seconds / 86400) | 0;
      const hours = ((seconds % 86400) / 3600) | 0;
      const minutes = ((seconds % 3600) / 60) | 0;
      return (days ? days + "d " : "") + hours + "h " + minutes + "m";
    }
