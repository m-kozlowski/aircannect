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

    async function loadWifi() {
      try {
        const response = await AirCANnect.http.requestOk("/api/wifi");
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
          root.appendChild(AirCANnect.ui.row(profile.ssid, button,
            (index === data.active ? "active, " : "") +
            (profile.open ? "open" : "password")));
        });
      } catch (error) {
        AirCANnect.ui.message("wifiMsg", error.message, false);
      }
    }

    async function requestWifiAction(action, extra) {
      const body = Object.assign({action}, extra || {});
      const response = await AirCANnect.http.requestOk("/api/wifi", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(body),
      });
      return await response.json();
    }

    async function wifiAction(action, extra) {
      try {
        const data = await requestWifiAction(action, extra);
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
