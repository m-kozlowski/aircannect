(() => {

    const i18n = AirCANnect.i18n;
    let storagePath = "/";
    let storageOffset = 0;
    let storageLimit = 64;
    let storageNextOffset = null;
    let storageListRequestSeq = 0;
    let storageOperationData = null;
    let storageUsbState = "local";
    let storageUsbRequestPending = false;
    let storageArchiveJobId = 0;
    let storageArchiveDownloadStartedId = 0;
    let storageDeleteJobId = 0;
    let storageEntries = [];
    let storageSelectedNames = new Set();
    let storageArchiveBusy = false;
    let storageDeleteBusy = false;
    let storageRenameBusy = false;
    let storageUploadBusy = false;
    let storageUploadOperation = null;
    let smbSyncBusy = false;
    let smbSyncEnabled = false;
    let smbSyncConfigured = false;
    let smbSyncCompleteMessage = i18n.message("storage.smb_complete");
    let sleepHqSyncBusy = false;
    let sleepHqSyncConfigured = false;
    let sleepHqSyncCompleteMessage = i18n.message("storage.shq_complete");

    function storageErrorText(text, status) {
      try {
        const data = JSON.parse(text);
        if (data && data.error) return data.error;
      } catch (_) {}
      return text || ("HTTP " + status);
    }

    async function fetchStorageList(path, offset, limit, refresh) {
      let forceRefresh = !!refresh;
      const deadline = Date.now() + 30000;
      while (Date.now() < deadline) {
        const url = "/api/storage/list?path=" + encodeURIComponent(path) +
          "&offset=" + encodeURIComponent(offset || 0) +
          "&limit=" + encodeURIComponent(limit || 128) +
          (forceRefresh ? "&refresh=1" : "");
        forceRefresh = false;

        const response = await AirCANnect.http.request(url, {cache: "no-store"});
        const text = await response.text();
        if (response.status === 202) {
          let retryMs = 750;
          try {
            const pending = JSON.parse(text);
            const requestedRetry = Number(pending && pending.retry_ms);
            if (Number.isFinite(requestedRetry)) {
              retryMs = Math.min(5000, Math.max(250, requestedRetry));
            }
          } catch (_) {}
          const remainingMs = Math.max(0, deadline - Date.now());
          await AirCANnect.time.delay(Math.min(retryMs, remainingMs));
          continue;
        }
        if (!response.ok) {
          throw new Error(storageErrorText(text, response.status));
        }
        return JSON.parse(text);
      }
      throw new Error("list_prepare_timeout");
    }

    function storageParentPath(path) {
      if (!path || path === "/") return "/";
      const index = path.lastIndexOf("/");
      return index <= 0 ? "/" : path.slice(0, index);
    }

    function storageSetBadge(text, cls) {
      const badge = document.getElementById("storageBadge");
      if (!badge) return;
      i18n.text(badge, text);
      badge.className = "badge" + (cls ? " " + cls : "");
    }

    function edfSessionLabel(prefix) {
      const match = String(prefix || "").match(/^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})$/);
      if (!match) return prefix || "--";
      return match[1] + "-" + match[2] + "-" + match[3] + " " +
        match[4] + ":" + match[5] + ":" + match[6];
    }

    function renderEdfOverview(status) {
      const edf = status && status.edf;
      const error = String(edf && edf.error || "").trim();
      if (!edf || !edf.ready) {
        AirCANnect.ui.text("edfLastSession", error ? i18n.message("status.unavailable") :
          (!edf || edf.refreshing ? i18n.message("ui.loading") : i18n.message("status.unavailable")));
        AirCANnect.ui.text("edfLastFiles", error || "--");
        return;
      }

      const session = String(edf.session || "");
      AirCANnect.ui.text("edfLastSession", session ?
        edfSessionLabel(session) : i18n.message("storage.no_sessions"));
      if (!session) {
        AirCANnect.ui.text("edfLastFiles", error || "--");
        return;
      }

      const fileCount = Number(edf.file_count) || 0;
      const recorded = i18n.join([
        i18n.message("storage.file_count", {count: fileCount}), ", ",
        AirCANnect.format.bytes(Number(edf.bytes) || 0),
        error ? "; " + error : "",
      ]);
      AirCANnect.ui.text("edfLastFiles", recorded);
    }

    function storageSelectionUi() {
      const selected = storageSelectedNames.size;
      const storageJobBusy = storageArchiveBusy || storageDeleteBusy ||
        storageRenameBusy || storageUploadBusy || storageUsbState !== "local";
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
        i18n.text(archiveBtn, selected > 0 ?
          i18n.message("storage.download_selected", {count: selected}) :
          i18n.message("ui.download.selected"));
      }
      const deleteBtn = document.getElementById("storageDeleteBtn");
      if (deleteBtn) {
        deleteBtn.disabled = destructiveBusy || selected === 0;
        i18n.text(deleteBtn, selected > 0 ?
          i18n.message("storage.delete_selected", {count: selected}) :
          i18n.message("ui.delete.selected"));
      }
      const uploadBtn = document.getElementById("storageUploadBtn");
      if (uploadBtn) uploadBtn.disabled = storageJobBusy || exportBusy;
      for (const [id, unavailable] of [
        ["storageUpBtn", storagePath === "/"],
        ["storagePrevBtn", storageOffset <= 0],
        ["storageNextBtn", storageNextOffset === null],
        ["storageRefreshBtn", false],
      ]) {
        const button = document.getElementById(id);
        if (button) button.disabled = storageUsbState !== "local" || unavailable;
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
          ((button.dataset.storageAction === "archive" ||
            button.dataset.storageAction === "rename") && exportBusy);
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
      AirCANnect.ui.text("storagePath", storagePath);
      const upBtn = document.getElementById("storageUpBtn");
      const prevBtn = document.getElementById("storagePrevBtn");
      const nextBtn = document.getElementById("storageNextBtn");
      if (upBtn) upBtn.disabled = storagePath === "/";
      if (prevBtn) prevBtn.disabled = storageOffset <= 0;
      if (nextBtn) nextBtn.disabled = storageNextOffset === null;
      storageSetBadge(i18n.message(data && data.truncated ?
        "storage.page_items" : "storage.items", {count: entries.length}),
        data && data.truncated ? "warn" : "good");

      const message = document.getElementById("storageMsg");
      if (message) message.className = "msg";
      const root = document.getElementById("storageList");
      if (!root) return;
      root.innerHTML = "";
      if (!entries.length) {
        const empty = document.createElement("div");
        empty.className = "storage-empty";
        i18n.text(empty, i18n.message("storage.no_files"));
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
        i18n.text(name, entry.name || entry.path || "--");
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
          (entry.path || "") : AirCANnect.format.bytes(entry.size)];
        const modified = AirCANnect.format.modified(entry.modified);
        if (modified) metaParts.push(modified);
        i18n.text(meta, metaParts.filter(Boolean).join(" | "));
        info.appendChild(name);
        info.appendChild(meta);

        const actions = document.createElement("div");
        actions.className = "storage-entry-actions";
        const rename = document.createElement("button");
        rename.className = "btn";
        i18n.text(rename, i18n.message("storage.rename"));
        rename.dataset.storageAction = "rename";
        rename.onclick = () => storageRenameEntry(entry);
        const download = document.createElement("button");
        download.className = "btn primary storage-action";
        i18n.text(download, "\u2193");
        i18n.text(download, entry.type === "dir" ?
          i18n.message("storage.download_directory") : i18n.message("storage.download_file"), "title");
        download.setAttribute("aria-label", download.title);
        download.dataset.storageAction = entry.type === "dir" ? "archive" : "download";
        download.onclick = () => storageDownloadEntry(entry);
        actions.appendChild(rename);
        actions.appendChild(download);
        row.appendChild(select);
        row.appendChild(info);
        row.appendChild(actions);
        root.appendChild(row);
      });
      storageSelectionUi();
    }

    async function loadStorageList(reset) {
      if (storageUsbState !== "local") return;
      const requestSeq = ++storageListRequestSeq;
      if (reset) {
        storageOffset = 0;
        storageClearSelection();
      }
      storageSetBadge(i18n.message("ui.loading"), "warn");
      try {
        const data = await fetchStorageList(storagePath,
          storageOffset, storageLimit, reset);
        if (requestSeq !== storageListRequestSeq) return;
        renderStorageList(data);
      } catch (error) {
        if (requestSeq !== storageListRequestSeq) return;
        storageSetBadge(i18n.message("status.unavailable"), "bad");
        AirCANnect.ui.message("storageMsg", i18n.error(error), false, true);
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

    async function storageDownload(path, messageId) {
      const target = messageId || "storageMsg";
      AirCANnect.ui.message(target, i18n.message("storage.prepare_download"), true, false);
      try {
        await AirCANnect.files.download(path);
        AirCANnect.ui.message(target, i18n.message("storage.download_started"), true, false);
      } catch (error) {
        AirCANnect.ui.message(target, i18n.error(error), false, true);
      }
    }

    async function storageRenamePath(base, currentName, title) {
      return AirCANnect.files.rename(base, currentName, title);
    }

    async function storageRenameEntry(entry) {
      if (!entry || !entry.name || storageRenameBusy) return;

      storageRenameBusy = true;
      storageSelectionUi();
      AirCANnect.ui.message("storageMsg", i18n.message(
        "storage.renaming", {name: entry.name}), true, false);
      try {
        const renamed = await storageRenamePath(storagePath, entry.name);
        if (!renamed) return;

        AirCANnect.ui.message("storageMsg", i18n.message(
          "storage.renamed", {name: entry.name, renamed}),
          true, false);
        storageClearSelection();
        await loadStorageList(true);
      } catch (error) {
        AirCANnect.ui.message("storageMsg", i18n.error(error), false, true);
      } finally {
        storageRenameBusy = false;
        storageSelectionUi();
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

    function storageUploadSetBusy(busy) {
      storageUploadBusy = !!busy;
      const progress = document.getElementById("storageUploadProgress");
      if (progress) progress.hidden = !storageUploadBusy;
      storageSelectionUi();
    }

    function storageUploadProgress(name, committed, total, fileIndex, fileCount) {
      AirCANnect.ui.uploadProgress("storageUpload", name, committed, total,
        fileIndex, fileCount);
    }

    function storageChooseUpload() {
      if (storageUploadBusy) return;
      const input = document.getElementById("storageUploadInput");
      if (input) input.click();
    }

    function storageFilesSelected(input) {
      const files = input && input.files ? Array.from(input.files) : [];
      if (input) input.value = "";
      if (!files.length || storageUploadBusy) return;

      const directory = storagePath;
      const queue = files.map((file) => ({file, directory}));
      storageUploadQueue(queue);
    }

    async function storageUploadQueue(queue) {
      const operation = AirCANnect.uploads.begin();
      if (!operation) {
        AirCANnect.ui.message(
          "storageMsg", i18n.message("storage.upload_busy"), false, true);
        return;
      }

      storageUploadOperation = operation;
      storageUploadSetBusy(true);
      let uploaded = 0;
      try {
        for (let index = 0; index < queue.length; index++) {
          if (operation.cancelled) break;
          const item = queue[index];
          storageUploadProgress(item.file.name, 0, item.file.size,
            index, queue.length);
          if (await operation.file(item.file, item.directory, {
              fileIndex: index,
              fileCount: queue.length,
              progress: storageUploadProgress,
            })) {
            uploaded++;
            await loadStorageList(true);
          }
        }
        if (operation.cancelled) {
          AirCANnect.ui.message("storageMsg", i18n.message("storage.upload_cancelled"), true, false);
        } else {
          AirCANnect.ui.message("storageMsg", i18n.message(
            "storage.uploaded", {count: uploaded}), true, false);
        }
      } catch (error) {
        if (error.message === "upload_cancelled") {
          AirCANnect.ui.message("storageMsg", i18n.message("storage.upload_cancelled"), true, false);
        } else {
          AirCANnect.ui.message("storageMsg", i18n.error(error), false, true);
        }
      } finally {
        operation.close();
        if (storageUploadOperation === operation) storageUploadOperation = null;
        storageUploadSetBusy(false);
      }
    }

    async function storageCancelUpload() {
      const operation = storageUploadOperation;
      if (!operation) return;

      const button = document.getElementById("storageUploadCancelBtn");
      if (button) button.disabled = true;
      try {
        await operation.cancel();
      } catch (_) {
      } finally {
        if (button) button.disabled = false;
      }
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
      const status = AirCANnect.snapshots.read("status").data;
      const current = status && Number(status.uptime) > 0 ?
        Number(status.uptime) * 1000 : 0;
      const value = Number(ms) || 0;
      if (!current || !value || current < value) return "";
      const age = Math.max(0, Math.round((current - value) / 1000));
      return age < 5 ? i18n.message("storage.just_now") :
        i18n.message("storage.ago", {duration: AirCANnect.format.duration(age)});
    }

    function retryInText(ms) {
      const status = AirCANnect.snapshots.read("status").data;
      const current = status && Number(status.uptime) > 0 ?
        Number(status.uptime) * 1000 : 0;
      const value = Number(ms) || 0;
      if (!current || !value || value <= current) return "";
      return i18n.message("storage.retry_in", {duration:
        AirCANnect.format.duration(Math.ceil((value - current) / 1000))});
    }

    function syncLastText(data, configured) {
      if (!data || data.ok === false || !configured) return "--";
      const seen = Number(data.last_sync_files_seen) || 0;
      const uploaded = Number(data.last_sync_files_uploaded) || 0;
      const failed = Number(data.last_sync_files_failed) || 0;
      const bytes = Number(data.last_sync_bytes_uploaded) || 0;
      const when = AirCANnect.format.modified(data.last_sync_epoch);
      if (!when && !seen) return "--";
      const parts = [when || i18n.message("storage.completed")];
      if (failed) {
        parts.push(i18n.message("storage.failed_files", {count: failed}));
      } else if (uploaded) {
        parts.push(i18n.message("storage.uploaded_size", {
          count: uploaded, size: AirCANnect.format.bytes(bytes),
        }));
      } else if (seen) {
        parts.push(i18n.message("storage.up_to_date"));
      }
      return i18n.join(parts, ", ");
    }

    function syncStatusActive(data) {
      if (!data || data.ok === false) return false;
      return data.state === "working" || data.state === "pending" ||
        !!data.pending;
    }

    function syncBadgeText(data, configured) {
      if (!data || data.ok === false) return "--";
      if (!configured) return i18n.message("storage.setup");
      const state = data.state || "unknown";
      if (state === "working") return i18n.message("storage.syncing");
      if (state === "pending") return i18n.message("status.queued");
      if (state === "error") return i18n.message("storage.error");
      if (state === "disabled") return i18n.message("ui.off");
      return i18n.message("status.ready");
    }

    function syncBadgeClass(data, configured) {
      const state = data && data.state ? data.state : "unknown";
      if (state === "error") return "badge bad";
      if (!configured || state === "working" || state === "pending") {
        return "badge warn";
      }
      return "badge good";
    }

    async function queueSyncAction(url, startMessage, messageId,
                                   inProgressMessage, setBusy, loadStatus) {
      setBusy(true);
      AirCANnect.ui.message(messageId, startMessage, true, false);
      try {
        const response = await AirCANnect.http.request(url, {
          method: "POST",
          cache: "no-store",
        });
        const text = await response.text();
        if (!response.ok) {
          throw new Error(storageErrorText(text, response.status));
        }
      } catch (error) {
        const current = await loadStatus();
        if (syncStatusActive(current)) {
          AirCANnect.ui.message(messageId, inProgressMessage, true, false);
          return;
        }
        setBusy(false);
        AirCANnect.ui.message(messageId, i18n.error(error), false, true);
      }
    }

    function smbFriendlyError(error) {
      const text = String(error || "");
      if (!text) return i18n.message("storage.sync_failed");
      if (/timed out/i.test(text)) return i18n.message("storage.connection_timeout");
      if (/auth|access|denied|logon/i.test(text)) return i18n.message("storage.authentication_failed");
      if (/resolve|address|host/i.test(text)) return i18n.message("storage.host_not_found");
      if (/No such file|not found|PATH_NOT_FOUND/i.test(text)) return i18n.message("storage.path_not_found");
      return text.replace(/^[a-z_]+:/i, "");
    }

    function smbSyncCheckingShare(data) {
      return !!data && data.pending_reason === "verify_recent";
    }

    function smbSyncResultText(data) {
      if (!data || data.ok === false) return i18n.message("status.unavailable");
      if (!data.enabled || !data.configured) return i18n.message("storage.configure_smb");
      const state = data.state || "unknown";
      const seen = Number(data.files_seen) || 0;
      const uploaded = Number(data.files_uploaded) || 0;
      const skipped = Number(data.files_skipped) || 0;
      const failed = Number(data.files_failed) || 0;
      const verify = !!data.last_run_verify ||
        data.pending_reason === "startup_check";
      if (state === "working") {
        if (smbSyncCheckingShare(data)) {
          return seen ? i18n.message("storage.checking_share_count", {count: seen}) :
            i18n.message("storage.checking_share");
        }
        if (verify) return i18n.message("storage.checking_latest");
        return seen ? i18n.message("storage.checking_files", {count: seen}) :
          i18n.message("storage.starting_sync");
      }
      if (state === "pending") {
        if (smbSyncCheckingShare(data)) {
          return data.network_available ? i18n.message("storage.waiting_share") :
            i18n.message("storage.waiting_network");
        }
        if (data.pending_reason === "startup_check") {
          return data.network_available ? i18n.message("storage.checking_latest") :
            i18n.message("storage.waiting_network");
        }
        return i18n.message("storage.waiting_sync");
      }
      if (state === "error") {
        const retry = retryInText(data.retry_due_ms);
        const age = runtimeAgeText(data.updated_ms);
        return i18n.message("storage.last_failed", {
          age, error: smbFriendlyError(data.error || data.last_error), retry,
        });
      }
      if (!data.started_ms) return i18n.message("storage.not_checked");
      const age = runtimeAgeText(data.updated_ms);
      const when = age ? i18n.join([" ", age]) : "";
      if (data.last_run_verify) {
        if (seen) {
          return i18n.message("storage.checked_count", {
            label: i18n.message(data.last_run_reconcile ?
              "storage.share_checked" : "storage.latest_checked"),
            when, count: seen,
          });
        }
        return i18n.join([i18n.message(data.last_run_reconcile ?
          "storage.share_reachable" : "storage.endpoint_reachable"), when]);
      }
      if (failed) {
        return i18n.message("storage.finished_failed", {when, count: failed});
      }
      if (uploaded) {
        return i18n.message("storage.synced", {when, count: uploaded,
          size: AirCANnect.format.bytes(Number(data.bytes_uploaded) || 0)});
      }
      if (seen && skipped === seen) {
        return i18n.message("storage.verified", {when, count: seen});
      }
      return seen ? i18n.message("storage.checked", {when, count: seen}) :
        i18n.message("status.ready");
    }

    function smbSyncNowText(data) {
      if (!data || data.ok === false) return "--";
      if (!data.enabled || !data.configured) return "--";
      const state = data.state || "unknown";
      if (state === "working") {
        if (smbSyncCheckingShare(data)) {
          return data.current_path ? i18n.message("storage.checking_path", {path: data.current_path}) :
            i18n.message("storage.checking_share");
        }
        if (data.last_run_verify) return i18n.message("storage.checking_latest");
        return data.current_path ? i18n.message("storage.syncing_path", {path: data.current_path}) : i18n.message("storage.syncing");
      }
      if (state === "pending") {
        if (smbSyncCheckingShare(data)) {
          return data.network_available ? i18n.message("status.queued") : i18n.message("storage.waiting_network");
        }
        if (data.pending_reason === "startup_check") {
          return data.network_available ? i18n.message("status.queued") : i18n.message("storage.waiting_network");
        }
        return i18n.message("status.queued");
      }
      if (state === "error") return retryInText(data.retry_due_ms) || "--";
      return "--";
    }

    function smbSyncCheckText(data) {
      if (!data || data.ok === false || !data.enabled || !data.configured) {
        return "--";
      }
      const reconcileSeen = Number(data.last_reconcile_files_seen) || 0;
      const reconcileWhen = AirCANnect.format.modified(data.last_reconcile_epoch);
      if (reconcileWhen || reconcileSeen) {
        const text = reconcileWhen || i18n.message("storage.share_checked");
        if (reconcileSeen) {
          return i18n.join([text,
            i18n.message("storage.file_count", {count: reconcileSeen})], ", ");
        }
        return text;
      }
      const seen = Number(data.last_verify_files_seen) || 0;
      const when = AirCANnect.format.modified(data.last_verify_epoch);
      if (!when && !seen) return "--";
      const text = when || i18n.message("storage.latest_checked");
      if (seen) {
        return i18n.join([text,
          i18n.message("storage.latest_files", {count: seen})], ", ");
      }
      return text;
    }

    function renderSmbSyncStatus(data) {
      const configured = !!(data && data.enabled && data.configured);
      AirCANnect.ui.text(
        "edfSyncEndpoint", data && data.endpoint ? data.endpoint : "--");
      AirCANnect.ui.text("edfSyncResult", smbSyncResultText(data));
      AirCANnect.ui.text("edfSyncLast", syncLastText(data, configured));
      AirCANnect.ui.text("edfSyncCheck", smbSyncCheckText(data));
      AirCANnect.ui.text("edfSyncCurrent", smbSyncNowText(data));
      const badge = document.getElementById("edfSyncBadge");
      if (badge) {
        i18n.text(badge, syncBadgeText(data, configured));
        badge.className = syncBadgeClass(data, configured);
      }
    }

    function applySmbSyncStatus(data) {
      const wasActive = smbSyncBusy;
      const active = syncStatusActive(data);
      if (active && !wasActive) {
        smbSyncCompleteMessage = data.pending_reason === "startup_check" ||
          data.pending_reason === "verify_recent" || data.last_run_verify ?
          i18n.message("storage.smb_checked") : i18n.message("storage.smb_complete");
      }

      smbSyncEnabled = !!(data && data.enabled);
      smbSyncConfigured = !!(data && data.configured);
      renderSmbSyncStatus(data);
      smbSyncSetBusy(active);

      if (data && data.state === "error") {
        AirCANnect.ui.message("edfSmbMsg", data.error || i18n.message("storage.smb_failed"), false, true);
      } else if (data && wasActive && !active && data.state === "idle") {
        AirCANnect.ui.message("edfSmbMsg", smbSyncCompleteMessage, true, false);
      }
    }

    async function loadSmbSyncStatus() {
      try {
        const response = await AirCANnect.http.request("/api/storage/sync/status", {cache: "no-store"});
        const text = await response.text();
        if (!response.ok) throw new Error(storageErrorText(text, response.status));
        const data = JSON.parse(text);
        applySmbSyncStatus(data);
        return data;
      } catch (error) {
        smbSyncEnabled = false;
        smbSyncConfigured = false;
        AirCANnect.ui.text("edfSyncResult", i18n.error(error));
        AirCANnect.ui.text("edfSyncLast", "--");
        AirCANnect.ui.text("edfSyncCheck", "--");
        AirCANnect.ui.text("edfSyncCurrent", "--");
        smbSyncSetBusy(false);
        return null;
      }
    }

    async function smbQueueAction(url, startMessage, completeMessage) {
      smbSyncCompleteMessage = completeMessage || i18n.message("storage.smb_complete");
      await queueSyncAction(url, startMessage, "edfSmbMsg",
        i18n.message("storage.smb_busy"), smbSyncSetBusy,
        loadSmbSyncStatus);
    }

    async function smbStartSync() {
      await smbQueueAction("/api/storage/sync/start",
        i18n.message("storage.smb_starting"), i18n.message("storage.smb_complete"));
    }

    async function smbVerifyRecent() {
      await smbQueueAction("/api/storage/sync/verify",
        i18n.message("storage.smb_checking"), i18n.message("storage.smb_checked"));
    }

    function sleepHqEndpointText(data) {
      if (!data || data.ok === false) return "--";
      if (!data.configured) return i18n.message("storage.configure_shq");
      const team = data.team_id || data.configured_team_id;
      return team ? i18n.message("storage.team", {team: String(team)}) : i18n.message("storage.shq_account");
    }

    function sleepHqFriendlyError(error) {
      const text = String(error || "");
      if (!text) return i18n.message("storage.sync_failed");
      if (/tls_heap|heap/i.test(text)) return i18n.message("storage.tls_heap");
      if (/auth|unauthor|token/i.test(text)) return i18n.message("storage.authentication_failed");
      if (/connect|timed out|timeout/i.test(text)) return i18n.message("storage.connection_failed");
      if (/team/i.test(text)) return i18n.message("storage.team_not_found");
      return text.replace(/^[a-z_]+:/i, "");
    }

    function sleepHqSyncResultText(data) {
      if (!data || data.ok === false) return i18n.message("status.unavailable");
      if (!data.configured) return i18n.message("storage.configure_shq");
      const state = data.state || "unknown";
      const seen = Number(data.files_seen) || 0;
      const uploaded = Number(data.files_uploaded) || 0;
      const failed = Number(data.files_failed) || 0;
      if (state === "working") {
        if (uploaded) {
          return i18n.message("storage.uploading", {count: uploaded});
        }
        return seen ? i18n.message("storage.checking_files", {count: seen}) :
          i18n.message("storage.connecting_shq");
      }
      if (state === "pending") {
        return data.network_available ? i18n.message("storage.waiting_sync") :
          i18n.message("storage.waiting_network");
      }
      if (state === "error") {
        const age = runtimeAgeText(data.updated_ms);
        const retry = retryInText(data.retry_due_ms);
        return i18n.message("storage.last_failed", {
          age, error: sleepHqFriendlyError(data.error), retry,
        });
      }
      if (!data.started_ms && !data.last_check_epoch) return i18n.message("storage.not_checked");
      if (failed) {
        return i18n.message("storage.finished_failed", {when: "", count: failed});
      }
      if (uploaded) {
        return i18n.message("storage.synced", {when: "", count: uploaded,
          size: AirCANnect.format.bytes(Number(data.bytes_uploaded) || 0)});
      }
      if (seen && Number(data.files_skipped) === seen) {
        return i18n.message("storage.verified", {when: "", count: seen});
      }
      return i18n.message("status.ready");
    }

    function sleepHqSyncCheckText(data) {
      if (!data || data.ok === false || !data.configured) return "--";
      const when = AirCANnect.format.modified(data.last_check_epoch);
      return when || "--";
    }

    function sleepHqSyncNowText(data) {
      if (!data || data.ok === false || !data.configured) return "--";
      const state = data.state || "unknown";
      if (state === "working") {
        if (data.import_status) return i18n.message("storage.import_status", {
          status: data.import_status,
        });
        return data.current_path ? i18n.message("storage.uploading_path", {path: data.current_path}) :
          i18n.message("storage.working");
      }
      if (state === "pending") {
        return data.network_available ? i18n.message("status.queued") : i18n.message("storage.waiting_network");
      }
      if (state === "error") return retryInText(data.retry_due_ms) || "--";
      return "--";
    }

    function renderSleepHqSyncStatus(data) {
      const configured = !!(data && data.configured);
      AirCANnect.ui.text("edfSleepHqEndpoint", sleepHqEndpointText(data));
      AirCANnect.ui.text("edfSleepHqResult", sleepHqSyncResultText(data));
      AirCANnect.ui.text("edfSleepHqLast", syncLastText(data, configured));
      AirCANnect.ui.text("edfSleepHqCheck", sleepHqSyncCheckText(data));
      AirCANnect.ui.text("edfSleepHqCurrent", sleepHqSyncNowText(data));
      const badge = document.getElementById("edfSleepHqBadge");
      if (badge) {
        i18n.text(badge, syncBadgeText(data, configured));
        badge.className = syncBadgeClass(data, configured);
      }
    }

    function applySleepHqSyncStatus(data) {
      const wasActive = sleepHqSyncBusy;
      const active = syncStatusActive(data);
      if (active && !wasActive) {
        sleepHqSyncCompleteMessage = data.pending_reason === "startup_check" ?
          i18n.message("storage.shq_checked") : i18n.message("storage.shq_complete");
      }

      sleepHqSyncConfigured = !!(data && data.configured);
      renderSleepHqSyncStatus(data);
      sleepHqSyncSetBusy(active);

      if (data && data.state === "error") {
        AirCANnect.ui.message("edfSleepHqMsg", data.error || i18n.message("storage.shq_failed"), false, true);
      } else if (data && !active && data.masks_error) {
        AirCANnect.ui.message("edfSleepHqMsg", i18n.message(
          "storage.mask_error", {error: i18n.error(data.masks_error)}), false, true);
      } else if (data && wasActive && !active && data.state === "idle") {
        AirCANnect.ui.message("edfSleepHqMsg", sleepHqSyncCompleteMessage, true, false);
      }
    }

    async function loadSleepHqSyncStatus() {
      try {
        const response = await AirCANnect.http.request("/api/sleephq/sync/status", {cache: "no-store"});
        const text = await response.text();
        if (!response.ok) throw new Error(storageErrorText(text, response.status));
        const data = JSON.parse(text);
        applySleepHqSyncStatus(data);
        return data;
      } catch (error) {
        sleepHqSyncConfigured = false;
        AirCANnect.ui.text("edfSleepHqResult", i18n.error(error));
        AirCANnect.ui.text("edfSleepHqLast", "--");
        AirCANnect.ui.text("edfSleepHqCheck", "--");
        AirCANnect.ui.text("edfSleepHqCurrent", "--");
        sleepHqSyncSetBusy(false);
        return null;
      }
    }

    async function sleepHqQueueAction(url, startMessage, completeMessage) {
      sleepHqSyncCompleteMessage = completeMessage ||
        i18n.message("storage.shq_complete");
      await queueSyncAction(url, startMessage, "edfSleepHqMsg",
        i18n.message("storage.shq_busy"), sleepHqSyncSetBusy,
        loadSleepHqSyncStatus);
    }

    async function sleepHqStartSync() {
      await sleepHqQueueAction("/api/sleephq/sync/start",
        i18n.message("storage.shq_starting"), i18n.message("storage.shq_complete"));
    }

    async function sleepHqCheckAccount() {
      await sleepHqQueueAction("/api/sleephq/sync/check",
        i18n.message("storage.shq_checking"), i18n.message("storage.shq_checked"));
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
      const bytesSent = Number(data && data.bytes_sent) || 0;
      const estimate = Number(data && data.estimated_archive_bytes) || 0;
      if (state === "preparing") {
        return files > 0 ? i18n.message("storage.preparing_files", {count: files}) : i18n.message("storage.prepare_file_list");
      }
      if (state === "building") {
        const pct = estimate > 0 ? " " + Math.min(99, Math.floor(bytes * 100 / estimate)) + "%" : "";
        return i18n.message("storage.building", {done: filesDone, total: files, percent: pct});
      }
      if (state === "ready") return i18n.message("storage.archive_ready");
      if (state === "downloading") {
        if (estimate <= 0) return i18n.message("storage.downloading");
        const pct = Math.min(100, Math.floor(bytesSent * 100 / estimate));
        const sentMiB = (bytesSent / (1024 * 1024)).toFixed(1);
        const totalMiB = (estimate / (1024 * 1024)).toFixed(1);
        return i18n.message("storage.download_progress", {percent: pct,
          sent: Number(sentMiB), total: Number(totalMiB)});
      }
      if (state === "error") return data && data.error ? data.error : i18n.message("storage.archive_failed");
      return state;
    }

    function storageApplyArchiveStatus(data) {
      const id = Number(storageArchiveJobId);
      if (!storageArchiveBusy || !id || !data) return;

      const state = data.state || "unknown";
      if (state === "idle" &&
          Number(storageArchiveDownloadStartedId) === id) {
        storageArchiveJobId = 0;
        storageArchiveDownloadStartedId = 0;
        storageArchiveSetBusy(false);
        AirCANnect.ui.message("storageMsg", i18n.message("storage.archive_downloaded"), true, false);
        return;
      }
      if (Number(data.id) !== id) return;

      AirCANnect.ui.message("storageMsg", storageArchiveStatusText(data),
        state !== "error", state === "error");
      if (state === "ready") {
        if (Number(storageArchiveDownloadStartedId) !== id) {
          storageArchiveDownloadStartedId = id;
          storageArchiveDownload(id);
        }
      } else if (state === "error" || state === "idle") {
        storageArchiveJobId = 0;
        storageArchiveDownloadStartedId = 0;
        storageArchiveSetBusy(false);
      }
    }

    function storageApplyDeleteStatus(data) {
      const id = Number(storageDeleteJobId);
      if (!storageDeleteBusy || !id || !data || Number(data.id) !== id) return;

      const state = data.state || "unknown";
      AirCANnect.ui.message("storageMsg", storageDeleteStatusText(data),
        state !== "error", state === "error");
      if (state === "done") {
        storageDeleteJobId = 0;
        storageDeleteSetBusy(false);
        storageClearSelection();
        loadStorageList(true);
      } else if (state === "error" || state === "idle") {
        storageDeleteJobId = 0;
        storageDeleteSetBusy(false);
      }
    }

    function applyStorageOperationSnapshot(data) {
      storageOperationData = data;
      applyStorageUsbStatus(data && data.usb);
      storageApplyArchiveStatus(data && data.archive);
      storageApplyDeleteStatus(data && data.delete);
    }

    function applyStorageUsbStatus(usb) {
      if (!usb) return;
      const previous = storageUsbState;
      storageUsbState = usb.state || "local";
      const row = document.getElementById("storageUsbRow");
      if (row) row.hidden = !usb.supported;
      const button = document.getElementById("storageUsbBtn");
      if (button) {
        const actions = {local: i18n.message("ui.share.sd.via.usb"), preparing: i18n.message("storage.usb_preparing"),
          shared: i18n.message("storage.usb_return"), returning: i18n.message("storage.usb_returning"),
          restoring: i18n.message("storage.usb_reloading"), error: i18n.message("storage.usb_retry")};
        i18n.text(button, storageUsbRequestPending ? i18n.message("storage.requesting")
          : actions[storageUsbState] || i18n.message("storage.usb_unavailable"));
        button.disabled = storageUsbRequestPending ||
          !["local", "shared", "error"].includes(storageUsbState);
      }
      const labels = {local: i18n.message("storage.local"), preparing: i18n.message("storage.preparing"),
        shared: i18n.message("storage.usb_rw"), returning: i18n.message("storage.returning"),
        restoring: i18n.message("storage.reloading"), error: i18n.message("status.unavailable")};
      AirCANnect.ui.text("storageUsbStatus", labels[storageUsbState] || storageUsbState);
      if (usb.error) AirCANnect.ui.message("storageUsbMsg", usb.error, false, true);
      else AirCANnect.ui.clearMessage("storageUsbMsg");

      if (previous !== storageUsbState && storageUsbState !== "local") {
        ++storageListRequestSeq;
        storageEntries = [];
        storageSelectedNames.clear();
        const list = document.getElementById("storageList");
        if (list) list.replaceChildren();
        storageSetBadge("USB", "warn");
      }
      storageSelectionUi();
      if (previous !== "local" && storageUsbState === "local") {
        storagePath = "/";
        if (AirCANnect.pages.isActive("storage")) loadStorageList(true);
      }
    }

    async function storageSetUsb(enabled) {
      const question = enabled
        ? i18n.message("storage.usb_confirm")
        : i18n.message("storage.usb_unmount");
      if (!confirm(i18n.format(question))) {
        applyStorageUsbStatus(storageOperationData && storageOperationData.usb);
        return;
      }

      storageUsbRequestPending = true;
      applyStorageUsbStatus(storageOperationData && storageOperationData.usb);
      let failure = "";
      try {
        const response = await AirCANnect.http.request("/api/storage/usb", {
          method: "POST", headers: {"Content-Type": "application/json"},
          body: JSON.stringify({enabled}),
        });
        if (!response.ok) throw new Error(storageErrorText(await response.text(), response.status));
      } catch (error) {
        failure = error.message;
      } finally {
        storageUsbRequestPending = false;
        applyStorageUsbStatus(storageOperationData && storageOperationData.usb);
        if (failure) AirCANnect.ui.message("storageUsbMsg", failure, false, true);
      }
    }

    async function storageStartArchive(url, options) {
      storageArchiveJobId = 0;
      storageArchiveDownloadStartedId = 0;
      storageArchiveSetBusy(true);
      AirCANnect.ui.message("storageMsg", i18n.message("storage.starting_archive"), true, false);
      try {
        const response = await AirCANnect.http.request(url, Object.assign({
          method: "POST",
          cache: "no-store",
        }, options || {}));
        const text = await response.text();
        if (!response.ok) throw new Error(storageErrorText(text, response.status));
        const data = JSON.parse(text);
        const id = Number(data.id);
        if (!id) throw new Error("bad_archive_id");
        storageArchiveJobId = id;
        applyStorageOperationSnapshot(storageOperationData);
      } catch (error) {
        storageArchiveJobId = 0;
        storageArchiveDownloadStartedId = 0;
        storageArchiveSetBusy(false);
        AirCANnect.ui.message("storageMsg", i18n.error(error), false, true);
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
        AirCANnect.ui.message("storageMsg", i18n.message("storage.select_first"), false, true);
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
        return i18n.message("storage.deleting", {
          done: rootsDone, total: roots, removed: files + dirs,
        });
      }
      if (state === "done") return i18n.message("storage.delete_complete");
      if (state === "error") return data && data.error ? data.error : i18n.message("storage.delete_failed");
      return state;
    }

    async function storageDeleteSelected() {
      const selected = storageEntries
        .filter((entry) => entry && storageSelectedNames.has(entry.name))
        .map((entry) => entry.name);
      if (!selected.length) {
        AirCANnect.ui.message("storageMsg", i18n.message("storage.select_first"), false, true);
        return;
      }
      if (!confirm(i18n.t("storage.delete_confirm"))) return;
      storageDeleteJobId = 0;
      storageDeleteSetBusy(true);
      AirCANnect.ui.message("storageMsg", i18n.message("storage.starting_delete"), true, false);
      try {
        const response = await AirCANnect.http.request("/api/storage/delete/start", {
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
        storageDeleteJobId = id;
        applyStorageOperationSnapshot(storageOperationData);
      } catch (error) {
        storageDeleteJobId = 0;
        storageDeleteSetBusy(false);
        AirCANnect.ui.message("storageMsg", i18n.error(error), false, true);
      }
    }

    AirCANnect.actions.register("storage.up", () => storageUp());
    AirCANnect.actions.register("storage.usb", () =>
      storageSetUsb(storageUsbState === "local"));
    AirCANnect.actions.register("storage.upload-choose", () =>
      storageChooseUpload());
    AirCANnect.actions.register("storage.refresh", () =>
      loadStorageList(true));
    AirCANnect.actions.register("storage.upload-files", (_event, element) =>
      storageFilesSelected(element));
    AirCANnect.actions.register("storage.upload-cancel", () =>
      storageCancelUpload());
    AirCANnect.actions.register("storage.select-all", (_event, element) =>
      storageToggleSelectAll(element.checked));
    AirCANnect.actions.register("storage.delete-selected", () =>
      storageDeleteSelected());
    AirCANnect.actions.register("storage.archive-selected", () =>
      storageArchiveSelected());
    AirCANnect.actions.register("storage.prev", () => storagePrevPage());
    AirCANnect.actions.register("storage.next", () => storageNextPage());
    AirCANnect.actions.register("sync.smb-start", () => smbStartSync());
    AirCANnect.actions.register("sync.smb-verify", () => smbVerifyRecent());
    AirCANnect.actions.register("sync.sleephq-start", () =>
      sleepHqStartSync());
    AirCANnect.actions.register("sync.sleephq-check", () =>
      sleepHqCheckAccount());
    AirCANnect.pages.onLoad("edf", () => {
      renderEdfOverview(AirCANnect.snapshots.read("status").data);
      loadSmbSyncStatus();
      loadSleepHqSyncStatus();
    });
    AirCANnect.pages.onLoad("storage", (refresh) => {
      loadStorageList(refresh);
      loadSmbSyncStatus();
    });
    AirCANnect.events.subscribe("exports", (data) => {
      if (data.smb) applySmbSyncStatus(data.smb);
      if (data.sleephq) applySleepHqSyncStatus(data.sleephq);
    });
    AirCANnect.events.subscribe(
      "storage_operation", applyStorageOperationSnapshot);
    AirCANnect.snapshots.subscribe("status", renderEdfOverview, true);
})();
