    AirCANnect.events.start("/api/events");
    AirCANnect.actions.start();
    if (location.hash && tabs[location.hash.slice(1)]) {
      showTab(location.hash.slice(1));
    } else {
      updateLiveViewState("dash");
      AirCANnect.pages.load("dash", false);
    }
    initOnboarding();

    setInterval(() => {
      if (document.getElementById("p-dash").classList.contains("active")) {
        updateLiveViewState("dash");
      }
    }, 5000);
