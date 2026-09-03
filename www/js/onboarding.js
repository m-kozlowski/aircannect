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
          const status = statusLoaded ? statusData : await loadStatus();
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
