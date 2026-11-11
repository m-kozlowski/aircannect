(() => {
    "use strict";

    let statusData = {};
    let statusLoadPromise = null;
    let oxiSensorData = {sensor_scan_results: [], sensor_known: []};
    let oxiSensorsLoading = false;

    const LIVE_FAST_POINTS = 6000;
    const LIVE_MEDIUM_POINTS = 3000;
    const LIVE_SLOW_POINTS = 1800;
    const liveData = {
      pressure: [],
      flow: [],
      leak: [],
      inspPressure: [],
      expPressure: [],
      spo2: [],
      pulse: [],
    };
    const chartScales = {
      pressure: {min: 0, max: 15, step: 5},
      flow: {min: -40, max: 40, step: 10, symmetric: true},
      leak: {min: 0, max: 20, step: 5},
      therapyPressure: {min: 0, max: 15, step: 5},
    };

    function setPageTitle(hostname) {
      const clean = String(hostname || "").trim();
      document.title = clean ? "AirCANnect - " + clean : "AirCANnect";
    }

    function apiError(error) {
      AirCANnect.ui.text("title", AirCANnect.i18n.message("status.api_unavailable"));
      if (location.protocol === "file:") {
        AirCANnect.ui.text("wifiTop", AirCANnect.i18n.message("status.file_preview"));
      } else {
        AirCANnect.ui.text(
          "wifiTop", error ? AirCANnect.i18n.error(error) :
            AirCANnect.i18n.message("status.api_error"));
      }
    }

    function fmtUp(seconds) {
      const days = (seconds / 86400) | 0;
      const hours = ((seconds % 86400) / 3600) | 0;
      const minutes = ((seconds % 3600) / 60) | 0;
      return (days ? days + "d " : "") + hours + "h " + minutes + "m";
    }

    function setWifiTop(data) {
      const element = document.getElementById("wifiTop");
      if (!element) return;
      AirCANnect.i18n.text(element, "");

      const state = data && data.wifi_state ? String(data.wifi_state) : "--";
      const ssid = data && data.wifi_ssid ? String(data.wifi_ssid) : "";
      const softap = !!(data && data.wifi_softap);
      const rssi = data && data.wifi_rssi;
      const staConnected = state === "sta" || state === "sta_ap";
      let connection = staConnected && ssid ? ssid : state;
      if (state === "softap") connection = "SoftAP";
      if (softap && state !== "softap") connection += " +AP";

      const wrapper = AirCANnect.ui.wifiSignal(
        rssi, "WiFi: " + connection);
      wrapper.classList.remove("value");
      const tooltip = AirCANnect.i18n.message("status.wifi_hint", {
        connection, rssi: AirCANnect.format.rssi(rssi),
        ip: staConnected && data.wifi_ip && data.wifi_ip !== "0.0.0.0" ?
          "\nSTA IP: " + data.wifi_ip : "",
      });
      AirCANnect.i18n.text(wrapper, tooltip, "title");
      AirCANnect.i18n.text(wrapper.querySelector(".wifi-bars"), tooltip, "title");
      element.appendChild(wrapper);
    }

    function fmtIsoMinute(value, ageMs) {
      value = String(value || "");
      if (!value) return "";

      const date = new Date(value);
      if (Number.isNaN(date.getTime())) return value;
      if (Number.isFinite(ageMs) && ageMs > 0) {
        date.setTime(date.getTime() + ageMs);
      }

      let timezone = "";
      try {
        const part = new Intl.DateTimeFormat(undefined, {
          timeZoneName: "short",
        }).formatToParts(date).find((entry) => entry.type === "timeZoneName");
        timezone = part && part.value ? " " + part.value : "";
      } catch (error) {
        timezone = "";
      }

      return date.getFullYear() + "-" +
        AirCANnect.format.pad2(date.getMonth() + 1) + "-" +
        AirCANnect.format.pad2(date.getDate()) + " " +
        AirCANnect.format.pad2(date.getHours()) + ":" +
        AirCANnect.format.pad2(date.getMinutes()) +
        timezone;
    }

    function fmtFirmware(value) {
      value = String(value || "");
      const match = value.match(/^[A-Z]{2}\d+\.(\d+\.\d+\.\d+\.\d+)/);
      return match ? match[1] : value;
    }

    function fmtProfile(value) {
      return String(value || "")
        .replace(/[_-]/g, " ")
        .replace(/\s*Profile$/i, "")
        .trim();
    }

    function fmtTherapy(value) {
      if (value === "running") return AirCANnect.i18n.message("ui.therapy");
      if (value === "standby") return AirCANnect.i18n.message("ui.standby");
      if (value === "unknown") return AirCANnect.i18n.message("ui.unknown");
      return value || AirCANnect.i18n.message("ui.unknown");
    }

    function fmtSync(data) {
      if (!data.esp_time_valid) return AirCANnect.i18n.message("status.clock_invalid");

      let source = data.esp_time_source === "ntp" ? "NTP" :
        data.esp_time_source === "resmed" ? "ResMed" : AirCANnect.i18n.message("status.clock");
      if (data.ntp_synced) source = "NTP";
      return AirCANnect.i18n.join([source, data.resmed_time_sync_enabled && data.resmed_time_write_supported ? " -> ResMed" : ""]);
    }

    function renderStorageStatus(data) {
      const summary = document.getElementById("storageSummary");
      const meter = document.getElementById("storageMeter");
      if (!summary || !meter) return;

      let text = data.storage_state || AirCANnect.i18n.message("status.not_mounted");
      let percent = 0;
      const total = Number(data.storage_total || 0);
      const used = Number(data.storage_used || 0);
      const free = Math.max(0, total - used);
      if (data.storage_state === "mounted" && total > 0) {
        text = AirCANnect.i18n.join([
          AirCANnect.i18n.bytes(free), " / ", AirCANnect.i18n.bytes(total),
        ]);
        percent = Math.max(0, Math.min(100, used / total * 100));
      }

      AirCANnect.i18n.text(summary, text);
      meter.style.width = percent.toFixed(1) + "%";
      AirCANnect.i18n.text(meter, AirCANnect.i18n.message("status.used", {percent: Math.round(percent * 10) / 10}), "title");
    }

    function renderSystemIssues(data) {
      const free = Math.max(0, Number(data.storage_total || 0) -
        Number(data.storage_used || 0));
      const recording = data.edf || {};
      const issues = [
        {tab: "storage", active: !!data.storage_low_space,
          text: AirCANnect.i18n.message("status.low_space", {free: AirCANnect.i18n.bytes(free)})},
        {tab: "edf", active: !!recording.storage_blocked,
          text: AirCANnect.i18n.message("status.edf_suspended"), detail: recording.storage_error || ""},
      ];

      const list = document.getElementById("systemIssues");
      issues.forEach((issue) => {
        document.querySelectorAll('.nav[data-tab="' + issue.tab + '"]')
          .forEach((link) => {
            link.toggleAttribute("data-fault", issue.active);
            AirCANnect.i18n.text(link, issue.active ? issue.text : "", "title");
          });

        const notice = document.getElementById(issue.tab + "Fault");
        if (notice) {
          notice.hidden = !issue.active;
          const text = issue.active ? AirCANnect.i18n.join([issue.text, issue.detail ? ": " + issue.detail : ""]) : "";
          if (notice.textContent !== AirCANnect.i18n.format(text)) {
            AirCANnect.i18n.text(notice, text);
          }
        }
        if (!list) return;

        let item = list.querySelector('[data-subsystem="' + issue.tab + '"]');
        if (!issue.active) {
          if (item) item.remove();
          return;
        }

        if (!item) {
          item = document.createElement("li");
          item.dataset.subsystem = issue.tab;
          const link = document.createElement("a");
          link.className = "badge warn subsystem-issue";
          link.href = "#" + issue.tab;
          link.dataset.action = "app.show-tab";
          link.dataset.value = issue.tab;
          item.appendChild(link);
          list.appendChild(item);
        }
        const link = item.firstElementChild;
        if (link.textContent !== AirCANnect.i18n.format(issue.text)) {
          AirCANnect.i18n.text(link, issue.text);
        }
      });
      if (list) list.hidden = !list.childElementCount;
    }

    function renderOximetryRuntime(data) {
      const oxi = data && data.oximetry ? data.oximetry : {};
      const sourceValid = !!(oxi.source_fresh && oxi.valid);
      AirCANnect.ui.text("oxiSpo2", sourceValid ? oxi.spo2 : "--");
      AirCANnect.ui.text("oxiPulse", sourceValid ? oxi.pulse_bpm : "--");
      AirCANnect.ui.text("oxiTabSpo2", sourceValid ? oxi.spo2 : "--");
      AirCANnect.ui.text("oxiTabPulse", sourceValid ? oxi.pulse_bpm : "--");

      let badgeLabel = AirCANnect.i18n.message("ui.off");
      let badgeStyle = "";
      if (!oxi.enabled) {
        badgeLabel = AirCANnect.i18n.message("ui.off");
      } else if (!oxi.ble_available) {
        badgeLabel = AirCANnect.i18n.message("status.ble_off");
        badgeStyle = "bad";
      } else if (oxi.subscribed) {
        badgeLabel = AirCANnect.i18n.message("status.recording");
        badgeStyle = "good";
      } else if (oxi.connected) {
        badgeLabel = AirCANnect.i18n.message("status.connected");
        badgeStyle = "warn";
      } else if (oxi.pairing_active) {
        badgeLabel = AirCANnect.i18n.message("status.pairing");
        badgeStyle = "warn";
      } else if (oxi.advertising) {
        badgeLabel = AirCANnect.i18n.message("ui.advertising");
        badgeStyle = "warn";
      } else if (sourceValid) {
        badgeLabel = AirCANnect.i18n.message("status.ready");
        badgeStyle = "good";
      } else {
        badgeLabel = AirCANnect.i18n.message("status.idle");
      }
      ["oxiBadge", "oxiTabBadge"].forEach((id) => {
        const badge = document.getElementById(id);
        if (!badge) return;
        AirCANnect.i18n.text(badge, badgeLabel);
        badge.className = "badge " + badgeStyle;
      });

      const sourceKind = oxi.source && oxi.source !== "none" ?
        oxi.source.toUpperCase() : "none";
      const sourceDetail = (oxi.source_detail || "").trim();
      const sourceIdentity = sourceKind === "none" ? "none" :
        sourceKind + (sourceDetail ? " " + sourceDetail : "");
      let sourceStatus = AirCANnect.i18n.message("status.no_source");
      if (!oxi.enabled) {
        sourceStatus = AirCANnect.i18n.message("ui.off");
      } else if (oxi.source_present && oxi.source_fresh && oxi.valid) {
        sourceStatus = AirCANnect.i18n.message("state.fresh");
      } else if (oxi.source_present && oxi.source_fresh &&
          oxi.contact_known && !oxi.contact_present) {
        sourceStatus = AirCANnect.i18n.message("status.no_finger");
      } else if (oxi.source_present && oxi.source_fresh) {
        sourceStatus = AirCANnect.i18n.message("state.invalid");
      } else if (oxi.source_present) {
        sourceStatus = AirCANnect.i18n.message("state.stale");
      }
      AirCANnect.ui.text("oxiSource", sourceIdentity === "none" ?
        sourceStatus : AirCANnect.i18n.join([sourceIdentity, sourceStatus], " "));
      AirCANnect.ui.text("oxiTabSource", sourceIdentity);
      AirCANnect.ui.text("oxiTabStatus", sourceStatus);

      const integrationAvailable =
        oxi.airsense_integration_available !== false;
      const as11 = !integrationAvailable ? AirCANnect.i18n.message("status.local_sa2") :
        oxi.subscribed ? AirCANnect.i18n.message("state.subscribed") :
          oxi.connected ? AirCANnect.i18n.message("status.connected") : AirCANnect.i18n.message("status.not_connected");
      AirCANnect.ui.text("oxiAs11", as11);
      AirCANnect.ui.text("oxiTabAs11", as11);

      let advertiseState = "idle";
      if (!integrationAvailable) {
        advertiseState = "disabled";
      } else if (!oxi.enabled) {
        advertiseState = "off";
      } else if (oxi.subscribed || oxi.connected) {
        advertiseState = "connected";
      } else if (oxi.advertising) {
        advertiseState = "advertising";
      } else if (oxi.manual_advertising_requested) {
        advertiseState = "requested";
      }
      let advertise = AirCANnect.i18n.join([oxi.advertise_mode || "--", " / ",
        AirCANnect.i18n.message("state." + advertiseState, {}, advertiseState)]);
      if (oxi.pairing_active) {
        advertise = AirCANnect.i18n.message("status.pairing_left", {
          advertise, seconds: Math.ceil((oxi.pairing_left_ms || 0) / 1000),
        });
      }
      AirCANnect.ui.text("oxiAdvertise", advertise);
      AirCANnect.ui.text("oxiTabAdvertise", advertise);

      AirCANnect.ui.text("oxiTabBleName", oxi.ble_name || "--");
      AirCANnect.ui.text("oxiTabPeer", oxi.ble_peer || "--");

      AirCANnect.ui.setControlValue("oxiAdvertiseMode", oxi.advertise_mode || "auto");

      const pair = document.getElementById("oxiPairBtn");
      const advertiseMode = document.getElementById("oxiAdvertiseMode");
      const advStart = document.getElementById("oxiAdvStartBtn");
      const advStop = document.getElementById("oxiAdvStopBtn");
      const forget = document.getElementById("oxiForgetBtn");
      if (pair) {
        AirCANnect.i18n.text(pair, oxi.pairing_active ? AirCANnect.i18n.message("status.stop_pairing") : AirCANnect.i18n.message("ui.pair.airsense"));
        pair.disabled = !oxi.ble_available || !integrationAvailable;
      }
      if (advertiseMode) advertiseMode.disabled = !integrationAvailable;
      if (forget) forget.disabled = !integrationAvailable;
      const manual = oxi.advertise_mode === "manual";
      if (advStart) {
        advStart.style.display = manual ? "" : "none";
        advStart.disabled = !integrationAvailable || !oxi.enabled ||
          oxi.advertising;
      }
      if (advStop) {
        advStop.style.display = manual ? "" : "none";
        advStop.disabled = !integrationAvailable ||
          (!oxi.advertising && !oxi.manual_advertising_requested);
      }

    }

    function sensorDisplayName(device) {
      if (!device) return "--";
      const name = (device.name || "").trim();
      const addr = (device.addr || "").trim();
      return name || addr || "--";
    }

    function sensorMeta(device) {
      if (!device) return "";
      const parts = [];
      if (device.addr) parts.push(device.addr);
      if (Number.isFinite(Number(device.rssi)) && Number(device.rssi) !== 0) {
        parts.push(Number(device.rssi) + " dBm");
      }
      return parts.join(" / ");
    }

    function renderSensorList(rootId, devices, emptyText, renderControls) {
      const root = document.getElementById(rootId);
      if (!root) return;
      root.innerHTML = "";
      if (!devices || !devices.length) {
        const empty = AirCANnect.ui.valueSpan(emptyText);
        empty.style.display = "block";
        empty.style.textAlign = "left";
        root.appendChild(empty);
        return;
      }
      devices.forEach((device, index) => {
        const controls = document.createElement("div");
        controls.className = "sensor-device-actions";
        renderControls(controls, device, index);

        const item = document.createElement("div");
        item.className = "sensor-device";
        const main = document.createElement("div");
        main.className = "sensor-device-main";
        const title = document.createElement("div");
        title.className = "sensor-device-title";
        AirCANnect.i18n.text(title, sensorDisplayName(device));
        main.appendChild(title);
        const metaText = sensorMeta(device);
        if (metaText) {
          const meta = document.createElement("div");
          meta.className = "sensor-device-meta";
          AirCANnect.i18n.text(meta, metaText);
          main.appendChild(meta);
        }
        item.appendChild(main);
        item.appendChild(controls);
        root.appendChild(item);
      });
    }

    function renderOximetrySensorManager(sensorData) {
      const state = sensorData.sensor_state || "off";
      let badgeStyle = "";
      let badgeText = state;
      if (!sensorData.enabled) {
        badgeText = "off";
      } else if (state === "streaming") {
        badgeStyle = "good";
      } else if (state === "connected") {
        badgeText = "connected";
        badgeStyle = "warn";
      } else if (state === "connecting" || state === "scanning") {
        badgeStyle = "warn";
      }
      const badge = document.getElementById("oxiSensorBadge");
      if (badge) {
        AirCANnect.i18n.text(badge, AirCANnect.i18n.message("state." + badgeText, {}, badgeText));
        badge.className = "badge " + badgeStyle;
      }

      const scanResults = sensorData.sensor_scan_results || [];
      const knownSensors = sensorData.sensor_known || [];

      const scanBtn = document.getElementById("oxiSensorScanBtn");
      const disconnectBtn = document.getElementById("oxiSensorDisconnectBtn");
      if (scanBtn) {
        scanBtn.disabled = !sensorData.enabled || !sensorData.ble_available ||
          sensorData.sensor_connected || state === "scanning" ||
          state === "connecting";
      }
      if (disconnectBtn) {
        disconnectBtn.disabled = !sensorData.sensor_connected;
      }
      const activeSensor = (sensorData.sensor_peer || "").toLowerCase();

      renderSensorList("oxiSensorResults", scanResults,
        AirCANnect.i18n.message("status.no_scan"), (controls, device) => {
          const button = document.createElement("button");
          button.className = "btn primary";
          const isActive = activeSensor &&
            (device.addr || "").toLowerCase() === activeSensor;
          AirCANnect.i18n.text(button, isActive ? AirCANnect.i18n.message("status.connected") : AirCANnect.i18n.message("ui.connect"));
          button.disabled = isActive || !sensorData.enabled ||
            !sensorData.ble_available || sensorData.sensor_connected ||
            state === "connecting";
          button.onclick = () => oxiSensorConnect(device, false);
          controls.appendChild(button);
        });

      renderSensorList("oxiSensorKnown", knownSensors,
        AirCANnect.i18n.message("status.no_known"), (controls, device) => {
          const isActive = activeSensor &&
            (device.addr || "").toLowerCase() === activeSensor;
          const auto = document.createElement("button");
          auto.className = "btn" + (device.autoconnect ? " primary" : "");
          AirCANnect.i18n.text(auto, device.autoconnect ?
            AirCANnect.i18n.message("status.autoconnect_on") : AirCANnect.i18n.message("status.autoconnect_off"));
          auto.onclick = () => oxiSensorAutoconnect(
            device.addr, !device.autoconnect);
          controls.appendChild(auto);

          const connect = document.createElement("button");
          connect.className = "btn primary";
          AirCANnect.i18n.text(connect, isActive ? AirCANnect.i18n.message("status.connected") : AirCANnect.i18n.message("ui.connect"));
          connect.disabled = isActive || !sensorData.enabled ||
            !sensorData.ble_available || sensorData.sensor_connected ||
            state === "connecting";
          connect.onclick = () => oxiSensorConnect(device, true);
          controls.appendChild(connect);

          const forget = document.createElement("button");
          forget.className = "btn danger";
          AirCANnect.i18n.text(forget, AirCANnect.i18n.message("pairing.forget"));
          forget.onclick = () => oxiSensorForget(device.addr);
          controls.appendChild(forget);
        });
    }

    function therapyPending(data) {
      return data.therapy_pending && data.therapy_pending !== "none";
    }

    function therapyDisplayState(data) {
      return therapyPending(data) ? data.therapy_pending : data.therapy;
    }

    function setTherapyButtons(data) {
      const start = document.getElementById("therapyStart");
      const stop = document.getElementById("therapyStop");
      if (!start || !stop) return;

      const pending = therapyPending(data);
      const shown = therapyDisplayState(data);
      const unavailable = data.as11_state === "unavailable";
      AirCANnect.i18n.text(start, pending && shown === "running" ? AirCANnect.i18n.message("status.starting") : AirCANnect.i18n.message("ui.therapy"));
      AirCANnect.i18n.text(stop, pending && shown === "standby" ? AirCANnect.i18n.message("status.stopping") : AirCANnect.i18n.message("ui.standby"));
      start.className = "btn" + (shown === "running" ? " primary" : "");
      stop.className = "btn" + (shown === "standby" ? " primary" : "");
      start.disabled = unavailable || !!pending || shown === "running";
      stop.disabled = unavailable || !!pending || shown === "standby";
    }

    function applyTherapyPending(action) {
      const data = Object.assign({}, statusData || {});
      data.therapy_pending = action === "start" ? "running" : "standby";
      if (!data.therapy) data.therapy = "unknown";
      statusData = data;
      renderStatus(data);
    }

    function applyStatusSnapshot(data) {
      statusData = data;
      renderStatus(statusData);
      return statusData;
    }

    async function loadStatus() {
      if (statusLoadPromise) return statusLoadPromise;

      statusLoadPromise = (async () => {
        const response = await AirCANnect.http.requestOk("/api/status");
        const data = await response.json();
        AirCANnect.snapshots.publish("status", data);
        return data;
      })();

      try {
        return await statusLoadPromise;
      } catch (error) {
        apiError(error);
        return null;
      } finally {
        statusLoadPromise = null;
      }
    }

    async function loadOximetrySensors() {
      if (oxiSensorsLoading) return;
      oxiSensorsLoading = true;
      try {
        const response = await AirCANnect.http.requestOk("/api/oximetry/sensors");
        oxiSensorData = await response.json();
        renderOximetrySensorManager(oxiSensorData);
      } catch (error) {
        apiError(error);
      } finally {
        oxiSensorsLoading = false;
      }
    }

    function renderStatus(data) {
      setPageTitle(data.hostname);
      AirCANnect.ui.text("ver", data.version);
      AirCANnect.ui.text("built", data.built);

      const updateNotice = document.getElementById("updateNotice");
      if (updateNotice) {
        updateNotice.hidden = !data.update_available;
        AirCANnect.i18n.text(updateNotice, data.update_available
          ? AirCANnect.i18n.message("status.update_available", {version: data.update_version || "--"})
          : "");
      }

      const memory = [AirCANnect.i18n.numberMessage(data.heap / 1024, 1, 1), " KB heap"];
      if (data.psram_available) {
        memory.push(" / ", AirCANnect.i18n.numberMessage(data.psram_free / 1048576, 1, 1), " MB psram");
      }
      AirCANnect.ui.text("heap", AirCANnect.i18n.join(memory));
      AirCANnect.ui.text("uptime", AirCANnect.i18n.message("status.uptime", {duration: fmtUp(data.uptime || 0)}));
      setWifiTop(data);
      AirCANnect.ui.text("productName", data.device_name || AirCANnect.i18n.message("status.resmed_device"));
      AirCANnect.ui.text("serial", data.serial);
      AirCANnect.ui.text("firmware", fmtFirmware(data.software_id || data.application));
      let as11Connection = (data.as11_transport || "can").toUpperCase();
      if (data.as11_transport === "ble") {
        as11Connection += " / " + (data.as11_link_state || "unknown");
        if (data.as11_link_connected && data.as11_link_rssi) {
          as11Connection += " / " + data.as11_link_rssi + " dBm";
        } else if (data.as11_link_error &&
                   data.as11_link_state !== "missing_credentials") {
          as11Connection += " / " + data.as11_link_error;
        }
      }
      AirCANnect.ui.text("as11Connection", as11Connection);

      const connectButton = document.getElementById("as11ConnectButton");
      if (connectButton) {
        connectButton.hidden = data.as11_transport !== "ble" ||
          !["quiesced", "backoff"].includes(data.as11_link_state);
      }

      const pairButton = document.getElementById("as11PairButton");
      if (pairButton) {
        pairButton.hidden = data.as11_transport !== "ble" ||
          data.as11_link_state !== "missing_credentials";
      }

      AirCANnect.ui.text("profile", fmtProfile(data.profile));
      AirCANnect.ui.text("motorHours", data.motor_hours ?
        AirCANnect.i18n.message("status.motor_hours", {hours: Number(data.motor_hours)}) : "--");
      AirCANnect.ui.text("deviceTime", fmtIsoMinute(data.device_datetime,
        data.device_datetime_age_ms));
      AirCANnect.ui.text("espTime", data.esp_time_valid ?
        fmtIsoMinute(data.esp_datetime) : AirCANnect.i18n.message("status.clock_invalid"));
      renderStorageStatus(data);
      renderSystemIssues(data);
      AirCANnect.ui.text("timeSync", fmtSync(data));
      const timeToResmed = document.getElementById("timeToResmed");
      if (timeToResmed) timeToResmed.disabled = !data.resmed_time_write_supported;
      renderOximetryRuntime(data);

      const badge = document.getElementById("therapyBadge");
      const pending = therapyPending(data);
      const unavailable = data.as11_state === "unavailable";
      const label = unavailable ? AirCANnect.i18n.message("status.unavailable") : pending ?
        (data.therapy_pending === "running" ? AirCANnect.i18n.message("status.starting") : AirCANnect.i18n.message("status.stopping")) :
        fmtTherapy(data.therapy);
      AirCANnect.ui.text("therapyState", label);
      badge.className = "badge " +
        (unavailable || pending ? "warn" :
          data.therapy === "running" ? "good" : "");

      const elapsed = document.getElementById("therapyElapsed");
      const showElapsed = !unavailable && !pending && data.therapy === "running" &&
        Number.isFinite(data.therapy_elapsed_s) && data.therapy_elapsed_s >= 0;
      AirCANnect.i18n.text(elapsed, showElapsed ? " \u00b7 " + fmtUp(data.therapy_elapsed_s) : "");
      elapsed.hidden = !showElapsed;

      setTherapyButtons(data);
    }

    function chartPush(name, values, limit) {
      if (!Array.isArray(values) || !values.length) return;
      const target = liveData[name];
      values.forEach((value) => {
        target.push(value === null || value === undefined ? null : Number(value));
      });
      while (target.length > limit) target.shift();
    }

    function lastNumber(values) {
      for (let index = values.length - 1; index >= 0; index--) {
        const value = values[index];
        if (Number.isFinite(value)) return value;
      }
      return null;
    }

    function setChartValue(id, value, decimals) {
      const element = document.getElementById(id);
      if (!element) return;
      AirCANnect.i18n.text(element, Number.isFinite(value) ?
        AirCANnect.i18n.number(value, decimals, decimals) : "--");
    }

    function expandChartScale(name, seriesList) {
      const scale = chartScales[name];
      if (!scale) return null;
      const series = Array.isArray(seriesList) &&
          seriesList.some((item) => Array.isArray(item))
        ? seriesList
        : [seriesList];
      series.forEach((values) => {
        if (!Array.isArray(values)) return;
        values.forEach((value) => {
          if (!Number.isFinite(value)) return;
          if (scale.symmetric) {
            while (value < scale.min || value > scale.max) {
              scale.min -= scale.step;
              scale.max += scale.step;
            }
          } else {
            while (value > scale.max) scale.max += scale.step;
          }
        });
      });
      return scale;
    }

    function drawChart(id, values, color, minY, maxY, options) {
      options = options || {};
      const multi = Array.isArray(values) &&
        values.some((item) => item && Array.isArray(item.values));
      const seriesList = multi ? values : [{values, color}];
      const canvas = document.getElementById(id);
      if (!canvas) return;
      const parent = canvas.parentElement;
      const width = parent ? Math.max(260, parent.clientWidth - 32) : 640;
      const height = parseInt(getComputedStyle(canvas).height, 10) || 140;
      const ratio = window.devicePixelRatio || 1;
      if (canvas.width !== Math.round(width * ratio) ||
          canvas.height !== Math.round(height * ratio)) {
        canvas.width = Math.round(width * ratio);
        canvas.height = Math.round(height * ratio);
      }
      const ctx = canvas.getContext("2d");
      ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
      ctx.clearRect(0, 0, width, height);
      ctx.fillStyle = "#080a0f";
      ctx.fillRect(0, 0, width, height);

      const pad = {left: 42, right: 8, top: 12, bottom: 12};
      const graphW = width - pad.left - pad.right;
      const graphH = height - pad.top - pad.bottom;
      ctx.strokeStyle = "#1f2633";
      ctx.lineWidth = 1;
      ctx.setLineDash([2, 5]);
      for (let i = 0; i <= 4; i++) {
        const y = pad.top + graphH * (i / 4);
        ctx.beginPath();
        ctx.moveTo(pad.left, y);
        ctx.lineTo(width - pad.right, y);
        ctx.stroke();
      }
      if (minY < 0 && maxY > 0) {
        const zeroY = pad.top + graphH * (1 - (0 - minY) / (maxY - minY));
        ctx.setLineDash([5, 5]);
        ctx.beginPath();
        ctx.moveTo(pad.left, zeroY);
        ctx.lineTo(width - pad.right, zeroY);
        ctx.stroke();
      }
      ctx.setLineDash([]);

      ctx.fillStyle = "#69717f";
      ctx.font = "10px SF Mono, Consolas, monospace";
      ctx.textAlign = "right";
      for (let i = 0; i <= 4; i++) {
        const value = maxY - (maxY - minY) * (i / 4);
        const decimals = Math.abs(value) >= 10 ? 0 : 1;
        const label = AirCANnect.i18n.number(value, decimals, decimals);
        ctx.fillText(label, pad.left - 6, pad.top + graphH * (i / 4) + 3);
      }

      const maxLen = seriesList.reduce((max, series) =>
        Math.max(max, Array.isArray(series.values) ? series.values.length : 0), 0);
      if (maxLen < 2) return;
      const pointLimit = Math.max(2, options.points || maxLen);
      function trace(seriesValues, lineWidth, strokeStyle) {
        const points = Array.isArray(seriesValues) ?
          seriesValues.slice(-pointLimit) : [];
        if (points.length < 2) return;
        ctx.beginPath();
        ctx.lineWidth = lineWidth;
        ctx.strokeStyle = strokeStyle;
        let started = false;
        points.forEach((value, index) => {
          if (!Number.isFinite(value)) {
            started = false;
            return;
          }
          const clipped = Math.max(minY, Math.min(maxY, value));
          const x = pad.left + graphW * (index / Math.max(1, points.length - 1));
          const y = pad.top + graphH * (1 - (clipped - minY) / (maxY - minY));
          if (!started) {
            ctx.moveTo(x, y);
            started = true;
          } else {
            ctx.lineTo(x, y);
          }
        });
        ctx.stroke();
      }
      seriesList.forEach((series) => {
        trace(series.values, 4, series.color + "44");
        trace(series.values, 1.6, series.color);
      });
    }

    function updateCharts() {
      const pressureScale = expandChartScale("pressure", liveData.pressure);
      const flowScale = expandChartScale("flow", liveData.flow);
      const leakScale = expandChartScale("leak", liveData.leak);
      const therapyPressureScale = expandChartScale("therapyPressure", [
        liveData.inspPressure,
        liveData.expPressure,
      ]);

      drawChart("chartPressureCanvas", liveData.pressure, "#22d3ee",
        pressureScale.min, pressureScale.max, {points: LIVE_FAST_POINTS});
      drawChart("chartFlowCanvas", liveData.flow, "#818cf8",
        flowScale.min, flowScale.max, {points: LIVE_FAST_POINTS});
      drawChart("chartLeakCanvas", liveData.leak, "#fb923c",
        leakScale.min, leakScale.max, {points: LIVE_MEDIUM_POINTS});
      drawChart("chartTherapyPressureCanvas", [
        {values: liveData.inspPressure, color: "#22c55e"},
        {values: liveData.expPressure, color: "#f97316"},
      ], "#22c55e", therapyPressureScale.min, therapyPressureScale.max,
        {points: LIVE_MEDIUM_POINTS});
      drawChart("chartSpo2Canvas", liveData.spo2, "#62d98f", 80, 100,
        {points: LIVE_SLOW_POINTS});
      drawChart("chartPulseCanvas", liveData.pulse, "#e85d75", 40, 160,
        {points: LIVE_SLOW_POINTS});

      setChartValue("chartPressure", lastNumber(liveData.pressure), 1);
      setChartValue("chartFlow", lastNumber(liveData.flow), 1);
      setChartValue("chartLeak", lastNumber(liveData.leak), 1);
      setChartValue("chartInspPressure", lastNumber(liveData.inspPressure), 1);
      setChartValue("chartExpPressure", lastNumber(liveData.expPressure), 1);
      setChartValue("chartSpo2", lastNumber(liveData.spo2), 0);
      setChartValue("chartPulse", lastNumber(liveData.pulse), 0);
    }

    function renderLive(data) {
      const active = data && data.active;
      const attached = data && data.attached;
      const badge = document.getElementById("liveBadge");
      if (badge) {
        AirCANnect.i18n.text(badge, attached ? AirCANnect.i18n.message("status.live") : active ? AirCANnect.i18n.message("status.starting") : AirCANnect.i18n.message("status.idle"));
        badge.className = "badge " + (attached ? "good" : active ? "warn" : "");
      }
      if (!data || !data.samples) {
        updateCharts();
        return;
      }
      chartPush("pressure", data.samples.pressure, LIVE_FAST_POINTS);
      chartPush("flow", data.samples.flow, LIVE_FAST_POINTS);
      chartPush("leak", data.samples.leak, LIVE_MEDIUM_POINTS);
      chartPush("inspPressure", data.samples.inspiratory_pressure,
        LIVE_MEDIUM_POINTS);
      chartPush("expPressure", data.samples.expiratory_pressure,
        LIVE_MEDIUM_POINTS);
      chartPush("spo2", data.samples.spo2, LIVE_SLOW_POINTS);
      chartPush("pulse", data.samples.pulse, LIVE_SLOW_POINTS);
      updateCharts();
    }

    async function therapy(action) {
      applyTherapyPending(action);
      try {
        const response = await AirCANnect.http.requestOk("/api/therapy", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({action}),
        });
        const data = await response.json();
        if (!data.ok) throw AirCANnect.i18n.failure("status.therapy_failed");
      } catch (error) {
        alert(AirCANnect.i18n.format(AirCANnect.i18n.error(error)));
        loadStatus();
      }
    }

    async function timeAction(action) {
      try {
        const response = await AirCANnect.http.requestOk("/api/time", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({action}),
        });
        const data = await response.json();
        AirCANnect.ui.message("timeMsg", data.result, data.ok);
        setTimeout(loadStatus, 900);
      } catch (error) {
        AirCANnect.ui.message("timeMsg", AirCANnect.i18n.error(error), false);
      }
    }

    async function oxiAction(action, extra, msgId) {
      try {
        const body = Object.assign({action}, extra || {});
        const response = await AirCANnect.http.requestOk("/api/oximetry", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify(body),
        });
        const data = await response.json();
        AirCANnect.ui.message(msgId || "oxiTabMsg",
          !data.result || data.result === "queued" ? AirCANnect.i18n.message("status.queued") : data.result, data.ok);
      } catch (error) {
        AirCANnect.ui.message(msgId || "oxiTabMsg", AirCANnect.i18n.error(error), false);
      }
    }

    function oxiPairToggle() {
      const oxi = statusData && statusData.oximetry ? statusData.oximetry : {};
      oxiAction(oxi.pairing_active ? "pair_stop" : "pair");
    }

    function oxiForget() {
      if (!confirm(AirCANnect.i18n.t("status.forget_airsense"))) return;
      oxiAction("forget");
    }

    function oxiSensorScan() {
      oxiAction("sensor_scan", null, "oxiSensorMsg");
    }

    function oxiSensorConnect(target, fromKnown) {
      if (target === undefined || target === null || target === "") return;
      const body = typeof target === "object" ? {
        target: target.addr || "",
        addr: target.addr || "",
        addr_type: target.addr_type || 0,
        name: target.name || "",
        rssi: target.rssi || 0,
      } : {target: String(target)};
      oxiAction("sensor_connect", body,
        fromKnown ? "oxiKnownMsg" : "oxiSensorMsg");
    }

    function oxiSensorDisconnect() {
      oxiAction("sensor_disconnect", null, "oxiSensorMsg");
    }

    function oxiSensorForget(addr) {
      if (!addr) return;
      if (!confirm(AirCANnect.i18n.t("status.forget_sensor", {address: addr}))) return;
      oxiAction("sensor_forget", {addr}, "oxiKnownMsg");
    }

    function oxiSensorAutoconnect(addr, enabled) {
      if (!addr) return;
      const known = oxiSensorData.sensor_known || [];
      known.forEach((device) => {
        if (device.addr === addr) device.autoconnect = enabled;
      });
      renderOximetrySensorManager(oxiSensorData);
      oxiAction("sensor_autoconnect", {addr, enabled}, "oxiKnownMsg");
    }

    async function saveOximetryAdvertiseConfig() {
      const advertiseMode = document.getElementById("oxiAdvertiseMode").value;
      try {
        const response = await AirCANnect.http.requestOk("/api/config", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({oxi_adv: advertiseMode}),
        });
        const data = await response.json();
        AirCANnect.ui.message("oxiTabMsg", data.queued ? AirCANnect.i18n.message("status.config_queued") : AirCANnect.i18n.message("config.saved"), true);
        setTimeout(loadStatus, 600);
      } catch (error) {
        AirCANnect.ui.message("oxiTabMsg", AirCANnect.i18n.error(error), false);
      }
    }

    AirCANnect.actions.register("therapy.command", (_event, element) =>
      therapy(element.dataset.value));
    AirCANnect.actions.register("time.command", (_event, element) =>
      timeAction(element.dataset.value));
    AirCANnect.actions.register("oximetry.advertise-config", () =>
      saveOximetryAdvertiseConfig());
    AirCANnect.actions.register("oximetry.pair-toggle", () => oxiPairToggle());
    AirCANnect.actions.register("oximetry.command", (_event, element) =>
      oxiAction(element.dataset.value));
    AirCANnect.actions.register("oximetry.forget", () => oxiForget());
    AirCANnect.actions.register("oximetry.sensor-scan", () => oxiSensorScan());
    AirCANnect.actions.register("oximetry.sensor-disconnect", () =>
      oxiSensorDisconnect());
    AirCANnect.pages.onLoad("dash", () => loadStatus());
    AirCANnect.pages.onLoad("edf", () => loadStatus());
    AirCANnect.pages.onLoad("oxi", () => {
      loadStatus();
      loadOximetrySensors();
    });
    AirCANnect.snapshots.subscribe("status", applyStatusSnapshot, false);
    AirCANnect.snapshots.subscribe("ota", (data) => {
      const built = document.getElementById("built");
      if (built) AirCANnect.i18n.text(built, data.release_target ?
        AirCANnect.i18n.message("status.build_target", {target: data.release_target}) : "", "title");
    }, true);
    AirCANnect.events.subscribe("status", () => {});
    AirCANnect.events.subscribe("oximetry", (data) => {
      oxiSensorData = data;
      const pane = document.getElementById("p-oxi");
      if (pane && pane.classList.contains("active")) {
        renderOximetrySensorManager(oxiSensorData);
      }
    });
    AirCANnect.events.subscribe("live", renderLive);
    AirCANnect.i18n.onChange(updateCharts);
    window.addEventListener("resize", () => updateCharts());
    updateCharts();
})();
