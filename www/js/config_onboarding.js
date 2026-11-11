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
            AirCANnect.i18n.message("config.action." + defaultAction.value,
              {}, defaultAction.label) : gesture.default;
          AirCANnect.i18n.text(defaultOption, AirCANnect.i18n.message("config.default"));
          select.appendChild(defaultOption);

          (field.actions || []).forEach((action) => {
            const option = document.createElement("option");
            option.value = action.value;
            AirCANnect.i18n.text(option, AirCANnect.i18n.message(
              "config.action." + action.value, {}, action.label || action.value));
            select.appendChild(option);
          });

          select.value = overrides.get(
            button.id + ":" + gesture.gesture) || "";
          const bindingRow = AirCANnect.ui.row(
            AirCANnect.i18n.join([button.label,
              AirCANnect.i18n.message("config.gesture." + gesture.gesture,
                {}, gesture.gesture)], " / "),
            select,
            AirCANnect.i18n.message("config.hardware_default", {label: defaultLabel}));
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
            AirCANnect.i18n.text(option, button.label);
            select.appendChild(option);
          });
        });
        second.selectedIndex = 1;

        const add = document.createElement("button");
        add.type = "button";
        add.className = "btn";
        AirCANnect.i18n.text(add, AirCANnect.i18n.message("config.add_combination"));
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

      const fieldRow = AirCANnect.ui.row(
        AirCANnect.i18n.message("config.field." + field.key + ".label", {},
          field.label || field.key), panel, field.key);
      fieldRow.classList.add("keybindings-field");
      root.appendChild(fieldRow);
    }

    function renderSleepHqMasks(control, selected) {
      const status = AirCANnect.snapshots.read("exports").data?.sleephq;
      const none = new Option("", "");
      AirCANnect.i18n.text(none, AirCANnect.i18n.message("config.mask.none"));
      control.replaceChildren(none);
      (status?.masks || []).forEach((mask) => {
        const option = new Option("", String(mask.id));
        AirCANnect.i18n.text(option, mask.name ||
          AirCANnect.i18n.message("config.mask.id", {id: String(mask.id)}));
        control.add(option);
      });
      if (selected && !Array.from(control.options).some((option) => option.value === selected)) {
        const option = new Option("", selected);
        AirCANnect.i18n.text(option,
          AirCANnect.i18n.message("config.mask.id", {id: selected}));
        control.add(option);
      }
      control.value = selected;
      AirCANnect.i18n.text(control, status?.masks_error || "", "title");
    }

    function renderConfigField(root, section, field, data) {
      data = data || configData || {};
      if (field.type === "keybindings") {
        renderKeybindingsField(root, section, field, data);
        return;
      }

      let control;
      if (field.key === "shq_mask") {
        control = document.createElement("select");
        renderSleepHqMasks(control, String(data[field.key] || ""));
      } else if (field.type === "bool") {
        control = document.createElement("select");
        ["false", "true"].forEach((value) => {
          const option = document.createElement("option");
          option.value = value;
          AirCANnect.i18n.text(option, AirCANnect.i18n.message(
            "config.option." + field.key + "." + (value === "true" ? "on" : "off"),
            {}, value === "true" ? "On" : "Off"));
          if (String(data[field.key]) === value) option.selected = true;
          control.appendChild(option);
        });
      } else if (field.type === "enum") {
        control = document.createElement("select");
        const saved = String(data[field.key] ?? "");
        if (!(field.enum || []).some((entry) =>
          String(typeof entry === "string" ? entry : entry.value) === saved)) {
          const unknown = document.createElement("option");
          unknown.value = saved;
          AirCANnect.i18n.text(unknown,
            AirCANnect.i18n.message("config.unknown", {value: saved}));
          unknown.selected = true;
          control.appendChild(unknown);
        }
        (field.enum || []).forEach((entry) => {
          const value = typeof entry === "string" ? entry : entry.value;
          const label = typeof entry === "string" ? entry : entry.label;
          const option = document.createElement("option");
          option.value = value;
          AirCANnect.i18n.text(option,
            field.key === "ui_language" && value !== "auto" ? label || value :
              AirCANnect.i18n.message("config.option." + field.key + "." + value,
                {}, label || value));
          if (String(data[field.key]) === value) option.selected = true;
          control.appendChild(option);
        });
      } else {
        control = document.createElement("input");
        control.type = field.type === "password" ? "password" : field.type;
        control.value = data[field.key] ?? "";
        if (field.secret) {
          const setKey = field.key + "_set";
          AirCANnect.i18n.text(control, data[setKey] ? AirCANnect.i18n.message("config.secret.set") : AirCANnect.i18n.message("config.secret.empty"), "placeholder");
          if (data[setKey]) control.value = "********";
        }
        if (field.key === "smb_ep") {
          AirCANnect.i18n.text(control, "smb://host/share/path", "placeholder");
        }
        if (field.key === "shq_team" || field.key === "shq_device") {
          AirCANnect.i18n.text(control, AirCANnect.i18n.message("config.optional_id"), "placeholder");
        }
        if (field.key === "syslog_host") {
          AirCANnect.i18n.text(control, AirCANnect.i18n.message("config.ipv4"), "placeholder");
        }
      }

      control.dataset.key = field.key;
      control.dataset.section = section.id;
      control.dataset.type = field.type;
      control.dataset.orig = String(control.value || "");
      const rendered = AirCANnect.forms.decorate(field.key, control);
      const label = AirCANnect.i18n.message(
        "config.field." + field.key + ".label", {}, field.label || field.key);
      root.appendChild(AirCANnect.ui.row(label, rendered, field.key));
    }

    function as11BleStateLabel(data) {
      const labels = {
        idle: data.paired ? AirCANnect.i18n.message("pairing.paired") : AirCANnect.i18n.message("pairing.not_paired"),
        scanning: AirCANnect.i18n.message("pairing.scanning"),
        select_device: AirCANnect.i18n.message("pairing.select"),
        connecting: AirCANnect.i18n.message("pairing.connecting"),
        awaiting_passkey: AirCANnect.i18n.message("pairing.enter_code"),
        verifying: AirCANnect.i18n.message("pairing.verifying"),
        saving: AirCANnect.i18n.message("pairing.saving"),
        complete: data.paired ? AirCANnect.i18n.message("pairing.paired") : AirCANnect.i18n.message("pairing.removed"),
        failed: AirCANnect.i18n.message("pairing.failed"),
      };
      return labels[data.state] || data.state || AirCANnect.i18n.message("ui.unknown");
    }

    function renderAs11BlePairingPanel(panel, data) {
      const snapshot = JSON.stringify(data || {});
      if (panel.dataset.snapshot === snapshot) return;

      panel.dataset.snapshot = snapshot;
      panel.innerHTML = "";

      const state = AirCANnect.ui.valueSpan(as11BleStateLabel(data));
      panel.appendChild(AirCANnect.ui.row(AirCANnect.i18n.message("pairing.ble"), state));

      if (data.paired_address) {
        panel.appendChild(AirCANnect.ui.row(AirCANnect.i18n.message("pairing.device"),
          AirCANnect.ui.valueSpan(data.paired_address)));
      }

      if ((data.selected_name || data.selected_address) &&
          (!data.paired_address || data.selected_address !== data.paired_address)) {
        const selected = [data.selected_name, data.selected_address]
          .filter(Boolean).join(" / ");
        panel.appendChild(AirCANnect.ui.row(AirCANnect.i18n.message("pairing.selected"), AirCANnect.ui.valueSpan(selected)));
      }

      if (data.state === "select_device") {
        const instruction = document.createElement("p");
        instruction.className = "as11-pairing-instruction";
        AirCANnect.i18n.text(instruction, AirCANnect.i18n.message("pairing.instructions"));
        panel.appendChild(instruction);

        const select = document.createElement("select");
        (data.devices || []).forEach((device) => {
          const option = document.createElement("option");
          option.value = device.address;
          AirCANnect.i18n.text(option, (device.name || "AS11") + " / " +
            device.address + " / " + device.rssi + " dBm");
          select.appendChild(option);
        });
        panel.appendChild(AirCANnect.ui.row(AirCANnect.i18n.message("ui.device"), select));
      }

      let passkey = null;
      if (data.passkey_required) {
        passkey = document.createElement("input");
        passkey.type = "text";
        passkey.inputMode = "numeric";
        passkey.autocomplete = "one-time-code";
        passkey.maxLength = 4;
        AirCANnect.i18n.text(passkey, "0000", "placeholder");
        panel.appendChild(AirCANnect.ui.row(AirCANnect.i18n.message("pairing.code"), passkey));
      }

      const buttons = document.createElement("div");
      buttons.className = "btns";
      if (data.state === "select_device") {
        const selectButton = document.createElement("button");
        selectButton.className = "btn primary";
        AirCANnect.i18n.text(selectButton, AirCANnect.i18n.message("pairing.continue"));
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
        AirCANnect.i18n.text(confirmButton, AirCANnect.i18n.message("ui.pair"));
        confirmButton.onclick = () => {
          const value = passkey ? passkey.value.trim() : "";
          if (!/^\d{4}$/.test(value)) {
            showAs11BlePairingError(
              AirCANnect.i18n.message("pairing.four_digits"));
            return;
          }
          as11BleAction("passkey", {passkey: value});
        };
        buttons.appendChild(confirmButton);
      } else if (!data.active) {
        const pairButton = document.createElement("button");
        pairButton.className = "btn primary";
        AirCANnect.i18n.text(pairButton, data.paired ? AirCANnect.i18n.message("pairing.another") : AirCANnect.i18n.message("ui.pair"));
        pairButton.disabled = !data.enabled;
        pairButton.onclick = () => as11BleAction("pair");
        buttons.appendChild(pairButton);
      }

      if (data.active) {
        const cancelButton = document.createElement("button");
        cancelButton.className = "btn";
        AirCANnect.i18n.text(cancelButton, AirCANnect.i18n.message("ui.cancel"));
        cancelButton.onclick = () => as11BleAction("cancel");
        buttons.appendChild(cancelButton);
      }
      if (data.paired && !data.active) {
        const forgetButton = document.createElement("button");
        forgetButton.className = "btn danger";
        AirCANnect.i18n.text(forgetButton, AirCANnect.i18n.message("pairing.forget"));
        forgetButton.onclick = as11BleForget;
        buttons.appendChild(forgetButton);
      }
      panel.appendChild(buttons);

      const message = document.createElement("div");
      message.className = "msg" + (data.error ? " err" : "");
      message.classList.add("as11-ble-msg");
      AirCANnect.i18n.text(message, data.error ||
        (!data.enabled ? AirCANnect.i18n.message("pairing.enable") : ""));
      panel.appendChild(message);
    }

    function as11BlePairingPanels() {
      return document.querySelectorAll("[data-as11-ble-pairing]");
    }

    function showAs11BlePairingError(text) {
      as11BlePairingPanels().forEach((panel) => {
        const message = panel.querySelector(".as11-ble-msg");
        if (!message) return;
        AirCANnect.i18n.text(message, text);
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
        if (showError) showAs11BlePairingError(AirCANnect.i18n.error(error));
      }
    }

    async function as11BleAction(action, values, showError = showAs11BlePairingError) {
      try {
        await AirCANnect.http.requestOk("/api/as11/ble", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify(Object.assign({action}, values || {})),
        });
        return true;
      } catch (error) {
        showError(AirCANnect.i18n.error(error));
        return false;
      }
    }

    function startAs11PairingFromDashboard() {
      AirCANnect.pages.show("config");
      as11BleAction("pair");
    }

    function as11BleForget() {
      if (!confirm(AirCANnect.i18n.t("pairing.forget_confirm"))) return;
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
          AirCANnect.i18n.text(heading, AirCANnect.i18n.message(
            "config.section." + section.id, {}, section.title));
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
          if (section.id === "vpn") renderWireguardImport(fields);
        });
        await loadAs11BlePairing(true);
      } catch (error) {
        AirCANnect.ui.message("configMsg", AirCANnect.i18n.error(error), false);
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
        return Promise.reject(AirCANnect.i18n.failure("error.missing_config_revision"));
      }
      return AirCANnect.snapshots.wait(
        "config", (data) => Number(data.revision || 0) === expected,
        afterSerial, 5000, AirCANnect.i18n.message("error.config_timeout")).then((data) => {
          if (!data.ok) {
            if (data.error) throw new Error(data.error);
            throw AirCANnect.i18n.failure("error.config_failed");
          }
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

    const wireguardKeyPattern = /^[A-Za-z0-9+/]{43}=$/;

    function validWireguardAddress(value) {
      const text = String(value || "").trim();
      if (!text.length) return true;
      const parts = text.split("/");
      if (parts.length > 2 || (parts.length === 2 && parts[1] !== "32")) return false;
      return parts[0].length > 0 && validIpv4(parts[0]);
    }

    function validWireguardEndpoint(value) {
      const text = String(value || "").trim();
      if (!text.length) return true;
      const match = /^([^\s:\[\]]+)(?::(\d{1,5}))?$/.exec(text);
      if (!match) return false;
      return !match[2] || (Number(match[2]) >= 1 && Number(match[2]) <= 65535);
    }

    function validWireguardAllowedIps(value) {
      return String(value || "").split(",").map((item) => item.trim())
        .filter((item) => item.length).every((item) => {
          const parts = item.split("/");
          if (parts.length > 2 || !validIpv4(parts[0]) || !parts[0].length) return false;
          return parts.length === 1 ||
            (/^\d{1,2}$/.test(parts[1]) && Number(parts[1]) <= 32);
        });
    }

    // Parses a wg-quick style .conf into config keys. Only IPv4 is used by
    // the device, so IPv6 addresses and routes are dropped with a note.
    function parseWireguardConf(text) {
      const result = {values: {}, notes: []};
      const peers = [];
      let section = null;
      let current = null;
      const iface = {};

      String(text || "").split(/\r?\n/).forEach((rawLine, index) => {
        const line = rawLine.replace(/[#;].*$/, "").trim();
        if (!line.length) return;
        const header = /^\[(\w+)\]$/.exec(line);
        if (header) {
          section = header[1].toLowerCase();
          if (section === "peer") {
            current = {};
            peers.push(current);
          } else if (section !== "interface") {
            throw AirCANnect.i18n.failure("wg.unknown_section", {line: index + 1, section: header[1]});
          }
          return;
        }
        const eq = line.indexOf("=");
        if (eq <= 0 || !section) {
          throw AirCANnect.i18n.failure("wg.invalid_line", {line: index + 1});
        }
        const key = line.slice(0, eq).trim().toLowerCase();
        const value = line.slice(eq + 1).trim();
        (section === "peer" ? current : iface)[key] = value;
      });

      if (!Object.prototype.hasOwnProperty.call(iface, "privatekey")) {
        throw AirCANnect.i18n.failure("wg.private_missing");
      }
      if (!iface.privatekey) {
        throw AirCANnect.i18n.failure("wg.private_empty");
      }
      if (peers.length !== 1) {
        throw AirCANnect.i18n.failure("wg.peer_count", {count: peers.length});
      }
      const peer = peers[0];
      if (!peer.publickey) throw AirCANnect.i18n.failure("wg.public_missing");
      if (!peer.endpoint) throw AirCANnect.i18n.failure("wg.endpoint_missing");

      const address = String(iface.address || "").split(",").map((item) => item.trim())
        .find((item) => validIpv4(item.split("/")[0]) && item.split("/")[0].length);
      if (!address) throw AirCANnect.i18n.failure("wg.address_missing");
      const addressIp = address.split("/")[0];
      if (address.includes("/") && address.split("/")[1] !== "32") {
        result.notes.push(AirCANnect.i18n.message("wg.address_used", {address, ip: addressIp}));
      }

      const allowedAll = String(peer.allowedips || "0.0.0.0/0").split(",")
        .map((item) => item.trim()).filter((item) => item.length);
      const allowed = allowedAll.filter((item) => !item.includes(":"));
      if (allowed.length !== allowedAll.length) result.notes.push(AirCANnect.i18n.message("wg.ipv6_ignored"));
      if (!allowed.length) throw AirCANnect.i18n.failure("wg.allowed_missing");

      // Hotspot resolvers often refuse queries arriving via the home WAN.
      const dnsAll = String(iface.dns || "").split(",").map((item) => item.trim())
        .filter((item) => item.length);
      const dns = dnsAll.find((item) => validIpv4(item));
      if (dnsAll.length && !dns) result.notes.push(AirCANnect.i18n.message("wg.dns_ignored"));
      if (dns && dnsAll.length > 1) result.notes.push(AirCANnect.i18n.message("wg.dns_used", {dns}));
      ["mtu", "listenport", "table", "preup", "postup", "predown", "postdown"]
        .forEach((key) => {
          if (iface[key]) result.notes.push(AirCANnect.i18n.message("wg.ignored", {key}));
        });

      result.values.wg_addr = addressIp;
      result.values.wg_key = iface.privatekey;
      result.values.wg_peer = peer.publickey;
      result.values.wg_psk = peer.presharedkey || "";
      result.values.wg_ep = peer.endpoint;
      result.values.wg_allowed = allowed.join(",");
      result.values.wg_dns = dns || "";
      if (peer.persistentkeepalive) {
        result.values.wg_ka = peer.persistentkeepalive;
      }

      const errors = [];
      if (!wireguardKeyPattern.test(result.values.wg_key)) errors.push("PrivateKey");
      if (!wireguardKeyPattern.test(result.values.wg_peer)) errors.push("PublicKey");
      if (result.values.wg_psk && !wireguardKeyPattern.test(result.values.wg_psk)) {
        errors.push("PresharedKey");
      }
      if (!validWireguardEndpoint(result.values.wg_ep)) errors.push("Endpoint");
      if (!validWireguardAllowedIps(result.values.wg_allowed)) errors.push("AllowedIPs");
      if (errors.length) throw AirCANnect.i18n.failure("wg.invalid", {fields: errors.join(", ")});
      return result;
    }

    function renderWireguardImport(root) {
      const panel = document.createElement("div");
      panel.className = "endpoint-config";

      const input = document.createElement("input");
      input.type = "file";
      input.accept = ".conf,text/plain";
      input.hidden = true;
      input.dataset.changeAction = "config.wireguard-import";

      const button = document.createElement("button");
      button.type = "button";
      button.className = "btn";
      AirCANnect.i18n.text(button, AirCANnect.i18n.message("wg.import"));
      button.dataset.action = "config.wireguard-pick";

      const status = document.createElement("small");
      status.dataset.wireguardImportStatus = "true";
      AirCANnect.i18n.text(status, AirCANnect.i18n.message("wg.import_hint"));

      panel.appendChild(input);
      panel.appendChild(AirCANnect.ui.row(AirCANnect.i18n.message("wg.config"), button));
      panel.appendChild(status);
      root.insertBefore(panel, root.firstChild);
    }

    async function importWireguardConf(input) {
      const file = input.files && input.files[0];
      input.value = "";
      if (!file) return;
      const root = input.closest(".config-section-fields");
      const status = root && root.querySelector("[data-wireguard-import-status]");
      try {
        if (file.size > 8192) throw AirCANnect.i18n.failure("wg.too_large");
        const parsed = parseWireguardConf(await file.text());
        Object.keys(parsed.values).forEach((key) => {
          const control = root.querySelector('[data-key="' + key + '"]');
          if (!control) return;
          control.value = parsed.values[key];
          control.dispatchEvent(new Event("input", {bubbles: true}));
        });
        const modeControl = root.querySelector('[data-key="wg_mode"]');
        if (modeControl && modeControl.value === "off") modeControl.value = "away";
        const text = AirCANnect.i18n.message("wg.imported", {file: file.name, notes: parsed.notes.length ? AirCANnect.i18n.join([" (", AirCANnect.i18n.join(parsed.notes, "; "), ")"]) : ""});
        if (status) AirCANnect.i18n.text(status, text);
        AirCANnect.ui.message("configMsg", text, true, true);
      } catch (error) {
        AirCANnect.ui.message("configMsg", AirCANnect.i18n.message("wg.import_failed", {error: AirCANnect.i18n.error(error)}), false);
      }
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

    function validateWireguardChanges(changes, messageId) {
      const has = (key) => Object.prototype.hasOwnProperty.call(changes, key);
      if (!Object.keys(changes).some((key) => key.startsWith("wg_"))) return true;
      const value = (key) => has(key) ? changes[key] : (configData ? configData[key] : "");
      const secretSet = (key) => has(key) ?
        String(changes[key] || "").length > 0 : !!(configData && configData[key + "_set"]);
      const fail = (text) => {
        AirCANnect.ui.message(messageId, text, false);
        return false;
      };

      for (const key of ["wg_key", "wg_psk"]) {
        if (has(key) && changes[key] && changes[key] !== "********" &&
            !wireguardKeyPattern.test(changes[key])) {
          return fail(AirCANnect.i18n.message("wg.invalid_key", {key}));
        }
      }
      if (has("wg_peer") && changes.wg_peer && !wireguardKeyPattern.test(changes.wg_peer)) {
        return fail(AirCANnect.i18n.message("wg.peer_invalid"));
      }
      if (!validWireguardAddress(value("wg_addr"))) {
        return fail(AirCANnect.i18n.message("wg.address_invalid"));
      }
      if (!validWireguardEndpoint(value("wg_ep"))) {
        return fail(AirCANnect.i18n.message("wg.endpoint_invalid"));
      }
      if (!validWireguardAllowedIps(value("wg_allowed"))) {
        return fail(AirCANnect.i18n.message("wg.allowed_invalid"));
      }
      if (String(value("wg_dns") || "").trim().length &&
          !validIpv4(String(value("wg_dns")).trim())) {
        return fail(AirCANnect.i18n.message("wg.dns_invalid"));
      }
      if (value("wg_mode") && value("wg_mode") !== "off") {
        const missing = ["wg_addr", "wg_peer", "wg_ep", "wg_allowed"]
          .filter((key) => !String(value(key) || "").trim().length);
        if (!secretSet("wg_key")) missing.push("wg_key");
        if (missing.length) {
          return fail(AirCANnect.i18n.message("wg.tunnel_missing", {fields: missing.join(", ")}));
        }
      }
      return true;
    }

    function validateConfigChanges(changes, messageId) {
      if (Object.prototype.hasOwnProperty.call(changes, "smb_ep") &&
          !validSmbEndpoint(changes.smb_ep)) {
        AirCANnect.ui.message(messageId, AirCANnect.i18n.message("config.smb_invalid"), false);
        return false;
      }
      if (Object.prototype.hasOwnProperty.call(changes, "shq_team") &&
          !validOptionalNumericId(changes.shq_team)) {
        AirCANnect.ui.message(messageId, AirCANnect.i18n.message("config.shq_team_invalid"), false);
        return false;
      }
      if (Object.prototype.hasOwnProperty.call(changes, "shq_device") &&
          !validOptionalNumericId(changes.shq_device)) {
        AirCANnect.ui.message(messageId, AirCANnect.i18n.message("config.shq_device_invalid"), false);
        return false;
      }

      if (!validateWireguardChanges(changes, messageId)) return false;

      const syslogHost = Object.prototype.hasOwnProperty.call(changes, "syslog_host") ?
        changes.syslog_host : (configData ? configData.syslog_host : "");
      const syslogEnabled = Object.prototype.hasOwnProperty.call(changes, "syslog_en") ?
        changes.syslog_en : !!(configData && configData.syslog_en);
      if (!validIpv4(syslogHost)) {
        AirCANnect.ui.message(messageId, AirCANnect.i18n.message("config.syslog_invalid"), false);
        return false;
      }
      if (syslogEnabled && !String(syslogHost || "").trim().length) {
        AirCANnect.ui.message(messageId, AirCANnect.i18n.message("config.syslog_required"), false);
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
        AirCANnect.ui.message(messageId, AirCANnect.i18n.message("config.no_changes"), true);
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
        AirCANnect.ui.message(messageId, AirCANnect.i18n.message("config.saved"), true);
        return true;
      } catch (error) {
        AirCANnect.ui.message(messageId, AirCANnect.i18n.error(error), false);
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
        AirCANnect.ui.message(panel.msg, AirCANnect.i18n.error(error), false, true);
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
      {id: "network", title: AirCANnect.i18n.message("onboarding.network")},
      {id: "as11", title: AirCANnect.i18n.message("onboarding.connection")},
      {id: "time", title: AirCANnect.i18n.message("ui.time")},
      {id: "access", title: AirCANnect.i18n.message("onboarding.access")},
      {id: "smb", title: AirCANnect.i18n.message("onboarding.smb")},
      {id: "sleephq", title: AirCANnect.i18n.message("onboarding.shq")},
    ];
    let onboardingStep = 0;
    let onboardingBusy = false;
    let onboardingTimezoneValue = null;
    let onboardingUser = null;
    let onboardingPassword = null;

    function onboardingNote(root, text) {
      const note = document.createElement("p");
      note.className = "onboarding-note";
      AirCANnect.i18n.text(note, text);
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
      AirCANnect.i18n.text(heading, AirCANnect.i18n.message("onboarding.wifi_add"));
      root.appendChild(heading);

      onboardingInputRow(root, "SSID", "onboardingWifiSsid", "text",
        "network-name");
      onboardingInputRow(root, AirCANnect.i18n.message("ui.password"), "onboardingWifiPass", "password",
        "current-password");

      try {
        const response = await AirCANnect.http.requestOk("/api/wifi");
        const data = await response.json();
        const current = data.ssid ? data.ssid + " / " + (data.ip || "--") :
          AirCANnect.i18n.message("onboarding.disconnected");
        root.insertBefore(AirCANnect.ui.row(AirCANnect.i18n.message("onboarding.current_wifi"), AirCANnect.ui.valueSpan(current)), heading);
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
        AirCANnect.i18n.message("onboarding.connection_hint"));
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
        AirCANnect.i18n.message("onboarding.time_hint"));
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
        AirCANnect.i18n.message("onboarding.access_hint"));
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
      AirCANnect.i18n.text(document.getElementById("onboardingTitle"), step.title);
      AirCANnect.i18n.text(document.getElementById("onboardingCounter"), AirCANnect.i18n.message("onboarding.counter", {step: onboardingStep + 1, total: onboardingSteps.length}));
      document.getElementById("onboardingBack").disabled =
        onboardingStep === 0;
      AirCANnect.i18n.text(document.getElementById("onboardingNext"), onboardingStep === onboardingSteps.length - 1 ? AirCANnect.i18n.message("onboarding.finish") : AirCANnect.i18n.message("ui.next"));
      root.innerHTML = "";
      AirCANnect.ui.clearMessage("onboardingMsg");
      onboardingRenderProgress();

      if (step.id === "network") await onboardingRenderNetwork(root);
      if (step.id === "as11") await onboardingRenderAs11(root);
      if (step.id === "time") onboardingRenderTime(root);
      if (step.id === "access") onboardingRenderAccess(root);
      if (step.id === "smb") {
        onboardingNote(root,
          AirCANnect.i18n.message("onboarding.smb_hint"));
        onboardingSectionFields(root, "smb");
      }
      if (step.id === "sleephq") {
        onboardingNote(root,
          AirCANnect.i18n.message("onboarding.shq_hint"));
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
            AirCANnect.i18n.message("config.no_changes") : AirCANnect.i18n.message("onboarding.access_deferred"),
          true);
        return true;
      }

      const changes = collectConfigChanges(root);
      if (Object.keys(changes).length) {
        const saved = await saveConfigFields(root, "onboardingMsg", null);
        if (!saved) return false;
      } else {
        AirCANnect.ui.message("onboardingMsg", AirCANnect.i18n.message("config.no_changes"), true);
      }

      if (onboardingSteps[onboardingStep].id !== "network") return true;
      const ssid = document.getElementById("onboardingWifiSsid").value.trim();
      if (!ssid) return true;

      const password = document.getElementById("onboardingWifiPass").value;
      try {
        await AirCANnect.http.postJson("/api/wifi", {
          action: "add", ssid, pass: password,
        });

        AirCANnect.ui.message("onboardingMsg", AirCANnect.i18n.message("onboarding.wifi_queued"), true);
        return true;
      } catch (error) {
        AirCANnect.ui.message("onboardingMsg", AirCANnect.i18n.error(error), false);
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
        5000, AirCANnect.i18n.message("onboarding.not_persisted"));
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
      AirCANnect.i18n.text(document.getElementById("onboardingTitle"), AirCANnect.i18n.message("onboarding.complete"));
      AirCANnect.i18n.text(document.getElementById("onboardingCounter"), "");
      document.getElementById("onboardingProgress").innerHTML = "";
      document.getElementById("onboardingActions").hidden = true;

      const root = document.getElementById("onboardingBody");
      root.innerHTML = "";
      const finish = document.createElement("div");
      finish.className = "onboarding-finish";
      const heading = document.createElement("h3");
      AirCANnect.i18n.text(heading, AirCANnect.i18n.message("onboarding.ready"));
      finish.appendChild(heading);

      const buttons = document.createElement("div");
      buttons.className = "btns";
      const dashboard = document.createElement("button");
      dashboard.className = "btn primary";
      AirCANnect.i18n.text(dashboard, AirCANnect.i18n.message("ui.dashboard"));
      dashboard.onclick = () => onboardingExit("dash");
      buttons.appendChild(dashboard);

      const configure = document.createElement("button");
      configure.className = "btn";
      AirCANnect.i18n.text(configure, AirCANnect.i18n.message("onboarding.configure_more"));
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
        AirCANnect.ui.message("onboardingMsg", AirCANnect.i18n.error(error), false);
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
    AirCANnect.actions.register("as11.connect-dashboard", async (_event, button) => {
      button.disabled = true;
      try {
        await as11BleAction("connect", null, (message) =>
          alert(AirCANnect.i18n.format(message)));
      } finally {
        button.disabled = false;
      }
    });
    AirCANnect.actions.register("config.save", () => saveConfig());
    AirCANnect.actions.register("config.wireguard-pick", (_event, element) => {
      const panel = element.closest(".endpoint-config");
      const input = panel && panel.querySelector('input[type="file"]');
      if (input) input.click();
    });
    AirCANnect.actions.register("config.wireguard-import", (_event, element) =>
      importWireguardConf(element));
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
    AirCANnect.snapshots.subscribe("exports", () => {
      document.querySelectorAll('[data-key="shq_mask"]').forEach((control) =>
        renderSleepHqMasks(control, control.value));
    });
    AirCANnect.events.subscribe("as11_ble", renderAs11BlePairing);
})();
