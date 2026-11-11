(() => {
    const pages = {
      dash: AirCANnect.i18n.message("ui.dashboard"),
      report: AirCANnect.i18n.message("ui.report"),
      edf: "EDF",
      storage: AirCANnect.i18n.message("ui.storage"),
      clinical: AirCANnect.i18n.message("ui.clinical"),
      oxi: AirCANnect.i18n.message("ui.oximetry"),
      wifi: "WiFi",
      ota: AirCANnect.i18n.message("ui.update"),
      console: AirCANnect.i18n.message("ui.console"),
      config: AirCANnect.i18n.message("ui.config"),
    };

    AirCANnect.pages.define(pages);
    AirCANnect.actions.start();
    AirCANnect.events.start("/api/events");

    const requested = location.hash.slice(1);
    AirCANnect.pages.show(pages[requested] ? requested : "dash");
    AirCANnect.startup.run();
})();
