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
        out.println("  status            concise current operating state");
        out.println("  stats             CAN, RPC, event, and stream counters");
        out.println("  memory            heap and PSRAM status");
        out.println("  version           AirCANnect firmware version");
        out.println("  restart           restart AirCANnect");
#if AC_CAN_ENABLED
        out.println("  can               CAN controller recovery helpers");
#endif
        out.println("  config            persistent app configuration");
        out.println("  wifi              Wi-Fi profiles, scan, and reconnect");
        out.println("  log               log levels and sink status");
        out.println("  as11              AS11 state, RPC, BLE, and therapy control");
        out.println("  time              ESP and AS11 clock sync commands");
        out.println("  oxi               oximetry source and BLE injector status");
        out.println("  report            therapy report preparation and catalog");
        out.println("  storage           persistent storage and file operations");
        out.println("  sync              SMB and SleepHQ export operations");
        out.println("  ota               AirCANnect firmware OTA status");
        out.println("  resmed-ota        AS11 firmware upload/apply workflow");
        return;
    }

    if (topic == "status") {
        out.println("[HELP status]");
        out.println("  status                    show concise operating state");
        out.println("  status -v                 show detailed subsystem state");
        return;
    }

    if (topic == "config") {
        out.println("[HELP config]");
        out.println("  config                    show persistent config with exact NVS keys");
        out.println("  config KEY                show one exact NVS key");
        out.println("  config KEY VALUE          set one exact NVS key");
        out.println("  config reset              reset app config, keep Wi-Fi profiles");
        out.println("  config factory-reset      reset app config and Wi-Fi profiles");
        out.println("  config keybindings        show button action bindings");
        out.println("  config keybindings reset  restore hardware defaults");
        out.println("  config keybindings BUTTON[+BUTTON] short|long ACTION|default");
        out.println("  keys are not aliased or normalized: use the NVS key exactly");
        out.println("  common keys:");
        out.println("    host tcp_en tcp_port softap_mode wifi_ctry tz resmed_time");
        out.println("    oxi_en oxi_udp oxi_adv edf_cap");
        out.println("    smb_ep smb_user smb_pass");
        out.println("    shq_id shq_secret shq_team shq_device");
        out.println("    http_user http_pass auth_wl telnet_en telnet_port ota_pass");
        out.println("    syslog_en syslog_host syslog_port file_log_en");
        out.println("    log_general log_can log_ble log_rpc log_tcp log_cli");
        out.println("    log_wifi log_stream log_ota log_oxi log_storage");
        out.println("    log_export log_report log_edf log_config");
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
        out.println("  log tail [LINES]          print current file log tail");
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
        out.println("  as11 get NAME [...]       request named fields");
        out.println("  as11 set NAME VALUE [...] write named fields");
        out.println("  as11 rpc METHOD [PARAMS]  call an AS11 RPC method");
        out.println("  as11 raw JSON             send a raw JSON-RPC payload");
        out.println("  as11 therapy start        queue EnterTherapy");
        out.println("  as11 therapy stop         queue EnterStandby");
        out.println("  as11 restart [fast|power|watchdog]");
        out.println("                            reset the AS11");
        out.println("  as11 ble status           show BLE link and pairing state");
        out.println("  as11 ble connect          connect or retry now");
        out.println("  as11 ble disconnect       quiesce and disconnect");
        out.println("  as11 ble pair             scan for an AS11 to pair");
        out.println("  as11 ble select ADDRESS   select a scanned AS11");
        out.println("  as11 ble passkey CODE     finish pairing with its code");
        out.println("  as11 ble cancel           cancel pairing");
        out.println("  as11 ble forget           remove paired credentials");
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

    if (topic == "storage") {
        out.println("[HELP storage]");
        out.println("  storage status            show mounted storage state");
        out.println("  storage mount             mount storage if unavailable");
        out.println("  storage pwd               show the current directory");
        out.println("  storage ls [PATH]         list a directory");
        out.println("  storage cd PATH           change the current directory");
        out.println("  storage rm PATH           remove a file or directory");
        out.println("  storage rename PATH NAME  rename within its directory");
        return;
    }

    if (topic == "sync") {
        out.println("[HELP sync]");
        out.println("  sync                      show SMB and SleepHQ status");
        out.println("  sync smb [status]         show SMB sync status");
        out.println("  sync smb verify           verify the SMB destination");
        out.println("  sync smb run              queue an SMB sync");
        out.println("  sync sleephq [status]     show SleepHQ sync status");
        out.println("  sync sleephq check        check SleepHQ connectivity");
        out.println("  sync sleephq run          queue a SleepHQ sync");
        out.println("  sync sleephq run YYYYMMDD sync one DATALOG day");
        return;
    }

    if (topic == "report") {
        out.println("[HELP report]");
        out.println("  report                    show report task status");
        out.println("  report status             show report task status");
        out.println("  report nights [latest|YYYYMMDD]  list report nights or one night");
        out.println("  report result latest [--force]   request latest result artifact");
        out.println("  report result YYYYMMDD [--force] request result by sleep day");
        return;
    }

    if (topic == "edf") {
        out.println("[HELP edf]");
        out.println("  edf str refresh YYYYMMDD [YYYYMMDD]");
        out.println("                            refresh Summary fields in existing STR records");
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
        out.println("  ota check                 check release manifest for an update");
        out.println("  ota install               install the available release");
        out.println("  ota url URL               install ESP32 image from URL");
        out.println("  ota abort                 abort current ESP32 OTA flow");
        return;
    }

    if (topic == "resmed-ota") {
        out.println("[HELP resmed-ota]");
        out.println("  resmed-ota status         show AS11 firmware install state");
        out.println("  resmed-ota check          queue CheckUpgradeFile");
        out.println("  resmed-ota abort          abort current AS11 OTA flow");
        out.println("  resmed-ota dump           dump current FGCB to repository");
        out.println("  resmed-ota dump confirm INSTALL_PATCHED_BOOTLOADER");
        out.println("                            install matching FGBL and retry dump");
        out.println("  resmed-ota install [rpc|service] [TARGET] PATH");
        out.println("                            verify and install an image");
        out.println("  resmed-ota repository     list repository images");
        out.println("  resmed-ota repository refresh  rebuild repository list");
        out.println("  resmed-ota repository remove PATH  remove one image");
        out.println("  resmed-ota repository install [rpc|service] [TARGET] PATH");
        out.println("                            install one repository image");
        out.println("  TARGET: APCX (default), APPL, CONF, FGBL, or FGCB");
        out.println("  resmed-ota apply plain CONFIRM         queue ApplyUpgrade");
        out.println("  resmed-ota apply authenticated TAG CONFIRM  queue ApplyAuthUpgrade");
        return;
    }

    if (topic == "stats") {
        out.println("[HELP stats]");
        out.println("  stats                     show CAN/RPC/event/stream counters");
        out.println("  stats edf                 show detailed EDF capture counters");
        out.println("  stats report              show report queue/cache counters");
        out.println("  stats reset               clear general counters");
        out.println("  use memory, storage, stats edf, oxi, or log for details");
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
        out.println("  restart                   restart AirCANnect");
        out.println("  restart ac                restart AirCANnect");
        return;
    }

    if (topic == "can") {
        out.println("[HELP can]");
#if AC_CAN_ENABLED
        out.println("  can                       show CAN controller state");
        out.println("  can status                show CAN controller state");
        out.println("  can restart               restart CAN and clear pending RPC work");
#else
        out.println("  CAN is unavailable in this build");
#endif
        return;
    }

    out.print("[HELP] no topic named ");
    out.println(topic);
}

}  // namespace aircannect
