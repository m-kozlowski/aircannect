    let statusData = {};
    let statusLoaded = false;
    let statusLoadPromise = null;
    let streamData = {};
    let settingsData = null;
    let settingsCatalog = [];
    let settingsComposites = [];
    let settingsCatalogPromise = null;
    let settingsCatalogRevision = 0;
    let configData = null;
    let otaData = null;
    let configSections = [];
    let as11BlePairingData = null;
    let configSchemaPromise = null;
    const configSectionById = {};
    const configSectionByKey = {};
    const configFieldByKey = {};

    function setPageTitle(hostname) {
      const clean = String(hostname || "").trim();
      document.title = clean ? "AirCANnect - " + clean : "AirCANnect";
    }

    function rebuildConfigSectionMaps() {
      Object.keys(configSectionById).forEach((key) =>
        delete configSectionById[key]);
      Object.keys(configSectionByKey).forEach((key) =>
        delete configSectionByKey[key]);
      Object.keys(configFieldByKey).forEach((key) =>
        delete configFieldByKey[key]);
      configSections.forEach((section) => {
        configSectionById[section.id] = section;
        section.fields.forEach((field) => {
          configSectionByKey[field.key] = section.id;
          configFieldByKey[field.key] = field;
        });
      });
    }
    let oxiSensorData = {sensor_scan_results: [], sensor_known: []};
    let oxiSensorsLoading = false;
    let consoleSeq = -1;
    let consoleEnd = 0;
    let settingsProfileMode = null;
    let settingsModeDirty = false;
    let settingsActiveMode = null;
    const resmedOtaRate = {
      phase: "",
      total: 0,
      bytes: 0,
      time: 0,
      bps: 0,
    };
    let resmedOtaData = null;
    let resmedRepositoryCatalogRevision = 0;
    let resmedRepositoryLoadGeneration = 0;
    let resmedRepositoryLoaded = false;
    let resmedDirectUploadBusy = false;
    let resmedUploadOperation = null;
    let resmedCanAvailable = true;
    const LIVE_FAST_POINTS = 6000;
    const LIVE_MEDIUM_POINTS = 3000;
    const LIVE_SLOW_POINTS = 1800;
    const liveData = {
      pressure: [],
      flow: [],
      leak: [],
      inspPressure: [],
      expPressure: [],
      spo2: [],
      pulse: [],
    };
    const chartScales = {
      pressure: {min: 0, max: 15, step: 5},
      flow: {min: -40, max: 40, step: 10, symmetric: true},
      leak: {min: 0, max: 20, step: 5},
      therapyPressure: {min: 0, max: 15, step: 5},
    };
    const tabs = {
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

    document.querySelectorAll(".nav").forEach((nav) => {
      nav.onclick = () => showTab(nav.dataset.tab);
    });

    const liveViewClientId = (() => {
      if (window.crypto && crypto.randomUUID) return crypto.randomUUID();
      return Date.now().toString(36) + Math.random().toString(36).slice(2);
    })();
    let liveViewActiveSent = null;
    let liveViewLastPostMs = 0;
    const liveViewHeartbeatMs = 5000;
