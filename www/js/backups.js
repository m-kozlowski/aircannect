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
      state.node.querySelectorAll("button").forEach(node => node.disabled = false);
    }
  }

  function selected(element) {
    return panels.get(element.closest("[data-backup-kind]")?.dataset.backupKind);
  }

  AirCANnect.actions.register("backup.create", (_event, element) => {
    const state = selected(element);
    run(state, async () => {
      state.node.querySelector("[data-backup-preview]").hidden = true;
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
      const preview = state.node.querySelector("[data-backup-preview]");
      preview.hidden = true;
      const data = await command(state, "preview", {path});
      state.plan = data.plan;
      i18n.text(state.node.querySelector("[data-backup-confirm]"),
        i18n.message("backup.confirm", {name: path.split("/").pop()}));
      const changes = state.node.querySelector("[data-backup-changes]");
      changes.replaceChildren();
      if (data.serial) {
        const source = document.createElement("p");
        source.textContent = data.serial + " / " + data.created;
        changes.append(source);
      }
      for (const entry of data.entries || []) {
        const row = document.createElement("p");
        row.textContent = entry.key + ": " + entry.before + " -> " + entry.value;
        if (entry.reason) row.append(" (" + i18n.t("clinical.backup.reason." + entry.reason) + ")");
        changes.append(row);
      }
      preview.hidden = false;
      state.node.scrollIntoView({block: "nearest"});
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
    run(state, async () => {
      const data = await command(state, "restore", {plan: state.plan});
      state.node.querySelector("[data-backup-preview]").hidden = true;
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
      state.node.querySelector("[data-backup-preview]").hidden = true;
    });
  });

  AirCANnect.actions.register("backup.options", () => {
    document.getElementById("configBackupWarning").hidden =
      !document.getElementById("configBackupSecrets").checked &&
      !document.getElementById("configBackupPairing").checked;
  });

  AirCANnect.startup.register(() => {
    document.querySelectorAll("[data-backup-kind]").forEach(node => {
      const kind = node.dataset.backupKind;
      const root = "/aircannect/backups/" + (kind === "config" ? "ac" : "airsense");
      const state = {node, kind, plan: 0, message: kind + "BackupMsg"};
      panels.set(kind, state);
      node.querySelector("[data-backup-controls]").innerHTML = `
        <div class="btns">
          <button class="btn" data-action="backup.create" data-i18n="backup.create"></button>
          <button class="btn" data-action="backup.history" data-path="${root}" data-i18n="backup.history"></button>
        </div>
        <div data-backup-preview hidden>
          <p data-backup-confirm></p><div data-backup-changes></div>
          <div class="btns">
            <button class="btn primary" data-action="backup.restore" data-i18n="clinical.backup.apply"></button>
            <button class="btn" data-action="backup.cancel" data-i18n="ui.cancel"></button>
          </div>
        </div>
        <div class="msg" id="${state.message}"></div><div data-backup-issues></div>`;
      i18n.translate(node);
    });
  });
})();
