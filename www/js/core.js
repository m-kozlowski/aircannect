(() => {
    const liveViewClientId = (() => {
      if (window.crypto && crypto.randomUUID) return crypto.randomUUID();
      return Date.now().toString(36) + Math.random().toString(36).slice(2);
    })();
    let liveViewActiveSent = null;
    let liveViewLastPostMs = 0;
    const liveViewHeartbeatMs = 5000;

    function liveViewWanted(page) {
      return page === "dash" && document.visibilityState !== "hidden";
    }

    function postLiveViewState(active, force) {
      const now = Date.now();
      if (!force && liveViewActiveSent === active) {
        if (!active || now - liveViewLastPostMs < liveViewHeartbeatMs) return;
      }
      liveViewActiveSent = active;
      liveViewLastPostMs = now;
      AirCANnect.http.request(
        "/api/live/view?id=" + encodeURIComponent(liveViewClientId) +
          "&active=" + (active ? "1" : "0"), {
          method: "POST",
          cache: "no-store",
          keepalive: true,
        }).catch(() => {
          liveViewActiveSent = null;
        });
    }

    function updateLiveViewState(page, force) {
      postLiveViewState(
        liveViewWanted(page || AirCANnect.pages.active()), force);
    }

    document.addEventListener(
      "visibilitychange", () => updateLiveViewState());
    window.addEventListener("beforeunload", () => {
      if (navigator.sendBeacon) {
        navigator.sendBeacon("/api/live/view?id=" +
          encodeURIComponent(liveViewClientId) + "&active=0", new Blob([]));
      }
    });

    AirCANnect.actions.register("app.show-tab", (event, element) => {
      event.preventDefault();
      AirCANnect.pages.show(element.dataset.value);
    });
    AirCANnect.actions.register("app.refresh", () => {
      updateLiveViewState();
      AirCANnect.pages.refresh();
    });
    AirCANnect.pages.onChange((page) => updateLiveViewState(page));

    setInterval(() => {
      if (AirCANnect.pages.isActive("dash")) updateLiveViewState("dash");
    }, liveViewHeartbeatMs);
})();
