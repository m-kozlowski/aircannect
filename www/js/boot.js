    initEvents();
    updateCharts();
    window.addEventListener("resize", () => {
      updateCharts();
      scheduleReportDraw();
    });
    if (location.hash && tabs[location.hash.slice(1)]) {
      showTab(location.hash.slice(1));
    } else {
      updateLiveViewState("dash");
      loadStatus();
    }
    initOnboarding();

    setInterval(() => {
      if (document.getElementById("p-dash").classList.contains("active")) {
        updateLiveViewState("dash");
      }
    }, 5000);
