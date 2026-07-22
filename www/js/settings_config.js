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
    function valueSpan(text) {
      const value = document.createElement("span");
      value.className = "value";
      value.textContent =
        text === undefined || text === null || text === "" ? "--" : text;
      return value;
    }

    function scheduleSettingsPoll(data) {
      if (settingsPollTimer) {
        clearTimeout(settingsPollTimer);
        settingsPollTimer = null;
      }

      const active = clinicalTabActive();
      const refreshing = Date.now() < settingsRefreshUntil;
      const snapshotPending = !!data.snapshot_pending;
      const needsRefresh = !data.valid && !data.refresh_queued &&
        !snapshotPending && !refreshing;
      if (active && (data.refresh_queued || snapshotPending || refreshing ||
          !data.valid || data.pending_count)) {
        settingsPollTimer = setTimeout(
          () => loadSettings(needsRefresh, !needsRefresh),
          needsRefresh ? 0 : 1200);
      }
    }

    function invalidateSettingsCatalog() {
      settingsCatalog = [];
      settingsComposites = [];
      settingsCatalogPromise = null;
      settingsProfileMode = null;
      settingsModeDirty = false;
      if (clinicalTabActive()) loadSettings(true);
    }

    function modeBit(mode) {
      const value = Number(mode);
      if (!Number.isInteger(value) || value < 0 || value > 15) return 0;
      return 1 << value;
    }

    async function ensureSettingsCatalog() {
      if (settingsCatalog.length) return;
      if (!settingsCatalogPromise) {
        settingsCatalogPromise = api("/api/settings-catalog")
          .then((response) => response.json())
          .then((data) => {
            const catalog = Array.isArray(data.settings) ?
              data.settings : [];
            if (!catalog.some((item) => item && item.kind && item.label)) {
              throw new Error("settings catalog unavailable");
            }
            settingsCatalog = catalog.map((item, index) =>
              Object.assign({_catalogIndex: index}, item));
            settingsComposites = Array.isArray(data.composites) ?
              data.composites.map((item, index) =>
                Object.assign({_catalogIndex: 10000 + index}, item)) : [];
          })
          .finally(() => {
            settingsCatalogPromise = null;
          });
      }
      await settingsCatalogPromise;
    }

    function settingMetaFor(setting, mode) {
      const bit = modeBit(mode);
      let fallback = null;
      for (const meta of settingsCatalog) {
        if (meta.key !== setting.key) continue;
        if (!fallback) fallback = meta;
        if (bit && (Number(meta.modes || 0) & bit)) return meta;
      }
      return fallback;
    }

    function settingsTherapyMode(data) {
      const setting = (data.settings || []).find((item) =>
        item && item.key === "MOP");
      if (!setting || setting.value === undefined || setting.value === null) {
        return null;
      }
      const mode = Number(setting.value);
      return Number.isFinite(mode) ? mode : null;
    }

    function mergeSettingsCatalog(data) {
      const activeMode = settingsTherapyMode(data);
      const mode = settingsModeDirty && settingsProfileMode !== null ?
        settingsProfileMode : activeMode;
      const merged = Object.assign({}, data);
      merged.settings = (data.settings || []).map((setting) => {
        const meta = settingMetaFor(setting, mode);
        return meta ? Object.assign({}, meta, setting) : setting;
      });
      return merged;
    }

    function optionValue(item, index) {
      return item && typeof item === "object" && item.value !== undefined ?
        String(item.value) : String(index);
    }

    function optionLabel(item) {
      return item && typeof item === "object" ?
        (item.label || String(item.value)) : String(item);
    }

    function visibleOptions(setting, supportedMask) {
      const options = setting.options || [];
      if (setting.key !== "MOP" || !supportedMask) return options;
      return options.filter((item, index) =>
        supportedMask & modeBit(optionValue(item, index)));
    }

    function settingRawValue(setting) {
      if (!setting) return "";
      if (setting.pending) return setting.pending_value || "";
      return setting.value || "";
    }

    function compositeOptionValue(item) {
      if (!item || typeof item !== "object") return "";
      if (item.numeric_raw !== undefined && item.numeric_raw !== null) {
        return "n:" + String(item.numeric_raw);
      }
      return "e:" + String(item.enum_value);
    }

    function compositeOptionLabel(item) {
      return item && typeof item === "object" ?
        (item.label || compositeOptionValue(item)) : String(item);
    }

    function compositeCurrentValue(composite, enumSetting, numericSetting) {
      const enumRaw = String(settingRawValue(enumSetting));
      const branch = String(composite.numeric_branch_enum_value);
      if (enumRaw === branch) {
        const numericRaw = String(settingRawValue(numericSetting));
        if (numericRaw !== "") return "n:" + numericRaw;
      }
      return "e:" + enumRaw;
    }

    function compositeDisplayFallback(value) {
      if (String(value).startsWith("n:")) return String(value).slice(2);
      if (String(value).startsWith("e:")) return String(value).slice(2);
      return String(value || "");
    }

    function formatSettingValue(setting, value) {
      if (value === undefined || value === null || value === "") return "";
      const raw = String(value);
      if (setting.kind === "composite") {
        const options = setting.options || [];
        for (let i = 0; i < options.length; i++) {
          if (optionValue(options[i], i) === raw) {
            return optionLabel(options[i]) || raw;
          }
        }
        return compositeDisplayFallback(raw);
      }
      if (setting.kind === "enum") {
        const options = setting.options || [];
        for (let i = 0; i < options.length; i++) {
          if (optionValue(options[i], i) === raw) {
            return optionLabel(options[i]) || raw;
          }
        }
        return raw;
      }
      if (setting.kind === "number") {
        const numeric = Number(raw);
        if (!Number.isFinite(numeric)) return raw;
        const scale = Number(setting.scale_div || 1);
        const displayValue = scale > 1 ? numeric / scale : numeric;
        const decimals = Number(setting.decimals || 0);
        if (Number.isInteger(decimals) && decimals >= 0) {
          return displayValue.toFixed(decimals);
        }
        return String(displayValue);
      }
      return raw;
    }

    function settingAvailable(setting) {
      return setting.available !== false;
    }

    function settingWritable(setting) {
      return setting.writable !== false;
    }

    function settingSmallText(setting) {
      const parts = [];
      if (setting.rpc_name) parts.push(setting.rpc_name);

      if (setting.pending) {
        parts.push("Pending readback; current " +
          (formatSettingValue(setting, setting.value) || "--"));
      } else if (setting.inferred && setting.key !== "MOP") {
        parts.push("Inferred from device status");
      }

      return parts.join("; ");
    }

    function settingGroupRank(group) {
      const order = {
        Therapy: 0,
        Comfort: 1,
        Circuit: 2,
        Preferences: 3,
        Device: 4,
      };
      return Object.prototype.hasOwnProperty.call(order, group) ?
        order[group] : 100;
    }

    function settingCategoryRank(category) {
      const order = {
        therapy: 0,
        pressure: 1,
        pressure_support: 2,
        backup_rate: 3,
        comfort: 4,
        timing: 5,
        trigger_cycle: 6,
        therapy_start_pressure: 10,
        ramp: 11,
        therapy_behavior: 12,
        climate: 13,
        circuit: 20,
        locale: 30,
        patient_access: 31,
        display: 32,
      };
      return Object.prototype.hasOwnProperty.call(order, category) ?
        order[category] : 100;
    }

    function compareSettings(a, b) {
      const groupDelta =
        settingGroupRank(a.group || "") - settingGroupRank(b.group || "");
      if (groupDelta) return groupDelta;

      const categoryDelta =
        settingCategoryRank(a.category || "") -
        settingCategoryRank(b.category || "");
      if (categoryDelta) return categoryDelta;

      return Number(a._catalogIndex || 0) - Number(b._catalogIndex || 0);
    }

    function buildCompositeSettings(settings) {
      const byKey = new Map();
      settings.forEach((setting) => byKey.set(setting.key, setting));

      const hiddenKeys = new Set();
      const composites = [];
      settingsComposites.forEach((composite) => {
        if (composite.kind !== "paired_enum_numeric") return;

        const enumSetting = byKey.get(composite.enum_key);
        const numericSetting = byKey.get(composite.numeric_key);
        if (!enumSetting || !numericSetting) return;

        const available =
          (settingAvailable(enumSetting) || enumSetting.pending) &&
          (settingAvailable(numericSetting) || numericSetting.pending);
        if (!available) return;

        const options = (composite.options || []).map((item) =>
          Object.assign({}, item, {value: compositeOptionValue(item)}));
        const value = compositeCurrentValue(
          composite, enumSetting, numericSetting);

        hiddenKeys.add(composite.enum_key);
        hiddenKeys.add(composite.numeric_key);
        composites.push(Object.assign({}, composite, {
          kind: "composite",
          value,
          pending_value: value,
          options,
          pending: enumSetting.pending || numericSetting.pending,
          writable: settingWritable(enumSetting) &&
            settingWritable(numericSetting),
          available: true,
          enum_setting: enumSetting,
          numeric_setting: numericSetting,
        }));
      });

      return {hiddenKeys, composites};
    }

    async function loadSettings(refresh, poll) {
      try {
        if (refresh) settingsRefreshUntil = Date.now() + 7000;

        const query = [];
        if (refresh) query.push("refresh=1");
        if (poll) query.push("poll=1");
        if (settingsModeDirty && settingsProfileMode !== null) {
          query.push("profile_mode=" + encodeURIComponent(settingsProfileMode));
        }

        const response = await api("/api/settings" +
          (query.length ? "?" + query.join("&") : ""));
        const data = await response.json();
        await ensureSettingsCatalog();
        settingsData = mergeSettingsCatalog(data);
        renderSettings(settingsData, !!refresh);
      } catch (error) {
        msg("settingsMsg", error.message, false);
      }
    }

    function renderSettings(data, requestedRefresh) {
      const parts = [];
      const activeMode = settingsTherapyMode(data);
      if (activeMode !== null) settingsActiveMode = activeMode;
      if (!settingsModeDirty && settingsActiveMode !== null) {
        settingsProfileMode = settingsActiveMode;
      }
      if (settingsModeDirty && settingsProfileMode !== null &&
          settingsActiveMode !== null &&
          settingsProfileMode === settingsActiveMode && !data.pending_count) {
        settingsModeDirty = false;
      }
      if (!requestedRefresh && data.valid && !data.refresh_queued &&
          (data.age_ms || 0) < 2500) {
        settingsRefreshUntil = 0;
      }

      const refreshing = Date.now() < settingsRefreshUntil;
      if (data.refresh_queued || refreshing) {
        parts.push("Refreshing settings");
      } else if (!data.valid) {
        parts.push("Waiting for device");
      }
      if (settingsModeDirty && settingsProfileMode !== null &&
          settingsActiveMode !== null &&
          settingsProfileMode !== settingsActiveMode) {
        parts.push("Unsaved mode change");
      }
      if (data.pending_count) parts.push(data.pending_count + " pending readback");

      const meta = document.getElementById("settingsMeta");
      const metaText = parts.join("; ");
      meta.textContent = metaText;
      meta.style.display = metaText ? "block" : "none";

      const root = document.getElementById("settingsFields");
      const save = document.getElementById("settingsSave");
      const revert = document.getElementById("settingsRevert");
      root.innerHTML = "";
      scheduleSettingsPoll(data);
      if (revert) {
        revert.style.display =
          settingsModeDirty && settingsProfileMode !== null &&
          settingsActiveMode !== null &&
          settingsProfileMode !== settingsActiveMode ? "" : "none";
      }

      const baseVisible = (data.settings || []).filter((setting) =>
        settingAvailable(setting) || setting.inferred || setting.pending);
      const compositeState = buildCompositeSettings(baseVisible);
      const visible = baseVisible.filter((setting) =>
          !compositeState.hiddenKeys.has(setting.key))
        .concat(compositeState.composites)
        .sort(compareSettings);
      if (!visible.length) {
        root.innerHTML = '<div class="value" style="text-align:left">' +
          (data.valid ? "No readable settings for this mode" :
            "Waiting for AS11 settings readback") +
          "</div>";
        if (save) save.disabled = true;
        return;
      }

      let group = "";
      visible.forEach((setting) => {
        const settingGroup = setting.group || "";
        if (settingGroup !== group) {
          group = settingGroup;
          const heading = document.createElement("div");
          heading.className = "section-title";
          heading.textContent = group || "settings";
          root.appendChild(heading);
        }

        let raw = setting.pending ?
          setting.pending_value : (setting.value || "");
        if (setting.key === "MOP" && settingsModeDirty &&
            settingsProfileMode !== null) {
          raw = String(settingsProfileMode);
        }
        const shown = formatSettingValue(setting, raw);
        let control;

        if (setting.kind === "enum" || setting.kind === "composite") {
          control = document.createElement("select");
          let seen = false;
          if (raw === "") {
            const option = document.createElement("option");
            option.value = "";
            option.textContent = "--";
            option.selected = true;
            option.disabled = true;
            control.appendChild(option);
          }
          visibleOptions(setting, data.supported_mode_mask || 0)
            .forEach((item, index) => {
              const label = setting.kind === "composite" ?
                compositeOptionLabel(item) : optionLabel(item);
              const value = optionValue(item, index);
              const option = document.createElement("option");
              option.value = value;
              option.textContent = label;
              if (setting.kind === "composite") {
                option.dataset.enumValue = item.enum_value;
                if (item.numeric_raw !== undefined &&
                    item.numeric_raw !== null) {
                  option.dataset.numericRaw = item.numeric_raw;
                }
              }
              if (value === String(raw)) {
                option.selected = true;
                seen = true;
              }
              control.appendChild(option);
            });
          if (raw !== "" && !seen) {
            const option = document.createElement("option");
            option.value = raw;
            option.textContent = shown || raw;
            option.selected = true;
            control.appendChild(option);
          }
          if (setting.key === "MOP") {
            control.onchange = () => {
              const nextMode = Number(control.value);
              if (!Number.isNaN(nextMode)) {
                settingsProfileMode = nextMode;
                settingsModeDirty = settingsActiveMode !== null &&
                  nextMode !== settingsActiveMode;
                loadSettings(false);
              }
            };
          }
        } else if (setting.kind === "bool") {
          control = document.createElement("select");
          ["false", "true"].forEach((value) => {
            const option = document.createElement("option");
            option.value = value;
            option.textContent = value === "true" ? "On" : "Off";
            const current = String(raw).toLowerCase();
            if (current === value ||
                current === (value === "true" ? "on" : "off")) {
              option.selected = true;
            }
            control.appendChild(option);
          });
        } else {
          control = document.createElement("input");
          control.type = setting.kind === "number" ? "number" : "text";
          control.value = settingAvailable(setting) || setting.inferred ?
            (shown || "") : "";
          if (setting.kind === "number") {
            control.min = setting.min;
            control.max = setting.max;
            control.step = setting.step;
          }
        }

        control.dataset.key = setting.key;
        control.dataset.kind = setting.kind;
        if (setting.kind === "composite") {
          control.dataset.enumKey = setting.enum_key;
          control.dataset.numericKey = setting.numeric_key;
          control.dataset.numericScaleDiv =
            setting.numeric_setting && setting.numeric_setting.scale_div ?
              setting.numeric_setting.scale_div : 1;
        }
        control.dataset.orig =
          setting.kind === "enum" || setting.kind === "bool" ?
            (raw || "") : (control.value || "");
        if (setting.kind === "composite") control.dataset.orig = raw || "";
        if (!settingAvailable(setting) || !settingWritable(setting)) {
          control.disabled = true;
        }
        if (setting.pending) control.classList.add("pending");

        const entry = row(
          setting.label || setting.key,
          control,
          settingSmallText(setting));
        if (setting.pending) entry.classList.add("pending");
        root.appendChild(entry);
      });

      if (save) save.disabled = !root.querySelector("[data-key]:not(:disabled)");
    }

    async function saveSettings() {
      const changes = {};
      if (settingsModeDirty && settingsProfileMode !== null &&
          settingsActiveMode !== null &&
          settingsProfileMode !== settingsActiveMode) {
        changes.MOP = settingsProfileMode;
      }
      document.querySelectorAll("#settingsFields [data-key]").forEach((input) => {
        if (input.disabled) return;
        if (String(input.value) === String(input.dataset.orig)) return;

        if (input.dataset.kind === "number") {
          if (input.value === "") return;
          changes[input.dataset.key] = Number(input.value);
        } else if (input.dataset.kind === "bool") {
          changes[input.dataset.key] = input.value === "true";
        } else if (input.dataset.kind === "enum") {
          if (input.value === "") return;
          changes[input.dataset.key] = Number(input.value);
        } else if (input.dataset.kind === "composite") {
          const selected = input.selectedOptions && input.selectedOptions[0];
          if (!selected || selected.dataset.enumValue === undefined) return;

          changes[input.dataset.enumKey] = Number(selected.dataset.enumValue);
          if (selected.dataset.numericRaw !== undefined) {
            const numeric = Number(selected.dataset.numericRaw);
            const scale = Number(input.dataset.numericScaleDiv || 1);
            changes[input.dataset.numericKey] =
              scale > 1 ? numeric / scale : numeric;
          }
        } else {
          changes[input.dataset.key] = input.value;
        }
      });

      if (!Object.keys(changes).length) {
        msg("settingsMsg", "No changes", true);
        return;
      }

      try {
        const response = await api("/api/settings", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify(changes),
        });
        const data = await response.json();
        msg("settingsMsg",
          data.ok ? "Settings queued" : "Queue failed",
          data.ok);
        settingsRefreshUntil = Date.now() + 7000;
        setTimeout(() => loadSettings(true), 1200);
      } catch (error) {
        msg("settingsMsg", error.message, false);
      }
    }

    function revertSettingsDraft() {
      if (settingsActiveMode === null) return;
      settingsProfileMode = settingsActiveMode;
      settingsModeDirty = false;
      loadSettings(false);
    }

    function renderWifiCurrent(data) {
      const root = document.getElementById("wifiCurrent");
      if (!root) return;
      root.innerHTML = "";

      const status = statusData || {};
      const state = data.state || status.wifi_state || "--";
      const ssid = data.ssid || status.wifi_ssid || "";
      const ip = data.ip || status.wifi_ip || "";
      const channel = data.channel || status.wifi_channel || 0;
      const bssid = data.bssid || status.wifi_bssid || "";
      const rssi = data.rssi !== undefined ? data.rssi : status.wifi_rssi;
      const roam = data.roam !== undefined ? data.roam : status.wifi_roam;
      const active = Number.isFinite(Number(data.active)) ?
        Number(data.active) : Number(status.wifi_profile);

      root.appendChild(row("State", valueSpan(state),
        roam ? "roaming on" : "roaming off"));
      root.appendChild(row("SSID", valueSpan(ssid),
        Number.isFinite(active) && active >= 0 ? "profile " + active : ""));
      root.appendChild(row("Signal", wifiSignalValue(rssi),
        channel > 0 ? "channel " + channel : ""));
      root.appendChild(row("IP", valueSpan(ip),
        bssid && bssid !== "00:00:00:00:00:00" ? bssid : ""));
    }

    async function loadWifi() {
      try {
        const response = await api("/api/wifi");
        const data = await response.json();
        renderWifiCurrent(data);
        const root = document.getElementById("wifiProfiles");
        root.innerHTML = "";
        if (!data.profiles.length) {
          root.innerHTML = '<div class="value" style="text-align:left">No profiles</div>';
        }
        data.profiles.forEach((profile, index) => {
          const button = document.createElement("button");
          button.className = "btn danger";
          button.textContent = "Remove";
          button.onclick = () => wifiRemove(index);
          root.appendChild(row(profile.ssid, button,
            (index === data.active ? "active, " : "") +
            (profile.open ? "open" : "password")));
        });
      } catch (error) {
        msg("wifiMsg", error.message, false);
      }
    }

    async function wifiAction(action, extra) {
      try {
        const body = Object.assign({action}, extra || {});
        const response = await api("/api/wifi", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify(body),
        });
        const data = await response.json();
        msg("wifiMsg", data.result, data.ok);
        setTimeout(loadWifi, 600);
        setTimeout(loadStatus, 900);
      } catch (error) {
        msg("wifiMsg", error.message, false);
      }
    }

    function wifiAdd() {
      wifiAction("add", {
        ssid: document.getElementById("wifiSsid").value,
        pass: document.getElementById("wifiPass").value,
      });
    }

    function wifiRemove(index) {
      wifiAction("remove", {index});
    }

    let otaReloadTimer = null;

    function scheduleOtaReload() {
      if (otaReloadTimer ||
          (location.protocol !== "http:" && location.protocol !== "https:")) {
        return;
      }

      const started = Date.now();
      const path = location.pathname || "/";
      const check = async () => {
        try {
          const response = await fetch(path, {cache: "no-store"});
          if (response.ok) {
            location.reload();
            return;
          }
        } catch (error) {}

        if (Date.now() - started < 120000) {
          otaReloadTimer = setTimeout(check, 2000);
        } else {
          otaReloadTimer = null;
        }
      };

      otaReloadTimer = setTimeout(check, 6000);
    }

    async function loadOta() {
      try {
        const response = await api("/api/ota");
        const data = await response.json();
        renderOta(data);

        const resmedResponse = await api("/api/resmed-ota");
        renderResmedOta(await resmedResponse.json());
        loadResmedRepository(true);
      } catch (error) {
        msg("otaMsg", error.message, false);
      }
    }

    function renderOta(data) {
      up("otaVersion", data.version || "--");
      const active = data.http_prepare_pending || data.http_prepared ||
        data.http_active || data.http_ready || data.url_active ||
        data.update_check_active || data.reboot_pending ||
        data.method === "http" || data.method === "http_prepare" ||
        data.method === "url" || data.method === "arduino";

      let latest = "--";
      if (!data.update_check_enabled) {
        latest = "Checks disabled";
      } else if (data.update_check_active) {
        latest = "Checking...";
      } else if (data.update_check_pending) {
        latest = "Check queued";
      } else if (data.update_version) {
        latest = data.update_version +
          (data.update_available ? " available" : " (current)");
      }
      up("otaLatest", latest);

      const check = document.getElementById("otaCheckUpdate");
      if (check) {
        check.disabled = !data.update_check_enabled ||
          data.update_check_active || data.update_check_pending || active;
      }

      const installUpdate = document.getElementById("otaInstallUpdate");
      if (installUpdate) {
        installUpdate.hidden = !data.update_available;
        installUpdate.textContent = data.update_version
          ? "Install " + data.update_version
          : "Install update";
        installUpdate.disabled = !data.update_installable || active;
      }
      const install = document.getElementById("otaInstall");
      if (install) install.disabled = active;

      if (!active && !(data.bytes || data.progress)) {
        up("otaProgress", "--");
        return;
      }
      if (data.url_active && !data.total_size && !data.wire_total_size) {
        up("otaProgress", "Resolving firmware URL...");
        return;
      }
      if (data.http_prepare_pending) {
        const size = data.total_size || data.wire_total_size || 0;
        up("otaProgress", "Preparing / " + fmtBytes(size));
        return;
      }
      if (data.http_prepared && !data.http_active) {
        const size = data.total_size || data.wire_total_size || 0;
        up("otaProgress", "Ready / " + fmtBytes(size));
        return;
      }
      let text;
      if (data.encoding === "zlib" && data.wire_total_size) {
        text = (data.progress || 0) + "% / " +
          fmtBytes(data.wire_bytes || 0) + " wire, " +
          fmtBytes(data.bytes || 0) + " raw";
      } else {
        text = (data.progress || 0) + "% / " + fmtBytes(data.bytes || 0);
      }
      up("otaProgress", text);

      const progress = document.getElementById("otaUploadProgress");
      const bar = document.getElementById("otaUploadBar");
      if (progress && bar && active) {
        progress.style.display = "block";
        bar.style.width = Math.min(100, data.progress || 0) + "%";
      }
    }

    async function otaCheckForUpdates() {
      const button = document.getElementById("otaCheckUpdate");
      if (button) button.disabled = true;
      msg("otaUpdateMsg", "Checking for updates...", true, true);

      let data = null;
      try {
        let response = await fetch("/api/ota/check", {method: "POST"});
        data = await response.json();
        renderOta(data);
        if (!response.ok) {
          throw new Error(data.update_error || "Update check rejected");
        }

        const started = Date.now();
        while (data.update_check_pending || data.update_check_active) {
          if (data.update_check_pending && !data.update_check_active &&
              Date.now() - started > 10000) {
            msg("otaUpdateMsg",
              "Check queued until the device is idle and network is available.",
              true, true);
            return;
          }
          if (Date.now() - started > 60000) {
            throw new Error("Update check timed out");
          }

          await new Promise(resolve => setTimeout(resolve, 1000));
          response = await api("/api/ota");
          data = await response.json();
          renderOta(data);
        }

        if (data.update_error) throw new Error(data.update_error);
        if (data.update_available) {
          msg("otaUpdateMsg",
            "Version " + data.update_version + " is available.", true);
        } else {
          msg("otaUpdateMsg", "This device is up to date.", true);
        }
      } catch (error) {
        msg("otaUpdateMsg", error.message, false);
      } finally {
        if (button) {
          button.disabled = !data || !data.update_check_enabled ||
            data.update_check_pending || data.update_check_active;
        }
      }
    }

    function setOtaUploadProgress(percent, bytes) {
      up("otaProgress", percent + "% / " + fmtBytes(bytes || 0));
    }

    function otaSourceChanged(source) {
      const urlInput = document.getElementById("otaUrl");
      const fileInput = document.getElementById("otaFile");

      if (source === "url" && urlInput.value.trim()) {
        fileInput.value = "";
      } else if (source === "file" && fileInput.files.length) {
        urlInput.value = "";
      }
    }

    function otaUrlQuery(url) {
      let parsed;
      try {
        parsed = new URL(url);
      } catch (error) {
        throw new Error("Enter a valid firmware URL");
      }
      if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
        throw new Error("Firmware URL must use HTTP or HTTPS");
      }

      return new URLSearchParams({url}).toString();
    }

    async function otaRunUrlUpdate(endpoint, messageId) {
      let response = await fetch(endpoint, {method: "POST"});
      let data = await response.json();
      renderOta(data);
      if (!response.ok) {
        throw new Error(data.last_error || data.error || "URL update rejected");
      }

      const started = Date.now();
      while (true) {
        if (data.last_error && !data.url_active && !data.reboot_pending) {
          throw new Error(data.last_error);
        }
        if (data.reboot_pending || data.http_ready) {
          msg(messageId, "Update installed. Restarting...", true, true);
          scheduleOtaReload();
          return true;
        }
        if (Date.now() - started > 15 * 60 * 1000) {
          throw new Error("URL update timed out");
        }

        await new Promise(resolve => setTimeout(resolve, 500));
        response = await api("/api/ota");
        data = await response.json();
        renderOta(data);
      }
    }

    async function otaInstallFromUrl(url) {
      const query = otaUrlQuery(url);
      msg("otaMsg", "Starting URL update...", true, true);
      return otaRunUrlUpdate("/api/ota/url?" + query, "otaMsg");
    }

    async function otaInstallAvailableUpdate() {
      const button = document.getElementById("otaInstallUpdate");
      if (button) button.disabled = true;
      msg("otaUpdateMsg", "Starting release update...", true, true);

      let restarting = false;
      try {
        restarting = await otaRunUrlUpdate(
          "/api/ota/install-update", "otaUpdateMsg");
      } catch (error) {
        msg("otaUpdateMsg", "Update failed: " + error.message, false, true);
        loadOta();
      } finally {
        if (button && !restarting) button.disabled = false;
      }
    }

    function otaUploadQuery(plan) {
      const params = new URLSearchParams();
      params.set("encoding", "auto");
      params.set("wire_size", String(plan.wireSize));
      return params.toString();
    }

    function otaUploadPlan(file) {
      return {wireSize: file.size};
    }

    async function prepareOtaUpload(plan) {
      const prepareResponse = await api("/api/ota/prepare?" +
        otaUploadQuery(plan), {method: "POST"});
      let data = await prepareResponse.json();
      renderOta(data);

      const started = Date.now();
      while (!data.http_prepared) {
        if (data.last_error) throw new Error(data.last_error);
        if (!data.http_prepare_pending) {
          throw new Error("OTA prepare did not start");
        }
        if (Date.now() - started > 15000) {
          throw new Error("OTA prepare timed out");
        }
        await new Promise(resolve => setTimeout(resolve, 250));
        const pollResponse = await api("/api/ota");
        data = await pollResponse.json();
        renderOta(data);
      }
      return data;
    }

    async function otaInstallFromFile(file) {
      const plan = otaUploadPlan(file);
      const bar = document.getElementById("otaUploadBar");

      msg("otaMsg", "Preparing OTA...", true, true);
      await prepareOtaUpload(plan);
      msg("otaMsg", "Uploading...", true, true);

      return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.upload.onprogress = (event) => {
          if (event.lengthComputable) {
            const percent = Math.min(100,
              Math.floor(event.loaded / event.total * 100));
            const bytes = Math.min(file.size, event.loaded);
            bar.style.width = percent + "%";
            setOtaUploadProgress(percent, bytes);
          }
        };
        xhr.onload = () => {
          try {
            const data = JSON.parse(xhr.responseText || "{}");
            if (xhr.status >= 300) {
              renderOta(data);
              reject(new Error(data.last_error || xhr.statusText));
              return;
            }

            bar.style.width = "100%";
            setOtaUploadProgress(100, data.wire_bytes || file.size);

            const restarting = data.reboot_pending || data.http_ready;
            msg("otaMsg", restarting ? "Update installed. Restarting..." :
              "Upload finished", restarting, true);
            if (restarting) {
              scheduleOtaReload();
            }
            resolve(restarting);
          } catch (error) {
            reject(error);
          }
        };
        xhr.onerror = () => reject(new Error("Upload error"));
        xhr.onabort = () => reject(new Error("Upload aborted"));
        xhr.open("POST", "/api/ota/upload?" + otaUploadQuery(plan));

        const form = new FormData();
        form.append("firmware", file);
        xhr.send(form);
      });
    }

    async function otaInstall() {
      const url = document.getElementById("otaUrl").value.trim();
      const file = document.getElementById("otaFile").files[0];

      if (!url && !file) {
        msg("otaMsg", "Enter a firmware URL or select an image", false);
        return;
      }
      if (url && file) {
        msg("otaMsg", "Choose either a firmware URL or an image", false);
        return;
      }

      const button = document.getElementById("otaInstall");
      const progress = document.getElementById("otaUploadProgress");
      const bar = document.getElementById("otaUploadBar");
      button.disabled = true;
      progress.style.display = "block";
      bar.style.width = "0%";
      setOtaUploadProgress(0, 0);

      let restarting = false;
      try {
        restarting = file ? await otaInstallFromFile(file) :
          await otaInstallFromUrl(url);
      } catch (error) {
        msg("otaMsg", "Update failed: " + error.message, false, true);
        loadOta();
      } finally {
        if (!restarting) button.disabled = false;
      }
    }

    function resmedOtaStatusText(data) {
      const target = data.target && data.target !== "ABC" ? " " + data.target : "";
      const prepareTarget = data.prepare_target ? " " + data.prepare_target : "";
      if (data.prepare_state === "queued") return "Preparation queued";
      if (data.prepare_state === "inspecting") {
        return "Inspecting image" + prepareTarget;
      }
      if (data.prepare_state === "converting") {
        return "Building ABC image" + prepareTarget;
      }
      if (data.prepare_state === "publishing") return "Saving ABC image";
      if (data.phase === "opening") return "Opening prepared image" + target;
      if (data.phase === "initiating") return "Starting device upload" + target;
      if (data.phase === "ready" || data.phase === "uploading") {
        return "Sending to ResMed" + target;
      }
      if (data.phase === "uploaded" || data.phase === "checking") {
        return "Verifying on ResMed";
      }
      if (data.phase === "verified") return "Upload complete. Ready to apply.";
      if (data.phase === "applying") return "Applying firmware";
      if (data.phase === "complete") return "Apply complete";
      if (data.phase === "error") return data.last_error || "ResMed OTA failed";
      return data.phase || "--";
    }

    function resmedOtaTransferActive(data) {
      return data.phase === "ready" || data.phase === "uploading";
    }

    function resetResmedOtaRate(data, now, bytes, total) {
      resmedOtaRate.phase = data.phase || "";
      resmedOtaRate.total = total || 0;
      resmedOtaRate.bytes = bytes || 0;
      resmedOtaRate.time = now || performance.now();
      resmedOtaRate.bps = 0;
    }

    function resmedOtaRateText(data) {
      const now = performance.now();
      const bytes = Number(data.uploaded_bytes || 0);
      const total = Number(data.total_size || 0);
      if (!resmedOtaTransferActive(data) || total <= 0) {
        resetResmedOtaRate(data, now, bytes, total);
        return "";
      }

      if (resmedOtaRate.phase !== data.phase ||
          resmedOtaRate.total !== total ||
          bytes < resmedOtaRate.bytes) {
        resetResmedOtaRate(data, now, bytes, total);
        return "";
      }

      const elapsed = (now - resmedOtaRate.time) / 1000;
      if (bytes > resmedOtaRate.bytes && elapsed > 0) {
        const sample = (bytes - resmedOtaRate.bytes) / elapsed;
        resmedOtaRate.bps =
          resmedOtaRate.bps ? (resmedOtaRate.bps * 0.7 + sample * 0.3) : sample;
        resmedOtaRate.bytes = bytes;
        resmedOtaRate.time = now;
      }

      if (!resmedOtaRate.bps) return "";
      const remaining = Math.max(0, total - bytes);
      const eta = fmtDuration(remaining / resmedOtaRate.bps);
      return fmtBytes(resmedOtaRate.bps) + "/s" + (eta ? " ETA " + eta : "");
    }

    function renderResmedOta(data) {
      const rateText = resmedOtaRateText(data);
      const stateText = resmedOtaStatusText(data) +
        (data.waiting ? " / waiting" : "");
      const progressValue = data.prepare_active ?
        Number(data.prepare_progress || 0) : Number(data.progress || 0);
      const progressText = progressValue + "%" +
        (rateText ? " " + rateText : "");
      up("resmedOtaState", stateText);
      up("resmedOtaProgress", progressText);
      up("resmedOtaHash", data.computed_sha256 || data.expected_sha256 || "--");

      const apply = document.getElementById("resmedOtaApplyBtn");
      if (apply) {
        const canApply = data.phase === "verified" && !data.waiting;
        const applying = data.phase === "applying";
        const complete = data.phase === "complete";
        apply.style.display = canApply || applying || complete ? "" : "none";
        apply.disabled = !canApply;
        apply.textContent = applying ? "Applying" : complete ? "Applied" : "Apply";
      }

      const cancel = document.getElementById("resmedOtaCancelBtn");
      if (cancel) {
        const cancellable = (data.active || resmedDirectUploadBusy) &&
          data.phase !== "applying";
        cancel.style.display = cancellable ? "" : "none";
        cancel.disabled = !cancellable;
      }

      const upload = document.getElementById("resmedOtaUploadBtn");
      if (upload) upload.disabled = data.active || resmedDirectUploadBusy;

      const progress = document.getElementById("resmedOtaUploadProgress");
      const bar = document.getElementById("resmedOtaUploadBar");
      progress.style.display =
        data.active || resmedDirectUploadBusy || data.phase === "complete" ||
          data.phase === "verified" ?
          "block" : "none";
      bar.style.width = progressValue + "%";
      if (data.prepare_error || data.last_error) {
        msg("resmedOtaMsg", data.prepare_error || data.last_error,
          false, true);
      } else if (data.phase === "verified") {
        msg("resmedOtaMsg", "Upload complete. Ready to apply.", true, true);
      } else if (data.phase === "complete") {
        msg("resmedOtaMsg", "Apply complete", true, true);
      } else if (data.active) {
        msg("resmedOtaMsg", stateText, true, true);
      }
    }

    function resmedRepositoryUploadProgress(name, committed, total,
                                             fileIndex, fileCount) {
      storageUploadProgress(name, committed, total, fileIndex, fileCount);

      const safeTotal = Math.max(0, Number(total) || 0);
      const safeCommitted = Math.min(safeTotal,
        Math.max(0, Number(committed) || 0));
      const nameNode = document.getElementById("resmedRepositoryUploadName");
      const amountNode = document.getElementById("resmedRepositoryUploadAmount");
      const bar = document.getElementById("resmedRepositoryUploadBar");
      if (nameNode) {
        nameNode.textContent = (fileCount > 1 ?
          (fileIndex + 1) + "/" + fileCount + " " : "") + name;
      }
      if (amountNode) {
        amountNode.textContent = fmtBytes(safeCommitted) + " / " +
          fmtBytes(safeTotal);
      }
      if (bar) {
        bar.max = Math.max(1, safeTotal);
        bar.value = safeCommitted;
      }
    }

    function setResmedRepositoryUploadBusy(busy) {
      const progress = document.getElementById(
        "resmedRepositoryUploadProgress");
      const add = document.getElementById("resmedRepositoryAddBtn");
      if (progress) progress.hidden = !busy;
      if (add) add.disabled = !!busy;
    }

    function renderResmedRepository(data) {
      up("resmedRepositoryPath", data.directory ||
        "/aircannect/resmed-firmware");

      const list = document.getElementById("resmedRepositoryList");
      if (!list) return;
      list.textContent = "";

      const entries = Array.isArray(data.entries) ? data.entries : [];
      if (!entries.length) {
        const empty = document.createElement("div");
        empty.className = "storage-empty";
        empty.textContent = data.error ||
          (data.refresh_pending ? "Refreshing" : "No firmware images");
        list.appendChild(empty);
      }

      entries.forEach((entry) => {
        const row = document.createElement("div");
        row.className = "storage-entry";

        const marker = document.createElement("span");
        marker.textContent = entry.kind === "abc" ? "ABC" :
          entry.kind === "raw" ? "RAW" : "?";
        marker.className = "storage-meta";

        const details = document.createElement("div");
        const name = document.createElement("div");
        name.className = "storage-name";
        name.textContent = entry.name || entry.path || "--";
        const meta = document.createElement("div");
        meta.className = "storage-meta";
        meta.textContent = fmtBytes(entry.size) +
          (fmtStorageModified(entry.modified) ?
            " / " + fmtStorageModified(entry.modified) : "");
        details.appendChild(name);
        details.appendChild(meta);

        const actions = document.createElement("div");
        actions.className = "resmed-repository-actions";
        const prepare = document.createElement("button");
        prepare.className = "btn primary";
        prepare.textContent = "Upload";
        prepare.onclick = () => resmedRepositoryPrepare(
          entry.path, entry.name || entry.path);
        actions.appendChild(prepare);

        const remove = document.createElement("button");
        remove.className = "btn danger";
        remove.textContent = "Remove";
        remove.onclick = () => resmedRepositoryRemove(entry.path,
          entry.name || entry.path);
        actions.appendChild(remove);

        row.appendChild(marker);
        row.appendChild(details);
        row.appendChild(actions);
        list.appendChild(row);
      });

      const refresh = document.getElementById("resmedRepositoryRefreshBtn");
      if (refresh) {
        refresh.disabled = !!data.refresh_pending ||
          ["preparing", "scanning", "removing"].includes(data.state);
      }

      if (data.error) {
        msg("resmedRepositoryMsg", data.error, false, true);
      }
    }

    async function fetchResmedRepository(refresh) {
      const entries = [];
      let offset = 0;
      let result = null;
      for (let page = 0; page < 4; page++) {
        const url = "/api/resmed-ota/repository?offset=" + offset +
          "&limit=128" + (refresh && page === 0 ? "&refresh=1" : "");
        const response = await fetch(url, {cache: "no-store"});
        const data = await response.json();
        if (!response.ok && response.status !== 202) {
          throw new Error(data.error || ("HTTP " + response.status));
        }
        if (!result) result = data;
        if (Array.isArray(data.entries)) entries.push(...data.entries);
        if (!data.more) break;
        offset += Number(data.count) || 0;
        if (!Number(data.count)) break;
      }
      result = result || {};
      result.entries = entries;
      return result;
    }

    async function loadResmedRepository(refresh) {
      if (resmedRepositoryPollTimer) {
        clearTimeout(resmedRepositoryPollTimer);
        resmedRepositoryPollTimer = null;
      }

      try {
        const data = await fetchResmedRepository(!!refresh);
        renderResmedRepository(data);
        if (data.refresh_pending ||
            ["idle", "preparing", "scanning", "removing"].includes(data.state)) {
          resmedRepositoryPollTimer = setTimeout(
            () => loadResmedRepository(false), 500);
        }
      } catch (error) {
        msg("resmedRepositoryMsg", error.message, false, true);
      }
    }

    function resmedRepositoryChooseFiles() {
      if (storageUploadBusy) return;
      const input = document.getElementById("resmedRepositoryInput");
      if (input) input.click();
    }

    function resmedRepositoryFilesSelected(input) {
      const files = input && input.files ? Array.from(input.files) : [];
      if (input) input.value = "";
      if (!files.length || storageUploadBusy) return;
      resmedRepositoryUploadQueue(files);
    }

    async function resmedRepositoryUploadQueue(files) {
      storageUploadCancelRequested = false;
      storageUploadSetBusy(true);
      setResmedRepositoryUploadBusy(true);
      let uploaded = 0;
      try {
        for (let index = 0; index < files.length; index++) {
          if (storageUploadCancelRequested) break;
          const file = files[index];
          resmedRepositoryUploadProgress(file.name, 0, file.size,
            index, files.length);
          if (await storageUploadFile(
              file, "/aircannect/resmed-firmware", index, files.length,
              resmedRepositoryUploadProgress)) {
            uploaded++;
          }
        }

        if (storageUploadCancelRequested) {
          msg("resmedRepositoryMsg", "Upload cancelled", true, false);
        } else {
          msg("resmedRepositoryMsg", "Added " + uploaded + " image" +
            (uploaded === 1 ? "" : "s"), true, false);
        }
        await loadResmedRepository(true);
      } catch (error) {
        msg("resmedRepositoryMsg", error.message, false, true);
      } finally {
        storageUploadCurrentId = 0;
        storageUploadSetBusy(false);
        setResmedRepositoryUploadBusy(false);
      }
    }

    async function resmedRepositoryCancelUpload() {
      storageUploadCancelRequested = true;
      const id = storageUploadCurrentId;
      try {
        if (id) {
          await storageUploadRequest("/api/storage/upload/cancel?id=" +
            encodeURIComponent(id), {method: "POST"});
        }
      } catch (_) {}
    }

    async function resmedRepositoryRemove(path, name) {
      if (!confirm("Remove " + name + " from the firmware repository?")) {
        return;
      }

      try {
        const response = await fetch("/api/resmed-ota/repository/remove", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({path}),
        });
        const data = await response.json();
        if (!response.ok) throw new Error(data.error || "Remove failed");
        await loadResmedRepository(false);
      } catch (error) {
        msg("resmedRepositoryMsg", error.message, false, true);
      }
    }

    async function resmedRepositoryPrepare(path, name) {
      try {
        msg("resmedRepositoryMsg", "Preparing " + name, true, true);
        await postResmedOta("/api/resmed-ota/prepare", {
          path,
          filename: name,
          transient: false,
        });
        await waitResmedOta((data) =>
          data.phase === "verified" || data.phase === "complete", 4200);
        msg("resmedRepositoryMsg",
          "Firmware uploaded to the device and ready to apply", true, true);
      } catch (error) {
        msg("resmedRepositoryMsg", error.message, false, true);
      }
    }

    function sleep(ms) {
      return new Promise((resolve) => setTimeout(resolve, ms));
    }

    async function getResmedOta() {
      const response = await api("/api/resmed-ota");
      const data = await response.json();
      renderResmedOta(data);
      return data;
    }

    async function waitResmedOta(predicate, attempts) {
      for (let index = 0; index < (attempts || 360); index++) {
        const data = await getResmedOta();
        if (data.phase === "error") {
          throw new Error(data.last_error || "ResMed OTA failed");
        }
        if (data.prepare_state === "error" ||
            data.prepare_state === "cancelled") {
          throw new Error(data.prepare_error || "Image preparation failed");
        }
        if (predicate(data)) return data;
        await sleep(500);
      }
      throw new Error("ResMed OTA timeout");
    }

    async function postResmedOta(url, body) {
      const response = await api(url, {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(body || {}),
      });
      const data = await response.json();
      if (!response.ok) {
        throw new Error(data.error || data.last_error ||
          ("HTTP " + response.status));
      }
      if (data.queued || data.result === "queued") {
        setTimeout(getResmedOta, 300);
        return data;
      }
      renderResmedOta(data);
      return data;
    }

    function resmedDirectUploadProgress(name, committed, total) {
      const safeTotal = Math.max(0, Number(total) || 0);
      const safeCommitted = Math.min(safeTotal,
        Math.max(0, Number(committed) || 0));
      const percent = safeTotal > 0 ?
        Math.round((safeCommitted * 100) / safeTotal) : 0;
      const progress = document.getElementById("resmedOtaUploadProgress");
      const bar = document.getElementById("resmedOtaUploadBar");
      const cancel = document.getElementById("resmedOtaCancelBtn");
      const upload = document.getElementById("resmedOtaUploadBtn");

      up("resmedOtaState", "Uploading " + name);
      up("resmedOtaProgress", percent + "% " + fmtBytes(safeCommitted) +
        " / " + fmtBytes(safeTotal));
      if (progress) progress.style.display = "block";
      if (bar) bar.style.width = percent + "%";
      if (cancel) {
        cancel.style.display = "";
        cancel.disabled = false;
      }
      if (upload) upload.disabled = true;
    }

    async function resmedOtaUpload() {
      const file = document.getElementById("resmedOtaFile").files[0];
      if (!file) {
        msg("resmedOtaMsg", "Select firmware image", false, true);
        return;
      }

      try {
        const current = await getResmedOta();
        if (current.active || current.phase === "verified") {
          throw new Error("Another ResMed firmware operation is active");
        }

        storageUploadCancelRequested = false;
        storageUploadSetBusy(true);
        resmedDirectUploadBusy = true;
        const upload = document.getElementById("resmedOtaUploadBtn");
        if (upload) upload.disabled = true;
        msg("resmedOtaMsg", "Uploading", true, true);
        const uploaded = await storageUploadFile(
          file, "/aircannect", 0, 1, resmedDirectUploadProgress, {
            filename: "resmed-ota-input.image",
            conflict: "replace",
            confirmReplace: false,
          });
        if (!uploaded) throw new Error("Upload was not completed");

        storageUploadCurrentId = 0;
        storageUploadSetBusy(false);
        resmedDirectUploadBusy = false;
        await postResmedOta("/api/resmed-ota/prepare", {
          path: "/aircannect/resmed-ota-input.image",
          filename: file.name,
          transient: true,
        });
        const result = await waitResmedOta((data) =>
          data.phase === "complete" || data.phase === "verified", 4200);
        msg("resmedOtaMsg",
          result.phase === "complete" ?
            "Apply complete" : "Upload complete. Ready to apply.",
          true, true);
      } catch (error) {
        msg("resmedOtaMsg", error.message, false, true);
        loadOta();
      } finally {
        storageUploadCurrentId = 0;
        storageUploadSetBusy(false);
        resmedDirectUploadBusy = false;
        setTimeout(getResmedOta, 0);
      }
    }

    async function resmedOtaApply() {
      if (!confirm("Apply uploaded firmware to the ResMed device now?")) {
        return;
      }

      try {
        await postResmedOta("/api/resmed-ota/apply", {
          mode: "plain",
          reset: false,
          confirm: "APPLY_RESMED_OTA",
        });
        await waitResmedOta((data) =>
          data.phase === "complete" || data.phase === "error", 240);
        msg("resmedOtaMsg", "Apply complete", true, true);
      } catch (error) {
        msg("resmedOtaMsg", error.message, false, true);
      }
    }

    async function resmedOtaCancel() {
      if (!confirm("Cancel the current ResMed firmware operation?")) {
        return;
      }

      try {
        storageUploadCancelRequested = true;
        if (storageUploadCurrentId) {
          try {
            await storageUploadRequest(
              "/api/storage/upload/cancel?id=" +
                encodeURIComponent(storageUploadCurrentId),
              {method: "POST"});
          } catch (_) {}
        }
        await postResmedOta("/api/resmed-ota/abort", {});
        msg("resmedOtaMsg", "Cancelled", false, true);
        setTimeout(getResmedOta, 300);
      } catch (error) {
        msg("resmedOtaMsg", error.message, false, true);
      }
    }

    async function loadConsole(showError) {
      try {
        const response = await api("/api/console");
        const before = consoleSeq;
        renderConsole(await response.json());
        return consoleSeq !== before;
      } catch (error) {
        if (showError) {
          const output = document.getElementById("consoleLog");
          output.textContent += "\nERR: " + error.message + "\n";
          output.scrollTop = output.scrollHeight;
        }
      }
      return false;
    }

    function renderConsole(data) {
      const seq = Number(data && data.seq);
      if (!data || !Number.isFinite(seq) || seq <= consoleSeq) return;
      const output = document.getElementById("consoleLog");
      if (data.reset || data.log !== undefined) {
        output.textContent = data.log || "";
        consoleEnd = Number(data.end ?? output.textContent.length);
        consoleSeq = seq;
      } else if (data.append !== undefined) {
        const from = Number(data.from ?? consoleEnd);
        const to = Number(data.to ?? (from + (data.append || "").length));
        if (from !== consoleEnd) {
          if (to > consoleEnd) setTimeout(() => loadConsole(false), 0);
          return;
        }
        output.textContent += data.append || "";
        if (output.textContent.length > 4096) {
          output.textContent = output.textContent.slice(-4096);
        }
        consoleEnd = to;
        consoleSeq = seq;
      }
      output.scrollTop = output.scrollHeight;
    }

    async function sendConsoleCommand() {
      const input = document.getElementById("consoleInput");
      const command = input.value.trim();
      if (!command) return;
      input.value = "";

      try {
        const response = await api("/api/console", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({cmd: command}),
        });
        const data = await response.json();
        if (data.log !== undefined) renderConsole(data);
        setTimeout(() => loadConsole(true), 300);
      } catch (error) {
        const output = document.getElementById("consoleLog");
        output.textContent += "\nERR: " + error.message + "\n";
        output.scrollTop = output.scrollHeight;
      }
    }

    async function clearConsoleLog() {
      try {
        await api("/api/console/clear", {method: "POST"});
        const output = document.getElementById("consoleLog");
        output.textContent = "";
        consoleSeq = -1;
        consoleEnd = 0;
        output.scrollTop = 0;
      } catch (error) {
        const output = document.getElementById("consoleLog");
        output.textContent += "\nERR: " + error.message + "\n";
        output.scrollTop = output.scrollHeight;
      }
    }

    function normalizeConfigSchema(data) {
      const groups = Array.isArray(data && data.groups) ? data.groups : [];
      configSections = groups.map((group) => ({
        id: group.id,
        title: group.label || group.id,
        fields: Array.isArray(group.fields) ? group.fields : [],
      })).filter((group) => group.id);
      rebuildConfigSectionMaps();
    }

    async function loadConfigSchema() {
      const response = await api("/api/config/schema");
      normalizeConfigSchema(await response.json());
      return configSections;
    }

    async function ensureConfigSchema() {
      if (configSections.length) return configSections;
      if (!configSchemaPromise) {
        configSchemaPromise = loadConfigSchema().finally(() => {
          configSchemaPromise = null;
        });
      }
      return await configSchemaPromise;
    }

    async function fetchConfigSection(sectionId) {
      const response = await api("/api/config/" + encodeURIComponent(sectionId));
      return await response.json();
    }

    async function fetchConfigData() {
      await ensureConfigSchema();
      const parts = await Promise.all(configSections.map(async (section) => {
        return await fetchConfigSection(section.id);
      }));
      return Object.assign({}, ...parts);
    }

    function renderConfigField(root, section, field, data) {
      data = data || configData || {};
      let control;
      if (field.type === "bool") {
        control = document.createElement("select");
        ["false", "true"].forEach((value) => {
          const option = document.createElement("option");
          option.value = value;
          option.textContent = value === "true" ? "On" : "Off";
          if (String(data[field.key]) === value) option.selected = true;
          control.appendChild(option);
        });
      } else if (field.type === "enum") {
        control = document.createElement("select");
        (field.enum || []).forEach((entry) => {
          const value = typeof entry === "string" ? entry : entry.value;
          const label = typeof entry === "string" ? entry : entry.label;
          const option = document.createElement("option");
          option.value = value;
          option.textContent = label || value;
          if (String(data[field.key]) === value) option.selected = true;
          control.appendChild(option);
        });
      } else {
        control = document.createElement("input");
        control.type = field.type === "password" ? "password" : field.type;
        control.value = data[field.key] || "";
        if (field.secret) {
          const setKey = field.key + "_set";
          control.placeholder = data[setKey] ? "set" : "empty";
          if (data[setKey]) control.value = "********";
        }
        if (field.key === "smb_ep") {
          control.placeholder = "smb://host/share/path";
        }
        if (field.key === "shq_team" || field.key === "shq_device") {
          control.placeholder = "optional numeric id";
        }
        if (field.key === "syslog_host") {
          control.placeholder = "IPv4 address";
        }
      }

      control.dataset.key = field.key;
      control.dataset.section = section.id;
      control.dataset.type = field.type;
      control.dataset.orig = String(control.value || "");
      let rendered = control;
      if (field.key === "tz") rendered = timezoneHelper(control);
      if (field.key === "wifi_ctry") rendered = wifiCountryHelper(control);
      root.appendChild(row(field.label || field.key, rendered, field.key));
    }

    async function loadConfig() {
      try {
        await ensureConfigSchema();
        configData = await fetchConfigData();

        const root = document.getElementById("configFields");
        root.innerHTML = "";
        configSections.forEach((section) => {
          const wrapper = document.createElement("details");
          wrapper.className = "config-section";
          if (section.id !== "logging") wrapper.open = true;

          const heading = document.createElement("summary");
          heading.className = "section-title";
          heading.textContent = section.title;
          wrapper.appendChild(heading);

          const fields = document.createElement("div");
          fields.className = "config-section-fields";
          wrapper.appendChild(fields);
          root.appendChild(wrapper);

          section.fields.forEach((field) =>
            renderConfigField(fields, section, field));
        });
      } catch (error) {
        msg("configMsg", error.message, false);
      }
    }

    function configFieldApplied(data, key, value) {
      if (!data) return false;
      const field = configFieldByKey[key];
      if (field && field.secret) {
        if (value === "********") return true;
        const setKey = key + "_set";
        const expectedSet = String(value || "").length > 0;
        return !!data[setKey] === expectedSet;
      }
      if (typeof value === "boolean") return !!data[key] === value;
      if (typeof value === "number") return Number(data[key]) === value;
      return String(data[key] || "") === String(value || "");
    }

    function configChangesApplied(data, changes) {
      return Object.keys(changes).every((key) =>
        configFieldApplied(data, key, changes[key]));
    }

    async function fetchConfigSections(sectionIds) {
      await ensureConfigSchema();
      const parts = await Promise.all(sectionIds.map((sectionId) =>
        fetchConfigSection(sectionId)));
      return Object.assign({}, ...parts);
    }

    function configSectionsForChanges(changes) {
      const ids = [];
      Object.keys(changes).forEach((key) => {
        const section = configSectionByKey[key] || "device";
        if (!ids.includes(section)) ids.push(section);
      });
      return ids;
    }

    async function waitForConfigChanges(changes, sectionIds) {
      const started = Date.now();
      let latest = null;
      while (Date.now() - started < 5000) {
        const fetched = sectionIds && sectionIds.length ?
          await fetchConfigSections(sectionIds) : await fetchConfigData();
        latest = Object.assign({}, configData || {}, fetched);
        if (configChangesApplied(latest, changes)) return latest;
        await new Promise(resolve => setTimeout(resolve, 250));
      }
      return latest;
    }

    function normalizeSmbEndpoint(value) {
      let endpoint = String(value || "").trim().replace(/\\/g, "/");
      if (endpoint.toLowerCase().startsWith("smb://")) {
        endpoint = "//" + endpoint.slice(6);
      }
      return endpoint;
    }

    function validSmbEndpoint(value) {
      const endpoint = normalizeSmbEndpoint(value);
      if (!endpoint.length) return true;
      if (!endpoint.startsWith("//")) return false;
      const parts = endpoint.slice(2).split("/");
      return parts.length >= 2 && parts[0].length > 0 && parts[1].length > 0;
    }

    function validOptionalNumericId(value) {
      const text = String(value || "").trim();
      return !text.length || /^\d+$/.test(text);
    }

    function validIpv4(value) {
      const text = String(value || "").trim();
      if (!text.length) return true;
      const parts = text.split(".");
      if (parts.length !== 4) return false;
      return parts.every((part) => {
        if (!/^\d+$/.test(part)) return false;
        const byte = Number(part);
        return byte >= 0 && byte <= 255;
      });
    }

    function collectConfigChanges(root) {
      const changes = {};
      if (!root) return changes;
      root.querySelectorAll("[data-key]").forEach((input) => {
        let rawValue = input.value;
        let originalValue = input.dataset.orig;
        if (input.dataset.key === "smb_ep") {
          rawValue = normalizeSmbEndpoint(rawValue);
          originalValue = normalizeSmbEndpoint(originalValue);
        }
        if (input.dataset.key === "syslog_host") {
          rawValue = String(rawValue || "").trim();
          originalValue = String(originalValue || "").trim();
        }
        if (String(rawValue) === String(originalValue)) return;
        if (input.dataset.type === "bool") {
          changes[input.dataset.key] = rawValue === "true";
        } else if (input.dataset.type === "number") {
          changes[input.dataset.key] = Number(rawValue);
        } else {
          changes[input.dataset.key] = rawValue;
        }
      });
      return changes;
    }

    function validateConfigChanges(changes, messageId) {
      if (Object.prototype.hasOwnProperty.call(changes, "smb_ep") &&
          !validSmbEndpoint(changes.smb_ep)) {
        msg(messageId, "smb_ep must be smb://host/share[/path]", false);
        return false;
      }
      if (Object.prototype.hasOwnProperty.call(changes, "shq_team") &&
          !validOptionalNumericId(changes.shq_team)) {
        msg(messageId, "shq_team must be numeric", false);
        return false;
      }
      if (Object.prototype.hasOwnProperty.call(changes, "shq_device") &&
          !validOptionalNumericId(changes.shq_device)) {
        msg(messageId, "shq_device must be numeric", false);
        return false;
      }

      const syslogHost = Object.prototype.hasOwnProperty.call(changes, "syslog_host") ?
        changes.syslog_host : (configData ? configData.syslog_host : "");
      const syslogEnabled = Object.prototype.hasOwnProperty.call(changes, "syslog_en") ?
        changes.syslog_en : !!(configData && configData.syslog_en);
      if (!validIpv4(syslogHost)) {
        msg(messageId, "syslog_host must be an IPv4 address", false);
        return false;
      }
      if (syslogEnabled && !String(syslogHost || "").trim().length) {
        msg(messageId, "syslog_host is required when syslog_en is true", false);
        return false;
      }
      return true;
    }

    async function postConfigChanges(changes) {
      await ensureConfigSchema();
      const sections = {};
      Object.keys(changes).forEach((key) => {
        const section = configSectionByKey[key] || "device";
        if (!sections[section]) sections[section] = {};
        sections[section][key] = changes[key];
      });
      const sectionOrder = configSections
        .map((section) => section.id)
        .filter((id) => id !== "access");
      sectionOrder.push("access");

      let queued = false;
      for (const section of sectionOrder) {
        if (!sections[section]) continue;
        const response = await api("/api/config/" +
          encodeURIComponent(section), {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify(sections[section]),
        });
        const data = await response.json();
        queued = queued || !!data.queued;
      }
      return queued;
    }

    async function saveConfigFields(root, messageId, reload) {
      await ensureConfigSchema();
      const changes = collectConfigChanges(root);
      if (!validateConfigChanges(changes, messageId)) return false;

      if (!Object.keys(changes).length) {
        msg(messageId, "No changes", true);
        return false;
      }

      try {
        const sectionIds = configSectionsForChanges(changes);
        const queued = await postConfigChanges(changes);

        if (queued) {
          const latest = await waitForConfigChanges(changes, sectionIds);
          if (latest) {
            configData = Object.assign({}, configData || {}, latest);
          }
          if (reload) await reload();
          msg(messageId,
            latest && configChangesApplied(latest, changes) ?
              "Saved" : "Config update queued", true);
        } else {
          const latest = await fetchConfigSections(sectionIds);
          configData = Object.assign({}, configData || {}, latest);
          if (reload) await reload();
          msg(messageId, "Saved", true);
        }
        return true;
      } catch (error) {
        msg(messageId, error.message, false);
        return false;
      }
    }

    async function saveConfig() {
      await saveConfigFields(document.getElementById("configFields"),
        "configMsg", loadConfig);
    }

    function clearMsg(id) {
      const element = document.getElementById(id);
      if (!element) return;
      if (msgTimers[id]) {
        clearTimeout(msgTimers[id]);
        msgTimers[id] = null;
      }
      element.textContent = "";
      element.className = "msg";
    }

    const endpointConfigPanels = {
      smb: {
        section: "smb",
        panel: "edfSmbConfig",
        fields: "edfSmbConfigFields",
        msg: "edfSmbConfigMsg",
        save: "edfSmbSaveBtn",
        status: loadSmbSyncStatus,
      },
      sleephq: {
        section: "sleephq",
        panel: "edfSleepHqConfig",
        fields: "edfSleepHqConfigFields",
        msg: "edfSleepHqConfigMsg",
        save: "edfSleepHqSaveBtn",
        status: loadSleepHqSyncStatus,
      },
    };

    async function loadEndpointConfig(id, clearMessage) {
      await ensureConfigSchema();
      const panel = endpointConfigPanels[id];
      if (!panel) return false;
      const section = configSectionById[panel.section];
      const root = document.getElementById(panel.fields);
      if (!section || !root) return false;
      if (clearMessage) clearMsg(panel.msg);
      try {
        const data = await fetchConfigSection(panel.section);
        configData = Object.assign({}, configData || {}, data);
        root.innerHTML = "";
        section.fields.forEach((field) =>
          renderConfigField(root, section, field, configData));
        return true;
      } catch (error) {
        msg(panel.msg, error.message, false, true);
        return false;
      }
    }

    async function toggleEndpointConfig(id) {
      const panel = endpointConfigPanels[id];
      if (!panel) return;
      const element = document.getElementById(panel.panel);
      if (!element) return;
      if (!element.hidden) {
        element.hidden = true;
        clearMsg(panel.msg);
        return;
      }
      element.hidden = false;
      await loadEndpointConfig(id, true);
    }

    function cancelEndpointConfig(id) {
      const panel = endpointConfigPanels[id];
      if (!panel) return;
      const element = document.getElementById(panel.panel);
      if (element) element.hidden = true;
      clearMsg(panel.msg);
    }

    async function saveEndpointConfig(id) {
      const panel = endpointConfigPanels[id];
      if (!panel) return;
      const root = document.getElementById(panel.fields);
      const save = document.getElementById(panel.save);
      if (save) save.disabled = true;
      try {
        await saveConfigFields(root, panel.msg, async () => {
          await loadEndpointConfig(id, false);
          if (panel.status) await panel.status();
        });
      } finally {
        if (save) save.disabled = false;
      }
    }
