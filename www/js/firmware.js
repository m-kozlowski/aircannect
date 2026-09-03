(() => {
    "use strict";

    let otaData = null;
    const resmedOtaRate = {
      phase: "",
      total: 0,
      bytes: 0,
      time: 0,
      bps: 0,
    };
    let resmedOtaData = null;
    let resmedRepositoryCatalogRevision = 0;
    let resmedRepositoryLoadGeneration = 0;
    let resmedRepositoryLoaded = false;
    let resmedDirectUploadBusy = false;
    let resmedUploadOperation = null;
    let resmedCanAvailable = true;

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
          const response = await AirCANnect.http.request(path, {cache: "no-store"});
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

    function applyOtaSnapshot(data) {
      otaData = data;
      renderOta(data);
      return data;
    }

    function waitForOtaSnapshot(predicate, afterSerial, timeoutMs,
                                timeoutMessage) {
      return AirCANnect.snapshots.wait(
        "ota", predicate, afterSerial, timeoutMs, timeoutMessage);
    }

    async function loadEspOta() {
      try {
        const eventSerial = AirCANnect.snapshots.read("ota").serial;
        const response = await AirCANnect.http.requestOk("/api/ota");
        const data = await response.json();
        if (AirCANnect.snapshots.read("ota").serial === eventSerial) {
          AirCANnect.snapshots.publish("ota", data);
        }
      } catch (error) {
        AirCANnect.ui.message("otaMsg", error.message, false);
      }
    }

    async function loadResmedOta(refreshRepository) {
      try {
        const resmedEventSerial =
          AirCANnect.snapshots.read("resmed_ota").serial;
        const resmedResponse = await AirCANnect.http.requestOk("/api/resmed-ota");
        const resmedData = await resmedResponse.json();
        if (AirCANnect.snapshots.read("resmed_ota").serial ===
            resmedEventSerial) {
          AirCANnect.snapshots.publish("resmed_ota", resmedData);
        }
        loadResmedRepository(!!refreshRepository);
      } catch (error) {
        AirCANnect.ui.message("resmedOtaMsg", error.message, false);
      }
    }

    function loadOta() {
      loadEspOta();
      loadResmedOta(true);
    }

    function renderOta(data) {
      AirCANnect.ui.text("otaVersion", data.version || "--");
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
      AirCANnect.ui.text("otaLatest", latest);

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

      const releaseNotes = document.getElementById("otaReleaseNotes");
      if (releaseNotes) {
        releaseNotes.hidden = !data.update_available ||
          !data.update_release_notes_available;
      }

      const install = document.getElementById("otaInstall");
      if (install) install.disabled = active;

      if (!active && !(data.bytes || data.progress)) {
        AirCANnect.ui.text("otaProgress", "--");
        return;
      }
      if (data.url_active && !data.total_size && !data.wire_total_size) {
        AirCANnect.ui.text("otaProgress", "Resolving firmware URL...");
        return;
      }
      if (data.http_prepare_pending) {
        const size = data.total_size || data.wire_total_size || 0;
        AirCANnect.ui.text("otaProgress", "Preparing / " + AirCANnect.format.bytes(size));
        return;
      }
      if (data.http_prepared && !data.http_active) {
        const size = data.total_size || data.wire_total_size || 0;
        AirCANnect.ui.text("otaProgress", "Ready / " + AirCANnect.format.bytes(size));
        return;
      }
      let text;
      if (data.encoding === "zlib" && data.wire_total_size) {
        text = (data.progress || 0) + "% / " +
          AirCANnect.format.bytes(data.wire_bytes || 0) + " wire, " +
          AirCANnect.format.bytes(data.bytes || 0) + " raw";
      } else {
        text = (data.progress || 0) + "% / " + AirCANnect.format.bytes(data.bytes || 0);
      }
      AirCANnect.ui.text("otaProgress", text);

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
      AirCANnect.ui.message("otaUpdateMsg", "Checking for updates...", true, true);

      let data = null;
      try {
        const eventSerial = AirCANnect.snapshots.read("ota").serial;
        const response = await AirCANnect.http.request("/api/ota/check", {method: "POST"});
        data = await response.json();
        renderOta(data);
        if (!response.ok) {
          throw new Error(data.update_error || "Update check rejected");
        }

        const started = Date.now();
        if (data.update_check_pending && !data.update_check_active) {
          const startedData = await waitForOtaSnapshot(
            (next) => next.update_check_active ||
              !next.update_check_pending,
            eventSerial, 10000, "");
          if (!startedData) {
            AirCANnect.ui.message("otaUpdateMsg",
              "Check queued until the device is idle and network is available.",
              true, true);
            return;
          }
          data = startedData;
        }
        if (data.update_check_pending || data.update_check_active) {
          data = await waitForOtaSnapshot(
            (next) => !next.update_check_pending &&
              !next.update_check_active,
            eventSerial, Math.max(1, 60000 - (Date.now() - started)),
            "Update check timed out");
        }

        if (data.update_error) throw new Error(data.update_error);
        if (data.update_available) {
          AirCANnect.ui.message("otaUpdateMsg",
            "Version " + data.update_version + " is available.", true);
        } else {
          AirCANnect.ui.message("otaUpdateMsg", "This device is up to date.", true);
        }
      } catch (error) {
        AirCANnect.ui.message("otaUpdateMsg", error.message, false);
      } finally {
        if (button) {
          button.disabled = !data || !data.update_check_enabled ||
            data.update_check_pending || data.update_check_active;
        }
      }
    }

    async function otaShowReleaseNotes() {
      const dialog = document.getElementById("otaReleaseNotesDialog");
      const title = document.getElementById("otaReleaseNotesTitle");
      const text = document.getElementById("otaReleaseNotesText");
      if (!dialog || !title || !text) return;

      title.textContent = otaData && otaData.update_version
        ? "Release notes " + otaData.update_version
        : "Release notes";
      text.textContent = "Loading...";
      if (!dialog.open) dialog.showModal();

      try {
        const response = await AirCANnect.http.requestOk(
          "/api/ota/release-notes", {cache: "no-store"});
        text.textContent = await response.text();
      } catch (error) {
        text.textContent = "Release notes could not be loaded: " +
          error.message;
      }
    }

    function setOtaUploadProgress(percent, bytes) {
      AirCANnect.ui.text("otaProgress", percent + "% / " + AirCANnect.format.bytes(bytes || 0));
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
      const eventSerial = AirCANnect.snapshots.read("ota").serial;
      const response = await AirCANnect.http.request(endpoint, {method: "POST"});
      let data = await response.json();
      renderOta(data);
      if (!response.ok) {
        throw new Error(data.last_error || data.error || "URL update rejected");
      }

      data = await waitForOtaSnapshot(
        (next) => next.reboot_pending || next.http_ready ||
          (!!next.last_error && !next.url_active),
        eventSerial, 15 * 60 * 1000, "URL update timed out");
      if (data.last_error && !data.url_active && !data.reboot_pending) {
        throw new Error(data.last_error);
      }

      AirCANnect.ui.message(messageId, "Update installed. Restarting...", true, true);
      scheduleOtaReload();
      return true;
    }

    async function otaInstallFromUrl(url) {
      const query = otaUrlQuery(url);
      AirCANnect.ui.message("otaMsg", "Starting URL update...", true, true);
      return otaRunUrlUpdate("/api/ota/url?" + query, "otaMsg");
    }

    async function otaInstallAvailableUpdate() {
      const button = document.getElementById("otaInstallUpdate");
      if (button) button.disabled = true;
      AirCANnect.ui.message("otaUpdateMsg", "Starting release update...", true, true);

      let restarting = false;
      try {
        restarting = await otaRunUrlUpdate(
          "/api/ota/install-update", "otaUpdateMsg");
      } catch (error) {
        AirCANnect.ui.message("otaUpdateMsg", "Update failed: " + error.message, false, true);
        loadEspOta();
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
      const eventSerial = AirCANnect.snapshots.read("ota").serial;
      const prepareResponse = await AirCANnect.http.requestOk("/api/ota/prepare?" +
        otaUploadQuery(plan), {method: "POST"});
      let data = await prepareResponse.json();
      renderOta(data);
      if (!prepareResponse.ok) {
        throw new Error(data.last_error || data.error ||
          "OTA prepare rejected");
      }

      if (!data.http_prepared) {
        data = await waitForOtaSnapshot(
          (next) => next.http_prepared || !!next.last_error ||
            !next.http_prepare_pending,
          eventSerial, 15000, "OTA prepare timed out");
      }
      if (data.last_error) throw new Error(data.last_error);
      if (!data.http_prepared) throw new Error("OTA prepare did not start");
      return data;
    }

    async function otaInstallFromFile(file) {
      const plan = otaUploadPlan(file);
      const bar = document.getElementById("otaUploadBar");

      AirCANnect.ui.message("otaMsg", "Preparing OTA...", true, true);
      await prepareOtaUpload(plan);
      AirCANnect.ui.message("otaMsg", "Uploading...", true, true);

      const form = new FormData();
      form.append("firmware", file);

      const result = await AirCANnect.http.upload(
        "/api/ota/upload?" + otaUploadQuery(plan), form, {
          onProgress: (event) => {
            if (event.lengthComputable) {
              const percent = Math.min(100,
                Math.floor(event.loaded / event.total * 100));
              const bytes = Math.min(file.size, event.loaded);
              bar.style.width = percent + "%";
              setOtaUploadProgress(percent, bytes);
            }
          },
        });
      const data = JSON.parse(result.responseText || "{}");
      if (result.status >= 300) {
        renderOta(data);
        throw new Error(data.last_error || result.statusText);
      }

      bar.style.width = "100%";
      setOtaUploadProgress(100, data.wire_bytes || file.size);

      const restarting = data.reboot_pending || data.http_ready;
      AirCANnect.ui.message("otaMsg", restarting ?
        "Update installed. Restarting..." : "Upload finished",
      restarting, true);
      if (restarting) scheduleOtaReload();
      return restarting;
    }

    async function otaInstall() {
      const url = document.getElementById("otaUrl").value.trim();
      const file = document.getElementById("otaFile").files[0];

      if (!url && !file) {
        AirCANnect.ui.message("otaMsg", "Enter a firmware URL or select an image", false);
        return;
      }
      if (url && file) {
        AirCANnect.ui.message("otaMsg", "Choose either a firmware URL or an image", false);
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
        AirCANnect.ui.message("otaMsg", "Update failed: " + error.message, false, true);
        loadEspOta();
      } finally {
        if (!restarting) button.disabled = false;
      }
    }

    function resmedOtaDisplayPhase(data) {
      const phase = data.phase || "";
      if (data.transport === "service" &&
          (phase === "erasing" || phase === "uploading")) {
        return "programming";
      }
      return phase;
    }

    function applyResmedOtaSnapshot(data) {
      resmedOtaData = data;
      renderResmedOta(data);
      return data;
    }

    function resmedOtaError(data) {
      if (data.phase === "error") {
        return data.last_error || "ResMed OTA failed";
      }
      if (data.prepare_state === "error" ||
          data.prepare_state === "cancelled") {
        return data.prepare_error || "Image preparation failed";
      }
      return "";
    }

    function waitForResmedOtaSnapshot(predicate, afterSerial, timeoutMs,
                                      timeoutMessage) {
      return AirCANnect.snapshots.wait(
        "resmed_ota", predicate, afterSerial, timeoutMs, timeoutMessage);
    }

    function resmedOtaDumpSavedText(data) {
      const path = String(data.output_path || "");
      const filename = String(data.filename || path.split("/").pop() || "");
      return "Firmware dump saved" + (filename ? ": " + filename : "");
    }

    function resmedOtaStatusText(data) {
      const phase = resmedOtaDisplayPhase(data);
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
      if (phase === "reading_identity") return "Reading ResMed identity";
      if (phase === "checking_storage") return "Checking firmware repository";
      if (phase === "opening") return "Opening prepared image" + target;
      if (phase === "entering_service") return "Entering service mode";
      if (phase === "dumping") return "Reading current firmware";
      if (phase === "publishing") return data.operation === "dump" ?
        "Saving firmware dump" : "Publishing firmware";
      if (phase === "bootloader_required") {
        return "Patched bootloader required";
      }
      if (phase === "preparing_bootloader") {
        return "Preparing patched bootloader";
      }
      if (phase === "programming") return "Programming ResMed" + target;
      if (phase === "initiating") return "Starting device upload" + target;
      if (phase === "ready" || phase === "uploading") {
        return data.transport === "service" ?
          "Programming ResMed" + target : "Sending to ResMed" + target;
      }
      if (phase === "uploaded" || phase === "checking") {
        return "Verifying on ResMed";
      }
      if (phase === "verified") return "Firmware verified";
      if (phase === "applying") return "Installing firmware";
      if (phase === "resetting") return "Restarting ResMed";
      if (phase === "complete") return data.operation === "dump" ?
        resmedOtaDumpSavedText(data) : "Installation complete";
      if (phase === "error") return data.last_error || "ResMed OTA failed";
      return phase || "--";
    }

    function resmedOtaTransferActive(data) {
      const phase = resmedOtaDisplayPhase(data);
      return phase === "programming" || phase === "ready" ||
        phase === "uploading" || phase === "dumping";
    }

    function resetResmedOtaRate(data, now, bytes, total) {
      resmedOtaRate.phase = resmedOtaDisplayPhase(data);
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

      if (resmedOtaRate.phase !== resmedOtaDisplayPhase(data) ||
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
      const eta = AirCANnect.format.duration(remaining / resmedOtaRate.bps);
      return AirCANnect.format.bytes(resmedOtaRate.bps) + "/s" + (eta ? " ETA " + eta : "");
    }

    function renderResmedOta(data) {
      resmedCanAvailable = data.can_available !== false;
      const installAvailable = resmedOtaInstallAvailable();
      document.querySelectorAll(".resmed-repository-actions .primary")
        .forEach((button) => {
          button.disabled = !installAvailable;
          button.title = installAvailable ? "" :
            "Patched Bootloader requires CAN";
        });

      const rateText = resmedOtaRateText(data);
      const displayPhase = resmedOtaDisplayPhase(data);
      const stateText = resmedOtaStatusText(data) +
        (data.waiting && displayPhase !== "programming" ?
          " / waiting" : "");
      const progressValue = data.prepare_active ?
        Number(data.prepare_progress || 0) : Number(data.progress || 0);
      const progressText = progressValue + "%" +
        (rateText ? " " + rateText : "");
      AirCANnect.ui.text("resmedOtaState", stateText);
      AirCANnect.ui.text("resmedOtaProgress", progressText);
      AirCANnect.ui.text("resmedOtaHash", data.computed_sha256 || data.expected_sha256 || "--");

      const cancel = document.getElementById("resmedOtaCancelBtn");
      if (cancel) {
        const cancellable = (data.active || resmedDirectUploadBusy) &&
          data.phase !== "applying";
        cancel.style.display = cancellable ? "" : "none";
        cancel.disabled = !cancellable;
      }

      const install = document.getElementById("resmedOtaInstallBtn");
      if (install) {
        install.disabled = !installAvailable || data.active ||
          resmedDirectUploadBusy;
      }

      const dump = document.getElementById("resmedOtaDumpBtn");
      if (dump) {
        dump.disabled = !resmedCanAvailable || data.active ||
          resmedDirectUploadBusy;
      }

      const dumpConfirm = document.getElementById("resmedOtaDumpConfirmBtn");
      if (dumpConfirm) {
        dumpConfirm.hidden = !data.confirmation_required;
        dumpConfirm.disabled = !data.confirmation_required ||
          !data.recovery_available;
      }
      const target = document.getElementById("resmedOtaTarget");
      if (target) {
        target.disabled = data.active || resmedDirectUploadBusy;
      }

      const transport = document.getElementById("resmedOtaTransport");
      if (transport) {
        transport.disabled = data.active || resmedDirectUploadBusy;
      }

      const progress = document.getElementById("resmedOtaUploadProgress");
      const bar = document.getElementById("resmedOtaUploadBar");
      progress.style.display =
        data.active || resmedDirectUploadBusy || data.phase === "complete" ||
          data.phase === "verified" ?
          "block" : "none";
      bar.style.width = progressValue + "%";
      if (data.confirmation_required) {
        AirCANnect.ui.message("resmedOtaMsg", "Matching patched bootloader found", false, true);
      } else if (data.prepare_error || data.last_error) {
        AirCANnect.ui.message("resmedOtaMsg", data.prepare_error || data.last_error,
          false, true);
      } else if (!installAvailable) {
        AirCANnect.ui.message("resmedOtaMsg", "Patched Bootloader requires CAN",
          false, true);
      } else if (data.phase === "verified") {
        AirCANnect.ui.message("resmedOtaMsg", "Firmware verified", true, true);
      } else if (data.phase === "complete") {
        AirCANnect.ui.message("resmedOtaMsg", data.operation === "dump" ?
          resmedOtaDumpSavedText(data) : "Installation complete", true, true);
      } else if (data.active) {
        AirCANnect.ui.message("resmedOtaMsg", stateText, true, true);
      }
    }

    function resmedRepositoryUploadProgress(name, committed, total,
                                             fileIndex, fileCount) {
      AirCANnect.ui.uploadProgress(
        "resmedRepositoryUpload", name, committed, total,
        fileIndex, fileCount);
    }

    function setResmedRepositoryUploadBusy(busy) {
      const progress = document.getElementById(
        "resmedRepositoryUploadProgress");
      const add = document.getElementById("resmedRepositoryAddBtn");
      if (progress) progress.hidden = !busy;
      if (add) add.disabled = !!busy;
    }

    function renderResmedRepositoryStatus(data) {
      AirCANnect.ui.text("resmedRepositoryPath", data.directory ||
        "/aircannect/resmed-firmware");

      const refresh = document.getElementById("resmedRepositoryRefreshBtn");
      if (refresh) {
        refresh.disabled = !!data.refresh_pending ||
          ["preparing", "scanning", "inspecting", "storing_bootloader",
           "removing"].includes(data.state);
      }

      if (data.error) {
        AirCANnect.ui.message("resmedRepositoryMsg", data.error, false, true);
      }
    }

    async function resmedRepositoryDownload(path) {
      AirCANnect.ui.message(
        "resmedRepositoryMsg", "Preparing download", true, false);
      try {
        await AirCANnect.files.download(path);
        AirCANnect.ui.message(
          "resmedRepositoryMsg", "Download started", true, false);
      } catch (error) {
        AirCANnect.ui.message(
          "resmedRepositoryMsg", error.message, false, true);
      }
    }

    function renderResmedRepository(data) {
      renderResmedRepositoryStatus(data);
      resmedRepositoryCatalogRevision = Number(data.revision) || 0;
      resmedRepositoryLoaded = true;

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
        marker.textContent = entry.kind_hint === "abc" ? "ABC" :
          entry.kind_hint === "raw" ? "RAW" : "?";
        marker.className = "storage-meta";

        const details = document.createElement("div");
        const name = document.createElement("div");
        name.className = "storage-name";
        name.textContent = entry.name || entry.path || "--";
        name.tabIndex = 0;
        name.onclick = () => resmedRepositoryDownload(entry.path);
        name.onkeydown = (event) => {
          if (event.key === "Enter" || event.key === " ") {
            event.preventDefault();
            resmedRepositoryDownload(entry.path);
          }
        };
        const meta = document.createElement("div");
        meta.className = "storage-meta";
        meta.textContent = AirCANnect.format.bytes(entry.size) +
          (AirCANnect.format.modified(entry.modified) ?
            " / " + AirCANnect.format.modified(entry.modified) : "");
        details.appendChild(name);
        details.appendChild(meta);

        const actions = document.createElement("div");
        actions.className = "resmed-repository-actions";
        const install = document.createElement("button");
        install.className = "btn primary";
        install.textContent = "Install";
        install.disabled = !resmedOtaInstallAvailable();
        install.title = resmedOtaInstallAvailable() ? "" :
          "Patched Bootloader requires CAN";
        install.onclick = () => resmedRepositoryInstall(
          entry.path, entry.name || entry.path);
        actions.appendChild(install);

        const rename = document.createElement("button");
        rename.className = "btn";
        rename.textContent = "Rename";
        rename.onclick = () => resmedRepositoryRename(entry.path,
          entry.name || entry.path);
        actions.appendChild(rename);

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

    }

    async function fetchResmedRepository(refresh) {
      const entries = [];
      let offset = 0;
      let result = null;
      for (let page = 0; page < 4; page++) {
        const url = "/api/resmed-ota/repository?offset=" + offset +
          "&limit=128" + (refresh && page === 0 ? "&refresh=1" : "");
        const response = await AirCANnect.http.request(url, {cache: "no-store"});
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
      const generation = ++resmedRepositoryLoadGeneration;

      try {
        const data = await fetchResmedRepository(!!refresh);
        if (generation !== resmedRepositoryLoadGeneration) return;
        renderResmedRepository(data);
      } catch (error) {
        if (generation !== resmedRepositoryLoadGeneration) return;
        AirCANnect.ui.message("resmedRepositoryMsg", error.message, false, true);
      }
    }

    function resmedRepositoryChooseFiles() {
      if (resmedUploadOperation) return;
      const input = document.getElementById("resmedRepositoryInput");
      if (input) input.click();
    }

    function resmedRepositoryFilesSelected(input) {
      const files = input && input.files ? Array.from(input.files) : [];
      if (input) input.value = "";
      if (!files.length || resmedUploadOperation) return;
      resmedRepositoryUploadQueue(files);
    }

    async function resmedRepositoryUploadQueue(files) {
      const operation = AirCANnect.uploads.begin();
      if (!operation) {
        AirCANnect.ui.message(
          "resmedRepositoryMsg", "Another upload is already active",
          false, true);
        return;
      }

      resmedUploadOperation = operation;
      setResmedRepositoryUploadBusy(true);
      let uploaded = 0;
      try {
        for (let index = 0; index < files.length; index++) {
          if (operation.cancelled) break;
          const file = files[index];
          resmedRepositoryUploadProgress(file.name, 0, file.size,
            index, files.length);
          if (await operation.file(file, "/aircannect/resmed-firmware", {
              fileIndex: index,
              fileCount: files.length,
              progress: resmedRepositoryUploadProgress,
            })) {
            uploaded++;
          }
        }

        if (operation.cancelled) {
          AirCANnect.ui.message("resmedRepositoryMsg", "Upload cancelled", true, false);
        } else {
          AirCANnect.ui.message("resmedRepositoryMsg", "Added " + uploaded + " image" +
            (uploaded === 1 ? "" : "s"), true, false);
        }
        await loadResmedRepository(true);
      } catch (error) {
        AirCANnect.ui.message("resmedRepositoryMsg", error.message, false, true);
      } finally {
        operation.close();
        if (resmedUploadOperation === operation) resmedUploadOperation = null;
        setResmedRepositoryUploadBusy(false);
      }
    }

    async function resmedRepositoryCancelUpload() {
      const operation = resmedUploadOperation;
      if (!operation) return;

      try {
        await operation.cancel();
      } catch (_) {}
    }

    async function resmedRepositoryRemove(path, name) {
      if (!confirm("Remove " + name + " from the firmware repository?")) {
        return;
      }

      try {
        const response = await AirCANnect.http.request("/api/resmed-ota/repository/remove", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({path}),
        });
        const data = await response.json();
        if (!response.ok) throw new Error(data.error || "Remove failed");
      } catch (error) {
        AirCANnect.ui.message("resmedRepositoryMsg", error.message, false, true);
      }
    }

    async function resmedRepositoryRename(path, currentName) {
      try {
        const slash = path.lastIndexOf("/");
        const base = slash > 0 ? path.slice(0, slash) : "/";
        const name = slash >= 0 ? path.slice(slash + 1) : currentName;
        const renamed = await AirCANnect.files.rename(
          base, name, "Rename firmware image");
        if (!renamed) return;

        AirCANnect.ui.message("resmedRepositoryMsg", "Renamed " + currentName + " to " +
          renamed, true, true);
        await loadResmedRepository(true);
      } catch (error) {
        AirCANnect.ui.message("resmedRepositoryMsg", error.message, false, true);
      }
    }

    function selectedResmedOtaTarget() {
      const select = document.getElementById("resmedOtaTarget");
      return select && select.value ? select.value : "APCX";
    }

    function selectedResmedOtaTransport() {
      const select = document.getElementById("resmedOtaTransport");
      return select && select.value ? select.value : "rpc";
    }

    function resmedOtaInstallAvailable() {
      return selectedResmedOtaTransport() !== "service" ||
        resmedCanAvailable;
    }

    function confirmResmedOtaTarget(target, name) {
      if (target === "FGBL") {
        return confirm("Install " + name +
          " into the ResMed bootloader region (FGBL)?");
      }
      if (target === "FGCB") {
        return confirm("Replace the complete ResMed internal flash with " +
          name + "?");
      }
      return true;
    }

    async function resmedRepositoryInstall(path, name) {
      if (!resmedOtaInstallAvailable()) {
        AirCANnect.ui.message("resmedRepositoryMsg",
          "Patched Bootloader requires CAN", false, true);
        return;
      }

      const target = selectedResmedOtaTarget();
      const transport = selectedResmedOtaTransport();
      if (!confirmResmedOtaTarget(target, name)) return;

      try {
        AirCANnect.ui.message("resmedRepositoryMsg", "Installing " + name, true, true);
        const request = await postResmedOta("/api/resmed-ota/install", {
          path,
          filename: name,
          transient: false,
          target,
          transport,
        });
        await waitResmedOtaStart(request.eventSerial);
        await waitResmedOta((data) => data.phase === "complete", 4200,
          request.eventSerial);
        AirCANnect.ui.message("resmedRepositoryMsg", "Installation complete", true, true);
      } catch (error) {
        AirCANnect.ui.message("resmedRepositoryMsg", error.message, false, true);
      }
    }

    async function getResmedOta() {
      const eventSerial = AirCANnect.snapshots.read("resmed_ota").serial;
      const response = await AirCANnect.http.requestOk("/api/resmed-ota");
      const data = await response.json();
      if (AirCANnect.snapshots.read("resmed_ota").serial === eventSerial) {
        AirCANnect.snapshots.publish("resmed_ota", data);
      }
      return data;
    }

    async function waitResmedOta(predicate, attempts, afterSerial) {
      const data = await waitForResmedOtaSnapshot(
        (next) => !!resmedOtaError(next) || predicate(next),
        afterSerial, (attempts || 360) * 500, "ResMed OTA timeout");
      const error = resmedOtaError(data);
      if (error) throw new Error(error);
      return data;
    }

    async function waitResmedOtaStart(afterSerial, attempts) {
      const data = await waitForResmedOtaSnapshot(
        (next) => next.active || !!resmedOtaError(next),
        afterSerial, (attempts || 20) * 500,
        "ResMed OTA did not start");
      const error = resmedOtaError(data);
      if (error) throw new Error(error);
      return data;
    }

    async function postResmedOta(url, body) {
      const eventSerial = AirCANnect.snapshots.read("resmed_ota").serial;
      const response = await AirCANnect.http.requestOk(url, {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(body || {}),
      });
      const data = await response.json();
      if (!response.ok) {
        throw new Error(data.error || data.last_error ||
          ("HTTP " + response.status));
      }
      if (!data.queued && data.result !== "queued") {
        AirCANnect.snapshots.publish("resmed_ota", data);
      }
      return {data, eventSerial};
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
      const install = document.getElementById("resmedOtaInstallBtn");

      AirCANnect.ui.text("resmedOtaState", "Uploading " + name);
      AirCANnect.ui.text("resmedOtaProgress", percent + "% " + AirCANnect.format.bytes(safeCommitted) +
        " / " + AirCANnect.format.bytes(safeTotal));
      if (progress) progress.style.display = "block";
      if (bar) bar.style.width = percent + "%";
      if (cancel) {
        cancel.style.display = "";
        cancel.disabled = false;
      }
      if (install) install.disabled = true;
    }

    async function resmedOtaInstall() {
      const file = document.getElementById("resmedOtaFile").files[0];
      if (!file) {
        AirCANnect.ui.message("resmedOtaMsg", "Select firmware image", false, true);
        return;
      }

      const target = selectedResmedOtaTarget();
      const transport = selectedResmedOtaTransport();
      if (!resmedOtaInstallAvailable()) {
        AirCANnect.ui.message("resmedOtaMsg", "Patched Bootloader requires CAN", false, true);
        return;
      }
      if (!confirmResmedOtaTarget(target, file.name)) return;

      try {
        const current = await getResmedOta();
        if (current.active) {
          throw new Error("Another ResMed firmware operation is active");
        }

        const operation = AirCANnect.uploads.begin();
        if (!operation) throw new Error("Another upload is already active");

        resmedUploadOperation = operation;
        resmedDirectUploadBusy = true;
        const install = document.getElementById("resmedOtaInstallBtn");
        if (install) install.disabled = true;
        AirCANnect.ui.message("resmedOtaMsg", "Uploading", true, true);
        const uploaded = await operation.file(file, "/aircannect", {
            fileIndex: 0,
            fileCount: 1,
            progress: resmedDirectUploadProgress,
            filename: "resmed-ota-input.image",
            conflict: "replace",
            confirmReplace: false,
          });
        if (!uploaded) throw new Error("Upload was not completed");

        operation.close();
        if (resmedUploadOperation === operation) resmedUploadOperation = null;
        resmedDirectUploadBusy = false;
        const request = await postResmedOta("/api/resmed-ota/install", {
          path: "/aircannect/resmed-ota-input.image",
          filename: file.name,
          transient: true,
          target,
          transport,
        });
        await waitResmedOtaStart(request.eventSerial);
        await waitResmedOta((data) => data.phase === "complete", 4200,
          request.eventSerial);
        AirCANnect.ui.message("resmedOtaMsg", "Installation complete", true, true);
      } catch (error) {
        AirCANnect.ui.message("resmedOtaMsg", error.message, false, true);
        loadResmedOta(false);
      } finally {
        if (resmedUploadOperation) {
          resmedUploadOperation.close();
          resmedUploadOperation = null;
        }
        resmedDirectUploadBusy = false;
      }
    }

    async function resmedOtaDump() {
      try {
        const request = await postResmedOta("/api/resmed-ota/dump", {});
        await waitResmedOtaStart(request.eventSerial);
        const result = await waitResmedOta((data) =>
          data.phase === "complete" || data.confirmation_required, 4200,
          request.eventSerial);
        if (result.confirmation_required) return;

        AirCANnect.ui.message("resmedOtaMsg", resmedOtaDumpSavedText(result), true, true);
        loadResmedRepository(true);
      } catch (error) {
        AirCANnect.ui.message("resmedOtaMsg", error.message, false, true);
      }
    }

    async function resmedOtaConfirmBootloader() {
      const current = await getResmedOta();
      if (!current.confirmation_required || !current.recovery_available) return;
      if (!confirm("Install the matching patched bootloader from " +
          current.recovery_path + " and retry the firmware dump?")) {
        return;
      }

      try {
        const request = await postResmedOta(
          "/api/resmed-ota/dump/confirm", {
          confirm: "INSTALL_PATCHED_BOOTLOADER",
        });
        const result = await waitResmedOta(
          (data) => data.phase === "complete", 4200,
          request.eventSerial);
        AirCANnect.ui.message("resmedOtaMsg", resmedOtaDumpSavedText(result), true, true);
        loadResmedRepository(true);
      } catch (error) {
        AirCANnect.ui.message("resmedOtaMsg", error.message, false, true);
      }
    }

    async function resmedOtaCancel() {
      if (!confirm("Cancel the current ResMed firmware operation?")) {
        return;
      }

      try {
        if (resmedUploadOperation) await resmedUploadOperation.cancel();
        await postResmedOta("/api/resmed-ota/abort", {});
        AirCANnect.ui.message("resmedOtaMsg", "Cancelled", false, true);
      } catch (error) {
        AirCANnect.ui.message("resmedOtaMsg", error.message, false, true);
      }
    }


    AirCANnect.actions.register("ota.check", () => otaCheckForUpdates());
    AirCANnect.actions.register("ota.install-update", () =>
      otaInstallAvailableUpdate());
    AirCANnect.actions.register("ota.release-notes", () =>
      otaShowReleaseNotes());
    AirCANnect.actions.register("ota.source", (_event, element) =>
      otaSourceChanged(element.dataset.value));
    AirCANnect.actions.register("ota.install", () => otaInstall());
    AirCANnect.actions.register("resmed.transport", () =>
      loadResmedOta(false));
    AirCANnect.actions.register("resmed.repository-choose", () =>
      resmedRepositoryChooseFiles());
    AirCANnect.actions.register("resmed.dump", () => resmedOtaDump());
    AirCANnect.actions.register("resmed.repository-refresh", () =>
      loadResmedRepository(true));
    AirCANnect.actions.register("resmed.dump-confirm", () =>
      resmedOtaConfirmBootloader());
    AirCANnect.actions.register(
      "resmed.repository-files", (_event, element) =>
        resmedRepositoryFilesSelected(element));
    AirCANnect.actions.register("resmed.repository-cancel", () =>
      resmedRepositoryCancelUpload());
    AirCANnect.actions.register("resmed.install", () => resmedOtaInstall());
    AirCANnect.actions.register("resmed.cancel", () => resmedOtaCancel());
    AirCANnect.pages.onLoad("ota", () => loadOta());
    AirCANnect.snapshots.subscribe("ota", applyOtaSnapshot, false);
    AirCANnect.events.subscribe("ota", () => {});
    AirCANnect.snapshots.subscribe(
      "resmed_ota", applyResmedOtaSnapshot, false);
    AirCANnect.events.subscribe("resmed_ota", () => {});
    AirCANnect.events.subscribe("resmed_repository", (data) => {
      renderResmedRepositoryStatus(data);
      if (resmedRepositoryLoaded && data.state === "ready" &&
          Number(data.revision) !== resmedRepositoryCatalogRevision) {
        loadResmedRepository(false);
      }
    });
})();
