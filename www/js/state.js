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
