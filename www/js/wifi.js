(() => {
    "use strict";

    function renderWifiCurrent(data) {
      const root = document.getElementById("wifiCurrent");
      if (!root) return;
      root.innerHTML = "";

      const status = AirCANnect.snapshots.read("status").data || {};
      const state = data.state || status.wifi_state || "--";
      const ssid = data.ssid || status.wifi_ssid || "";
      const ip = data.ip || status.wifi_ip || "";
      const channel = data.channel || status.wifi_channel || 0;
      const bssid = data.bssid || status.wifi_bssid || "";
      const rssi = data.rssi !== undefined ? data.rssi : status.wifi_rssi;
      const roam = data.roam !== undefined ? data.roam : status.wifi_roam;
      const active = Number.isFinite(Number(data.active)) ?
        Number(data.active) : Number(status.wifi_profile);

      root.appendChild(AirCANnect.ui.row("State", AirCANnect.ui.valueSpan(state),
        roam ? "roaming on" : "roaming off"));
      root.appendChild(AirCANnect.ui.row("SSID", AirCANnect.ui.valueSpan(ssid),
        Number.isFinite(active) && active >= 0 ? "profile " + active : ""));
      root.appendChild(AirCANnect.ui.row(
        "Signal", AirCANnect.ui.wifiSignal(rssi),
        channel > 0 ? "channel " + channel : ""));
      root.appendChild(AirCANnect.ui.row("IP", AirCANnect.ui.valueSpan(ip),
        bssid && bssid !== "00:00:00:00:00:00" ? bssid : ""));
    }

    // Mirrors AC_WIREGUARD_HOME_SSIDS_MAX in the firmware.
    const HOME_SSIDS_MAX = 160;

    function parseHomeSsids(value) {
      return String(value || "").split(",").map((item) => item.trim())
        .filter((item) => item.length);
    }

    // Without the VPN section the profiles still render, just without
    // the home toggle.
    async function loadHomeSsids() {
      try {
        const response = await AirCANnect.http.requestOk("/api/config/vpn");
        const data = await response.json();
        return parseHomeSsids(data.wg_home);
      } catch (_error) {
        return null;
      }
    }

    function homeButton(ssid, homeSsids) {
      const isHome = homeSsids.includes(ssid);
      const button = document.createElement("button");
      button.className = isHome ? "btn primary" : "btn";
      button.textContent = isHome ? "Home" : "Set as home";
      button.title = isHome ?
        "The VPN stays down on this network in Away mode. Click to unset." :
        "Keep the VPN down on this network in Away mode.";
      // wg_home is a comma-separated list.
      button.disabled = ssid.includes(",");
      button.onclick = () => setHomeSsid(ssid, !isHome, homeSsids);
      return button;
    }

    async function loadWifi() {
      try {
        const [response, homeSsids] = await Promise.all([
          AirCANnect.http.requestOk("/api/wifi"),
          loadHomeSsids(),
        ]);
        const data = await response.json();
        renderWifiCurrent(data);
        const root = document.getElementById("wifiProfiles");
        root.innerHTML = "";
        if (!data.profiles.length) {
          root.innerHTML = '<div class="value" style="text-align:left">No profiles</div>';
        }
        data.profiles.forEach((profile, index) => {
          const controls = document.createElement("div");
          controls.className = "btns";
          if (homeSsids) controls.appendChild(homeButton(profile.ssid, homeSsids));
          const button = document.createElement("button");
          button.className = "btn danger";
          button.textContent = "Remove";
          button.onclick = () => wifiRemove(index);
          controls.appendChild(button);
          root.appendChild(AirCANnect.ui.row(profile.ssid, controls,
            (index === data.active ? "active, " : "") +
            (profile.open ? "open" : "password")));
        });
      } catch (error) {
        AirCANnect.ui.message("wifiMsg", error.message, false);
      }
    }

    async function setHomeSsid(ssid, home, homeSsids) {
      const next = homeSsids.filter((item) => item !== ssid);
      if (home) next.push(ssid);
      const value = next.join(",");
      if (value.length > HOME_SSIDS_MAX) {
        AirCANnect.ui.message("wifiMsg",
          "Home networks list is limited to " + HOME_SSIDS_MAX + " characters",
          false);
        return;
      }
      try {
        await AirCANnect.http.requestOk("/api/config", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({wg_home: value}),
        });
        AirCANnect.ui.message("wifiMsg",
          home ? ssid + " set as home Wi-Fi" : ssid + " no longer home Wi-Fi",
          true);
        setTimeout(loadWifi, 600);
      } catch (error) {
        AirCANnect.ui.message("wifiMsg", error.message, false);
      }
    }

    async function wifiAction(action, extra) {
      try {
        const data = await AirCANnect.http.postJson("/api/wifi",
          Object.assign({action}, extra || {}));

        AirCANnect.ui.message("wifiMsg", data.result, data.ok);
        setTimeout(loadWifi, 600);
      } catch (error) {
        AirCANnect.ui.message("wifiMsg", error.message, false);
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


    AirCANnect.actions.register("wifi.add", () => wifiAdd());
    AirCANnect.actions.register("wifi.command", (_event, element) =>
      wifiAction(element.dataset.value));
    AirCANnect.pages.onLoad("wifi", () => loadWifi());
    AirCANnect.snapshots.subscribe("status", () => {
      if (AirCANnect.pages.isActive("wifi")) renderWifiCurrent({});
    }, false);
})();
