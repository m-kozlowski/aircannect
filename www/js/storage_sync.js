    function fmtBytes(bytes) {
      const value = Number(bytes);
      if (!Number.isFinite(value) || value < 0) return "--";
      if (value < 1024) return Math.round(value) + " B";
      if (value < 1024 * 1024) return (value / 1024).toFixed(1) + " KiB";
      if (value < 1024 * 1024 * 1024) {
        return (value / (1024 * 1024)).toFixed(1) + " MiB";
      }
      return (value / (1024 * 1024 * 1024)).toFixed(2) + " GiB";
    }

    function fmtStorageModified(value) {
      const seconds = Number(value);
      if (!Number.isFinite(seconds) || seconds <= 0) return "";
      const date = new Date(seconds * 1000);
      if (Number.isNaN(date.getTime())) return "";
      return date.getFullYear() + "-" +
        pad2(date.getMonth() + 1) + "-" +
        pad2(date.getDate()) + " " +
        pad2(date.getHours()) + ":" +
        pad2(date.getMinutes());
    }

    function storageErrorText(text, status) {
      try {
        const data = JSON.parse(text);
        if (data && data.error) return data.error;
      } catch (_) {}
      return text || ("HTTP " + status);
    }

    function storageDelay(ms) {
      return new Promise((resolve) => setTimeout(resolve, ms));
    }

    async function fetchStorageList(path, offset, limit, refresh) {
      let forceRefresh = !!refresh;
      for (let attempt = 0; attempt < 200; attempt++) {
        const url = "/api/storage/list?path=" + encodeURIComponent(path) +
          "&offset=" + encodeURIComponent(offset || 0) +
          "&limit=" + encodeURIComponent(limit || 128) +
          (forceRefresh ? "&refresh=1" : "");
        forceRefresh = false;

        const response = await fetch(url, {cache: "no-store"});
        const text = await response.text();
        if (response.status === 202) {
          await storageDelay(150);
          continue;
        }
        if (!response.ok) {
          throw new Error(storageErrorText(text, response.status));
        }
        return JSON.parse(text);
      }
      throw new Error("list_prepare_timeout");
    }

    async function fetchStorageEntries(path, maxPages) {
      let offset = 0;
      const entries = [];
      const pages = maxPages || 4;
      for (let page = 0; page < pages; page++) {
        const data = await fetchStorageList(path, offset, 128);
        if (data && Array.isArray(data.entries)) {
          data.entries.forEach((entry) => entries.push(entry));
        }
        if (!data || data.next_offset === null || data.next_offset === undefined) break;
        offset = Number(data.next_offset);
        if (!Number.isFinite(offset)) break;
      }
      return entries;
    }

    function storageParentPath(path) {
      if (!path || path === "/") return "/";
      const index = path.lastIndexOf("/");
      return index <= 0 ? "/" : path.slice(0, index);
    }

    function storageSetBadge(text, cls) {
      const badge = document.getElementById("storageBadge");
      if (!badge) return;
      badge.textContent = text;
      badge.className = "badge" + (cls ? " " + cls : "");
    }

    function edfSessionLabel(prefix) {
      const match = String(prefix || "").match(/^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})$/);
      if (!match) return prefix || "--";
      return match[1] + "-" + match[2] + "-" + match[3] + " " +
        match[4] + ":" + match[5] + ":" + match[6];
    }

    function edfFileKind(name) {
      const match = String(name || "").match(/^(\d{8}_\d{6})_(BRP|PLD|SA2|EVE|CSL)\.edf$/i);
      if (!match) return null;
      return {prefix: match[1], kind: match[2].toUpperCase()};
    }

    async function loadEdfOverview() {
      if (edfOverviewLoading) return;
      edfOverviewLoading = true;
      up("edfLastSession", "Loading");
      up("edfLastFiles", "--");
      try {
        let datalogEntries = [];
        try {
          datalogEntries = await fetchStorageEntries("/DATALOG", 1);
        } catch (error) {
          if (String(error && error.message || "") !== "not_found") throw error;
        }
        const days = datalogEntries
          .filter((entry) => entry.type === "dir" && /^\d{8}$/.test(entry.name || ""))
          .map((entry) => entry.name)
          .sort()
          .reverse();

        let bestPrefix = "";
        let bestFiles = null;
        for (const day of days.slice(0, 8)) {
          const entries = await fetchStorageEntries("/DATALOG/" + day, 8);
          const sessions = new Map();
          entries.forEach((entry) => {
            const parsed = edfFileKind(entry.name);
            if (!parsed) return;
            if (!sessions.has(parsed.prefix)) sessions.set(parsed.prefix, []);
            sessions.get(parsed.prefix).push(entry);
          });
          Array.from(sessions.keys()).sort().reverse().some((prefix) => {
            if (!bestPrefix || prefix > bestPrefix) {
              bestPrefix = prefix;
              bestFiles = sessions.get(prefix);
            }
            return true;
          });
          if (bestPrefix) break;
        }

        if (!bestPrefix || !bestFiles) {
          up("edfLastSession", "No EDF sessions");
          up("edfLastFiles", "--");
          return;
        }

        const totalBytes = bestFiles.reduce((sum, entry) =>
          sum + (Number(entry.size) || 0), 0);
        up("edfLastSession", edfSessionLabel(bestPrefix));
        up("edfLastFiles", bestFiles.length + " file" +
          (bestFiles.length === 1 ? "" : "s") + ", " + fmtBytes(totalBytes));
      } catch (error) {
        up("edfLastSession", "Unavailable");
        up("edfLastFiles", error.message || "Storage error");
      } finally {
        edfOverviewLoading = false;
      }
    }

    function storageSelectionUi() {
      const selected = storageSelectedNames.size;
      const storageJobBusy = storageArchiveBusy || storageDeleteBusy;
      const exportBusy = smbSyncBusy || sleepHqSyncBusy;
      const archiveBusy = storageJobBusy || exportBusy;
      const destructiveBusy = storageJobBusy || exportBusy;
      const endpointBusy = storageJobBusy || exportBusy;
      const selectAll = document.getElementById("storageSelectAll");
      if (selectAll) {
        const selectable = storageEntries.length;
        selectAll.disabled = storageJobBusy || selectable === 0;
        selectAll.checked = selectable > 0 && selected === selectable;
        selectAll.indeterminate = selected > 0 && selected < selectable;
      }
      document.querySelectorAll("[data-storage-select]").forEach((input) => {
        input.disabled = storageJobBusy;
        input.checked = storageSelectedNames.has(input.dataset.storageSelect || "");
      });
      const archiveBtn = document.getElementById("storageArchiveBtn");
      if (archiveBtn) {
        archiveBtn.disabled = archiveBusy || selected === 0;
        archiveBtn.textContent = selected > 0 ?
          "Download Selected (" + selected + ")" : "Download Selected";
      }
      const deleteBtn = document.getElementById("storageDeleteBtn");
      if (deleteBtn) {
        deleteBtn.disabled = destructiveBusy || selected === 0;
        deleteBtn.textContent = selected > 0 ?
          "Delete Selected (" + selected + ")" : "Delete Selected";
      }
      const syncBtn = document.getElementById("edfSyncBtn");
      if (syncBtn) syncBtn.disabled = endpointBusy || !smbSyncEnabled || !smbSyncConfigured;
      const verifyBtn = document.getElementById("edfVerifyBtn");
      if (verifyBtn) verifyBtn.disabled = endpointBusy || !smbSyncEnabled || !smbSyncConfigured;
      const sleepSyncBtn = document.getElementById("edfSleepHqSyncBtn");
      if (sleepSyncBtn) sleepSyncBtn.disabled = endpointBusy || !sleepHqSyncConfigured;
      const sleepCheckBtn = document.getElementById("edfSleepHqCheckBtn");
      if (sleepCheckBtn) sleepCheckBtn.disabled = endpointBusy || !sleepHqSyncConfigured;
      document.querySelectorAll("[data-storage-action]").forEach((button) => {
        button.disabled = storageJobBusy ||
          (button.dataset.storageAction === "archive" && exportBusy);
      });
    }

    function storageClearSelection() {
      storageSelectedNames.clear();
      storageSelectionUi();
    }

    function storageToggleSelected(name, checked) {
      if (!name) return;
      if (checked) {
        storageSelectedNames.add(name);
      } else {
        storageSelectedNames.delete(name);
      }
      storageSelectionUi();
    }

    function storageToggleSelectAll(checked) {
      storageSelectedNames.clear();
      if (checked) {
        storageEntries.forEach((entry) => {
          if (entry && entry.name) storageSelectedNames.add(entry.name);
        });
      }
      storageSelectionUi();
    }

    function storageOpenEntry(entry) {
      if (!entry) return;
      if (entry.type === "dir") {
        storagePath = entry.path || "/";
        storageOffset = 0;
        storageClearSelection();
        loadStorageList(false);
      } else if (entry.path) {
        storageDownload(entry.path);
      }
    }

    function storageDownloadEntry(entry) {
      if (!entry || !entry.path) return;
      if (entry.type === "dir") {
        storageArchivePath(entry.path);
      } else {
        storageDownload(entry.path);
      }
    }

    function renderStorageList(data) {
      storagePath = data && data.path ? data.path : storagePath;
      storageLimit = Number(data && data.limit) || storageLimit;
      storageNextOffset =
        data && data.next_offset !== null && data.next_offset !== undefined &&
        Number.isFinite(Number(data.next_offset)) ?
          Number(data.next_offset) : null;
      const entries = data && Array.isArray(data.entries) ? data.entries : [];
      storageEntries = entries;
      const visibleNames = new Set(entries.map((entry) => entry.name));
      storageSelectedNames.forEach((name) => {
        if (!visibleNames.has(name)) storageSelectedNames.delete(name);
      });
      up("storagePath", storagePath);
      const upBtn = document.getElementById("storageUpBtn");
      const prevBtn = document.getElementById("storagePrevBtn");
      const nextBtn = document.getElementById("storageNextBtn");
      if (upBtn) upBtn.disabled = storagePath === "/";
      if (prevBtn) prevBtn.disabled = storageOffset <= 0;
      if (nextBtn) nextBtn.disabled = storageNextOffset === null;
      storageSetBadge((data && data.truncated ? "Page " : "") +
                  entries.length + " item" + (entries.length === 1 ? "" : "s"),
                  data && data.truncated ? "warn" : "good");

      const message = document.getElementById("storageMsg");
      if (message) message.className = "msg";
      const root = document.getElementById("storageList");
      if (!root) return;
      root.innerHTML = "";
      if (!entries.length) {
        const empty = document.createElement("div");
        empty.className = "storage-empty";
        empty.textContent = "No files";
        root.appendChild(empty);
        storageSelectionUi();
        return;
      }
      entries.forEach((entry) => {
        const row = document.createElement("div");
        row.className = "storage-entry";
        const select = document.createElement("label");
        select.className = "storage-select";
        const checkbox = document.createElement("input");
        checkbox.type = "checkbox";
        checkbox.dataset.storageSelect = entry.name || "";
        checkbox.checked = storageSelectedNames.has(entry.name || "");
        checkbox.onchange = () => storageToggleSelected(entry.name, checkbox.checked);
        select.appendChild(checkbox);
        const info = document.createElement("div");
        const name = document.createElement("div");
        name.className = "storage-name";
        name.textContent = entry.name || entry.path || "--";
        name.tabIndex = 0;
        name.onclick = () => storageOpenEntry(entry);
        name.onkeydown = (event) => {
          if (event.key === "Enter" || event.key === " ") {
            event.preventDefault();
            storageOpenEntry(entry);
          }
        };
        const meta = document.createElement("div");
        meta.className = "storage-meta";
        const metaParts = [entry.type === "dir" ?
          (entry.path || "") : fmtBytes(entry.size)];
        const modified = fmtStorageModified(entry.modified);
        if (modified) metaParts.push(modified);
        meta.textContent = metaParts.filter(Boolean).join(" | ");
        info.appendChild(name);
        info.appendChild(meta);

        const action = document.createElement("button");
        action.className = "btn primary storage-action";
        action.textContent = "\u2193";
        action.title = entry.type === "dir" ?
          "Download directory as ZIP" : "Download file";
        action.setAttribute("aria-label", action.title);
        action.dataset.storageAction = entry.type === "dir" ? "archive" : "download";
        action.onclick = () => storageDownloadEntry(entry);
        row.appendChild(select);
        row.appendChild(info);
        row.appendChild(action);
        root.appendChild(row);
      });
      storageSelectionUi();
    }

    async function loadStorageList(reset) {
      const requestSeq = ++storageListRequestSeq;
      if (reset) {
        storageOffset = 0;
        storageClearSelection();
      }
      storageSetBadge("Loading", "warn");
      try {
        const data = await fetchStorageList(storagePath,
          storageOffset, storageLimit, reset);
        if (requestSeq !== storageListRequestSeq) return;
        renderStorageList(data);
      } catch (error) {
        if (requestSeq !== storageListRequestSeq) return;
        storageSetBadge("Unavailable", "bad");
        msg("storageMsg", error.message, false, true);
      }
    }

    function storageUp() {
      if (storagePath === "/") return;
      storagePath = storageParentPath(storagePath);
      storageOffset = 0;
      storageClearSelection();
      loadStorageList(false);
    }

    function storagePrevPage() {
      storageOffset = Math.max(0, storageOffset - storageLimit);
      storageClearSelection();
      loadStorageList(false);
    }

    function storageNextPage() {
      if (storageNextOffset === null) return;
      storageOffset = storageNextOffset;
      storageClearSelection();
      loadStorageList(false);
    }

    async function storageDownload(path) {
      msg("storageMsg", "Preparing download", true, false);
      try {
        for (let attempt = 0; attempt < 400; attempt++) {
          const response = await fetch("/api/storage/download?path=" +
            encodeURIComponent(path), {cache: "no-store"});
          const text = await response.text();
          if (response.status === 202) {
            await storageDelay(100);
            continue;
          }
          if (!response.ok) {
            throw new Error(storageErrorText(text, response.status));
          }

          const data = JSON.parse(text);
          if (data.state !== "ready" || !Number(data.id)) {
            throw new Error("download_not_ready");
          }

          const link = document.createElement("a");
          link.href = "/api/storage/download?id=" + encodeURIComponent(data.id);
          link.download = data.filename || path.split("/").pop() || "download";
          document.body.appendChild(link);
          link.click();
          link.remove();
          msg("storageMsg", "Download started", true, false);
          return;
        }
        throw new Error("download_prepare_timeout");
      } catch (error) {
        msg("storageMsg", error.message, false, true);
      }
    }

    function storageArchiveSetBusy(busy) {
      storageArchiveBusy = !!busy;
      storageSelectionUi();
    }

    function storageDeleteSetBusy(busy) {
      storageDeleteBusy = !!busy;
      storageSelectionUi();
    }

    function smbSyncSetBusy(busy) {
      smbSyncBusy = !!busy;
      storageSelectionUi();
    }

    function sleepHqSyncSetBusy(busy) {
      sleepHqSyncBusy = !!busy;
      storageSelectionUi();
    }

    function runtimeAgeText(ms) {
      const current = statusData && Number(statusData.uptime) > 0 ?
        Number(statusData.uptime) * 1000 : 0;
      const value = Number(ms) || 0;
      if (!current || !value || current < value) return "";
      const age = Math.max(0, Math.round((current - value) / 1000));
      return age < 5 ? "just now" : fmtDuration(age) + " ago";
    }

    function retryInText(ms) {
      const current = statusData && Number(statusData.uptime) > 0 ?
        Number(statusData.uptime) * 1000 : 0;
      const value = Number(ms) || 0;
      if (!current || !value || value <= current) return "";
      return "Retry in " + fmtDuration(Math.ceil((value - current) / 1000));
    }

    function smbFriendlyError(error) {
      const text = String(error || "");
      if (!text) return "Sync failed";
      if (/timed out/i.test(text)) return "Connection timed out";
      if (/auth|access|denied|logon/i.test(text)) return "Authentication failed";
      if (/resolve|address|host/i.test(text)) return "Host not found";
      if (/No such file|not found|PATH_NOT_FOUND/i.test(text)) return "Remote path not found";
      return text.replace(/^[a-z_]+:/i, "");
    }

    function smbSyncCheckingShare(data) {
      return !!data && data.pending_reason === "verify_recent";
    }

    function smbSyncResultText(data) {
      if (!data || data.ok === false) return "Unavailable";
      if (!data.enabled || !data.configured) return "Configure an SMB endpoint first";
      const state = data.state || "unknown";
      const seen = Number(data.files_seen) || 0;
      const uploaded = Number(data.files_uploaded) || 0;
      const skipped = Number(data.files_skipped) || 0;
      const failed = Number(data.files_failed) || 0;
      const verify = !!data.last_run_verify ||
        data.pending_reason === "startup_check";
      if (state === "working") {
        if (smbSyncCheckingShare(data)) {
          return seen ? "Checking share: " + seen + " files checked" :
            "Checking share";
        }
        if (verify) return "Checking latest data";
        return seen ? "Checking files: " + seen + " seen" : "Starting sync";
      }
      if (state === "pending") {
        if (smbSyncCheckingShare(data)) {
          return data.network_available ? "Waiting to check share" :
            "Waiting for network";
        }
        if (data.pending_reason === "startup_check") {
          return data.network_available ? "Checking latest data" :
            "Waiting for network";
        }
        return "Waiting to sync";
      }
      if (state === "error") {
        const retry = retryInText(data.retry_due_ms);
        const age = runtimeAgeText(data.updated_ms);
        return "Last sync failed" + (age ? " " + age : "") + ": " +
          smbFriendlyError(data.error || data.last_error) +
          (retry ? ". " + retry : "");
      }
      if (!data.started_ms) return "Not checked yet";
      const age = runtimeAgeText(data.updated_ms);
      const when = age ? " " + age : "";
      if (data.last_run_verify) {
        if (seen) {
          return (data.last_run_reconcile ? "Share checked" :
            "Latest data checked") + when + " (" + seen +
            " file" + (seen === 1 ? "" : "s") + ")";
        }
        return (data.last_run_reconcile ? "Share reachable" :
          "Endpoint reachable") + when;
      }
      if (failed) {
        return "Last sync finished" + when + " with " + failed +
          " failed file" +
          (failed === 1 ? "" : "s");
      }
      if (uploaded) {
        return "Synced" + when + ": uploaded " + uploaded + " file" +
          (uploaded === 1 ? "" : "s") + " (" +
          fmtBytes(Number(data.bytes_uploaded) || 0) + ")";
      }
      if (seen && skipped === seen) {
        return "Up to date" + when + " (" + seen + " files checked)";
      }
      return seen ? "Checked" + when + ": " + seen + " files" :
        "Ready";
    }

    function smbSyncNowText(data) {
      if (!data || data.ok === false) return "--";
      if (!data.enabled || !data.configured) return "--";
      const state = data.state || "unknown";
      if (state === "working") {
        if (smbSyncCheckingShare(data)) {
          return data.current_path ? "Checking " + data.current_path :
            "Checking share";
        }
        if (data.last_run_verify) return "Checking latest data";
        return data.current_path ? "Syncing " + data.current_path : "Syncing";
      }
      if (state === "pending") {
        if (smbSyncCheckingShare(data)) {
          return data.network_available ? "Queued" : "Waiting for network";
        }
        if (data.pending_reason === "startup_check") {
          return data.network_available ? "Queued" : "Waiting for network";
        }
        return "Queued";
      }
      if (state === "error") return retryInText(data.retry_due_ms) || "--";
      return "--";
    }

    function smbSyncLastText(data) {
      if (!data || data.ok === false || !data.enabled || !data.configured) {
        return "--";
      }
      const seen = Number(data.last_sync_files_seen) || 0;
      const uploaded = Number(data.last_sync_files_uploaded) || 0;
      const failed = Number(data.last_sync_files_failed) || 0;
      const bytes = Number(data.last_sync_bytes_uploaded) || 0;
      const when = fmtStorageModified(data.last_sync_epoch);
      if (!when && !seen) return "--";
      let text = when || "Completed";
      if (failed) {
        text += ", " + failed + " failed";
      } else if (uploaded) {
        text += ", uploaded " + uploaded + " file" +
          (uploaded === 1 ? "" : "s") + " (" + fmtBytes(bytes) + ")";
      } else if (seen) {
        text += ", up to date";
      }
      return text;
    }

    function smbSyncCheckText(data) {
      if (!data || data.ok === false || !data.enabled || !data.configured) {
        return "--";
      }
      const reconcileSeen = Number(data.last_reconcile_files_seen) || 0;
      const reconcileWhen = fmtStorageModified(data.last_reconcile_epoch);
      if (reconcileWhen || reconcileSeen) {
        let text = reconcileWhen || "Share checked";
        if (reconcileSeen) {
          text += ", " + reconcileSeen + " file" +
            (reconcileSeen === 1 ? "" : "s");
        }
        return text;
      }
      const seen = Number(data.last_verify_files_seen) || 0;
      const when = fmtStorageModified(data.last_verify_epoch);
      if (!when && !seen) return "--";
      let text = when || "Latest data checked";
      if (seen) {
        text += ", latest " + seen + " file" +
          (seen === 1 ? "" : "s");
      }
      return text;
    }

    function smbSyncStatusActive(data) {
      if (!data || data.ok === false) return false;
      return data.state === "working" || data.state === "pending" || !!data.pending;
    }

    function smbSyncBadgeText(data) {
      if (!data || data.ok === false) return "--";
      if (!data.enabled || !data.configured) return "Setup";
      const state = data.state || "unknown";
      if (state === "working") return "Syncing";
      if (state === "pending") return "Queued";
      if (state === "error") return "Error";
      if (state === "disabled") return "Off";
      return "Ready";
    }

    function renderSmbSyncStatus(data) {
      const state = data && data.state ? data.state : "unknown";
      up("edfSyncEndpoint", data && data.endpoint ? data.endpoint :
        (configData && configData.smb_ep ? configData.smb_ep : "--"));
      up("edfSyncResult", smbSyncResultText(data));
      up("edfSyncLast", smbSyncLastText(data));
      up("edfSyncCheck", smbSyncCheckText(data));
      up("edfSyncCurrent", smbSyncNowText(data));
      const badge = document.getElementById("edfSyncBadge");
      if (badge) {
        badge.textContent = smbSyncBadgeText(data);
        badge.className = "badge" +
          (state === "error" ? " bad" :
           (!data || !data.enabled || !data.configured) ? " warn" :
           (state === "working" || state === "pending") ? " warn" : " good");
      }
    }

    async function loadSmbSyncStatus() {
      try {
        const response = await fetch("/api/storage/sync/status", {cache: "no-store"});
        const text = await response.text();
        if (!response.ok) throw new Error(storageErrorText(text, response.status));
        const data = JSON.parse(text);
        smbSyncEnabled = !!data.enabled;
        smbSyncConfigured = !!data.configured;
        renderSmbSyncStatus(data);
        smbSyncSetBusy(smbSyncStatusActive(data));
        return data;
      } catch (error) {
        smbSyncEnabled = false;
        smbSyncConfigured = false;
        up("edfSyncResult", error.message);
        up("edfSyncLast", "--");
        up("edfSyncCheck", "--");
        up("edfSyncCurrent", "--");
        smbSyncSetBusy(false);
        return null;
      }
    }

    async function smbPollSync() {
      const data = await loadSmbSyncStatus();
      const active = smbSyncStatusActive(data);
      if (!active && smbSyncPollTimer) {
        clearInterval(smbSyncPollTimer);
        smbSyncPollTimer = null;
      }
      if (data && data.state === "error") {
        msg("edfSmbMsg", data.error || "SMB sync failed", false, true);
      } else if (data && !active && data.state === "idle") {
        msg("edfSmbMsg", smbSyncCompleteMessage, true, false);
      }
      return !active;
    }

    async function smbQueueAction(url, startMessage, completeMessage) {
      if (smbSyncPollTimer) clearInterval(smbSyncPollTimer);
      smbSyncPollTimer = null;
      smbSyncCompleteMessage = completeMessage || "SMB sync complete";
      smbSyncSetBusy(true);
      msg("edfSmbMsg", startMessage, true, false);
      try {
        const response = await fetch(url, {
          method: "POST",
          cache: "no-store",
        });
        const text = await response.text();
        if (!response.ok) throw new Error(storageErrorText(text, response.status));
        const done = await smbPollSync();
        if (!done) smbSyncPollTimer = setInterval(smbPollSync, 1000);
      } catch (error) {
        smbSyncSetBusy(false);
        await loadSmbSyncStatus();
        msg("edfSmbMsg", error.message, false, true);
      }
    }

    async function smbStartSync() {
      await smbQueueAction("/api/storage/sync/start",
        "Starting SMB sync", "SMB sync complete");
    }

    async function smbVerifyRecent() {
      await smbQueueAction("/api/storage/sync/verify",
        "Checking SMB share", "SMB check complete");
    }

    function sleepHqEndpointText(data) {
      if (!data || data.ok === false) return "--";
      if (!data.configured) return "Configure SleepHQ account first";
      const team = data.team_id || data.configured_team_id;
      return team ? "SleepHQ team " + team : "SleepHQ account";
    }

    function sleepHqFriendlyError(error) {
      const text = String(error || "");
      if (!text) return "Sync failed";
      if (/tls_heap|heap/i.test(text)) return "Not enough free heap for TLS";
      if (/auth|unauthor|token/i.test(text)) return "Authentication failed";
      if (/connect|timed out|timeout/i.test(text)) return "Connection failed";
      if (/team/i.test(text)) return "Team not found";
      return text.replace(/^[a-z_]+:/i, "");
    }

    function sleepHqSyncResultText(data) {
      if (!data || data.ok === false) return "Unavailable";
      if (!data.configured) return "Configure SleepHQ account first";
      const state = data.state || "unknown";
      const seen = Number(data.files_seen) || 0;
      const uploaded = Number(data.files_uploaded) || 0;
      const failed = Number(data.files_failed) || 0;
      if (state === "working") {
        if (uploaded) {
          return "Uploading: " + uploaded + " file" +
            (uploaded === 1 ? "" : "s");
        }
        return seen ? "Checking files: " + seen + " seen" :
          "Connecting to SleepHQ";
      }
      if (state === "pending") {
        return data.network_available ? "Waiting to sync" :
          "Waiting for network";
      }
      if (state === "error") {
        const age = runtimeAgeText(data.updated_ms);
        const retry = retryInText(data.retry_due_ms);
        return "Last sync failed" + (age ? " " + age : "") + ": " +
          sleepHqFriendlyError(data.error) +
          (retry ? ". " + retry : "");
      }
      if (!data.started_ms && !data.last_check_epoch) return "Not checked yet";
      if (failed) {
        return "Last sync finished with " + failed + " failed file" +
          (failed === 1 ? "" : "s");
      }
      if (uploaded) {
        return "Synced: uploaded " + uploaded + " file" +
          (uploaded === 1 ? "" : "s") + " (" +
          fmtBytes(Number(data.bytes_uploaded) || 0) + ")";
      }
      if (seen && Number(data.files_skipped) === seen) {
        return "Up to date (" + seen + " files checked)";
      }
      return "Ready";
    }

    function sleepHqSyncLastText(data) {
      if (!data || data.ok === false || !data.configured) return "--";
      const when = fmtStorageModified(data.last_sync_epoch);
      const seen = Number(data.last_sync_files_seen) || 0;
      const uploaded = Number(data.last_sync_files_uploaded) || 0;
      const failed = Number(data.last_sync_files_failed) || 0;
      const bytes = Number(data.last_sync_bytes_uploaded) || 0;
      if (!when && !seen) return "--";
      let text = when || "Completed";
      if (failed) {
        text += ", " + failed + " failed";
      } else if (uploaded) {
        text += ", uploaded " + uploaded + " file" +
          (uploaded === 1 ? "" : "s") + " (" + fmtBytes(bytes) + ")";
      } else if (seen) {
        text += ", up to date";
      }
      return text;
    }

    function sleepHqSyncCheckText(data) {
      if (!data || data.ok === false || !data.configured) return "--";
      const when = fmtStorageModified(data.last_check_epoch);
      return when || "--";
    }

    function sleepHqSyncNowText(data) {
      if (!data || data.ok === false || !data.configured) return "--";
      const state = data.state || "unknown";
      if (state === "working") {
        if (data.import_status) return "Processing import: " +
          data.import_status;
        return data.current_path ? "Uploading " + data.current_path :
          "Working";
      }
      if (state === "pending") {
        return data.network_available ? "Queued" : "Waiting for network";
      }
      if (state === "error") return retryInText(data.retry_due_ms) || "--";
      return "--";
    }

    function sleepHqSyncStatusActive(data) {
      if (!data || data.ok === false) return false;
      return data.state === "working" || data.state === "pending" ||
        !!data.pending;
    }

    function sleepHqSyncBadgeText(data) {
      if (!data || data.ok === false) return "--";
      if (!data.configured) return "Setup";
      const state = data.state || "unknown";
      if (state === "working") return "Syncing";
      if (state === "pending") return "Queued";
      if (state === "error") return "Error";
      if (state === "disabled") return "Off";
      return "Ready";
    }

    function renderSleepHqSyncStatus(data) {
      const state = data && data.state ? data.state : "unknown";
      up("edfSleepHqEndpoint", sleepHqEndpointText(data));
      up("edfSleepHqResult", sleepHqSyncResultText(data));
      up("edfSleepHqLast", sleepHqSyncLastText(data));
      up("edfSleepHqCheck", sleepHqSyncCheckText(data));
      up("edfSleepHqCurrent", sleepHqSyncNowText(data));
      const badge = document.getElementById("edfSleepHqBadge");
      if (badge) {
        badge.textContent = sleepHqSyncBadgeText(data);
        badge.className = "badge" +
          (state === "error" ? " bad" :
           (!data || !data.configured) ? " warn" :
           (state === "working" || state === "pending") ? " warn" : " good");
      }
    }

    async function loadSleepHqSyncStatus() {
      try {
        const response = await fetch("/api/sleephq/sync/status", {cache: "no-store"});
        const text = await response.text();
        if (!response.ok) throw new Error(storageErrorText(text, response.status));
        const data = JSON.parse(text);
        sleepHqSyncConfigured = !!data.configured;
        renderSleepHqSyncStatus(data);
        sleepHqSyncSetBusy(sleepHqSyncStatusActive(data));
        return data;
      } catch (error) {
        sleepHqSyncConfigured = false;
        up("edfSleepHqResult", error.message);
        up("edfSleepHqLast", "--");
        up("edfSleepHqCheck", "--");
        up("edfSleepHqCurrent", "--");
        sleepHqSyncSetBusy(false);
        return null;
      }
    }

    async function sleepHqPollSync() {
      const data = await loadSleepHqSyncStatus();
      const active = sleepHqSyncStatusActive(data);
      if (!active && sleepHqSyncPollTimer) {
        clearInterval(sleepHqSyncPollTimer);
        sleepHqSyncPollTimer = null;
      }
      if (data && data.state === "error") {
        msg("edfSleepHqMsg", data.error || "SleepHQ sync failed", false, true);
      } else if (data && !active && data.state === "idle") {
        msg("edfSleepHqMsg", sleepHqSyncCompleteMessage, true, false);
      }
      return !active;
    }

    async function sleepHqQueueAction(url, startMessage, completeMessage) {
      if (sleepHqSyncPollTimer) clearInterval(sleepHqSyncPollTimer);
      sleepHqSyncPollTimer = null;
      sleepHqSyncCompleteMessage = completeMessage ||
        "SleepHQ sync complete";
      sleepHqSyncSetBusy(true);
      msg("edfSleepHqMsg", startMessage, true, false);
      try {
        const response = await fetch(url, {
          method: "POST",
          cache: "no-store",
        });
        const text = await response.text();
        if (!response.ok) throw new Error(storageErrorText(text, response.status));
        const done = await sleepHqPollSync();
        if (!done) {
          sleepHqSyncPollTimer = setInterval(sleepHqPollSync, 1000);
        }
      } catch (error) {
        sleepHqSyncSetBusy(false);
        await loadSleepHqSyncStatus();
        msg("edfSleepHqMsg", error.message, false, true);
      }
    }

    async function sleepHqStartSync() {
      await sleepHqQueueAction("/api/sleephq/sync/start",
        "Starting SleepHQ sync", "SleepHQ sync complete");
    }

    async function sleepHqCheckAccount() {
      await sleepHqQueueAction("/api/sleephq/sync/check",
        "Checking SleepHQ account", "SleepHQ account check complete");
    }

    function storageArchiveDownload(id) {
      const link = document.createElement("a");
      link.href = "/api/storage/archive/download?id=" + encodeURIComponent(id);
      link.download = "archive.zip";
      document.body.appendChild(link);
      link.click();
      link.remove();
    }

    function storageArchiveStatusText(data) {
      const state = data && data.state ? data.state : "unknown";
      const filesDone = Number(data && data.files_done) || 0;
      const files = Number(data && data.files) || 0;
      const bytes = Number(data && data.bytes_done) || 0;
      const estimate = Number(data && data.estimated_archive_bytes) || 0;
      if (state === "preparing") {
        return files > 0 ? "Preparing " + files + " files" : "Preparing file list";
      }
      if (state === "building") {
        const pct = estimate > 0 ? " " + Math.min(99, Math.floor(bytes * 100 / estimate)) + "%" : "";
        return "Building " + filesDone + "/" + files + pct;
      }
      if (state === "ready") return "Archive ready";
      if (state === "downloading") return "Downloading";
      if (state === "error") return data && data.error ? data.error : "Archive failed";
      return state;
    }

    async function storagePollArchive(id) {
      try {
        const response = await fetch("/api/storage/archive/status", {cache: "no-store"});
        const text = await response.text();
        if (!response.ok) throw new Error(storageErrorText(text, response.status));
        const data = JSON.parse(text);
        if (Number(data.id) !== Number(id)) return false;
        const state = data.state || "unknown";
        msg("storageMsg", storageArchiveStatusText(data), state !== "error", state === "error");
        if (state === "ready") {
          clearInterval(storageArchivePollTimer);
          storageArchivePollTimer = null;
          storageArchiveSetBusy(false);
          storageArchiveDownload(id);
          return true;
        } else if (state === "error" || state === "idle") {
          clearInterval(storageArchivePollTimer);
          storageArchivePollTimer = null;
          storageArchiveSetBusy(false);
          return true;
        }
        return false;
      } catch (error) {
        clearInterval(storageArchivePollTimer);
        storageArchivePollTimer = null;
        storageArchiveSetBusy(false);
        msg("storageMsg", error.message, false, true);
        return true;
      }
    }

    async function storageStartArchive(url, options) {
      if (storageArchivePollTimer) clearInterval(storageArchivePollTimer);
      storageArchivePollTimer = null;
      storageArchiveSetBusy(true);
      msg("storageMsg", "Starting archive", true, false);
      try {
        const response = await fetch(url, Object.assign({
          method: "POST",
          cache: "no-store",
        }, options || {}));
        const text = await response.text();
        if (!response.ok) throw new Error(storageErrorText(text, response.status));
        const data = JSON.parse(text);
        const id = Number(data.id);
        if (!id) throw new Error("bad_archive_id");
        const done = await storagePollArchive(id);
        if (!done) {
          storageArchivePollTimer = setInterval(() => storagePollArchive(id), 1000);
        }
      } catch (error) {
        storageArchiveSetBusy(false);
        msg("storageMsg", error.message, false, true);
      }
    }

    async function storageArchivePath(path) {
      if (!path) return;
      await storageStartArchive(
        "/api/storage/archive/start?path=" + encodeURIComponent(path));
    }

    async function storageArchiveSelected() {
      const selected = storageEntries
        .filter((entry) => entry && storageSelectedNames.has(entry.name))
        .map((entry) => entry.name);
      if (!selected.length) {
        msg("storageMsg", "Select files or folders first", false, true);
        return;
      }
      await storageStartArchive("/api/storage/archive/start", {
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({base: storagePath, items: selected}),
      });
    }

    function storageDeleteStatusText(data) {
      const state = data && data.state ? data.state : "unknown";
      const rootsDone = Number(data && data.roots_done) || 0;
      const roots = Number(data && data.roots) || 0;
      const files = Number(data && data.files_deleted) || 0;
      const dirs = Number(data && data.dirs_deleted) || 0;
      if (state === "deleting") {
        return "Deleting " + rootsDone + "/" + roots +
          " selected, removed " + (files + dirs);
      }
      if (state === "done") return "Delete complete";
      if (state === "error") return data && data.error ? data.error : "Delete failed";
      return state;
    }

    async function storagePollDelete(id) {
      try {
        const response = await fetch("/api/storage/delete/status", {cache: "no-store"});
        const text = await response.text();
        if (!response.ok) throw new Error(storageErrorText(text, response.status));
        const data = JSON.parse(text);
        if (Number(data.id) !== Number(id)) return false;
        const state = data.state || "unknown";
        msg("storageMsg", storageDeleteStatusText(data), state !== "error", state === "error");
        if (state === "done") {
          clearInterval(storageDeletePollTimer);
          storageDeletePollTimer = null;
          storageDeleteSetBusy(false);
          storageClearSelection();
          loadStorageList(true);
          return true;
        } else if (state === "error" || state === "idle") {
          clearInterval(storageDeletePollTimer);
          storageDeletePollTimer = null;
          storageDeleteSetBusy(false);
          return true;
        }
        return false;
      } catch (error) {
        clearInterval(storageDeletePollTimer);
        storageDeletePollTimer = null;
        storageDeleteSetBusy(false);
        msg("storageMsg", error.message, false, true);
        return true;
      }
    }

    async function storageDeleteSelected() {
      const selected = storageEntries
        .filter((entry) => entry && storageSelectedNames.has(entry.name))
        .map((entry) => entry.name);
      if (!selected.length) {
        msg("storageMsg", "Select files or folders first", false, true);
        return;
      }
      if (!confirm("Delete selected files and folders recursively?")) return;
      if (storageDeletePollTimer) clearInterval(storageDeletePollTimer);
      storageDeletePollTimer = null;
      storageDeleteSetBusy(true);
      msg("storageMsg", "Starting delete", true, false);
      try {
        const response = await fetch("/api/storage/delete/start", {
          method: "POST",
          cache: "no-store",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({base: storagePath, items: selected}),
        });
        const text = await response.text();
        if (!response.ok) throw new Error(storageErrorText(text, response.status));
        const data = JSON.parse(text);
        const id = Number(data.id);
        if (!id) throw new Error("bad_delete_id");
        const done = await storagePollDelete(id);
        if (!done) {
          storageDeletePollTimer = setInterval(() => storagePollDelete(id), 1000);
        }
      } catch (error) {
        storageDeleteSetBusy(false);
        msg("storageMsg", error.message, false, true);
      }
    }
