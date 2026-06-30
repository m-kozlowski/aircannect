#include "management_console.h"

#include "string_util.h"

namespace aircannect {

void ManagementConsole::print_help(Print &out, const String &topic_arg) {
    String topic = topic_arg;
    trim_inplace(topic);
    to_lower_inplace(topic);

    if (!topic.length()) {
        out.println("[HELP] AirCANnect management console");
        out.println("  help COMMAND      show detailed help for a command");
        out.println("  status            current CAN, AS11, session, and sink state");
        out.println("  stats             counters for CAN, RPC, network, logs, storage");
        out.println("  memory            heap and PSRAM status");
        out.println("  version           AirCANnect firmware version");
        out.println("  restart           restart the ESP");
        out.println("  can               CAN controller recovery helpers");
        out.println("  config            persistent app configuration");
        out.println("  wifi              Wi-Fi profiles, scan, and reconnect");
        out.println("  log               log levels and sink status");
        out.println("  as11              AS11 state, polling, and device version");
        out.println("  therapy           start/stop therapy commands");
        out.println("  time              ESP and AS11 clock sync commands");
        out.println("  stream            AS11 stream subscription controls");
        out.println("  sink              stream sink status");
        out.println("  edf               live EDF recorder status and monitor toggle");
        out.println("  oxi               oximetry source and BLE injector status");
        out.println("  report            therapy report index/cache status");
        out.println("  storage           persistent storage and writer status");
        out.println("  smb               SMB sync status and share verification");
        out.println("  sleephq           SleepHQ sync status and connectivity check");
        out.println("  session           therapy session tracking state");
        out.println("  tcp               raw JSON-RPC TCP bridge status");
        out.println("  ota               AirCANnect firmware OTA status");
        out.println("  resmed-ota        AS11 firmware upload/apply workflow");
        out.println("  get               AS11 Get helper for named fields");
        out.println("  set               AS11 Set helper for named fields");
        out.println("  rpc               AS11 method call helper");
        out.println("  raw               send raw JSON-RPC payload");
        out.println("[HELP] TCP bridge: one JSON-RPC payload per line.");
        return;
    }

    if (topic == "config") {
        out.println("[HELP config]");
        out.println("  config                    show persistent config with exact NVS keys");
        out.println("  config KEY                show one exact NVS key");
        out.println("  config KEY VALUE          set one exact NVS key");
        out.println("  config reset              reset app config, keep Wi-Fi profiles");
        out.println("  config factory-reset      reset app config and Wi-Fi profiles");
        out.println("  keys are not aliased or normalized: use the NVS key exactly");
        out.println("  common keys:");
        out.println("    host tcp_en tcp_port softap_mode wifi_ctry tz resmed_time");
        out.println("    oxi_en oxi_udp oxi_adv edf_cap");
        out.println("    smb_ep smb_user smb_pass");
        out.println("    shq_id shq_secret shq_team shq_device");
        out.println("    http_user http_pass auth_wl telnet_en telnet_port ota_pass");
        out.println("    syslog_en syslog_host syslog_port file_log_en");
        out.println("    log_general log_can log_rpc log_tcp log_cli log_wifi");
        out.println("    log_stream log_ota log_oxi log_storage log_bgworker");
        out.println("    log_report log_edf log_config log_sleephq");
        return;
    }

    if (topic == "wifi") {
        out.println("[HELP wifi]");
        out.println("  wifi status               show current Wi-Fi mode and IP");
        out.println("  wifi list                 show stored STA profiles");
        out.println("  wifi scan                 start/show async nearby-network scan");
        out.println("  wifi set SSID PASSWORD    replace profiles with one secured STA");
        out.println("  wifi add SSID PASSWORD    add secured STA profile");
        out.println("  wifi open SSID            replace profiles with one open STA");
        out.println("  wifi remove INDEX         remove stored profile");
        out.println("  wifi clear                clear all STA profiles");
        out.println("  wifi restart              reconnect Wi-Fi");
        out.println("  wifi set \"SSID\" \"PASS\"  quote values when they contain spaces");
        return;
    }

    if (topic == "log") {
        out.println("[HELP log]");
        out.println("  log                       show log levels and sink status");
        out.println("  log level LEVEL           set all categories");
        out.println("  log level CATEGORY LEVEL  set one category");
        out.println("  log syslog off            disable syslog");
        out.println("  log syslog HOST [PORT]    send logs to syslog host");
        out.println("  log test [MESSAGE]        emit a test log line");
        out.println("  log level rpc debug       show RPC request/response payloads");
        out.println("  log level oxi debug       show oximetry BLE/protocol details");
        return;
    }

    if (topic == "as11") {
        out.println("[HELP as11]");
        out.println("  as11 status               show cached AS11 state");
        out.println("  as11 poll                 queue status and clock refresh");
        out.println("  as11 version              request AS11 GetVersion");
        return;
    }

    if (topic == "therapy") {
        out.println("[HELP therapy]");
        out.println("  therapy status            show cached therapy state");
        out.println("  therapy start             queue EnterTherapy");
        out.println("  therapy stop              queue EnterStandby");
        return;
    }

    if (topic == "time") {
        out.println("[HELP time]");
        out.println("  time                      show ESP clock and sync state");
        out.println("  time get                  request AS11 GetDateTime");
        out.println("  time push                 push ESP UTC time to AS11");
        out.println("  time pull                 pull AS11 UTC time into ESP");
        out.println("  time ntp                  trigger NTP resync");
        return;
    }

    if (topic == "stream") {
        out.println("[HELP stream]");
        out.println("  stream                    show stream broker state");
        out.println("  stream status             show stream broker state");
        out.println("  stream edf|full|default   subscribe EDF-oriented stream set");
        out.println("  stream stop               release console stream consumer");
        out.println("  stream IDS [SAMPLE] [REP] subscribe custom IDs and intervals");
        out.println("  stream {JSON_PARAMS}      subscribe with raw StartStream params");
        return;
    }

    if (topic == "storage") {
        out.println("[HELP storage]");
        out.println("  storage status            show mounted storage and writer state");
        out.println("  storage remount           retry storage mount");
        out.println("  storage queue             show async writer queue state");
        out.println("  storage write-test P T    append test text through writer");
        return;
    }

    if (topic == "smb") {
        out.println("[HELP smb]");
        out.println("  smb status                show SMB sync status");
        out.println("  smb verify                queue SMB share/recent-files verification");
        out.println("  smb sync                  queue manual SMB export sync");
        return;
    }

    if (topic == "sleephq") {
        out.println("[HELP sleephq]");
        out.println("  sleephq status            show SleepHQ sync status");
        out.println("  sleephq check             queue OAuth/API connectivity check");
        out.println("  sleephq sync              queue manual SleepHQ export sync");
        out.println("  sleephq sync YYYYMMDD     sync one DATALOG day");
        return;
    }

    if (topic == "report") {
        out.println("[HELP report]");
        out.println("  report                    show report store status");
        out.println("  report status             show report store status");
        out.println("  report store              show durable report store status");
        out.println("  report store check        validate report store indexes");
        out.println("  report store repair       repair report store indexes/cache metadata");
        out.println("  report nights             list indexed therapy nights");
        out.println("  report coverage latest    show latest night cache coverage");
        out.println("  report coverage INDEX     show coverage for report nights index");
        out.println("  report coverage ms VALUE  show coverage for raw start ms");
        out.println("  report cache latest       fetch missing cached source data");
        out.println("  report cache INDEX        fetch cache for report nights index");
        out.println("  report cache force INDEX  refetch supported cached sources");
        out.println("  report cache cancel       cancel active cache fetch");
        out.println("  report cache clear all    clear all report cache");
        out.println("  report cache clear oldest N clear oldest cached report nights");
        out.println("  report cache clear latest clear latest night cache");
        out.println("  report cache clear INDEX  clear cache for report night");
        out.println("  report cache clear ms VAL clear cache for raw start ms");
        out.println("  report cache prune [KEEP] keep only latest cached report nights");
        out.println("  report result latest      prepare latest report manifest");
        out.println("  report result INDEX       prepare report manifest by index");
        out.println("  report prefetch           show background prefetch status");
        out.println("  report prefetch on|off    enable/disable idle prefetch");
        return;
    }

    if (topic == "sink") {
        out.println("[HELP sink]");
        out.println("  sink status               show stream sink state");
        return;
    }

    if (topic == "edf") {
        out.println("[HELP edf]");
        out.println("  edf                       show live EDF recorder state");
        out.println("  edf status                show live EDF recorder state");
        out.println("  edf on|off                enable/disable recorder monitor");
        return;
    }

    if (topic == "oxi" || topic == "oximetry") {
        out.println("[HELP oxi]");
        out.println("  oxi status                show oximetry source and BLE state");
        out.println("  oxi on|off                enable/disable oximetry bridge");
        out.println("  oxi cpap pair             advertise temporarily for CPAP pairing");
        out.println("  oxi cpap pair stop        stop the CPAP pairing window");
        out.println("  oxi cpap forget           clear CPAP-side BLE bonds");
        out.println("  oxi sensor status         show BLE oximeter source status");
        out.println("  oxi sensor scan           scan for BLE oximeters");
        out.println("  oxi sensor results        list last scan results");
        out.println("  oxi sensor connect ID     connect scan result index/address");
        out.println("  oxi sensor disconnect     disconnect current BLE oximeter");
        out.println("  oxi sensor list           list known BLE oximeters");
        out.println("  oxi sensor forget ADDR|all remove known BLE oximeter");
        out.println("  oxi sensor autoconnect ADDR on|off");
        out.println("  oxi advertise auto|manual set source-driven or on-demand advertising");
        out.println("  oxi advertise start|stop  request/stop manual advertising");
        return;
    }

    if (topic == "ota") {
        out.println("[HELP ota]");
        out.println("  ota status                show ArduinoOTA and HTTP OTA state");
        return;
    }

    if (topic == "resmed-ota") {
        out.println("[HELP resmed-ota]");
        out.println("  resmed-ota status         show upload/apply state");
        out.println("  resmed-ota check          queue CheckUpgradeFile");
        out.println("  resmed-ota abort          abort current AS11 OTA flow");
        out.println("  resmed-ota apply plain CONFIRM         queue ApplyUpgrade");
        out.println("  resmed-ota apply authenticated TAG CONFIRM  queue ApplyAuthUpgrade");
        return;
    }

    if (topic == "rpc" || topic == "raw" || topic == "get" ||
        topic == "set") {
        out.println("[HELP rpc]");
        out.println("  get NAME [NAME...]        queue AS11 Get for named fields");
        out.println("  set NAME VALUE [...]      queue AS11 Set for fields");
        out.println("  set {JSON_PARAMS}         queue AS11 Set with raw params");
        out.println("  rpc METHOD [JSON_PARAMS]  queue AS11 method call");
        out.println("  raw JSON                  send raw JSON-RPC payload");
        return;
    }

    if (topic == "status") {
        out.println("[HELP status]");
        out.println("  status                    show CAN, AS11, session, and sink state");
        return;
    }

    if (topic == "stats") {
        out.println("[HELP stats]");
        out.println("  stats                     show runtime counters");
        out.println("  stats reset               clear runtime counters");
        return;
    }

    if (topic == "memory") {
        out.println("[HELP memory]");
        out.println("  memory                    show heap and PSRAM usage");
        out.println("  memory detail             show heap regions and owned buffers");
        return;
    }

    if (topic == "version") {
        out.println("[HELP version]");
        out.println("  version                   show firmware version and build date");
        return;
    }

    if (topic == "restart") {
        out.println("[HELP restart]");
        out.println("  restart                   schedule an ESP restart");
        return;
    }

    if (topic == "can") {
        out.println("[HELP can]");
        out.println("  can                       show CAN controller state");
        out.println("  can status                show CAN controller state");
        out.println("  can restart               restart CAN and clear pending RPC work");
        return;
    }

    if (topic == "system") {
        out.println("[HELP system]");
        out.println("  status                    current high-level runtime state");
        out.println("  stats                     runtime counters");
        out.println("  stats reset               clear counters");
        out.println("  memory                    heap and PSRAM status");
        out.println("  version                   firmware version and build date");
        out.println("  restart                   restart the ESP");
        out.println("  can restart               restart CAN controller");
        return;
    }

    if (topic == "tcp") {
        out.println("[HELP tcp]");
        out.println("  tcp                       show raw JSON-RPC bridge status");
        out.println("  tcp status                show raw JSON-RPC bridge status");
        return;
    }

    if (topic == "session") {
        out.println("[HELP session]");
        out.println("  session status            show therapy session tracker state");
        return;
    }

    out.print("[HELP] no topic named ");
    out.println(topic);
}

}  // namespace aircannect
