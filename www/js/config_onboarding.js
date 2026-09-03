(() => {
    "use strict";

    let configData = null;
    let configSections = [];
    let as11BlePairingData = null;
    let configSchemaPromise = null;
    const configSectionById = {};
    const configSectionByKey = {};
    const configFieldByKey = {};

    function rebuildConfigSectionMaps() {
      Object.keys(configSectionById).forEach((key) =>
        delete configSectionById[key]);
      Object.keys(configSectionByKey).forEach((key) =>
        delete configSectionByKey[key]);
      Object.keys(configFieldByKey).forEach((key) =>
        delete configFieldByKey[key]);

      configSections.forEach((section) => {
        configSectionById[section.id] = section;
        section.fields.forEach((field) => {
          configSectionByKey[field.key] = section.id;
          configFieldByKey[field.key] = field;
        });
      });
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
      const response = await AirCANnect.http.requestOk("/api/config/schema");
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
      const response = await AirCANnect.http.requestOk("/api/config/" + encodeURIComponent(sectionId));
      return await response.json();
    }

    async function fetchConfigData() {
      await ensureConfigSchema();
      const parts = await Promise.all(configSections.map(async (section) => {
        return await fetchConfigSection(section.id);
      }));
      return Object.assign({}, ...parts);
    }

    function normalizedKeybindingOverrides(value) {
      const overrides = value && Array.isArray(value.overrides) ?
        value.overrides : [];
      return overrides.map((binding) => ({
        button: String(binding.button || ""),
        gesture: String(binding.gesture || ""),
        action: String(binding.action || ""),
      })).sort((left, right) =>
        (left.button + ":" + left.gesture).localeCompare(
          right.button + ":" + right.gesture));
    }

    function renderKeybindingsField(root, section, field, data) {
      const value = data[field.key] || {};
      const normalizedOverrides = normalizedKeybindingOverrides(value);
      const overrides = new Map(normalizedOverrides.map(
        (binding) => [binding.button + ":" + binding.gesture,
          binding.action]));
      const panel = document.createElement("div");
      panel.className = "keybindings-config";
      panel.dataset.keybindings = field.key;
      panel.dataset.section = section.id;
      panel.dataset.orig = JSON.stringify(normalizedOverrides);

      const schemaInputs = new Map((field.buttons || []).map(
        (button) => [button.id, button]));
      const physicalButtons = (field.buttons || []).filter(
        (button) => button.key !== undefined);
      const physicalById = new Map(physicalButtons.map(
        (button) => [button.id, button]));

      normalizedOverrides.forEach((binding) => {
        if (schemaInputs.has(binding.button) ||
            !binding.button.includes("+")) return;

        const ids = binding.button.split("+");
        if (ids.length !== 2 || ids[0] === ids[1] ||
            !physicalById.has(ids[0]) || !physicalById.has(ids[1])) return;

        schemaInputs.set(binding.button, {
          id: binding.button,
          label: physicalById.get(ids[0]).label + " + " +
            physicalById.get(ids[1]).label,
          gestures: [
            {gesture: "short", default: "none"},
            {gesture: "long", default: "none"},
          ],
        });
      });

      function appendInputRows(button, before) {
        (button.gestures || []).forEach((gesture) => {
          const select = document.createElement("select");
          select.dataset.button = button.id;
          select.dataset.gesture = gesture.gesture;
          select.dataset.defaultAction = gesture.default;

          const defaultOption = document.createElement("option");
          defaultOption.value = "";
          const defaultAction = (field.actions || []).find(
            (action) => action.value === gesture.default);
          const defaultLabel = defaultAction ?
            defaultAction.label : gesture.default;
          defaultOption.textContent = "Default";
          select.appendChild(defaultOption);

          (field.actions || []).forEach((action) => {
            const option = document.createElement("option");
            option.value = action.value;
            option.textContent = action.label || action.value;
            select.appendChild(option);
          });

          select.value = overrides.get(
            button.id + ":" + gesture.gesture) || "";
          const bindingRow = AirCANnect.ui.row(
            button.label + " / " + gesture.gesture,
            select,
            "Hardware default: " + defaultLabel);
          panel.insertBefore(bindingRow, before || null);
        });
      }

      schemaInputs.forEach((button) => appendInputRows(button));

      if (physicalButtons.length > 1) {
        const addControls = document.createElement("div");
        addControls.className = "config-helper keybinding-add";
        const first = document.createElement("select");
        const second = document.createElement("select");
        [first, second].forEach((select) => {
          physicalButtons.forEach((button) => {
            const option = document.createElement("option");
            option.value = button.id;
            option.textContent = button.label;
            select.appendChild(option);
          });
        });
        second.selectedIndex = 1;

        const add = document.createElement("button");
        add.type = "button";
        add.className = "btn";
        add.textContent = "Add combination";
        add.onclick = () => {
          if (first.value === second.value) return;
          const selected = [physicalById.get(first.value),
            physicalById.get(second.value)].sort(
            (left, right) => Number(left.key) - Number(right.key));
          const id = selected[0].id + "+" + selected[1].id;
          if (schemaInputs.has(id)) return;

          const input = {
            id,
            label: selected[0].label + " + " + selected[1].label,
            gestures: [
              {gesture: "short", default: "none"},
              {gesture: "long", default: "none"},
            ],
          };
          schemaInputs.set(id, input);
          appendInputRows(input, addControls);
        };

        addControls.appendChild(first);
        addControls.appendChild(second);
        addControls.appendChild(add);
        panel.appendChild(addControls);
      }

      const fieldRow = AirCANnect.ui.row(field.label || field.key, panel, field.key);
      fieldRow.classList.add("keybindings-field");
      root.appendChild(fieldRow);
    }

    function renderConfigField(root, section, field, data) {
      data = data || configData || {};
      if (field.type === "keybindings") {
        renderKeybindingsField(root, section, field, data);
        return;
      }

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
      const rendered = AirCANnect.forms.decorate(field.key, control);
      root.appendChild(AirCANnect.ui.row(field.label || field.key, rendered, field.key));
    }

    function as11BleStateLabel(data) {
      const labels = {
        idle: data.paired ? "Paired" : "Not paired",
        scanning: "Scanning",
        select_device: "Select a device",
        connecting: "Connecting",
        awaiting_passkey: "Enter the code shown on the AS11",
        verifying: "Verifying",
        saving: "Saving pairing",
        complete: data.paired ? "Paired" : "Pairing removed",
        failed: "Pairing failed",
      };
      return labels[data.state] || data.state || "Unknown";
    }

    function renderAs11BlePairingPanel(panel, data) {
      const snapshot = JSON.stringify(data || {});
      if (panel.dataset.snapshot === snapshot) return;

      panel.dataset.snapshot = snapshot;
      panel.innerHTML = "";

      const state = AirCANnect.ui.valueSpan(as11BleStateLabel(data));
      panel.appendChild(AirCANnect.ui.row("BLE pairing", state));

      if (data.selected_name || data.selected_address) {
        const selected = [data.selected_name, data.selected_address]
          .filter(Boolean).join(" / ");
        panel.appendChild(AirCANnect.ui.row("AS11", AirCANnect.ui.valueSpan(selected)));
      }

      if (data.state === "select_device") {
        const instruction = document.createElement("p");
        instruction.className = "as11-pairing-instruction";
        instruction.textContent =
          "Before continuing, on the AirSense open More > myAir App, " +
          "then select OK, Downloaded.";
        panel.appendChild(instruction);

        const select = document.createElement("select");
        (data.devices || []).forEach((device) => {
          const option = document.createElement("option");
          option.value = device.address;
          option.textContent = (device.name || "AS11") + " / " +
            device.address + " / " + device.rssi + " dBm";
          select.appendChild(option);
        });
        panel.appendChild(AirCANnect.ui.row("Device", select));
      }

      let passkey = null;
      if (data.passkey_required) {
        passkey = document.createElement("input");
        passkey.type = "text";
        passkey.inputMode = "numeric";
        passkey.autocomplete = "one-time-code";
        passkey.maxLength = 4;
        passkey.placeholder = "0000";
        panel.appendChild(AirCANnect.ui.row("Pairing code", passkey));
      }

      const buttons = document.createElement("div");
      buttons.className = "btns";
      if (data.state === "select_device") {
        const selectButton = document.createElement("button");
        selectButton.className = "btn primary";
        selectButton.textContent = "Continue";
        const select = panel.querySelector("select");
        selectButton.onclick = () => {
          if (select && select.value) {
            as11BleAction("select", {address: select.value});
          }
        };
        buttons.appendChild(selectButton);
      } else if (data.passkey_required) {
        const confirmButton = document.createElement("button");
        confirmButton.className = "btn primary";
        confirmButton.textContent = "Pair";
        confirmButton.onclick = () => {
          const value = passkey ? passkey.value.trim() : "";
          if (!/^\d{4}$/.test(value)) {
            showAs11BlePairingError(
              "Enter the four-digit code shown on the AS11.");
            return;
          }
          as11BleAction("passkey", {passkey: value});
        };
        buttons.appendChild(confirmButton);
      } else if (!data.active) {
        const pairButton = document.createElement("button");
        pairButton.className = "btn primary";
        pairButton.textContent = data.paired ? "Pair another" : "Pair";
        pairButton.disabled = !data.enabled;
        pairButton.onclick = () => as11BleAction("pair");
        buttons.appendChild(pairButton);
      }

      if (data.active) {
        const cancelButton = document.createElement("button");
        cancelButton.className = "btn";
        cancelButton.textContent = "Cancel";
        cancelButton.onclick = () => as11BleAction("cancel");
        buttons.appendChild(cancelButton);
      }
      if (data.paired && !data.active) {
        const forgetButton = document.createElement("button");
        forgetButton.className = "btn danger";
        forgetButton.textContent = "Forget";
        forgetButton.onclick = as11BleForget;
        buttons.appendChild(forgetButton);
      }
      panel.appendChild(buttons);

      const message = document.createElement("div");
      message.className = "msg" + (data.error ? " err" : "");
      message.classList.add("as11-ble-msg");
      message.textContent = data.error ||
        (!data.enabled ? "Select BLE transport and save first." : "");
      panel.appendChild(message);
    }

    function as11BlePairingPanels() {
      return document.querySelectorAll("[data-as11-ble-pairing]");
    }

    function showAs11BlePairingError(text) {
      as11BlePairingPanels().forEach((panel) => {
        const message = panel.querySelector(".as11-ble-msg");
        if (!message) return;
        message.textContent = text;
        message.className = "msg err as11-ble-msg";
      });
    }

    function renderAs11BlePairing(data) {
      as11BlePairingData = data;
      as11BlePairingPanels().forEach((panel) =>
        renderAs11BlePairingPanel(panel, data));
    }

    async function loadAs11BlePairing(showError) {
      try {
        const response = await AirCANnect.http.requestOk("/api/as11/ble");
        renderAs11BlePairing(await response.json());
      } catch (error) {
        if (showError) showAs11BlePairingError(error.message);
      }
    }

    async function as11BleAction(action, values) {
      try {
        await AirCANnect.http.requestOk("/api/as11/ble", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify(Object.assign({action}, values || {})),
        });
        return true;
      } catch (error) {
        showAs11BlePairingError(error.message);
        return false;
      }
    }

    function startAs11PairingFromDashboard() {
      AirCANnect.pages.show("config");
      as11BleAction("pair");
    }

    function as11BleForget() {
      if (!confirm("Forget the paired AS11?")) return;
      as11BleAction("forget");
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
          if (section.id === "as11") {
            const pairing = document.createElement("div");
            pairing.className = "endpoint-config";
            pairing.dataset.as11BlePairing = "true";
            fields.appendChild(pairing);
          }
        });
        await loadAs11BlePairing(true);
      } catch (error) {
        AirCANnect.ui.message("configMsg", error.message, false);
      }
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

    function waitForConfigUpdate(revision, afterSerial) {
      const expected = Number(revision || 0);
      if (!expected) {
        return Promise.reject(new Error("missing config update revision"));
      }
      return AirCANnect.snapshots.wait(
        "config", (data) => Number(data.revision || 0) === expected,
        afterSerial, 5000, "config update timed out").then((data) => {
          if (!data.ok) throw new Error(data.error || "config update failed");
          return data;
        });
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

      root.querySelectorAll("[data-keybindings]").forEach((panel) => {
        const overrides = [];
        panel.querySelectorAll("select[data-button]").forEach((select) => {
          if (!select.value) return;
          overrides.push({
            button: select.dataset.button,
            gesture: select.dataset.gesture,
            action: select.value,
          });
        });
        const normalized = normalizedKeybindingOverrides({overrides});
        if (JSON.stringify(normalized) !== panel.dataset.orig) {
          changes[panel.dataset.keybindings] = {overrides: normalized};
        }
      });
      return changes;
    }

    function validateConfigChanges(changes, messageId) {
      if (Object.prototype.hasOwnProperty.call(changes, "smb_ep") &&
          !validSmbEndpoint(changes.smb_ep)) {
        AirCANnect.ui.message(messageId, "smb_ep must be smb://host/share[/path]", false);
        return false;
      }
      if (Object.prototype.hasOwnProperty.call(changes, "shq_team") &&
          !validOptionalNumericId(changes.shq_team)) {
        AirCANnect.ui.message(messageId, "shq_team must be numeric", false);
        return false;
      }
      if (Object.prototype.hasOwnProperty.call(changes, "shq_device") &&
          !validOptionalNumericId(changes.shq_device)) {
        AirCANnect.ui.message(messageId, "shq_device must be numeric", false);
        return false;
      }

      const syslogHost = Object.prototype.hasOwnProperty.call(changes, "syslog_host") ?
        changes.syslog_host : (configData ? configData.syslog_host : "");
      const syslogEnabled = Object.prototype.hasOwnProperty.call(changes, "syslog_en") ?
        changes.syslog_en : !!(configData && configData.syslog_en);
      if (!validIpv4(syslogHost)) {
        AirCANnect.ui.message(messageId, "syslog_host must be an IPv4 address", false);
        return false;
      }
      if (syslogEnabled && !String(syslogHost || "").trim().length) {
        AirCANnect.ui.message(messageId, "syslog_host is required when syslog_en is true", false);
        return false;
      }
      return true;
    }

    async function postConfigChanges(changes) {
      const afterSerial = AirCANnect.snapshots.read("config").serial;
      const response = await AirCANnect.http.requestOk("/api/config", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(changes),
      });
      const data = await response.json();
      return {revision: Number(data.revision || 0), afterSerial};
    }

    async function saveConfigFields(root, messageId, reload) {
      await ensureConfigSchema();
      const changes = collectConfigChanges(root);
      if (!validateConfigChanges(changes, messageId)) return false;

      if (!Object.keys(changes).length) {
        AirCANnect.ui.message(messageId, "No changes", true);
        return false;
      }

      try {
        const sectionIds = configSectionsForChanges(changes);
        const update = await postConfigChanges(changes);
        await waitForConfigUpdate(update.revision, update.afterSerial);

        if (reload) {
          await reload();
        } else {
          const latest = await fetchConfigSections(sectionIds);
          configData = Object.assign({}, configData || {}, latest);
        }
        AirCANnect.ui.message(messageId, "Saved", true);
        return true;
      } catch (error) {
        AirCANnect.ui.message(messageId, error.message, false);
        return false;
      }
    }

    async function saveConfig() {
      await saveConfigFields(document.getElementById("configFields"),
        "configMsg", loadConfig);
    }

    const endpointConfigPanels = {
      smb: {
        section: "smb",
        panel: "edfSmbConfig",
        fields: "edfSmbConfigFields",
        msg: "edfSmbConfigMsg",
        save: "edfSmbSaveBtn",
      },
      sleephq: {
        section: "sleephq",
        panel: "edfSleepHqConfig",
        fields: "edfSleepHqConfigFields",
        msg: "edfSleepHqConfigMsg",
        save: "edfSleepHqSaveBtn",
      },
    };

    async function loadEndpointConfig(id, clearMessage) {
      await ensureConfigSchema();
      const panel = endpointConfigPanels[id];
      if (!panel) return false;
      const section = configSectionById[panel.section];
      const root = document.getElementById(panel.fields);
      if (!section || !root) return false;
      if (clearMessage) AirCANnect.ui.clearMessage(panel.msg);
      try {
        const data = await fetchConfigSection(panel.section);
        configData = Object.assign({}, configData || {}, data);
        root.innerHTML = "";
        section.fields.forEach((field) =>
          renderConfigField(root, section, field, configData));
        return true;
      } catch (error) {
        AirCANnect.ui.message(panel.msg, error.message, false, true);
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
        AirCANnect.ui.clearMessage(panel.msg);
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
      AirCANnect.ui.clearMessage(panel.msg);
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
          AirCANnect.pages.load("edf", true);
        });
      } finally {
        if (save) save.disabled = false;
      }
    }


    const ONBOARDING_STEP_KEY = "aircannect-onboarding-step";
    const onboardingSteps = [
      {id: "network", title: "Network"},
      {id: "as11", title: "Therapy device connection"},
      {id: "time", title: "Time"},
      {id: "access", title: "Access"},
      {id: "smb", title: "SMB export"},
      {id: "sleephq", title: "SleepHQ export"},
    ];
    let onboardingStep = 0;
    let onboardingBusy = false;
    let onboardingTimezoneValue = null;
    let onboardingUser = null;
    let onboardingPassword = null;

    function onboardingNote(root, text) {
      const note = document.createElement("p");
      note.className = "onboarding-note";
      note.textContent = text;
      root.appendChild(note);
    }

    function onboardingRenderConfigFields(root, keys) {
      keys.forEach((key) => {
        const sectionId = configSectionByKey[key];
        const section = configSectionById[sectionId];
        const field = configFieldByKey[key];
        if (section && field) renderConfigField(root, section, field, configData);
      });
    }

    function onboardingSectionFields(root, sectionId) {
      const section = configSectionById[sectionId];
      if (!section) return;
      section.fields.forEach((field) =>
        renderConfigField(root, section, field, configData));
    }

    function onboardingInputRow(root, label, id, type, autocomplete) {
      const input = document.createElement("input");
      input.id = id;
      input.type = type;
      if (autocomplete) input.autocomplete = autocomplete;
      root.appendChild(AirCANnect.ui.row(label, input));
      return input;
    }

    function onboardingSetBusy(busy) {
      onboardingBusy = busy;
      ["onboardingBack", "onboardingSkip", "onboardingNext"]
        .forEach((id) => {
          const button = document.getElementById(id);
          if (button) {
            button.disabled = busy ||
              (id === "onboardingBack" && onboardingStep === 0);
          }
        });
    }

    function onboardingRenderProgress() {
      const progress = document.getElementById("onboardingProgress");
      progress.innerHTML = "";
      onboardingSteps.forEach((step, index) => {
        const marker = document.createElement("span");
        if (index < onboardingStep) marker.className = "complete";
        if (index === onboardingStep) marker.className = "active";
        progress.appendChild(marker);
      });
    }

    async function onboardingRenderNetwork(root) {
      onboardingRenderConfigFields(root, ["host"]);

      const heading = document.createElement("div");
      heading.className = "section-title";
      heading.textContent = "Add Wi-Fi profile";
      root.appendChild(heading);

      onboardingInputRow(root, "SSID", "onboardingWifiSsid", "text",
        "network-name");
      onboardingInputRow(root, "Password", "onboardingWifiPass", "password",
        "current-password");

      try {
        const response = await AirCANnect.http.requestOk("/api/wifi");
        const data = await response.json();
        const current = data.ssid ? data.ssid + " / " + (data.ip || "--") :
          "Not connected";
        root.insertBefore(AirCANnect.ui.row("Current Wi-Fi", AirCANnect.ui.valueSpan(current)), heading);
      } catch (error) {
        // The current connection is informational; setup can continue offline.
      }
    }

    async function onboardingApplyAs11Transport() {
      if (onboardingBusy) return;
      const root = document.getElementById("onboardingBody");
      onboardingSetBusy(true);
      const saved = await saveConfigFields(root, "onboardingMsg", null);
      onboardingSetBusy(false);
      if (!saved) return;
    }

    async function onboardingRenderAs11(root) {
      onboardingNote(root,
        "Choose how AirCANnect connects to your AirSense or AirCurve. " +
        "CAN uses a wired connection. Bluetooth requires pairing below, and " +
        "some device options may be unavailable over Bluetooth or with stock " +
        "ResMed firmware.");
      onboardingRenderConfigFields(root, ["as11_transport"]);
      const transport = root.querySelector('[data-key="as11_transport"]');
      if (transport) {
        transport.addEventListener("change", onboardingApplyAs11Transport);
      }

      const pairing = document.createElement("div");
      pairing.className = "endpoint-config";
      pairing.dataset.as11BlePairing = "true";
      root.appendChild(pairing);
      await loadAs11BlePairing(true);
    }

    function onboardingRenderTime(root) {
      onboardingNote(root,
        "The timezone keeps local dates and report times correct. AS11 time " +
        "sync also sets the machine clock when supported. Stock firmware " +
        "does not allow this over Bluetooth, but AirCANnect still corrects " +
        "timestamps in recorded EDF files.");
      onboardingRenderConfigFields(root, ["tz", "resmed_time"]);
      const timezone = root.querySelector('[data-key="tz"]');
      if (!timezone) return;

      if (onboardingTimezoneValue === null) {
        AirCANnect.forms.setDefault("tz", timezone);
        onboardingTimezoneValue = timezone.value;
      } else {
        timezone.value = onboardingTimezoneValue;
      }
      timezone.addEventListener("input", () => {
        onboardingTimezoneValue = timezone.value;
      });
    }

    function onboardingRenderAccess(root) {
      onboardingNote(root,
        "These credentials protect the Web UI and HTTP API. Other access " +
        "channels, including the TCP bridge and Telnet, can be configured " +
        "later.");
      onboardingRenderConfigFields(root, ["http_user", "http_pass"]);

      const user = root.querySelector('[data-key="http_user"]');
      const password = root.querySelector('[data-key="http_pass"]');
      if (user && onboardingUser !== null) user.value = onboardingUser;
      if (password && onboardingPassword !== null) {
        password.value = onboardingPassword;
      }
    }

    async function renderOnboardingStep() {
      const step = onboardingSteps[onboardingStep];
      const root = document.getElementById("onboardingBody");
      document.getElementById("onboardingTitle").textContent = step.title;
      document.getElementById("onboardingCounter").textContent =
        (onboardingStep + 1) + " of " + onboardingSteps.length;
      document.getElementById("onboardingBack").disabled =
        onboardingStep === 0;
      document.getElementById("onboardingNext").textContent =
        onboardingStep === onboardingSteps.length - 1 ? "Finish" : "Next";
      root.innerHTML = "";
      AirCANnect.ui.clearMessage("onboardingMsg");
      onboardingRenderProgress();

      if (step.id === "network") await onboardingRenderNetwork(root);
      if (step.id === "as11") await onboardingRenderAs11(root);
      if (step.id === "time") onboardingRenderTime(root);
      if (step.id === "access") onboardingRenderAccess(root);
      if (step.id === "smb") {
        onboardingNote(root,
          "Export EDF files automatically to a shared folder on your " +
          "computer or NAS.");
        onboardingSectionFields(root, "smb");
      }
      if (step.id === "sleephq") {
        onboardingNote(root,
          "Connect your SleepHQ account to upload new therapy data " +
          "automatically.");
        onboardingRenderConfigFields(root, ["shq_id", "shq_secret"]);
      }

      sessionStorage.setItem(ONBOARDING_STEP_KEY, String(onboardingStep));
    }

    async function onboardingSaveCurrentStep() {
      const root = document.getElementById("onboardingBody");
      if (onboardingSteps[onboardingStep].id === "access") {
        const changes = collectConfigChanges(root);
        if (!validateConfigChanges(changes, "onboardingMsg")) return false;
        onboardingUser = Object.prototype.hasOwnProperty.call(
          changes, "http_user") ? changes.http_user : null;
        onboardingPassword = Object.prototype.hasOwnProperty.call(
          changes, "http_pass") ? changes.http_pass : null;
        AirCANnect.ui.message("onboardingMsg",
          onboardingUser === null && onboardingPassword === null ?
            "No changes" : "Access credentials will change after setup",
          true);
        return true;
      }

      const changes = collectConfigChanges(root);
      if (Object.keys(changes).length) {
        const saved = await saveConfigFields(root, "onboardingMsg", null);
        if (!saved) return false;
      } else {
        AirCANnect.ui.message("onboardingMsg", "No changes", true);
      }

      if (onboardingSteps[onboardingStep].id !== "network") return true;
      const ssid = document.getElementById("onboardingWifiSsid").value.trim();
      if (!ssid) return true;

      const password = document.getElementById("onboardingWifiPass").value;
      try {
        await requestWifiAction("add", {ssid, pass: password});
        AirCANnect.ui.message("onboardingMsg", "Wi-Fi profile queued", true);
        return true;
      } catch (error) {
        AirCANnect.ui.message("onboardingMsg", error.message, false);
        return false;
      }
    }

    async function onboardingLeaveStep() {
      if (onboardingSteps[onboardingStep].id !== "as11" ||
          !as11BlePairingData || !as11BlePairingData.active) {
        return;
      }
      await as11BleAction("cancel");
    }

    function waitForOnboardingCompletion() {
      const current = AirCANnect.snapshots.read("status");
      if (current.data && current.data.onboarding_complete) {
        return Promise.resolve();
      }
      return AirCANnect.snapshots.wait(
        "status", (data) => !!data.onboarding_complete, current.serial,
        5000, "Onboarding completion was not persisted");
    }

    async function onboardingComplete() {
      const body = {};
      if (onboardingUser !== null) body.http_user = onboardingUser;
      if (onboardingPassword !== null) body.http_password = onboardingPassword;
      const response = await AirCANnect.http.requestOk("/api/onboarding", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(body),
      });
      await response.json();

      if (onboardingUser !== null || onboardingPassword !== null) return true;
      await waitForOnboardingCompletion();
      return true;
    }

    function onboardingRenderFinish() {
      document.getElementById("onboardingTitle").textContent = "Setup complete";
      document.getElementById("onboardingCounter").textContent = "";
      document.getElementById("onboardingProgress").innerHTML = "";
      document.getElementById("onboardingActions").hidden = true;

      const root = document.getElementById("onboardingBody");
      root.innerHTML = "";
      const finish = document.createElement("div");
      finish.className = "onboarding-finish";
      const heading = document.createElement("h3");
      heading.textContent = "AirCANnect is ready";
      finish.appendChild(heading);

      const buttons = document.createElement("div");
      buttons.className = "btns";
      const dashboard = document.createElement("button");
      dashboard.className = "btn primary";
      dashboard.textContent = "Dashboard";
      dashboard.onclick = () => onboardingExit("dash");
      buttons.appendChild(dashboard);

      const configure = document.createElement("button");
      configure.className = "btn";
      configure.textContent = "Configure more";
      configure.onclick = () => onboardingExit("config");
      buttons.appendChild(configure);
      finish.appendChild(buttons);
      root.appendChild(finish);
    }

    async function onboardingNext(skip) {
      if (onboardingBusy) return;
      onboardingSetBusy(true);
      if (skip && onboardingSteps[onboardingStep].id === "access") {
        onboardingUser = null;
        onboardingPassword = null;
      }
      if (!skip && !await onboardingSaveCurrentStep()) {
        onboardingSetBusy(false);
        return;
      }
      await onboardingLeaveStep();

      if (onboardingStep < onboardingSteps.length - 1) {
        onboardingStep++;
        onboardingSetBusy(false);
        await renderOnboardingStep();
        return;
      }

      try {
        await onboardingComplete();
        sessionStorage.removeItem(ONBOARDING_STEP_KEY);
        onboardingRenderFinish();
      } catch (error) {
        AirCANnect.ui.message("onboardingMsg", error.message, false);
        onboardingSetBusy(false);
      }
    }

    async function onboardingBack() {
      if (onboardingBusy || onboardingStep === 0) return;
      onboardingSetBusy(true);
      await onboardingLeaveStep();
      onboardingStep--;
      onboardingSetBusy(false);
      await renderOnboardingStep();
    }

    function onboardingExit(tab) {
      document.getElementById("onboarding").hidden = true;
      document.getElementById("onboardingActions").hidden = false;
      if (location.pathname === "/wizard") {
        location.assign("/#" + tab);
        return;
      }
      AirCANnect.pages.show(tab);
    }

    async function initOnboarding() {
      try {
        const forced = location.pathname === "/wizard";
        if (!forced) {
          const current = AirCANnect.snapshots.read("status");
          const status = current.data || await AirCANnect.snapshots.wait(
            "status", () => true, current.serial, 5000);
          if (!status || status.onboarding_complete) return;
        }

        await ensureConfigSchema();
        configData = await fetchConfigData();
        const storedStep = Number(sessionStorage.getItem(ONBOARDING_STEP_KEY));
        onboardingStep = Number.isInteger(storedStep) ?
          Math.max(0, Math.min(onboardingSteps.length - 1, storedStep)) : 0;

        document.getElementById("onboardingBack").onclick = onboardingBack;
        document.getElementById("onboardingSkip").onclick = () =>
          onboardingNext(true);
        document.getElementById("onboardingNext").onclick = () =>
          onboardingNext(false);
        document.getElementById("onboarding").hidden = false;
        await renderOnboardingStep();
      } catch (error) {
        console.warn("Onboarding unavailable", error);
      }
    }

    AirCANnect.startup.register(initOnboarding);

    AirCANnect.actions.register("as11.pair-dashboard", () =>
      startAs11PairingFromDashboard());
    AirCANnect.actions.register("config.save", () => saveConfig());
    AirCANnect.actions.register(
      "config.endpoint-toggle", (_event, element) =>
        toggleEndpointConfig(element.dataset.value));
    AirCANnect.actions.register(
      "config.endpoint-save", (_event, element) =>
        saveEndpointConfig(element.dataset.value));
    AirCANnect.actions.register(
      "config.endpoint-cancel", (_event, element) =>
        cancelEndpointConfig(element.dataset.value));
    AirCANnect.pages.onLoad("config", () => loadConfig());
    AirCANnect.events.subscribe("config", () => {});
    AirCANnect.events.subscribe("as11_ble", renderAs11BlePairing);
})();
