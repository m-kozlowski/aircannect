    function fmtRssi(rssi) {
      const value = Number(rssi);
      return Number.isFinite(value) && value < 0 ?
        Math.round(value) + " dBm" : "--";
    }

    function wifiSignalLevel(rssi) {
      const value = Number(rssi);
      if (!Number.isFinite(value) || value >= 0) return 0;
      if (value >= -55) return 4;
      if (value >= -65) return 3;
      if (value >= -75) return 2;
      return 1;
    }

    function wifiBarsElement(rssi) {
      const level = wifiSignalLevel(rssi);
      const bars = document.createElement("span");
      bars.className = "wifi-bars";
      bars.title = fmtRssi(rssi);
      for (let index = 1; index <= 4; index++) {
        const bar = document.createElement("span");
        if (index <= level) bar.className = "on";
        bars.appendChild(bar);
      }
      return bars;
    }

    function wifiSignalValue(rssi, prefix) {
      const value = document.createElement("span");
      value.className = "value wifi-signal";
      value.appendChild(wifiBarsElement(rssi));
      const text = document.createElement("span");
      text.textContent = (prefix ? prefix + " " : "") + fmtRssi(rssi);
      value.appendChild(text);
      return value;
    }

    function setWifiTop(data) {
      const element = document.getElementById("wifiTop");
      if (!element) return;
      element.textContent = "";

      const state = data && data.wifi_state ? String(data.wifi_state) : "--";
      const rssi = data && data.wifi_rssi;
      const wrapper = document.createElement("span");
      wrapper.className = "wifi-signal";
      wrapper.appendChild(wifiBarsElement(rssi));

      const text = document.createElement("span");
      const rssiText = fmtRssi(rssi);
      text.textContent = "WiFi: " + state +
        (rssiText !== "--" ? " " + rssiText : "");
      wrapper.appendChild(text);
      element.appendChild(wrapper);
    }

    function pad2(value) {
      return String(value).padStart(2, "0");
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
        pad2(date.getMonth() + 1) + "-" +
        pad2(date.getDate()) + " " +
        pad2(date.getHours()) + ":" +
        pad2(date.getMinutes()) +
        timezone;
    }

    const TZ_ABBR = {
      "Africa/Cairo": ["EET", null],
      "Africa/Lagos": ["WAT", null],
      "Africa/Nairobi": ["EAT", null],
      "America/Anchorage": ["AKST", "AKDT"],
      "America/Argentina/Buenos_Aires": ["ART", null],
      "America/Chicago": ["CST", "CDT"],
      "America/Denver": ["MST", "MDT"],
      "America/Los_Angeles": ["PST", "PDT"],
      "America/Mexico_City": ["CST", null],
      "America/New_York": ["EST", "EDT"],
      "America/Sao_Paulo": ["BRT", null],
      "America/Toronto": ["EST", "EDT"],
      "Asia/Bangkok": ["ICT", null],
      "Asia/Dhaka": ["BST", null],
      "Asia/Dubai": ["GST", null],
      "Asia/Hong_Kong": ["HKT", null],
      "Asia/Jerusalem": ["IST", "IDT"],
      "Asia/Karachi": ["PKT", null],
      "Asia/Kathmandu": ["NPT", null],
      "Asia/Kolkata": ["IST", null],
      "Asia/Seoul": ["KST", null],
      "Asia/Shanghai": ["CST", null],
      "Asia/Singapore": ["SGT", null],
      "Asia/Tokyo": ["JST", null],
      "Australia/Sydney": ["AEST", "AEDT"],
      "Europe/Berlin": ["CET", "CEST"],
      "Europe/Helsinki": ["EET", "EEST"],
      "Europe/Istanbul": ["TRT", null],
      "Europe/London": ["GMT", "BST"],
      "Europe/Moscow": ["MSK", null],
      "Europe/Paris": ["CET", "CEST"],
      "Europe/Warsaw": ["CET", "CEST"],
      "Pacific/Auckland": ["NZST", "NZDT"],
      "Pacific/Fiji": ["FJT", "FJST"],
      "Pacific/Honolulu": ["HST", null],
    };

    const TZ_ZONES = [
      "UTC",
      "Pacific/Honolulu",
      "America/Anchorage",
      "America/Los_Angeles",
      "America/Denver",
      "America/Chicago",
      "America/New_York",
      "America/Toronto",
      "America/Mexico_City",
      "America/Sao_Paulo",
      "America/Argentina/Buenos_Aires",
      "Europe/London",
      "Europe/Paris",
      "Europe/Berlin",
      "Europe/Warsaw",
      "Europe/Helsinki",
      "Europe/Moscow",
      "Europe/Istanbul",
      "Africa/Cairo",
      "Africa/Lagos",
      "Africa/Nairobi",
      "Asia/Dubai",
      "Asia/Karachi",
      "Asia/Kolkata",
      "Asia/Kathmandu",
      "Asia/Dhaka",
      "Asia/Bangkok",
      "Asia/Shanghai",
      "Asia/Hong_Kong",
      "Asia/Singapore",
      "Asia/Tokyo",
      "Asia/Seoul",
      "Asia/Jerusalem",
      "Australia/Sydney",
      "Pacific/Auckland",
      "Pacific/Fiji",
    ].sort();

    const TZ_FORMATTERS = {};

    function timezoneFormatter(timezone) {
      if (!TZ_FORMATTERS[timezone]) {
        TZ_FORMATTERS[timezone] = new Intl.DateTimeFormat("en-US", {
          timeZone: timezone,
          hourCycle: "h23",
          year: "numeric",
          month: "2-digit",
          day: "2-digit",
          hour: "2-digit",
          minute: "2-digit",
          second: "2-digit",
        });
      }
      return TZ_FORMATTERS[timezone];
    }

    function timezoneWallTime(date, timezone) {
      const parts = {};
      timezoneFormatter(timezone).formatToParts(date).forEach((part) => {
        if (part.type !== "literal") parts[part.type] = part.value;
      });
      return {
        year: Number(parts.year),
        month: Number(parts.month),
        day: Number(parts.day),
        hour: Number(parts.hour),
        minute: Number(parts.minute),
        second: Number(parts.second),
      };
    }

    function timezoneOffsetMinutes(date, timezone) {
      const wall = timezoneWallTime(date, timezone);
      const wallMs = Date.UTC(
        wall.year,
        wall.month - 1,
        wall.day,
        wall.hour,
        wall.minute,
        wall.second
      );
      const instantMs = Math.floor(date.getTime() / 1000) * 1000;
      return (wallMs - instantMs) / 60000;
    }

    function timezoneAbbr(date, timezone, daylight) {
      if (timezone === "UTC" || timezone === "Etc/UTC") return "UTC";

      const known = TZ_ABBR[timezone];
      if (known) {
        const abbr = daylight ? known[1] : known[0];
        if (abbr) return abbr;
      }

      const parts = new Intl.DateTimeFormat("en-US", {
        timeZone: timezone,
        timeZoneName: "short",
      }).formatToParts(date);
      const part = parts.find((entry) => entry.type === "timeZoneName");
      const value = part && part.value ? part.value.replace(/[<>]/g, "") : "";
      if (/^[A-Za-z]{3,}$/.test(value)) return value;
      return "<" + (value || (daylight ? "DST" : "STD")) + ">";
    }

    function formatPosixOffset(minutes) {
      const hours = Math.trunc(-minutes / 60);
      const mins = Math.abs(minutes % 60);
      return String(hours) + (mins ? ":" + pad2(mins) : "");
    }

    function formatTimezoneTransition(timezone, reference, year) {
      const referenceOffset = timezoneOffsetMinutes(reference, timezone);
      let start = null;
      let end = null;

      for (let month = 0; month < 12; month++) {
        const first = new Date(year, month, 1);
        const next = new Date(year, month + 1, 1);
        if (timezoneOffsetMinutes(first, timezone) === referenceOffset &&
            timezoneOffsetMinutes(next, timezone) !== referenceOffset) {
          start = first.getTime();
          end = next.getTime();
          break;
        }
      }

      if (start === null) return "J365/25";

      while (end - start > 1000) {
        const mid = start + Math.floor((end - start) / 2);
        if (timezoneOffsetMinutes(new Date(mid), timezone) === referenceOffset) {
          start = mid;
        } else {
          end = mid;
        }
      }

      const wall = new Date(end + referenceOffset * 60000);
      const month = wall.getUTCMonth() + 1;
      const day = wall.getUTCDate();
      const weekday = wall.getUTCDay();
      const hour = wall.getUTCHours();
      const minute = wall.getUTCMinutes();
      const second = wall.getUTCSeconds();
      const daysInMonth = new Date(Date.UTC(wall.getUTCFullYear(), month, 0))
        .getUTCDate();
      const week = day + 7 > daysInMonth ? 5 : Math.ceil(day / 7);
      let rule = "M" + month + "." + week + "." + weekday;

      if (hour !== 2 || minute || second) {
        rule += "/" + hour;
        if (minute || second) {
          rule += ":" + pad2(minute);
          if (second) rule += ":" + pad2(second);
        }
      }

      return rule;
    }

    function getPosixTimezone(timezone) {
      if (timezone === "UTC" || timezone === "Etc/UTC") return "UTC0";

      const year = new Date().getFullYear();
      const jan = new Date(year, 0, 1);
      const jun = new Date(year, 5, 1);
      const janOffset = timezoneOffsetMinutes(jan, timezone);
      const junOffset = timezoneOffsetMinutes(jun, timezone);
      const standardOffset = Math.min(janOffset, junOffset);
      const daylightOffset = Math.max(janOffset, junOffset);
      const standardRef = standardOffset === janOffset ? jan : jun;
      const daylightRef = daylightOffset === janOffset ? jan : jun;

      let value = timezoneAbbr(standardRef, timezone, false) +
        formatPosixOffset(standardOffset);
      if (standardOffset !== daylightOffset) {
        value += timezoneAbbr(daylightRef, timezone, true);
        if (daylightOffset !== standardOffset + 60) {
          value += formatPosixOffset(daylightOffset);
        }
        value += "," +
          formatTimezoneTransition(timezone, standardRef, year) + "," +
          formatTimezoneTransition(timezone, daylightRef, year);
      }

      return value;
    }

    function setTimezoneFromZone(input, timezone) {
      const value = getPosixTimezone(timezone);
      input.value = value;
      input.title = timezone + " -> " + value;
    }

    function timezoneHelper(input) {
      const wrapper = document.createElement("div");
      wrapper.className = "config-helper";
      wrapper.appendChild(input);

      const detect = document.createElement("button");
      detect.type = "button";
      detect.className = "btn";
      detect.textContent = "Detect";
      detect.onclick = () => {
        try {
          const timezone = Intl.DateTimeFormat().resolvedOptions().timeZone ||
            "UTC";
          setTimezoneFromZone(input, timezone);
        } catch (error) {
          input.value = "UTC0";
          input.title = "Timezone detection failed";
        }
      };
      wrapper.appendChild(detect);

      const select = document.createElement("select");
      const placeholder = document.createElement("option");
      placeholder.value = "";
      placeholder.textContent = "Preset...";
      select.appendChild(placeholder);
      TZ_ZONES.forEach((timezone) => {
        const option = document.createElement("option");
        option.value = timezone;
        option.textContent = timezone;
        select.appendChild(option);
      });
      select.onchange = () => {
        if (select.value) setTimezoneFromZone(input, select.value);
      };
      wrapper.appendChild(select);

      return wrapper;
    }

    const WIFI_COUNTRY_PRESETS = [
      ["01", "Worldwide / default"],
      ["AU", "Australia"],
      ["BR", "Brazil"],
      ["CA", "Canada"],
      ["CN", "China"],
      ["DE", "Germany"],
      ["ES", "Spain"],
      ["FI", "Finland"],
      ["FR", "France"],
      ["GB", "United Kingdom"],
      ["IN", "India"],
      ["IT", "Italy"],
      ["JP", "Japan"],
      ["KR", "South Korea"],
      ["MX", "Mexico"],
      ["NL", "Netherlands"],
      ["NO", "Norway"],
      ["NZ", "New Zealand"],
      ["PL", "Poland"],
      ["SE", "Sweden"],
      ["US", "United States"],
    ];

    function wifiCountryHelper(input) {
      const wrapper = document.createElement("div");
      wrapper.className = "config-helper";
      wrapper.appendChild(input);
      input.maxLength = 2;
      input.autocapitalize = "characters";
      input.oninput = () => {
        input.value = input.value.toUpperCase();
      };

      const world = document.createElement("button");
      world.type = "button";
      world.className = "btn";
      world.textContent = "Default";
      world.onclick = () => {
        input.value = "01";
        input.title = "Worldwide / ESP-IDF default";
      };
      wrapper.appendChild(world);

      const select = document.createElement("select");
      const placeholder = document.createElement("option");
      placeholder.value = "";
      placeholder.textContent = "Preset...";
      select.appendChild(placeholder);
      WIFI_COUNTRY_PRESETS.forEach((entry) => {
        const option = document.createElement("option");
        option.value = entry[0];
        option.textContent = entry[0] + " - " + entry[1];
        select.appendChild(option);
      });
      select.onchange = () => {
        if (!select.value) return;
        input.value = select.value;
        const selected = WIFI_COUNTRY_PRESETS.find((entry) =>
          entry[0] === select.value);
        input.title = selected ? selected[1] : "";
      };
      wrapper.appendChild(select);

      return wrapper;
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
        text = fmtBytes(free) + " / " + fmtBytes(total);
        percent = Math.max(0, Math.min(100, used / total * 100));
      }

      summary.textContent = text;
      meter.style.width = percent.toFixed(1) + "%";
      meter.title = percent.toFixed(1) + "% used";
    }

    function renderOximetryRuntime(data) {
      const oxi = data && data.oximetry ? data.oximetry : {};
      const sourceValid = !!(oxi.source_fresh && oxi.valid);
      up("oxiSpo2", sourceValid ? oxi.spo2 : "--");
      up("oxiPulse", sourceValid ? oxi.pulse_bpm : "--");
      up("oxiTabSpo2", sourceValid ? oxi.spo2 : "--");
      up("oxiTabPulse", sourceValid ? oxi.pulse_bpm : "--");

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
      up("oxiSource", sourceIdentity === "none" ?
        sourceStatus : sourceIdentity + " " + sourceStatus);
      up("oxiTabSource", sourceIdentity);
      up("oxiTabStatus", sourceStatus);

      const as11 = oxi.subscribed ? "subscribed" :
        oxi.connected ? "connected" : "not connected";
      up("oxiAs11", as11);
      up("oxiTabAs11", as11);

      let advertiseState = "idle";
      if (!oxi.enabled) {
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
      up("oxiAdvertise", advertise);
      up("oxiTabAdvertise", advertise);

      up("oxiTabBleName", oxi.ble_name || "--");
      up("oxiTabPeer", oxi.ble_peer || "--");

      setControlValue("oxiAdvertiseMode", oxi.advertise_mode || "auto");

      const pair = document.getElementById("oxiPairBtn");
      const advStart = document.getElementById("oxiAdvStartBtn");
      const advStop = document.getElementById("oxiAdvStopBtn");
      if (pair) {
        pair.textContent = oxi.pairing_active ? "Stop Pairing" : "Pair AirSense";
        pair.disabled = !oxi.ble_available;
      }
      const manual = oxi.advertise_mode === "manual";
      if (advStart) {
        advStart.style.display = manual ? "" : "none";
        advStart.disabled = !oxi.enabled || oxi.advertising;
      }
      if (advStop) {
        advStop.style.display = manual ? "" : "none";
        advStop.disabled = !oxi.advertising &&
          !oxi.manual_advertising_requested;
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
        const empty = valueSpan(emptyText);
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
      start.textContent = pending && shown === "running" ? "Starting" : "Therapy";
      stop.textContent = pending && shown === "standby" ? "Stopping" : "Standby";
      start.className = "btn" + (shown === "running" ? " primary" : "");
      stop.className = "btn" + (shown === "standby" ? " primary" : "");
      start.disabled = !!pending || shown === "running";
      stop.disabled = !!pending || shown === "standby";
    }

    function applyTherapyPending(action) {
      const data = Object.assign({}, statusData || {});
      data.therapy_pending = action === "start" ? "running" : "standby";
      if (!data.therapy) data.therapy = "unknown";
      statusData = data;
      renderStatus(data);
    }

    async function loadStatus() {
      try {
        const response = await api("/api/status");
        statusData = await response.json();
        renderStatus(statusData);
      } catch (error) {
        apiError(error);
      }
    }

    async function loadOximetrySensors() {
      if (oxiSensorsLoading) return;
      oxiSensorsLoading = true;
      try {
        const response = await api("/api/oximetry/sensors");
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
      up("ver", data.version);
      up("built", data.built);

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
      up("heap", memory);
      up("uptime", "Up: " + fmtUp(data.uptime || 0));
      setWifiTop(data);
      const wifiPane = document.getElementById("p-wifi");
      if (wifiPane && wifiPane.classList.contains("active")) {
        renderWifiCurrent({});
      }
      up("productName", data.device_name || "AirSense 11");
      up("serial", data.serial);
      up("firmware", fmtFirmware(data.software_id || data.application));
      up("profile", fmtProfile(data.profile));
      up("motorHours", data.motor_hours ?
        Number(data.motor_hours).toLocaleString() + " hrs" : "--");
      up("deviceTime", fmtIsoMinute(data.device_datetime,
        data.device_datetime_age_ms));
      up("espTime", data.esp_time_valid ? fmtIsoMinute(data.esp_datetime) : "invalid");
      renderStorageStatus(data);
      up("timeSync", fmtSync(data));
      renderOximetryRuntime(data);

      const badge = document.getElementById("therapyBadge");
      const pending = therapyPending(data);
      const label = pending ?
        (data.therapy_pending === "running" ? "Starting" : "Stopping") :
        fmtTherapy(data.therapy);
      badge.textContent = label;
      badge.className = "badge " +
        (pending ? "warn" : data.therapy === "running" ? "good" : "");
      setTherapyButtons(data);
    }

    function renderStream(data) {
      const state = data.error ? "error" :
        data.pending_start ? "starting" :
        data.pending_stop ? "stopping" :
        data.subscribed ? "subscribed" :
        data.desired ? "requested" : "idle";
      up("streamState", state);
      up("streamConsumers", data.consumers);
      up("streamNotifications", data.notifications || 0);
      up("streamFanout", (data.fanout_targets || 0) + " targets, " +
        (data.fanout_drops || 0) + " drops");
      up("streamFrames", (data.frame_pool_used || 0) + "/" +
        (data.frame_pool_capacity || 0) + " used, " +
        (data.parse_errors || 0) + " parse, " +
        (data.truncated_frames || 0) + " trunc");
      up("streamCommands", (data.start_requests || 0) + " start, " +
        (data.stop_requests || 0) + " stop, " +
        (data.command_errors || 0) + " errors");
      up("streamLast", data.last_age_ms === null ?
        "--" : Math.round(data.last_age_ms / 1000) + " s ago");
      up("streamId", data.stream_id || "--");
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

    function initEvents() {
      if (!window.EventSource) return;
      if (evtSrc) evtSrc.close();

      evtSrc = new EventSource("/api/events");
      evtSrc.addEventListener("status", (event) => {
        try {
          statusData = JSON.parse(event.data);
          renderStatus(statusData);
        } catch (error) {}
      });
      evtSrc.addEventListener("device_boot", () => {
        invalidateSettingsCatalog();
      });
      evtSrc.addEventListener("stream", (event) => {
        try {
          streamData = JSON.parse(event.data);
          renderStream(streamData);
        } catch (error) {}
      });
      evtSrc.addEventListener("console", (event) => {
        try {
          renderConsole(JSON.parse(event.data));
        } catch (error) {}
      });
      evtSrc.addEventListener("live", (event) => {
        try {
          renderLive(JSON.parse(event.data));
        } catch (error) {}
      });
    }

    async function therapy(action) {
      applyTherapyPending(action);
      try {
        const response = await api("/api/therapy", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({action}),
        });
        const data = await response.json();
        if (!data.ok) throw new Error("therapy command queue failed");
        [300, 900, 1800, 3200, 5200].forEach((delay) => {
          setTimeout(loadStatus, delay);
        });
      } catch (error) {
        alert(error.message);
        loadStatus();
      }
    }

    async function timeAction(action) {
      try {
        const response = await api("/api/time", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({action}),
        });
        const data = await response.json();
        msg("timeMsg", data.result, data.ok);
        setTimeout(loadStatus, 900);
      } catch (error) {
        msg("timeMsg", error.message, false);
      }
    }

    async function oxiAction(action, extra, msgId) {
      try {
        const body = Object.assign({action}, extra || {});
        const response = await api("/api/oximetry", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify(body),
        });
        const data = await response.json();
        msg(msgId || "oxiTabMsg", data.result || "Queued", data.ok);
        setTimeout(loadStatus, 500);
        setTimeout(loadStatus, 1500);
        setTimeout(loadOximetrySensors, 700);
        setTimeout(loadOximetrySensors, 2500);
      } catch (error) {
        msg(msgId || "oxiTabMsg", error.message, false);
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
        const response = await api("/api/config", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({oxi_adv: advertiseMode}),
        });
        const data = await response.json();
        msg("oxiTabMsg", data.queued ? "Config update queued" : "Saved", true);
        setTimeout(loadStatus, 600);
      } catch (error) {
        msg("oxiTabMsg", error.message, false);
      }
    }
