    let statusData = {};
    let streamData = {};
    let settingsData = null;
    let settingsCatalog = [];
    let settingsComposites = [];
    let settingsCatalogPromise = null;
    let configData = null;
    let configSections = [];
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
    let evtSrc = null;
    let consoleSeq = -1;
    let consoleEnd = 0;
    let settingsPollTimer = null;
    let settingsRefreshUntil = 0;
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
    let resmedRepositoryPollTimer = null;
    let resmedDirectUploadBusy = false;
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
    let reportSummary = null;
    let reportPollTimer = null;
    let reportLoadToken = 0;
    let reportSummaryEtag = "";
    let reportResult = null;
    const reportResultClientCache = new Map();
    let reportSeries = {};
    let reportEvents = [];
    // Non-persistent per-view set of hidden session start timestamps. Cleared
    // whenever a different night loads; toggling re-ranges the charts.
    const reportHiddenSessions = new Set();
    let reportDrawItems = [];
    let reportDrawPending = false;
    let reportDrawRetryCount = 0;
    let reportResizeObserver = null;
    const reportPlotClientCache = new Map();
    let reportZoom = null;        // {start,end} when drag-zoomed, else null
    // Zoom: exact 15-minute tiles composed into the selected window.
    let reportBaseSeries = {};
    let reportBaseEvents = [];
    let reportCurrentNightId = "";
    let reportCurrentRevision = "";
    let reportCurrentPlotEtag = "";
    const reportRangeCache = new Map();
    let reportRangeInFlightKey = "";
    let reportRangeToken = 0;
    let reportHoverTime = null;   // cursor time shared across all charts
    let reportDrag = null;        // active drag-to-zoom selection state
    let reportSelectedNightId = ""; // user-picked night id; "" defaults to newest
    let reportCalView = null;     // Date at the 1st of the calendar's shown month
    let storagePath = "/";
    let storageOffset = 0;
    let storageLimit = 64;
    let storageNextOffset = null;
    let storageListRequestSeq = 0;
    let storageArchivePollTimer = null;
    let storageArchiveDownloadStartedId = 0;
    let storageDeletePollTimer = null;
    let smbSyncPollTimer = null;
    let sleepHqSyncPollTimer = null;
    let storageEntries = [];
    let storageSelectedNames = new Set();
    let storageArchiveBusy = false;
    let storageDeleteBusy = false;
    let storageUploadBusy = false;
    let storageUploadCancelRequested = false;
    let storageUploadCurrentId = 0;
    let smbSyncBusy = false;
    let smbSyncEnabled = false;
    let smbSyncConfigured = false;
    let smbSyncCompleteMessage = "SMB sync complete";
    let sleepHqSyncBusy = false;
    let sleepHqSyncConfigured = false;
    let sleepHqSyncCompleteMessage = "SleepHQ sync complete";
    let edfOverviewLoading = false;

    const SVG_NS = "http:" + "/" + "/www.w3.org/2000/svg";
    const REPORT_RESULT_CLIENT_CACHE_MAX = 8;
    const REPORT_PLOT_CLIENT_CACHE_MAX = 32;
    const REPORT_RANGE_CACHE_MAX = 24;
    const REPORT_RANGE_TILE_MS = 15 * 60 * 1000;
    const REPORT_RESULT_POLL_MAX_ATTEMPTS = 160;
    const REPORT_PLOT_POLL_MAX_ATTEMPTS = 120;
    const REPORT_RANGE_POLL_MAX_ATTEMPTS = 120;
    const REPORT_POLL_DELAY_MS = 300;
    const REPORT_RANGE_POLL_DELAY_MS = 250;
    const reportChartDefs = [
      {key: "flow", title: "Flow", color: "#8b5cf6", unit: "L/min"},
      {
        key: "pressure",
        title: "Pressure",
        unit: "cmH2O",
        series: [
          {key: "inspiratory_pressure", label: "IPAP", color: "#22c55e"},
          {key: "expiratory_pressure", label: "EPAP", color: "#f97316"},
        ],
      },
      {key: "leak", title: "Leak", color: "#fb923c", unit: "L/min"},
      {
        key: "flow_limitation",
        title: "Flow Limit",
        color: "#ec4899",
        unit: "",
      },
      {
        key: "minute_ventilation",
        title: "Minute Vent",
        color: "#a78bfa",
        unit: "L/min",
      },
      {
        key: "mask_pressure",
        title: "Mask Pressure",
        color: "#22d3ee",
        unit: "cmH2O",
      },
      {
        key: "inspiratory_duration",
        title: "Insp. Duration",
        color: "#f59e0b",
        unit: "s",
      },
      {
        key: "respiratory_rate",
        title: "Resp. Rate",
        color: "#06b6d4",
        unit: "/min",
      },
      {key: "ie_ratio", title: "I:E", color: "#f43f5e", unit: ""},
    ];
    const reportEventDefs = [
      {code: 7, key: "CSR", label: "CSR", color: "#2563eb"},
      {code: 3, key: "CA", label: "Central Apnea", color: "#d946ef"},
      {code: 4, key: "OA", label: "Obstructive Apnea", color: "#22d3ee"},
      {code: 5, key: "UA", label: "Apnea", color: "#a78bfa"},
      {code: 2, key: "H", label: "Hypopnea", color: "#fb923c"},
      {code: 6, key: "Ar", label: "Arousal", color: "#cbd5e1"},
    ];
    // Maps the events metric chips to the count fields in the result JSON.
    const reportEventCountFields = [
      {key: "CA", field: "ca_count"},
      {key: "OA", field: "oa_count"},
      {key: "UA", field: "ua_count"},
      {key: "H", field: "hypopnea_count"},
      {key: "Ar", field: "arousal_count"},
    ];

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
