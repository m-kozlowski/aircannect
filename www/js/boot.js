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

    setInterval(() => {
      if (document.getElementById("p-dash").classList.contains("active")) {
        updateLiveViewState("dash");
        loadStatus();
      }
      if (document.getElementById("p-oxi").classList.contains("active")) {
        loadOximetrySensors();
      }
    }, 5000);
