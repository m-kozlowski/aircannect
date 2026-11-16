(() => {
  const i18n = AirCANnect.i18n;
  const panels = new Map();

  async function command(state, action, values = {}) {
    const response = await AirCANnect.http.request("/api/backup", {
      method: "POST", cache: "no-store",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify({kind: state.kind, action, ...values}),
    });
    const data = await response.json();
    if (!response.ok || !data.ok) {
      const error = new Error(data.error || "backup_failed");
      if (action === "restore" && state.kind === "config" && data.applied != null) {
        error.ui = i18n.join([i18n.error(error), "; ", i18n.message("config.backup.restored", data)]);
      }
      throw error;
    }
    return data;
  }

  async function run(state, work) {
    if (state.busy) return;
    state.busy = true;
    state.node.setAttribute("aria-busy", "true");
    state.node.querySelectorAll("button").forEach(node => node.disabled = true);
    AirCANnect.ui.clearMessage(state.message);
    try {
      await work();
    } catch (error) {
      AirCANnect.ui.message(state.message, i18n.error(error), false, true);
    } finally {
      state.busy = false;
      state.node.removeAttribute("aria-busy");
      state.node.querySelectorAll("button").forEach(node => {
        node.disabled = node.dataset.action === "backup.restore" && !state.canRestore;
      });
      const dialog = state.node.querySelector("[data-backup-preview]");
      if (dialog.open) {
        dialog.querySelector("[data-action='backup.cancel']").focus({preventScroll: true});
      }
    }
  }

  function showPreview(state, visible) {
    const dialog = state.node.querySelector("[data-backup-preview]");
    const message = document.getElementById(state.message);
    if (visible) {
      dialog.querySelector("[data-backup-notice]").append(message);
      dialog.hidden = false;
      if (!dialog.open) dialog.showModal();
    } else {
      if (dialog.open) dialog.close();
      dialog.hidden = true;
      state.node.querySelector("[data-backup-controls]").append(message);
    }
  }

  function settingsTable(entries, clinical) {
    const table = document.createElement("table");
    table.className = "backup-diff";
    const header = table.createTHead().insertRow();
    for (const key of ["ui.settings", "backup.current", "backup.in_file"]) {
      const cell = document.createElement("th");
      cell.scope = "col";
      i18n.text(cell, i18n.message(key));
      header.append(cell);
    }

    const body = table.createTBody();
    for (const entry of entries) {
      const row = body.insertRow();
      row.className = entry.reason ? "backup-skipped" : entry.unchanged ? "" : "backup-changed";
      const label = document.createElement("th");
      label.scope = "row";
      const caption = document.createElement("span");
      const wifi = !clinical && /^(ssid|pass)_(\d+)$/.exec(entry.key);
      const sensor = !clinical && /^oxi_sensor_(\d+)$/.exec(entry.key);
      i18n.text(caption, wifi ?
        i18n.message(wifi[1] === "ssid" ? "backup.wifi_ssid" : "backup.wifi_password", {number: +wifi[2] + 1}) :
        sensor ? i18n.message("backup.oximeter", {number: +sensor[1] + 1}) :
        !clinical ? i18n.message("config.field." + entry.key + ".label", {}, entry.label || entry.key) : entry.stock ?
        i18n.message("clinical.label." + entry.key, {}, entry.label || entry.key) :
        entry.label || entry.key);
      label.append(caption);
      row.append(label);

      if (entry.reason) {
        const reason = document.createElement("small");
        i18n.text(reason, i18n.message((clinical ? "clinical" : "config") + ".backup.reason." + entry.reason));
        label.append(reason);
      }
      for (const key of ["before", "value"]) {
        const cell = row.insertCell();
        const option = entry[key + "_option"];
        if (entry.secret) {
          const disposition = key === "before" ? (entry.before ? "set" : "empty") :
            entry.kept ? "kept" : entry.unchanged ? (entry.value ? "set" : "empty") :
            entry.value ? "replace" : "remove";
          i18n.text(cell, i18n.message("backup.secret." + disposition));
          continue;
        }
        i18n.text(cell, entry.stock && Number.isInteger(option) ?
          i18n.message("clinical.option." + entry.key + "." + option, {}, entry[key]) :
          !clinical && entry[key] ?
          i18n.message("config.option." + entry.key + "." + entry[key], {}, entry[key]) :
          entry[key] || "--");
      }
    }
    return table;
  }

  function renderPreview(state, data, path) {
    const clinical = state.kind === "clinical";
    const entries = data.entries || [];
    const changed = entries.filter(entry => !entry.unchanged && !entry.reason);
    const skipped = entries.filter(entry => entry.reason);
    const unchanged = entries.filter(entry => entry.unchanged);
    state.canRestore = changed.length > 0;

    i18n.text(state.node.querySelector("[data-backup-heading]"),
      i18n.message(clinical ? "backup.clinical_heading" : "backup.config_heading"));
    state.node.querySelector("[data-backup-file]").textContent = path.split("/").pop();
    state.node.querySelector("[data-backup-source]").hidden = !clinical;
    state.node.querySelector("[data-backup-device]").textContent =
      [data.device_name, data.serial].filter(Boolean).join(" / ") || "--";
    state.node.querySelector("[data-backup-created]").textContent =
      AirCANnect.format.modified(Date.parse(data.created) / 1000) || "--";

    const changes = state.node.querySelector("[data-backup-changes]");
    changes.replaceChildren();
    const summary = document.createElement("p");
    summary.className = "backup-counts";
    for (const [key, count] of [
      ["backup.changes", changed.length],
      ["backup.unchanged", data.unchanged || 0],
      ["backup.skipped", skipped.length],
    ]) {
      if (key === "backup.skipped" && !count) continue;
      const item = document.createElement("span");
      i18n.text(item, i18n.message(key, {count}));
      summary.append(item);
    }
    changes.append(summary);

    if (!changed.length) {
      const empty = document.createElement("p");
      empty.className = "backup-no-changes";
      i18n.text(empty, i18n.message("backup.no_changes"));
      changes.append(empty);
    }
    if (changed.length || skipped.length) changes.append(settingsTable([...changed, ...skipped], clinical));
    if (unchanged.length) {
      const details = document.createElement("details");
      details.className = "backup-unchanged";
      const caption = document.createElement("summary");
      i18n.text(caption, i18n.message("backup.unchanged", {count: unchanged.length}));
      details.append(caption, settingsTable(unchanged, clinical));
      changes.append(details);
    }

    state.node.querySelector("[data-action='backup.restore']").hidden = !state.canRestore;
    i18n.text(state.node.querySelector("[data-action='backup.cancel']"),
      i18n.message(state.canRestore ? "ui.cancel" : "ui.close"));
    showPreview(state, true);
  }

  function selected(element) {
    return panels.get(element.closest("[data-backup-kind]")?.dataset.backupKind);
  }

  AirCANnect.actions.register("backup.create", (_event, element) => {
    const state = selected(element);
    run(state, async () => {
      showPreview(state, false);
      state.plan = 0;
      const data = await command(state, "create", {
        secrets: !!document.getElementById("configBackupSecrets")?.checked,
        internal: !!document.getElementById("configBackupPairing")?.checked,
      });
      state.node.querySelector("[data-action='backup.history']").dataset.path =
        data.path.slice(0, data.path.lastIndexOf("/"));
      AirCANnect.ui.message(state.message, i18n.message("backup.saved"), true, false);
    });
  });

  AirCANnect.actions.register("backup.preview", (_event, element) => {
    const path = element.dataset.path;
    const state = panels.get(path.startsWith("/aircannect/backups/ac/") ? "config" : "clinical");
    if (state.busy) return;
    AirCANnect.pages.show(state.kind);
    state.node.open = true;
    run(state, async () => {
      showPreview(state, false);
      state.plan = 0;
      state.canRestore = false;
      const data = await command(state, "preview", {path});
      state.plan = data.plan;
      renderPreview(state, data, path);
    });
  });

  AirCANnect.actions.register("backup.history", (_event, element) => {
    const state = selected(element);
    run(state, async () => {
      const data = await command(state, "directory");
      if (!element.dataset.path) element.dataset.path = data.path;
      AirCANnect.actions.invoke("storage.open-path", null, element);
    });
  });

  AirCANnect.actions.register("backup.restore", (_event, element) => {
    const state = selected(element);
    if (!state.canRestore) return;
    run(state, async () => {
      const data = await command(state, "restore", {plan: state.plan});
      showPreview(state, false);
      state.plan = 0;
      AirCANnect.ui.message(state.message, state.kind === "config" ?
        i18n.message("config.backup.restored", data) :
        i18n.message("clinical.backup.verified"), true, false);
      const changes = state.node.querySelector("[data-backup-issues]");
      changes.replaceChildren();
      for (const issue of data.issues || []) {
        const row = document.createElement("p");
        row.textContent = issue.key + ": " + i18n.t("config.backup.reason." + issue.reason);
        changes.append(row);
      }
      if (state.kind === "config") AirCANnect.pages.refresh();
    });
  });

  AirCANnect.actions.register("backup.cancel", (_event, element) => {
    const state = selected(element);
    run(state, async () => {
      await command(state, "cancel", {plan: state.plan});
      state.plan = 0;
      showPreview(state, false);
    });
  });

  AirCANnect.startup.register(() => {
    document.querySelectorAll("[data-backup-kind]").forEach(node => {
      const kind = node.dataset.backupKind;
      const root = "/aircannect/backups/" + (kind === "config" ? "ac" : "airsense");
      const state = {node, kind, plan: 0, canRestore: false, message: kind + "BackupMsg"};
      panels.set(kind, state);
      node.querySelector("[data-backup-controls]").innerHTML = `
        <div class="btns" data-backup-toolbar>
          <button class="btn" data-action="backup.create" data-i18n="backup.create"></button>
          <button class="btn" data-action="backup.history" data-path="${root}" data-i18n="backup.history"></button>
        </div>
        <dialog class="backup-dialog" data-backup-preview hidden aria-labelledby="${kind}BackupHeading">
          <header>
            <h2 id="${kind}BackupHeading" data-backup-heading></h2>
            <div class="backup-file" data-backup-file></div>
          </header>
          <div class="backup-body">
            <dl class="backup-source" data-backup-source>
              <div><dt data-i18n="backup.device"></dt><dd data-backup-device></dd></div>
              <div><dt data-i18n="backup.created"></dt><dd data-backup-created></dd></div>
            </dl>
            <div data-backup-changes></div>
            <div data-backup-notice></div>
          </div>
          <footer class="btns">
            <button class="btn" autofocus data-action="backup.cancel" data-i18n="ui.cancel"></button>
            <button class="btn primary" data-action="backup.restore" data-i18n="clinical.backup.apply"></button>
          </footer>
        </dialog>
        <div class="msg" id="${state.message}"></div><div data-backup-issues></div>`;
      node.querySelector("[data-backup-preview]").addEventListener("cancel", event => {
        event.preventDefault();
        if (!state.busy) {
          AirCANnect.actions.invoke("backup.cancel", event,
            node.querySelector("[data-action='backup.cancel']"));
        }
      });
      i18n.translate(node);
    });
  });
})();
