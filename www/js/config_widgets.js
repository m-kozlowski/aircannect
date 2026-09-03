(() => {
    const TZ_ABBR = {
      "Africa/Cairo": ["EET", null],
      "Africa/Lagos": ["WAT", null],
      "Africa/Nairobi": ["EAT", null],
      "America/Anchorage": ["AKST", "AKDT"],
      "America/Argentina/Buenos_Aires": ["ART", null],
      "America/Chicago": ["CST", "CDT"],
      "America/Denver": ["MST", "MDT"],
      "America/Los_Angeles": ["PST", "PDT"],
      "America/Mexico_City": ["CST", null],
      "America/New_York": ["EST", "EDT"],
      "America/Sao_Paulo": ["BRT", null],
      "America/Toronto": ["EST", "EDT"],
      "Asia/Bangkok": ["ICT", null],
      "Asia/Dhaka": ["BST", null],
      "Asia/Dubai": ["GST", null],
      "Asia/Hong_Kong": ["HKT", null],
      "Asia/Jerusalem": ["IST", "IDT"],
      "Asia/Karachi": ["PKT", null],
      "Asia/Kathmandu": ["NPT", null],
      "Asia/Kolkata": ["IST", null],
      "Asia/Seoul": ["KST", null],
      "Asia/Shanghai": ["CST", null],
      "Asia/Singapore": ["SGT", null],
      "Asia/Tokyo": ["JST", null],
      "Australia/Sydney": ["AEST", "AEDT"],
      "Europe/Berlin": ["CET", "CEST"],
      "Europe/Helsinki": ["EET", "EEST"],
      "Europe/Istanbul": ["TRT", null],
      "Europe/London": ["GMT", "BST"],
      "Europe/Moscow": ["MSK", null],
      "Europe/Paris": ["CET", "CEST"],
      "Europe/Warsaw": ["CET", "CEST"],
      "Pacific/Auckland": ["NZST", "NZDT"],
      "Pacific/Fiji": ["FJT", "FJST"],
      "Pacific/Honolulu": ["HST", null],
    };

    const TZ_ZONES = [
      "UTC",
      "Pacific/Honolulu",
      "America/Anchorage",
      "America/Los_Angeles",
      "America/Denver",
      "America/Chicago",
      "America/New_York",
      "America/Toronto",
      "America/Mexico_City",
      "America/Sao_Paulo",
      "America/Argentina/Buenos_Aires",
      "Europe/London",
      "Europe/Paris",
      "Europe/Berlin",
      "Europe/Warsaw",
      "Europe/Helsinki",
      "Europe/Moscow",
      "Europe/Istanbul",
      "Africa/Cairo",
      "Africa/Lagos",
      "Africa/Nairobi",
      "Asia/Dubai",
      "Asia/Karachi",
      "Asia/Kolkata",
      "Asia/Kathmandu",
      "Asia/Dhaka",
      "Asia/Bangkok",
      "Asia/Shanghai",
      "Asia/Hong_Kong",
      "Asia/Singapore",
      "Asia/Tokyo",
      "Asia/Seoul",
      "Asia/Jerusalem",
      "Australia/Sydney",
      "Pacific/Auckland",
      "Pacific/Fiji",
    ].sort();

    const TZ_FORMATTERS = {};

    function timezoneFormatter(timezone) {
      if (!TZ_FORMATTERS[timezone]) {
        TZ_FORMATTERS[timezone] = new Intl.DateTimeFormat("en-US", {
          timeZone: timezone,
          hourCycle: "h23",
          year: "numeric",
          month: "2-digit",
          day: "2-digit",
          hour: "2-digit",
          minute: "2-digit",
          second: "2-digit",
        });
      }
      return TZ_FORMATTERS[timezone];
    }

    function timezoneWallTime(date, timezone) {
      const parts = {};
      timezoneFormatter(timezone).formatToParts(date).forEach((part) => {
        if (part.type !== "literal") parts[part.type] = part.value;
      });
      return {
        year: Number(parts.year),
        month: Number(parts.month),
        day: Number(parts.day),
        hour: Number(parts.hour),
        minute: Number(parts.minute),
        second: Number(parts.second),
      };
    }

    function timezoneOffsetMinutes(date, timezone) {
      const wall = timezoneWallTime(date, timezone);
      const wallMs = Date.UTC(
        wall.year,
        wall.month - 1,
        wall.day,
        wall.hour,
        wall.minute,
        wall.second
      );
      const instantMs = Math.floor(date.getTime() / 1000) * 1000;
      return (wallMs - instantMs) / 60000;
    }

    function timezoneAbbr(date, timezone, daylight) {
      if (timezone === "UTC" || timezone === "Etc/UTC") return "UTC";

      const known = TZ_ABBR[timezone];
      if (known) {
        const abbr = daylight ? known[1] : known[0];
        if (abbr) return abbr;
      }

      const parts = new Intl.DateTimeFormat("en-US", {
        timeZone: timezone,
        timeZoneName: "short",
      }).formatToParts(date);
      const part = parts.find((entry) => entry.type === "timeZoneName");
      const value = part && part.value ? part.value.replace(/[<>]/g, "") : "";
      if (/^[A-Za-z]{3,}$/.test(value)) return value;
      return "<" + (value || (daylight ? "DST" : "STD")) + ">";
    }

    function formatPosixOffset(minutes) {
      const hours = Math.trunc(-minutes / 60);
      const mins = Math.abs(minutes % 60);
      return String(hours) + (mins ? ":" + AirCANnect.format.pad2(mins) : "");
    }

    function formatTimezoneTransition(timezone, reference, year) {
      const referenceOffset = timezoneOffsetMinutes(reference, timezone);
      let start = null;
      let end = null;

      for (let month = 0; month < 12; month++) {
        const first = new Date(year, month, 1);
        const next = new Date(year, month + 1, 1);
        if (timezoneOffsetMinutes(first, timezone) === referenceOffset &&
            timezoneOffsetMinutes(next, timezone) !== referenceOffset) {
          start = first.getTime();
          end = next.getTime();
          break;
        }
      }

      if (start === null) return "J365/25";

      while (end - start > 1000) {
        const mid = start + Math.floor((end - start) / 2);
        if (timezoneOffsetMinutes(new Date(mid), timezone) === referenceOffset) {
          start = mid;
        } else {
          end = mid;
        }
      }

      const wall = new Date(end + referenceOffset * 60000);
      const month = wall.getUTCMonth() + 1;
      const day = wall.getUTCDate();
      const weekday = wall.getUTCDay();
      const hour = wall.getUTCHours();
      const minute = wall.getUTCMinutes();
      const second = wall.getUTCSeconds();
      const daysInMonth = new Date(Date.UTC(wall.getUTCFullYear(), month, 0))
        .getUTCDate();
      const week = day + 7 > daysInMonth ? 5 : Math.ceil(day / 7);
      let rule = "M" + month + "." + week + "." + weekday;

      if (hour !== 2 || minute || second) {
        rule += "/" + hour;
        if (minute || second) {
          rule += ":" + AirCANnect.format.pad2(minute);
          if (second) rule += ":" + AirCANnect.format.pad2(second);
        }
      }

      return rule;
    }

    function getPosixTimezone(timezone) {
      if (timezone === "UTC" || timezone === "Etc/UTC") return "UTC0";

      const year = new Date().getFullYear();
      const jan = new Date(year, 0, 1);
      const jun = new Date(year, 5, 1);
      const janOffset = timezoneOffsetMinutes(jan, timezone);
      const junOffset = timezoneOffsetMinutes(jun, timezone);
      const standardOffset = Math.min(janOffset, junOffset);
      const daylightOffset = Math.max(janOffset, junOffset);
      const standardRef = standardOffset === janOffset ? jan : jun;
      const daylightRef = daylightOffset === janOffset ? jan : jun;

      let value = timezoneAbbr(standardRef, timezone, false) +
        formatPosixOffset(standardOffset);
      if (standardOffset !== daylightOffset) {
        value += timezoneAbbr(daylightRef, timezone, true);
        if (daylightOffset !== standardOffset + 60) {
          value += formatPosixOffset(daylightOffset);
        }
        value += "," +
          formatTimezoneTransition(timezone, standardRef, year) + "," +
          formatTimezoneTransition(timezone, daylightRef, year);
      }

      return value;
    }

    function setTimezoneFromZone(input, timezone) {
      const value = getPosixTimezone(timezone);
      input.value = value;
      input.title = timezone + " -> " + value;
    }

    function timezoneHelper(input) {
      const wrapper = document.createElement("div");
      wrapper.className = "config-helper";
      wrapper.appendChild(input);

      const detect = document.createElement("button");
      detect.type = "button";
      detect.className = "btn";
      detect.textContent = "Detect";
      detect.onclick = () => {
        try {
          const timezone = Intl.DateTimeFormat().resolvedOptions().timeZone ||
            "UTC";
          setTimezoneFromZone(input, timezone);
        } catch (error) {
          input.value = "UTC0";
          input.title = "Timezone detection failed";
        }
      };
      wrapper.appendChild(detect);

      const select = document.createElement("select");
      const placeholder = document.createElement("option");
      placeholder.value = "";
      placeholder.textContent = "Preset...";
      select.appendChild(placeholder);
      TZ_ZONES.forEach((timezone) => {
        const option = document.createElement("option");
        option.value = timezone;
        option.textContent = timezone;
        select.appendChild(option);
      });
      select.onchange = () => {
        if (select.value) setTimezoneFromZone(input, select.value);
      };
      wrapper.appendChild(select);

      return wrapper;
    }

    const WIFI_COUNTRY_PRESETS = [
      ["01", "Worldwide / default"],
      ["AU", "Australia"],
      ["BR", "Brazil"],
      ["CA", "Canada"],
      ["CN", "China"],
      ["DE", "Germany"],
      ["ES", "Spain"],
      ["FI", "Finland"],
      ["FR", "France"],
      ["GB", "United Kingdom"],
      ["IN", "India"],
      ["IT", "Italy"],
      ["JP", "Japan"],
      ["KR", "South Korea"],
      ["MX", "Mexico"],
      ["NL", "Netherlands"],
      ["NO", "Norway"],
      ["NZ", "New Zealand"],
      ["PL", "Poland"],
      ["SE", "Sweden"],
      ["US", "United States"],
    ];

    function wifiCountryHelper(input) {
      const wrapper = document.createElement("div");
      wrapper.className = "config-helper";
      wrapper.appendChild(input);
      input.maxLength = 2;
      input.autocapitalize = "characters";
      input.oninput = () => {
        input.value = input.value.toUpperCase();
      };

      const world = document.createElement("button");
      world.type = "button";
      world.className = "btn";
      world.textContent = "Default";
      world.onclick = () => {
        input.value = "01";
        input.title = "Worldwide / ESP-IDF default";
      };
      wrapper.appendChild(world);

      const select = document.createElement("select");
      const placeholder = document.createElement("option");
      placeholder.value = "";
      placeholder.textContent = "Preset...";
      select.appendChild(placeholder);
      WIFI_COUNTRY_PRESETS.forEach((entry) => {
        const option = document.createElement("option");
        option.value = entry[0];
        option.textContent = entry[0] + " - " + entry[1];
        select.appendChild(option);
      });
      select.onchange = () => {
        if (!select.value) return;
        input.value = select.value;
        const selected = WIFI_COUNTRY_PRESETS.find((entry) =>
          entry[0] === select.value);
        input.title = selected ? selected[1] : "";
      };
      wrapper.appendChild(select);

      return wrapper;
    }

    AirCANnect.forms.register("tz", {
      decorate: timezoneHelper,
      setDefault: (input) => {
        try {
          const timezone = Intl.DateTimeFormat().resolvedOptions().timeZone ||
            "UTC";
          setTimezoneFromZone(input, timezone);
        } catch (error) {
          input.value = "UTC0";
        }
      },
    });
    AirCANnect.forms.register("wifi_ctry", {
      decorate: wifiCountryHelper,
    });
})();
