(() => {
    const pages = {
      dash: "Dashboard",
      report: "Report",
      edf: "EDF",
      storage: "Storage",
      clinical: "Clinical",
      oxi: "Oximetry",
      wifi: "WiFi",
      ota: "Update",
      console: "Console",
      config: "Config",
    };

    AirCANnect.pages.define(pages);
    AirCANnect.actions.start();
    AirCANnect.events.start("/api/events");

    const requested = location.hash.slice(1);
    AirCANnect.pages.show(pages[requested] ? requested : "dash");
    AirCANnect.startup.run();
})();
