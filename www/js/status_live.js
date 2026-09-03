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

    function setWifiTop(data) {
      const element = document.getElementById("wifiTop");
      if (!element) return;
      element.textContent = "";

      const state = data && data.wifi_state ? String(data.wifi_state) : "--";
      const rssi = data && data.wifi_rssi;
      const wrapper = AirCANnect.ui.wifiSignal(rssi, "WiFi: " + state);
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
      if (value === "running") return "Therapy";
      if (value === "standby") return "Standby";
      if (value === "unknown") return "Unknown";
      return value || "Unknown";
    }

    function fmtSync(data) {
      if (!data.esp_time_valid) return "Clock invalid";

      let source = data.esp_time_source === "ntp" ? "NTP" :
        data.esp_time_source === "resmed" ? "ResMed" : "Clock";
      if (data.ntp_synced) source = "NTP";
      return source + (data.resmed_time_sync_enabled ? " -> ResMed" : "");
    }

    function renderStorageStatus(data) {
      const summary = document.getElementById("storageSummary");
      const meter = document.getElementById("storageMeter");
      if (!summary || !meter) return;

      let text = data.storage_state || "Not mounted";
      let percent = 0;
      const total = Number(data.storage_total || 0);
      const used = Number(data.storage_used || 0);
      const free = Math.max(0, total - used);
      if (data.storage_state === "mounted" && total > 0) {
        text = AirCANnect.format.bytes(free) + " / " + AirCANnect.format.bytes(total);
        percent = Math.max(0, Math.min(100, used / total * 100));
      }

      summary.textContent = text;
      meter.style.width = percent.toFixed(1) + "%";
      meter.title = percent.toFixed(1) + "% used";
    }

    function renderOximetryRuntime(data) {
      const oxi = data && data.oximetry ? data.oximetry : {};
      const sourceValid = !!(oxi.source_fresh && oxi.valid);
      AirCANnect.ui.text("oxiSpo2", sourceValid ? oxi.spo2 : "--");
      AirCANnect.ui.text("oxiPulse", sourceValid ? oxi.pulse_bpm : "--");
      AirCANnect.ui.text("oxiTabSpo2", sourceValid ? oxi.spo2 : "--");
      AirCANnect.ui.text("oxiTabPulse", sourceValid ? oxi.pulse_bpm : "--");

      let badgeLabel = "Off";
      let badgeStyle = "";
      if (!oxi.enabled) {
        badgeLabel = "Off";
      } else if (!oxi.ble_available) {
        badgeLabel = "BLE Off";
        badgeStyle = "bad";
      } else if (oxi.subscribed) {
        badgeLabel = "Recording";
        badgeStyle = "good";
      } else if (oxi.connected) {
        badgeLabel = "Connected";
        badgeStyle = "warn";
      } else if (oxi.pairing_active) {
        badgeLabel = "Pairing";
        badgeStyle = "warn";
      } else if (oxi.advertising) {
        badgeLabel = "Advertising";
        badgeStyle = "warn";
      } else if (sourceValid) {
        badgeLabel = "Ready";
        badgeStyle = "good";
      } else {
        badgeLabel = "Idle";
      }
      ["oxiBadge", "oxiTabBadge"].forEach((id) => {
        const badge = document.getElementById(id);
        if (!badge) return;
        badge.textContent = badgeLabel;
        badge.className = "badge " + badgeStyle;
      });

      const sourceKind = oxi.source && oxi.source !== "none" ?
        oxi.source.toUpperCase() : "none";
      const sourceDetail = (oxi.source_detail || "").trim();
      const sourceIdentity = sourceKind === "none" ? "none" :
        sourceKind + (sourceDetail ? " " + sourceDetail : "");
      let sourceStatus = "no source";
      if (!oxi.enabled) {
        sourceStatus = "off";
      } else if (oxi.source_present && oxi.source_fresh && oxi.valid) {
        sourceStatus = "fresh";
      } else if (oxi.source_present && oxi.source_fresh &&
          oxi.contact_known && !oxi.contact_present) {
        sourceStatus = "no finger";
      } else if (oxi.source_present && oxi.source_fresh) {
        sourceStatus = "invalid";
      } else if (oxi.source_present) {
        sourceStatus = "stale";
      }
      AirCANnect.ui.text("oxiSource", sourceIdentity === "none" ?
        sourceStatus : sourceIdentity + " " + sourceStatus);
      AirCANnect.ui.text("oxiTabSource", sourceIdentity);
      AirCANnect.ui.text("oxiTabStatus", sourceStatus);

      const integrationAvailable =
        oxi.airsense_integration_available !== false;
      const as11 = !integrationAvailable ? "local SA2" :
        oxi.subscribed ? "subscribed" :
          oxi.connected ? "connected" : "not connected";
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
      let advertise = (oxi.advertise_mode || "--") + " / " + advertiseState;
      if (oxi.pairing_active) {
        advertise += " / pairing " +
          Math.ceil((oxi.pairing_left_ms || 0) / 1000) + "s";
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
        pair.textContent = oxi.pairing_active ? "Stop Pairing" : "Pair AirSense";
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
        title.textContent = sensorDisplayName(device);
        main.appendChild(title);
        const metaText = sensorMeta(device);
        if (metaText) {
          const meta = document.createElement("div");
          meta.className = "sensor-device-meta";
          meta.textContent = metaText;
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
        badge.textContent = badgeText;
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
        "No scan results", (controls, device) => {
          const button = document.createElement("button");
          button.className = "btn primary";
          const isActive = activeSensor &&
            (device.addr || "").toLowerCase() === activeSensor;
          button.textContent = isActive ? "Connected" : "Connect";
          button.disabled = isActive || !sensorData.enabled ||
            !sensorData.ble_available || sensorData.sensor_connected ||
            state === "connecting";
          button.onclick = () => oxiSensorConnect(device, false);
          controls.appendChild(button);
        });

      renderSensorList("oxiSensorKnown", knownSensors,
        "No known devices", (controls, device) => {
          const isActive = activeSensor &&
            (device.addr || "").toLowerCase() === activeSensor;
          const auto = document.createElement("button");
          auto.className = "btn" + (device.autoconnect ? " primary" : "");
          auto.textContent = device.autoconnect ?
            "Auto-connect On" : "Auto-connect Off";
          auto.onclick = () => oxiSensorAutoconnect(
            device.addr, !device.autoconnect);
          controls.appendChild(auto);

          const connect = document.createElement("button");
          connect.className = "btn primary";
          connect.textContent = isActive ? "Connected" : "Connect";
          connect.disabled = isActive || !sensorData.enabled ||
            !sensorData.ble_available || sensorData.sensor_connected ||
            state === "connecting";
          connect.onclick = () => oxiSensorConnect(device, true);
          controls.appendChild(connect);

          const forget = document.createElement("button");
          forget.className = "btn danger";
          forget.textContent = "Forget";
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
      start.textContent = pending && shown === "running" ? "Starting" : "Therapy";
      stop.textContent = pending && shown === "standby" ? "Stopping" : "Standby";
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
        updateNotice.textContent = data.update_available
          ? "Version " + (data.update_version || "new") + " available"
          : "";
      }

      let memory = (data.heap / 1024).toFixed(1) + " KB heap";
      if (data.psram_available) {
        memory += " / " + (data.psram_free / 1048576).toFixed(1) + " MB psram";
      }
      AirCANnect.ui.text("heap", memory);
      AirCANnect.ui.text("uptime", "Up: " + fmtUp(data.uptime || 0));
      setWifiTop(data);
      AirCANnect.ui.text("productName", data.device_name || "ResMed device");
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

      const pairButton = document.getElementById("as11PairButton");
      if (pairButton) {
        pairButton.hidden = data.as11_transport !== "ble" ||
          data.as11_link_state !== "missing_credentials";
      }

      AirCANnect.ui.text("profile", fmtProfile(data.profile));
      AirCANnect.ui.text("motorHours", data.motor_hours ?
        Number(data.motor_hours).toLocaleString() + " hrs" : "--");
      AirCANnect.ui.text("deviceTime", fmtIsoMinute(data.device_datetime,
        data.device_datetime_age_ms));
      AirCANnect.ui.text("espTime", data.esp_time_valid ? fmtIsoMinute(data.esp_datetime) : "invalid");
      renderStorageStatus(data);
      AirCANnect.ui.text("timeSync", fmtSync(data));
      renderOximetryRuntime(data);

      const badge = document.getElementById("therapyBadge");
      const pending = therapyPending(data);
      const unavailable = data.as11_state === "unavailable";
      const label = unavailable ? "Unavailable" : pending ?
        (data.therapy_pending === "running" ? "Starting" : "Stopping") :
        fmtTherapy(data.therapy);
      badge.textContent = label;
      badge.className = "badge " +
        (unavailable || pending ? "warn" :
          data.therapy === "running" ? "good" : "");
      setTherapyButtons(data);
    }

    function renderStream(data) {
      const state = data.error ? "error" :
        data.pending_start ? "starting" :
        data.pending_stop ? "stopping" :
        data.subscribed ? "subscribed" :
        data.desired ? "requested" : "idle";
      AirCANnect.ui.text("streamState", state);
      AirCANnect.ui.text("streamConsumers", data.consumers);
      AirCANnect.ui.text("streamNotifications", data.notifications || 0);
      AirCANnect.ui.text("streamFanout", (data.consumers || 0) + " consumers, " +
        (data.fanout_drops || 0) + " drops");
      AirCANnect.ui.text("streamFrames", (data.frame_pool_used || 0) + "/" +
        (data.frame_pool_capacity || 0) + " used, " +
        (data.parse_errors || 0) + " parse, " +
        (data.truncated_frames || 0) + " trunc");
      AirCANnect.ui.text("streamCommands", (data.command_errors || 0) + " errors");
      AirCANnect.ui.text("streamLast", data.last_age_ms === null ?
        "--" : Math.round(data.last_age_ms / 1000) + " s ago");
      AirCANnect.ui.text("streamId", data.stream_id || "--");
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
      element.textContent = Number.isFinite(value) ? value.toFixed(decimals) : "--";
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
        const label = Math.abs(value) >= 10 ? value.toFixed(0) : value.toFixed(1);
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
        badge.textContent = attached ? "Live" : active ? "Starting" : "Idle";
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
        if (!data.ok) throw new Error("therapy command queue failed");
      } catch (error) {
        alert(error.message);
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
        AirCANnect.ui.message("timeMsg", error.message, false);
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
        AirCANnect.ui.message(msgId || "oxiTabMsg", data.result || "Queued", data.ok);
      } catch (error) {
        AirCANnect.ui.message(msgId || "oxiTabMsg", error.message, false);
      }
    }

    function oxiPairToggle() {
      const oxi = statusData && statusData.oximetry ? statusData.oximetry : {};
      oxiAction(oxi.pairing_active ? "pair_stop" : "pair");
    }

    function oxiForget() {
      if (!confirm("Forget paired AirSense oximeter bond?")) return;
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
      if (!confirm("Forget BLE oximeter " + addr + "?")) return;
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
        AirCANnect.ui.message("oxiTabMsg", data.queued ? "Config update queued" : "Saved", true);
        setTimeout(loadStatus, 600);
      } catch (error) {
        AirCANnect.ui.message("oxiTabMsg", error.message, false);
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
    AirCANnect.events.subscribe("status", () => {});
    AirCANnect.events.subscribe("oximetry", (data) => {
      oxiSensorData = data;
      const pane = document.getElementById("p-oxi");
      if (pane && pane.classList.contains("active")) {
        renderOximetrySensorManager(oxiSensorData);
      }
    });
    AirCANnect.events.subscribe("stream", renderStream);
    AirCANnect.events.subscribe("live", renderLive);
    window.addEventListener("resize", () => updateCharts());
    updateCharts();
})();
