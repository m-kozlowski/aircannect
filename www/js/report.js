(() => {
    function fmtMinutes(minutes) {
      const value = Number(minutes);
      if (!Number.isFinite(value) || value <= 0) return "--";
      const hours = Math.floor(value / 60);
      const mins = value - hours * 60;
      if (!hours) return mins + "m";
      return hours + "h" + (mins ? " " + mins + "m" : "");
    }

    function fmtReportDate(ms) {
      const date = new Date(Number(ms));
      if (Number.isNaN(date.getTime())) return "--";
      return date.getFullYear() + "-" +
        AirCANnect.format.pad2(date.getMonth() + 1) + "-" +
        AirCANnect.format.pad2(date.getDate());
    }

    function reportNightsNewestFirst() {
      const nights = reportSummary && Array.isArray(reportSummary.nights) ?
        reportSummary.nights.slice() : [];
      nights.sort((a, b) => Number(b.start || 0) - Number(a.start || 0));
      return nights;
    }

    function selectedReportNight() {
      const nights = reportNightsNewestFirst();
      return nights.find((night) => String(night.id) === reportSelectedNightId) ||
        nights[0] || null;
    }

    function startOfMonth(ms) {
      const date = new Date(Number(ms));
      return new Date(date.getFullYear(), date.getMonth(), 1);
    }

    function pickReportNight(id) {
      reportSelectedNightId = String(id);
      closeReportCalendar();
      loadSelectedReportNight();
    }

    function stepReportNight(delta) {
      const nights = reportNightsNewestFirst();
      if (!nights.length) return;
      const current = selectedReportNight();
      let index = nights.findIndex((night) => night === current);
      if (index < 0) index = 0;
      const next = index + delta;
      if (next < 0 || next >= nights.length) return;
      pickReportNight(nights[next].id);
    }

    function selectLatestReportNight() {
      const nights = reportNightsNewestFirst();
      if (nights.length) pickReportNight(nights[0].id);
    }

    function loadReportChartPreferences() {
      const known = reportChartDefs.map((definition) => definition.key);
      const knownSet = new Set(known);
      let stored = null;
      try {
        stored = JSON.parse(localStorage.getItem(
          REPORT_CHART_PREFERENCES_KEY) || "null");
      } catch (error) {
        stored = null;
      }

      const order = [];
      const seen = new Set();
      if (stored && Array.isArray(stored.order)) {
        stored.order.forEach((key) => {
          if (!knownSet.has(key) || seen.has(key)) return;
          seen.add(key);
          order.push(key);
        });
      }
      known.forEach((key) => {
        if (!seen.has(key)) order.push(key);
      });

      const collapsed = new Set();
      if (stored && Array.isArray(stored.collapsed)) {
        stored.collapsed.forEach((key) => {
          if (knownSet.has(key)) collapsed.add(key);
        });
      }
      reportChartPreferences = {order, collapsed};
    }

    function saveReportChartPreferences() {
      try {
        localStorage.setItem(REPORT_CHART_PREFERENCES_KEY, JSON.stringify({
          order: reportChartPreferences.order,
          collapsed: Array.from(reportChartPreferences.collapsed),
        }));
      } catch (error) {
        // Browser storage can be disabled; preferences remain valid in memory.
      }
    }

    function visibleReportChartOrder() {
      const definitions = new Map(reportChartDefs.map((definition) =>
        [definition.key, definition]));
      return reportChartPreferences.order.filter((key) => {
        const definition = definitions.get(key);
        if (!definition) return false;
        if (definition.type === "events") {
          return !!((reportResult && reportResult.events_available) ||
            reportBaseEvents.length);
        }
        if (!definition.optional) return true;

        const series = definition.series || [definition];
        return series.some((item) =>
          (reportSeries[item.key] || []).length > 0 ||
          (reportBaseSeries[item.key] || []).length > 0 ||
          reportSignalTracks(item.key).length > 0);
      });
    }

    function moveReportChart(key, delta) {
      const visible = visibleReportChartOrder();
      const visibleIndex = visible.indexOf(key);
      const targetIndex = visibleIndex + delta;
      if (visibleIndex < 0 || targetIndex < 0 ||
          targetIndex >= visible.length) return;

      const order = reportChartPreferences.order;
      const index = order.indexOf(key);
      const target = order.indexOf(visible[targetIndex]);
      [order[index], order[target]] = [order[target], order[index]];
      saveReportChartPreferences();
      renderReportCharts();
    }

    function toggleReportChartCollapsed(key) {
      if (reportChartPreferences.collapsed.has(key)) {
        reportChartPreferences.collapsed.delete(key);
        ensureSignalStoreChartLoaded(key);
      } else {
        reportChartPreferences.collapsed.add(key);
      }
      saveReportChartPreferences();
      renderReportCharts();
    }

    function appendReportChartActions(title, key) {
      const actions = document.createElement("span");
      actions.className = "report-chart-actions";
      const visible = visibleReportChartOrder();
      const index = visible.indexOf(key);

      const up = document.createElement("button");
      up.className = "btn report-chart-action";
      up.type = "button";
      up.title = "Move chart up";
      up.textContent = "\u2191";
      up.disabled = index <= 0;
      up.onclick = () => moveReportChart(key, -1);
      actions.appendChild(up);

      const down = document.createElement("button");
      down.className = "btn report-chart-action";
      down.type = "button";
      down.title = "Move chart down";
      down.textContent = "\u2193";
      down.disabled = index < 0 ||
        index >= visible.length - 1;
      down.onclick = () => moveReportChart(key, 1);
      actions.appendChild(down);

      const collapsed = reportChartPreferences.collapsed.has(key);
      const toggle = document.createElement("button");
      toggle.className = "btn report-chart-action";
      toggle.type = "button";
      toggle.title = collapsed ? "Expand chart" : "Collapse chart";
      toggle.textContent = collapsed ? "\u25b8" : "\u25be";
      toggle.onclick = () => toggleReportChartCollapsed(key);
      actions.appendChild(toggle);

      title.appendChild(actions);
    }

    function toggleReportCalendar() {
      const pop = document.getElementById("reportCalPop");
      if (!pop) return;
      if (pop.classList.contains("open")) {
        closeReportCalendar();
        return;
      }
      const selected = selectedReportNight();
      reportCalView = startOfMonth(selected ? selected.start : Date.now());
      renderReportCalendar();
      positionReportCalendar();
      pop.classList.add("open");
    }

    function positionReportCalendar() {
      const pop = document.getElementById("reportCalPop");
      const button = document.getElementById("reportDateBtn");
      if (!pop || !button) return;
      const rect = button.getBoundingClientRect();
      const maxLeft = window.innerWidth - 262 - 8;
      let left = rect.left;
      if (left > maxLeft) left = maxLeft;
      if (left < 8) left = 8;
      pop.style.left = left + "px";
      pop.style.top = (rect.bottom + 6) + "px";
    }

    function closeReportCalendar() {
      const pop = document.getElementById("reportCalPop");
      if (pop) pop.classList.remove("open");
    }

    function stepReportCalMonth(delta) {
      if (!reportCalView) reportCalView = startOfMonth(Date.now());
      reportCalView = new Date(reportCalView.getFullYear(),
                              reportCalView.getMonth() + delta, 1);
      renderReportCalendar();
    }

    function renderReportCalendar() {
      const label = document.getElementById("reportCalMonth");
      const grid = document.getElementById("reportCalGrid");
      if (!label || !grid || !reportCalView) return;
      const months = ["January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"];
      const year = reportCalView.getFullYear();
      const month = reportCalView.getMonth();
      label.textContent = months[month] + " " + year;

      const byDay = {};
      reportNightsNewestFirst().forEach((night) => {
        const date = new Date(Number(night.start));
        if (date.getFullYear() === year && date.getMonth() === month) {
          byDay[date.getDate()] = night;
        }
      });
      const selected = selectedReportNight();
      const selId = selected ? String(selected.id) : "";

      grid.textContent = "";
      ["S", "M", "T", "W", "T", "F", "S"].forEach((name) => {
        const cell = document.createElement("div");
        cell.className = "np-dow";
        cell.textContent = name;
        grid.appendChild(cell);
      });
      const lead = new Date(year, month, 1).getDay();
      for (let blank = 0; blank < lead; blank++) {
        const cell = document.createElement("div");
        cell.className = "np-day empty";
        grid.appendChild(cell);
      }
      const days = new Date(year, month + 1, 0).getDate();
      for (let day = 1; day <= days; day++) {
        const cell = document.createElement("div");
        cell.textContent = String(day);
        const night = byDay[day];
        if (night) {
          cell.className = "np-day has" +
            (String(night.id) === selId ? " sel" : "");
          cell.title = fmtMinutes(night.duration_min);
          cell.onclick = () => pickReportNight(night.id);
        } else {
          cell.className = "np-day";
        }
        grid.appendChild(cell);
      }
    }

    document.addEventListener("click", (event) => {
      const pop = document.getElementById("reportCalPop");
      if (!pop || !pop.classList.contains("open")) return;
      if (!event.target.closest || !event.target.closest(".night-picker")) {
        closeReportCalendar();
      }
    });

    // The calendar is position:fixed anchored to the date button; a scroll or
    // resize would leave it detached, so close it on either (capture catches the
    // scroll-container's scroll, which does not bubble).
    function closeReportCalendarOnViewportChange() {
      const pop = document.getElementById("reportCalPop");
      if (pop && pop.classList.contains("open")) closeReportCalendar();
    }
    window.addEventListener("scroll", closeReportCalendarOnViewportChange, true);
    window.addEventListener("resize", closeReportCalendarOnViewportChange);

    function reportNightLoadKey(night) {
      return night && night.id ? String(night.id) : "";
    }

    function fmtReportTime(ms) {
      const date = new Date(Number(ms));
      if (Number.isNaN(date.getTime())) return "--";
      return AirCANnect.format.pad2(date.getHours()) + ":" + AirCANnect.format.pad2(date.getMinutes());
    }

    function renderReportSessions(sessions) {
      const container = document.getElementById("reportSessions");
      if (!container) return;
      container.textContent = "";
      if (!Array.isArray(sessions) || !sessions.length) return;
      const toggleable = sessions.length > 1;
      sessions.forEach((session) => {
        const item = document.createElement("span");
        item.className = "report-session";
        const start = Number(session.start || 0);
        const minutes = Number(session.duration_min || 0);
        const explicitEnd = Number(session.end || 0);
        const end = explicitEnd > start ? explicitEnd : start + minutes * 60000;
        item.textContent = fmtReportTime(start) + "-" + fmtReportTime(end) +
          " " + fmtMinutes(minutes);
        if (toggleable) {
          item.classList.add("toggle");
          if (reportHiddenSessions.has(start)) item.classList.add("hidden");
          item.onclick = () => {
            if (reportHiddenSessions.has(start)) {
              reportHiddenSessions.delete(start);
            } else {
              reportHiddenSessions.add(start);
            }
            renderReportSessions(sessions);
            renderReportCharts();
          };
        }
        container.appendChild(item);
      });
    }

    function renderReportEventCounts(result) {
      const container = document.getElementById("reportEventCounts");
      if (!container) return;
      container.textContent = "";
      // Event source not covered for this night: counts are unknown, not zero.
      const unavailable = !!result && result.events_available === false;
      reportEventCountFields.forEach((item) => {
        const count = result ? Number(result[item.field] || 0) : 0;
        const chip = document.createElement("span");
        chip.className = "evt";
        chip.appendChild(document.createTextNode(item.key));
        const value = document.createElement("b");
        value.textContent = unavailable ? "--" : String(count);
        chip.appendChild(value);
        container.appendChild(chip);
      });
      if (unavailable) {
        const note = document.createElement("span");
        note.className = "report-res-badge";
        note.textContent = "events not captured";
        container.appendChild(note);
      }
    }

    function resetReportData() {
      cancelReportRangeRequest();
      disconnectReportResizeObserver();
      reportHiddenSessions.clear();
      reportZoom = null;
      reportHoverTime = null;
      reportDrag = null;
      reportResult = null;
      reportSeries = {};
      reportEvents = [];
      reportBaseSeries = {};
      reportBaseEvents = [];
      reportSignalStore = null;
      reportBaseLoadedCharts.clear();
      reportBaseChartPromises.clear();
      reportCurrentNightId = "";
      reportCurrentRevision = "";
      reportCurrentGeneration = 0;
      reportRangeView = null;
      reportRangeActiveKey = "";
      reportRangeToken++;
      reportDrawItems = [];
      reportDrawPending = false;
      reportDrawRetryCount = 0;
      const charts = document.getElementById("reportCharts");
      if (charts) charts.textContent = "";
    }

    function lruGet(cache, key) {
      if (!cache.has(key)) return null;
      const value = cache.get(key);
      cache.delete(key);
      cache.set(key, value);
      return value;
    }

    function lruSet(cache, key, value, maxEntries) {
      if (cache.has(key)) cache.delete(key);
      cache.set(key, value);
      while (cache.size > maxEntries) {
        const oldest = cache.keys().next().value;
        cache.delete(oldest);
      }
    }

    function activateSignalStoreNight(nightId, revision, generation, store) {
      cancelReportRangeRequest();

      const nightPrefix = String(nightId) + ":";
      const generationPrefix = nightPrefix + generation + ":";
      reportSignalBlockCache.forEach((entry, key) => {
        if (key.startsWith(nightPrefix) &&
            (!key.startsWith(generationPrefix) ||
             (!entry.closed && entry.revision !== String(revision)))) {
          reportSignalBlockCache.delete(key);
        }
      });

      reportSeries = {};
      reportEvents = [];
      reportBaseSeries = {};
      reportBaseEvents = [];
      reportSignalStore = store || null;
      reportBaseLoadedCharts.clear();
      reportBaseChartPromises.clear();
      reportCurrentNightId = String(nightId || "");
      reportCurrentRevision = String(revision || "");
      reportCurrentGeneration = Number(generation) || 0;
      reportRangeView = null;
      reportRangeActiveKey = "";
      reportRangeToken++;
    }

    function reportRange() {
      const ranges = reportVisibleSessionRanges();
      let start = Infinity;
      let end = -Infinity;
      ranges.forEach((range) => {
        start = Math.min(start, range.start);
        end = Math.max(end, range.end);
      });
      if (!Number.isFinite(start) || !Number.isFinite(end) || end <= start) {
        start = Number(reportResult && reportResult.start || 0);
        end = Number(reportResult && reportResult.end || 0);
      }
      return {start, end};
    }

    function reportSessionRanges() {
      const sessions = reportResult && Array.isArray(reportResult.sessions) ?
        reportResult.sessions : [];
      return sessions.map((session) => {
        const start = Number(session.start || 0);
        const minutes = Number(session.duration_min || 0);
        const explicitEnd = Number(session.end || 0);
        return {
          start,
          end: explicitEnd > start ? explicitEnd : start + minutes * 60000,
        };
      }).filter((range) => range.start > 0 && range.end > range.start)
        .sort((a, b) => a.start - b.start);
    }

    // Session ranges minus any toggled-off sessions. If every session is
    // hidden, fall back to all of them so the view is never stranded blank.
    function reportVisibleSessionRanges() {
      const all = reportSessionRanges();
      if (!reportHiddenSessions.size) return all;
      const visible = all.filter(
        (range) => !reportHiddenSessions.has(range.start));
      return visible.length ? visible : all;
    }

    function reportPointRangeIndex(t, ranges) {
      if (!Array.isArray(ranges) || !ranges.length) return 0;
      for (let i = 0; i < ranges.length; i++) {
        if (t >= ranges[i].start && t <= ranges[i].end) return i;
      }
      return -1;
    }

    function reportPointMin(point) {
      if (!point || point.gap) return NaN;
      if (Number.isFinite(point.min)) return point.min;
      return Number.isFinite(point.value) ? point.value : NaN;
    }

    function reportPointMax(point) {
      if (!point || point.gap) return NaN;
      if (Number.isFinite(point.max)) return point.max;
      return Number.isFinite(point.value) ? point.value : NaN;
    }

    function reportPointValue(point) {
      if (!point || point.gap) return NaN;
      if (Number.isFinite(point.value)) return point.value;
      const min = reportPointMin(point);
      const max = reportPointMax(point);
      return Number.isFinite(min) && Number.isFinite(max)
        ? (min + max) / 2
        : NaN;
    }

    function reportPointOverlapsRange(point, range) {
      if (!point || point.gap || !range || !Number.isFinite(point.t)) {
        return false;
      }
      const pointEnd = Number.isFinite(point.end) ? point.end : point.t;
      return pointEnd >= range.start && point.t <= range.end;
    }

    function reportPointOverlapsAnyRange(point, ranges) {
      if (!Array.isArray(ranges) || !ranges.length) return false;
      return ranges.some((range) => reportPointOverlapsRange(point, range));
    }

    function reportSeriesExtent(seriesList) {
      let min = Infinity;
      let max = -Infinity;
      seriesList.forEach((series) => {
        series.forEach((point) => {
          const pointMin = reportPointMin(point);
          const pointMax = reportPointMax(point);
          if (!Number.isFinite(pointMin) || !Number.isFinite(pointMax)) return;
          min = Math.min(min, pointMin);
          max = Math.max(max, pointMax);
        });
      });
      if (!Number.isFinite(min) || !Number.isFinite(max)) {
        return {min: 0, max: 1};
      }
      if (min === max) {
        min -= 1;
        max += 1;
      }
      const pad = Math.max(0.5, (max - min) * 0.08);
      return {min: min - pad, max: max + pad};
    }

    function disconnectReportResizeObserver() {
      if (!reportResizeObserver) return;
      reportResizeObserver.disconnect();
      reportResizeObserver = null;
    }

    function observeReportCanvas(canvas) {
      if (!canvas || typeof ResizeObserver !== "function") return;
      if (!reportResizeObserver) {
        reportResizeObserver = new ResizeObserver(() => {
          scheduleReportDraw();
        });
      }
      reportResizeObserver.observe(canvas);
    }

    function reportVisibleWidthFallback(canvas) {
      let node = canvas ? canvas.parentElement : null;
      while (node) {
        const rect = node.getBoundingClientRect();
        if (rect.width > 24) return rect.width - 24;
        if (node.id === "p-report") break;
        node = node.parentElement;
      }
      const host = document.getElementById("reportCharts");
      return host ? host.clientWidth - 24 : 0;
    }

    function reportSvgBox(svg, fallbackHeight) {
      const rect = svg.getBoundingClientRect();
      const parentRect = svg.parentElement
        ? svg.parentElement.getBoundingClientRect()
        : {width: 0};
      const cssHeight = parseFloat(getComputedStyle(svg).height);
      let measuredWidth = rect.width || svg.clientWidth ||
        Math.max(0, parentRect.width - 24) ||
        reportVisibleWidthFallback(svg);
      // Firefox often has no laid-out width on the first paint after the tab is
      // revealed and never re-fires ResizeObserver for display:none -> shown, so
      // the chart could stay blank until a manual resize. The SVG uses viewBox +
      // preserveAspectRatio="none", so a logical fallback width is stretched to
      // the real CSS width and renders correctly; a later resize/redraw refines
      // the coordinate resolution. Only force a width while the tab is visible.
      if (measuredWidth <= 0 && AirCANnect.pages.isActive("report")) {
        measuredWidth = 800;
      }
      if (measuredWidth <= 0) return null;
      const width = Math.max(320, Math.floor(measuredWidth || 0));
      const height = Math.max(80,
        Math.floor(rect.height || svg.clientHeight || cssHeight ||
          fallbackHeight));
      svg.setAttribute("viewBox", "0 0 " + width + " " + height);
      svg.setAttribute("preserveAspectRatio", "none");
      return {width, height};
    }

    function svgNode(name, attrs) {
      const node = document.createElementNS(SVG_NS, name);
      Object.keys(attrs || {}).forEach((key) => {
        node.setAttribute(key, attrs[key]);
      });
      return node;
    }

    function svgText(text, x, y, attrs) {
      const node = svgNode("text", Object.assign({
        x: x.toFixed(1),
        y: y.toFixed(1),
        fill: "#69717f",
        "font-size": "10",
        "font-family": "SF Mono, Consolas, monospace",
      }, attrs || {}));
      node.textContent = text;
      return node;
    }

    function clearReportSvg(svg, width, height) {
      svg.textContent = "";
      svg._selectionRect = null;
      svg._cursor = null;
      svg._timeTooltip = null;
      svg.appendChild(svgNode("rect", {
        x: "0",
        y: "0",
        width: String(width),
        height: String(height),
        fill: "#080a0f",
      }));
    }

    function reportTimeTickCount(graphW) {
      return Math.max(3, Math.min(12, Math.round(graphW / 110)));
    }

    // Vertical gridlines + time labels along the x-axis, shared by every chart
    // so they line up. Drawn before the data so traces/marks sit on top.
    function drawReportTimeAxis(svg, pad, graphW, graphH, height, start, end) {
      const ticks = reportTimeTickCount(graphW);
      const formatTime = (end - start) / ticks < 60000
        ? fmtReportClock : fmtReportTime;
      for (let i = 0; i <= ticks; i++) {
        const frac = i / ticks;
        const x = pad.left + graphW * frac;
        const anchor = i === 0 ? "start" : i === ticks ? "end" : "middle";
        svg.appendChild(svgNode("line", {
          x1: x.toFixed(1),
          y1: String(pad.top),
          x2: x.toFixed(1),
          y2: (pad.top + graphH).toFixed(1),
          stroke: "#1f2633",
          "stroke-width": "1",
          "stroke-dasharray": "2 5",
        }));
        svg.appendChild(svgText(formatTime(start + (end - start) * frac),
          x, height - 5, {"text-anchor": anchor}));
      }
    }

    function drawReportEventMarkers(svg, events, start, end, ranges,
                                    pad, graphW, graphH) {
      const markers = (events || []).map((event) => ({
        event,
        definition: reportEventDefs.find((item) => item.code === event.code),
      })).filter((marker) => marker.definition &&
        Number.isFinite(marker.event.t) &&
        marker.event.t >= start && marker.event.t <= end &&
        reportPointRangeIndex(marker.event.t, ranges) >= 0)
        .sort((a, b) => a.event.t - b.event.t);
      const laneLastX = [-Infinity, -Infinity, -Infinity];

      markers.forEach((marker) => {
        const x = pad.left + graphW *
          ((marker.event.t - start) / (end - start));
        let lane = laneLastX.findIndex((lastX) => x - lastX >= 12);
        if (lane < 0) {
          lane = laneLastX.indexOf(Math.min(...laneLastX));
        }
        laneLastX[lane] = x;

        svg.appendChild(svgNode("line", {
          x1: x.toFixed(1),
          y1: String(pad.top),
          x2: x.toFixed(1),
          y2: (pad.top + graphH).toFixed(1),
          stroke: marker.definition.color,
          "stroke-opacity": "0.9",
          "stroke-width": "1",
          "vector-effect": "non-scaling-stroke",
          "pointer-events": "none",
        }));

        const labelX = x + 3;
        const labelY = pad.top + 4 + lane * 18;
        svg.appendChild(svgText(marker.definition.key, labelX, labelY, {
          fill: marker.definition.color,
          "font-size": "9",
          "font-weight": "600",
          transform: "rotate(90 " + labelX.toFixed(1) + " " +
            labelY.toFixed(1) + ")",
          "pointer-events": "none",
        }));
      });
    }

    function attachReportCursor(svg, pad, graphH) {
      const cursor = svgNode("line", {
        x1: "0", x2: "0",
        y1: String(pad.top), y2: (pad.top + graphH).toFixed(1),
        stroke: "#7c8595", "stroke-width": "1", "stroke-dasharray": "3 3",
        "pointer-events": "none", visibility: "hidden",
      });
      svg.appendChild(cursor);
      svg._cursor = cursor;
      svg._timeTooltip = createReportTimeTooltip(svg);
    }

    function drawReportChart(svg,
                             seriesList,
                             events,
                             minY,
                             maxY,
                             start,
                             end,
                             ranges,
                             rangePending) {
      const box = reportSvgBox(svg, 150);
      if (!box) return false;
      const {width, height} = box;
      clearReportSvg(svg, width, height);

      const pad = {left: 44, right: 10, top: 12, bottom: 20};
      const graphW = width - pad.left - pad.right;
      const graphH = height - pad.top - pad.bottom;

      for (let i = 0; i <= 4; i++) {
        const y = pad.top + graphH * (i / 4);
        svg.appendChild(svgNode("line", {
          x1: String(pad.left),
          y1: y.toFixed(1),
          x2: String(width - pad.right),
          y2: y.toFixed(1),
          stroke: "#1f2633",
          "stroke-width": "1",
          "stroke-dasharray": "2 5",
        }));
      }

      for (let i = 0; i <= 4; i++) {
        const value = maxY - (maxY - minY) * (i / 4);
        const label = Math.abs(value) >= 10 ? value.toFixed(0) :
          value.toFixed(1);
        svg.appendChild(svgText(label, pad.left - 6,
          pad.top + graphH * (i / 4) + 3, {"text-anchor": "end"}));
      }

      drawReportTimeAxis(svg, pad, graphW, graphH, height, start, end);

      seriesList.forEach((series) => {
        const points = Array.isArray(series.points) ? series.points : [];
        const isEnvelope = points.some((point) => point && point.envelope);
        if ((!isEnvelope && points.length < 2) ||
            (isEnvelope && points.length < 1)) {
          return;
        }
        if (isEnvelope) {
          const flushSegment = (segment) => {
            if (!segment.length) return;
            const midParts = [];
            const barParts = [];
            let midStarted = false;
            segment.forEach((point) => {
              const pointEnd = Number.isFinite(point.end) ? point.end : point.t;
              const tMid = point.t + Math.max(0, pointEnd - point.t) / 2;
              const x = pad.left + graphW * ((tMid - start) / (end - start));
              const yMax = pad.top + graphH *
                (1 - (reportPointMax(point) - minY) / (maxY - minY));
              const yMin = pad.top + graphH *
                (1 - (reportPointMin(point) - minY) / (maxY - minY));
              const yMid = (yMax + yMin) / 2;
              barParts.push("M" + x.toFixed(1) + " " + yMax.toFixed(1) +
                "L" + x.toFixed(1) + " " + yMin.toFixed(1));
              midParts.push((midStarted ? "L" : "M") + x.toFixed(1) + " " +
                yMid.toFixed(1));
              midStarted = true;
            });
            if (barParts.length) {
              svg.appendChild(svgNode("path", {
                d: barParts.join(" "),
                fill: "none",
                stroke: series.color,
                "stroke-opacity": "0.55",
                "stroke-width": "1.15",
                "vector-effect": "non-scaling-stroke",
              }));
            }
            if (midParts.length > 1) {
              svg.appendChild(svgNode("path", {
                d: midParts.join(" "),
                fill: "none",
                stroke: series.color,
                "stroke-opacity": "0.95",
                "stroke-width": "1.25",
                "vector-effect": "non-scaling-stroke",
              }));
            }
          };
          let segment = [];
          let currentRange = -1;
          points.forEach((point) => {
            if (point && point.gap) {
              flushSegment(segment);
              segment = [];
              currentRange = -1;
              return;
            }
            const minValue = reportPointMin(point);
            const maxValue = reportPointMax(point);
            if (!Number.isFinite(point.t) ||
                !Number.isFinite(minValue) ||
                !Number.isFinite(maxValue)) {
              flushSegment(segment);
              segment = [];
              currentRange = -1;
              return;
            }
            const pointEnd = Number.isFinite(point.end) ? point.end : point.t;
            if (pointEnd < start || point.t > end) {
              flushSegment(segment);
              segment = [];
              currentRange = -1;
              return;
            }
            const visibleT = Math.max(point.t, start);
            const rangeIndex = reportPointRangeIndex(visibleT, ranges);
            if (rangeIndex < 0 || rangeIndex !== currentRange) {
              flushSegment(segment);
              segment = [];
              currentRange = rangeIndex;
            }
            if (rangeIndex < 0) return;
            segment.push({
              t: Math.max(point.t, start),
              end: Math.min(pointEnd, end),
              min: minValue,
              max: maxValue,
            });
          });
          flushSegment(segment);
          return;
        }
        const parts = [];
        let started = false;
        let currentRange = -1;
        points.forEach((point) => {
          if (point && point.gap) {
            started = false;
            currentRange = -1;
            return;
          }
          const pointValue = reportPointValue(point);
          if (!Number.isFinite(point.t) || !Number.isFinite(pointValue) ||
              point.t < start || point.t > end) {
            started = false;
            currentRange = -1;
            return;
          }
          const rangeIndex = reportPointRangeIndex(point.t, ranges);
          if (rangeIndex < 0) {
            started = false;
            currentRange = -1;
            return;
          }
          if (rangeIndex !== currentRange) {
            started = false;
            currentRange = rangeIndex;
          }
          const x = pad.left + graphW * ((point.t - start) / (end - start));
          const y = pad.top + graphH *
            (1 - (pointValue - minY) / (maxY - minY));
          parts.push((started ? "L" : "M") + x.toFixed(1) + " " +
            y.toFixed(1));
          started = true;
        });
        if (!parts.length) return;
        svg.appendChild(svgNode("path", {
          d: parts.join(" "),
          fill: "none",
          stroke: series.color,
          "stroke-width": "1.5",
          "vector-effect": "non-scaling-stroke",
        }));
      });

      drawReportEventMarkers(svg, events, start, end, ranges,
        pad, graphW, graphH);

      svg._geom = {pad, graphW, width, height};
      attachReportCursor(svg, pad, graphH);
      return true;
    }

    function drawReportEventFlags(svg, events, start, end, ranges) {
      const box = reportSvgBox(svg, 120);
      if (!box) return false;
      const {width, height} = box;
      clearReportSvg(svg, width, height);

      const pad = {left: 44, right: 10, top: 10, bottom: 18};
      const graphW = width - pad.left - pad.right;
      const graphH = height - pad.top - pad.bottom;
      const rowH = graphH / reportEventDefs.length;

      reportEventDefs.forEach((def, row) => {
        const y = pad.top + rowH * row;
        svg.appendChild(svgNode("rect", {
          x: String(pad.left),
          y: y.toFixed(1),
          width: graphW.toFixed(1),
          height: rowH.toFixed(1),
          fill: row % 2 ? "#0b1010" : "#0b140f",
        }));
        svg.appendChild(svgText(def.key, pad.left - 8,
          y + rowH * 0.65, {
            fill: "#9aa7bd",
            "text-anchor": "end",
          }));
      });

      drawReportTimeAxis(svg, pad, graphW, graphH, height, start, end);

      events.forEach((event) => {
        const def = reportEventDefs.find((item) => item.code === event.code);
        if (!def) return;
        const duration = Math.max(0, Number(event.duration || 0));
        const eventEnd = event.t + duration;
        const overlaps = duration > 0
          ? event.t < end && eventEnd > start
          : event.t >= start && event.t <= end;
        if (!overlaps) return;
        const visibleT = Math.max(event.t, start);
        if (reportPointRangeIndex(visibleT, ranges) < 0) return;
        const row = reportEventDefs.indexOf(def);
        const x = pad.left + graphW * ((visibleT - start) / (end - start));
        const y = pad.top + rowH * row + 2;
        const visibleEnd = duration > 0 ? Math.min(eventEnd, end) : visibleT;
        const visibleDuration = Math.max(0, visibleEnd - visibleT);
        const w = Math.max(2, graphW * (visibleDuration / (end - start)));
        svg.appendChild(svgNode("rect", {
          x: x.toFixed(1),
          y: y.toFixed(1),
          width: w.toFixed(1),
          height: Math.max(2, rowH - 4).toFixed(1),
          fill: def.color,
        }));
      });
      svg._geom = {pad, graphW, width, height};
      attachReportCursor(svg, pad, graphH);
      return true;
    }

    function fmtReportClock(ms) {
      const d = new Date(Number(ms));
      if (Number.isNaN(d.getTime())) return "--";
      return AirCANnect.format.pad2(d.getHours()) + ":" + AirCANnect.format.pad2(d.getMinutes()) + ":" +
        AirCANnect.format.pad2(d.getSeconds());
    }

    function fmtReportVal(v) {
      if (!Number.isFinite(v)) return "--";
      return Math.abs(v) >= 10 ? v.toFixed(1) : v.toFixed(2);
    }

    // Nearest point to time t (points sorted ascending). Returns null if the
    // closest sample is too far away (i.e. the cursor sits in a data gap).
    function nearestSeriesValue(points, t) {
      if (!points || !points.length) return null;
      let lo = 0;
      let hi = points.length - 1;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (points[mid].t < t) lo = mid + 1; else hi = mid;
      }
      const dataPoint = (idx, step) => {
        for (let i = idx; i >= 0 && i < points.length; i += step) {
          const point = points[i];
          const value = reportPointValue(point);
          if (point && !point.gap && Number.isFinite(point.t) &&
              Number.isFinite(value)) {
            return point;
          }
        }
        return null;
      };
      let best = dataPoint(lo, 1) || dataPoint(lo - 1, -1);
      const left = dataPoint(lo - 1, -1);
      if (!best) return null;
      if (left && Math.abs(left.t - t) < Math.abs(best.t - t)) {
        best = left;
      }
      if (Math.abs(best.t - t) > 120000) return null;
      return reportPointValue(best);
    }

    // Mouse clientX over a chart svg -> time within its range (viewBox is
    // stretched to the CSS width via preserveAspectRatio=none).
    function reportCanvasTime(canvas, clientX) {
      const g = canvas._geom;
      const it = canvas._item;
      if (!g || !it || !g.graphW) return null;
      const rect = canvas.getBoundingClientRect();
      if (!rect.width || it.end <= it.start) return null;
      const logicalX = ((clientX - rect.left) / rect.width) * g.width;
      const frac = (logicalX - g.pad.left) / g.graphW;
      const t = it.start + frac * (it.end - it.start);
      return Math.max(it.start, Math.min(it.end, t));
    }

    function reportReadoutText(item, t) {
      if (item.type === "events") return fmtReportClock(t);
      const parts = [];
      (item.seriesList || []).forEach((s) => {
        const v = nearestSeriesValue(s.points, t);
        if (v != null) parts.push(s.label + " " + fmtReportVal(v));
      });
      return parts.join("   ");
    }

    function reportTimeToX(canvas, t) {
      const g = canvas._geom;
      const it = canvas._item;
      return g.pad.left + g.graphW * ((t - it.start) / (it.end - it.start));
    }

    function createReportTimeTooltip(svg) {
      const group = svgNode("g", {
        "pointer-events": "none",
        visibility: "hidden",
      });
      const rect = svgNode("rect", {
        x: "0",
        y: "0",
        width: "58",
        height: "16",
        rx: "2",
        ry: "2",
        fill: "#0b1020",
        "fill-opacity": "0.96",
        stroke: "#aeb8c8",
        "stroke-width": "1",
      });
      const text = svgText("00:00:00", 0, 0, {
        fill: "#f2f6fb",
        "font-size": "10",
        "font-family": "SF Mono, Consolas, monospace",
        "text-anchor": "middle",
      });
      group.appendChild(rect);
      group.appendChild(text);
      svg.appendChild(group);
      return {group, rect, text};
    }

    function hideReportTimeTooltip(canvas) {
      if (canvas._timeTooltip) {
        canvas._timeTooltip.group.setAttribute("visibility", "hidden");
      }
    }

    function updateReportTimeTooltip(canvas, x, t) {
      const tip = canvas._timeTooltip;
      const g = canvas._geom;
      if (!tip || !g) return;
      const label = fmtReportClock(t);
      const boxW = 58;
      const boxH = 16;
      const minCx = g.pad.left + boxW / 2;
      const maxCx = g.width - g.pad.right - boxW / 2;
      const cx = Math.max(minCx, Math.min(maxCx, x));
      const y = Math.max(g.pad.top + 2, g.height - boxH - 2);
      tip.text.textContent = label;
      tip.rect.setAttribute("x", (cx - boxW / 2).toFixed(1));
      tip.rect.setAttribute("y", y.toFixed(1));
      tip.text.setAttribute("x", cx.toFixed(1));
      tip.text.setAttribute("y", (y + 11).toFixed(1));
      tip.group.setAttribute("visibility", "visible");
    }

    // Item 6: one cursor time, drawn on every chart, with each header showing
    // that chart's value at the cursor.
    function updateReportHover() {
      reportDrawItems.forEach((item) => {
        const canvas = item.canvas;
        if (!canvas || !canvas._geom || !canvas._cursor) return;
        const t = reportHoverTime;
        const inRange = t != null && t >= item.start && t <= item.end;
        if (!inRange) {
          canvas._cursor.setAttribute("visibility", "hidden");
          hideReportTimeTooltip(canvas);
          if (canvas._readout) canvas._readout.textContent = "";
          return;
        }
        const x = reportTimeToX(canvas, t);
        canvas._cursor.setAttribute("x1", x.toFixed(1));
        canvas._cursor.setAttribute("x2", x.toFixed(1));
        canvas._cursor.setAttribute("visibility", "visible");
        updateReportTimeTooltip(canvas, x, t);
        if (canvas._readout) {
          canvas._readout.textContent = reportReadoutText(item, t);
        }
      });
    }

    function reportSelectionRect(canvas) {
      if (!canvas._selectionRect) {
        const rect = svgNode("rect", {
          fill: "#8b5cf633",
          stroke: "#8b5cf6",
          "stroke-width": "1",
          "pointer-events": "none",
          visibility: "hidden",
        });
        if (canvas._cursor && canvas._cursor.parentNode === canvas) {
          canvas.insertBefore(rect, canvas._cursor);
        } else {
          canvas.appendChild(rect);
        }
        canvas._selectionRect = rect;
      }
      return canvas._selectionRect;
    }

    function hideReportSelectionRects() {
      reportDrawItems.forEach((item) => {
        const canvas = item && item.canvas;
        if (canvas && canvas._selectionRect) {
          canvas._selectionRect.setAttribute("visibility", "hidden");
        }
      });
    }

    function updateReportSelectionRects() {
      if (!reportDrag) {
        hideReportSelectionRects();
        return;
      }
      const lo = Math.min(reportDrag.t0, reportDrag.t1);
      const hi = Math.max(reportDrag.t0, reportDrag.t1);
      reportDrawItems.forEach((item) => {
        const canvas = item && item.canvas;
        if (!canvas || !canvas._geom || item.end <= item.start) return;
        const g = canvas._geom;
        const rect = reportSelectionRect(canvas);
        const visibleLo = Math.max(lo, item.start);
        const visibleHi = Math.min(hi, item.end);
        if (visibleHi <= visibleLo) {
          rect.setAttribute("visibility", "hidden");
          return;
        }
        const x0 = reportTimeToX(canvas, visibleLo);
        const x1 = reportTimeToX(canvas, visibleHi);
        rect.setAttribute("x", Math.min(x0, x1).toFixed(1));
        rect.setAttribute("y", String(g.pad.top));
        rect.setAttribute("width", Math.abs(x1 - x0).toFixed(1));
        rect.setAttribute("height",
          (g.height - g.pad.top - g.pad.bottom).toFixed(1));
        rect.setAttribute("visibility", "visible");
      });
    }

    // Item 7: drag a range on any chart to zoom every chart to it.
    function reportFinishDrag() {
      if (!reportDrag) return;
      const drag = reportDrag;
      reportDrag = null;
      hideReportSelectionRects();
      const lo = Math.min(drag.t0, drag.t1);
      const hi = Math.max(drag.t0, drag.t1);
      if (hi - lo > 60000) {
        setReportZoomRange(lo, hi);
      }
    }
    document.addEventListener("mouseup", reportFinishDrag);

    function validReportRange(range) {
      return range && Number.isFinite(range.start) && Number.isFinite(range.end) &&
        range.end - range.start >= 60000;
    }

    function updateReportZoomControls() {
      const haveRange = validReportRange(reportRange());
      const zoomOut = document.getElementById("reportZoomOut");
      const zoomIn = document.getElementById("reportZoomIn");
      const zoomReset = document.getElementById("reportZoomReset");
      if (zoomOut) zoomOut.disabled = !reportZoom;
      if (zoomReset) zoomReset.disabled = !reportZoom;
      if (zoomIn) zoomIn.disabled = !haveRange;
    }

    function setReportZoomRange(start, end) {
      const bounds = reportRange();
      if (!validReportRange(bounds)) return false;
      let width = Math.max(60000, end - start);
      const boundsWidth = bounds.end - bounds.start;
      if (width >= boundsWidth - 1000) {
        resetReportZoom();
        return true;
      }
      width = Math.min(width, boundsWidth);
      let center = start + (end - start) / 2;
      let lo = center - width / 2;
      let hi = center + width / 2;
      if (lo < bounds.start) {
        hi += bounds.start - lo;
        lo = bounds.start;
      }
      if (hi > bounds.end) {
        lo -= hi - bounds.end;
        hi = bounds.end;
      }
      lo = Math.max(bounds.start, lo);
      hi = Math.min(bounds.end, hi);
      if (hi - lo < 60000) return false;
      reportZoom = {start: lo, end: hi};
      if (!updateRenderedReportRange()) renderReportCharts();
      return true;
    }

    function zoomReportWindow(scale) {
      const base = reportZoom || reportRange();
      if (!validReportRange(base)) return;
      if (!reportZoom && scale > 1) return;
      const center = base.start + (base.end - base.start) / 2;
      const width = (base.end - base.start) * scale;
      setReportZoomRange(center - width / 2, center + width / 2);
    }

    function panReportZoom(direction) {
      if (!reportZoom || !direction) return;
      const width = reportZoom.end - reportZoom.start;
      const shift = width * 0.25 * direction;
      setReportZoomRange(reportZoom.start + shift, reportZoom.end + shift);
    }

    function resetReportZoom() {
      if (!reportZoom) return;
      cancelReportRangeRequest();
      reportZoom = null;
      reportRangeActiveKey = "";
      reportRangeToken++;
      reportSeries = reportBaseSeries;
      reportEvents = reportBaseEvents;
      if (!updateRenderedReportRange()) renderReportCharts();
    }

    document.addEventListener("keydown", (ev) => {
      if (!AirCANnect.pages.isActive("report") || !reportZoom) return;
      const tag = ev.target && ev.target.tagName ?
        ev.target.tagName.toLowerCase() : "";
      if (tag === "input" || tag === "textarea" || tag === "select" ||
          (ev.target && ev.target.isContentEditable)) {
        return;
      }
      if (ev.key === "ArrowLeft") {
        ev.preventDefault();
        panReportZoom(-1);
      } else if (ev.key === "ArrowRight") {
        ev.preventDefault();
        panReportZoom(1);
      }
    });

    function attachReportChartPointer(canvas) {
      canvas.addEventListener("mousemove", (ev) => {
        const t = reportCanvasTime(canvas, ev.clientX);
        if (t == null) return;
        if (reportDrag) {
          if (reportDrag.canvas !== canvas) return;
          reportDrag.t1 = t;
          updateReportSelectionRects();
          return;
        }
        reportHoverTime = t;
        updateReportHover();
      });
      canvas.addEventListener("mouseleave", () => {
        if (reportDrag) return;
        reportHoverTime = null;
        updateReportHover();
      });
      canvas.addEventListener("mousedown", (ev) => {
        const t = reportCanvasTime(canvas, ev.clientX);
        if (t == null || !canvas._geom) return;
        ev.preventDefault();
        reportDrag = {canvas, t0: t, t1: t};
        reportHoverTime = null;
        updateReportHover();
        updateReportSelectionRects();
      });
    }

    function drawReportItems() {
      reportDrawPending = false;
      let retry = false;
      reportDrawItems.forEach((item) => {
        if (!item || !item.canvas ||
            !document.body.contains(item.canvas)) {
          return;
        }
        if (item.type === "events") {
          retry = !drawReportEventFlags(item.canvas, item.events, item.start,
                                        item.end, item.ranges) || retry;
        } else {
          retry = !drawReportChart(item.canvas,
                                   item.seriesList,
                                   item.events,
                                   item.minY,
                                   item.maxY,
                                   item.start,
                                   item.end,
                                   item.ranges,
                                   item.rangePending) || retry;
        }
      });
      updateReportHover();
      updateReportSelectionRects();
      if (retry && AirCANnect.pages.isActive("report")) {
        reportDrawRetryCount++;
        setTimeout(() => scheduleReportDraw(), 50);
      } else if (!retry) {
        reportDrawRetryCount = 0;
      }
    }

    function scheduleReportDraw() {
      if (!reportDrawItems.length || reportDrawPending) return;
      reportDrawPending = true;
      if (typeof requestAnimationFrame === "function") {
        requestAnimationFrame(drawReportItems);
      } else {
        setTimeout(drawReportItems, 0);
      }
    }

    function scheduleReportDrawAfterReveal() {
      if (typeof requestAnimationFrame === "function") {
        requestAnimationFrame(() => scheduleReportDraw());
      } else {
        setTimeout(() => scheduleReportDraw(), 0);
      }
    }

    function reportChartSeriesList(definition) {
      return (definition.series || [definition]).map((series) => ({
        label: series.label || definition.title,
        color: series.color || definition.color,
        points: (reportSeries[series.key] || []).slice()
          .sort((a, b) => a.t - b.t),
      })).filter((series) => series.points.length > 0);
    }

    function reportChartExtent(definition, ranges) {
      const series = (definition.series || [definition]).map((item) => {
        const base = reportBaseSeries[item.key] || [];
        const source = base.length ? base : reportSeries[item.key] || [];
        return source.filter((point) =>
          point && !point.gap && reportPointOverlapsAnyRange(point, ranges));
      }).filter((points) => points.length > 0);
      return reportSeriesExtent(series);
    }

    function setReportItemLoading(item, loading) {
      if (!item || !item.name) return;
      loading = !!loading &&
        !reportChartPreferences.collapsed.has(item.key);
      if (loading && !item.loadingBadge) {
        const badge = document.createElement("span");
        badge.className = "report-res-badge";
        badge.textContent = "loading";
        item.name.appendChild(badge);
        item.loadingBadge = badge;
      } else if (!loading && item.loadingBadge) {
        item.loadingBadge.remove();
        item.loadingBadge = null;
      }
    }

    function updateRenderedReportRange() {
      const range = reportZoom || reportRange();
      if (!validReportRange(range) || !reportDrawItems.length) return false;

      if (reportZoom) {
        ensureSignalStoreRangeLoaded(reportZoom.start, reportZoom.end);
      }
      const ranges = reportVisibleSessionRanges();
      reportDrawItems.forEach((item) => {
        item.events = reportEvents.slice();
        item.start = range.start;
        item.end = range.end;
        item.ranges = ranges;

        const pending = !reportBaseLoadedCharts.has(item.key) ||
          (!!reportZoom && !signalStoreRangeChartReady(item.key));
        if (item.type === "series") {
          const definition = reportChartDefinition(item.key);
          if (definition) {
            const extent = reportChartExtent(definition, ranges);
            item.seriesList = reportChartSeriesList(definition);
            item.minY = extent.min;
            item.maxY = extent.max;
          }
          item.rangePending = pending;
        }
        setReportItemLoading(item, pending);
      });

      scheduleReportDraw();
      updateReportZoomControls();
      return true;
    }

    function updateRenderedReportChart(key) {
      const range = reportZoom || reportRange();
      if (!validReportRange(range)) return false;

      const ranges = reportVisibleSessionRanges();
      const pending = !!reportZoom && !signalStoreRangeChartReady(key);
      if (key === "events") {
        reportDrawItems.forEach((item) => {
          item.events = reportEvents.slice();
          if (item.type === "events") {
            item.start = range.start;
            item.end = range.end;
            item.ranges = ranges;
            setReportItemLoading(item, pending);
          }
        });
        scheduleReportDraw();
        updateReportZoomControls();
        return reportDrawItems.length > 0;
      }

      const item = reportDrawItems.find((candidate) => candidate.key === key);
      const definition = reportChartDefinition(key);
      if (!definition) return false;
      if (!item || item.type !== "series") {
        return reportChartSeriesList(definition).length === 0;
      }

      const extent = reportChartExtent(definition, ranges);
      const seriesList = reportChartSeriesList(definition);
      item.seriesList = seriesList;
      item.events = reportEvents.slice();
      item.minY = extent.min;
      item.maxY = extent.max;
      item.start = range.start;
      item.end = range.end;
      item.ranges = ranges;
      item.rangePending = pending;
      setReportItemLoading(item, pending);
      scheduleReportDraw();
      updateReportZoomControls();
      return true;
    }

    function renderReportEventFlags(container, range, ranges, definition) {
      // Stay visible if the night has events; do not vanish on an event-free zoom.
      if (!((reportResult && reportResult.events_available) ||
            reportBaseEvents.length)) {
        return;
      }
      const card = document.createElement("div");
      card.className = "report-chart";
      const title = document.createElement("div");
      title.className = "report-chart-title";
      const name = document.createElement("span");
      name.textContent = definition.title;
      let loadingBadge = null;
      if (!reportChartPreferences.collapsed.has(definition.key) &&
          (!reportBaseLoadedCharts.has(definition.key) ||
           (reportZoom && !signalStoreRangeChartReady(definition.key)))) {
        loadingBadge = document.createElement("span");
        loadingBadge.className = "report-res-badge";
        loadingBadge.textContent = "loading";
        name.appendChild(loadingBadge);
      }
      title.appendChild(name);
      const readout = document.createElement("span");
      readout.className = "readout";
      title.appendChild(readout);
      const legend = document.createElement("span");
      legend.className = "value";
      reportEventDefs.forEach((def) => {
        const tag = document.createElement("span");
        tag.style.marginLeft = "10px";
        const dot = document.createElement("span");
        dot.className = "report-legend-dot";
        dot.style.background = def.color;
        tag.appendChild(dot);
        tag.appendChild(document.createTextNode(def.key));
        legend.appendChild(tag);
      });
      title.appendChild(legend);
      appendReportChartActions(title, definition.key);
      card.appendChild(title);
      if (reportChartPreferences.collapsed.has(definition.key)) {
        card.classList.add("collapsed");
        container.appendChild(card);
        return;
      }

      const canvas = document.createElementNS(SVG_NS, "svg");
      canvas.style.height = "120px";
      card.appendChild(canvas);
      container.appendChild(card);
      const item = {
        key: definition.key,
        type: "events",
        canvas,
        name,
        loadingBadge,
        events: reportEvents.slice(),
        start: range.start,
        end: range.end,
        ranges: ranges,
      };
      reportDrawItems.push(item);
      canvas._item = item;
      canvas._readout = readout;
      attachReportChartPointer(canvas);
      observeReportCanvas(canvas);
    }

    function renderReportCharts() {
      const container = document.getElementById("reportCharts");
      if (!container) return;
      disconnectReportResizeObserver();
      container.textContent = "";
      reportDrawItems = [];
      reportDrawPending = false;
      reportDrawRetryCount = 0;
      if (!reportResult ||
          (reportResult.state !== "ready" && reportResult.state !== "partial")) {
        updateReportZoomControls();
        return;
      }
      if (reportResult.state === "partial") {
        const pnote = document.createElement("div");
        pnote.className = "report-chart-note";
        pnote.textContent =
          "Incomplete night - available data is shown; missing sources are marked below.";
        container.appendChild(pnote);
      }
      const range = reportZoom || reportRange();
      if (!(range.end > range.start)) {
        updateReportZoomControls();
        return;
      }
      if (reportZoom) {
        ensureSignalStoreRangeLoaded(reportZoom.start, reportZoom.end);
      }
      const sessionRanges = reportVisibleSessionRanges();

      // Map each signal name -> whether it fell back to the low-res 1-min trend
      // (high-res aged out); used to badge the affected charts.
      const lowResByName = {};
      (reportResult.stream_details || []).forEach((s) => {
        if (s && s.name) lowResByName[s.name] = !!s.low_res;
      });

      const chartByKey = new Map(reportChartDefs.map((definition) =>
        [definition.key, definition]));
      reportChartPreferences.order.forEach((key) => {
        const def = chartByKey.get(key);
        if (!def) return;
        if (def.type === "events") {
          renderReportEventFlags(container, range, sessionRanges, def);
          return;
        }

        const seriesDefs = def.series || [def];
        const availableParts = signalStoreChartParts(def);
        const seriesList = reportChartSeriesList(def);
        const collapsed = reportChartPreferences.collapsed.has(def.key);
        const basePending = !collapsed && availableParts.length > 0 &&
          !reportBaseLoadedCharts.has(def.key);
        if (!seriesList.length && !basePending) {
          if (def.optional && !availableParts.length) return;

          // Expected signal with no data: high-res aged out on the device
          // (best-effort night) or not yet backfilled. Show a labelled
          // placeholder instead of silently dropping the chart.
          const card = document.createElement("div");
          card.className = "report-chart report-chart-empty";
          const ctitle = document.createElement("div");
          ctitle.className = "report-chart-title";
          const cname = document.createElement("span");
          cname.textContent = def.title;
          ctitle.appendChild(cname);
          const cnote = document.createElement("span");
          cnote.className = "report-chart-note";
          if (!collapsed && availableParts.length &&
              !reportBaseLoadedCharts.has(def.key)) {
            cnote.textContent = "loading...";
          } else if (!collapsed) {
            cnote.textContent =
              reportResult && reportResult.missing_required > 0
                ? "backfilling..."
                : "not retained for this night";
          }
          if (!collapsed) ctitle.appendChild(cnote);
          appendReportChartActions(ctitle, def.key);
          card.appendChild(ctitle);
          if (reportChartPreferences.collapsed.has(def.key)) {
            card.classList.add("collapsed");
          } else if (availableParts.length &&
                     !reportBaseLoadedCharts.has(def.key)) {
            const placeholder = document.createElement("div");
            placeholder.className = "report-chart-placeholder";
            card.appendChild(placeholder);
          }
          container.appendChild(card);
          return;
        }
        const extent = reportChartExtent(def, sessionRanges);

        const card = document.createElement("div");
        card.className = "report-chart";
        const title = document.createElement("div");
        title.className = "report-chart-title";
        const name = document.createElement("span");
        name.textContent = def.title;
        const rangePending = !collapsed && !!reportZoom &&
          !signalStoreRangeChartReady(def.key);
        let loadingBadge = null;
        if (basePending || rangePending) {
          loadingBadge = document.createElement("span");
          loadingBadge.className = "report-res-badge";
          loadingBadge.textContent = "loading";
          name.appendChild(loadingBadge);
        }
        if (seriesDefs.some((sd) => lowResByName[sd.key])) {
          const badge = document.createElement("span");
          badge.className = "report-res-badge";
          badge.textContent = "1-min";
          badge.title =
            "High-res not retained for this night; showing the 1-minute trend.";
          name.appendChild(badge);
        }
        title.appendChild(name);
        const readout = document.createElement("span");
        readout.className = "readout";
        title.appendChild(readout);
        const legend = document.createElement("span");
        legend.className = "value";
        seriesList.forEach((series) => {
          const tag = document.createElement("span");
          tag.style.marginLeft = "10px";
          const dot = document.createElement("span");
          dot.className = "report-legend-dot";
          dot.style.background = series.color;
          tag.appendChild(dot);
          tag.appendChild(document.createTextNode(series.label));
          legend.appendChild(tag);
        });
        if (def.unit) {
          const unit = document.createElement("span");
          unit.style.marginLeft = "8px";
          unit.style.color = "var(--muted)";
          unit.textContent = def.unit;
          legend.appendChild(unit);
        }
        title.appendChild(legend);
        appendReportChartActions(title, def.key);
        card.appendChild(title);
        if (reportChartPreferences.collapsed.has(def.key)) {
          card.classList.add("collapsed");
          container.appendChild(card);
          return;
        }

        const canvas = document.createElementNS(SVG_NS, "svg");
        card.appendChild(canvas);
        container.appendChild(card);
        const item = {
          key: def.key,
          type: "series",
          canvas,
          name,
          loadingBadge,
          seriesList,
          events: reportEvents.slice(),
          minY: extent.min,
          maxY: extent.max,
          start: range.start,
          end: range.end,
          ranges: sessionRanges,
          rangePending,
        };
        reportDrawItems.push(item);
        canvas._item = item;
        canvas._readout = readout;
        attachReportChartPointer(canvas);
        observeReportCanvas(canvas);
      });
      drawReportItems();
      updateReportZoomControls();
    }

    function reportMetricValues(result, fields, scale) {
      const values = fields.map((field) =>
        result && Number.isFinite(Number(result[field])) ?
          Number(result[field]) * scale : null);
      if (values.every((value) => value === null)) return "--";

      const decimals = scale === 1000 ? 0 : 1;
      return values.map((value) =>
        value === null ? "--" : value.toFixed(decimals)).join(" / ");
    }

    function renderOptionalReportMetric(cardId, valueId, visible, value) {
      const card = document.getElementById(cardId);
      if (card) card.hidden = !visible;
      AirCANnect.ui.text(valueId, visible ? value : "--");
    }

    function renderReportSummary() {
      const nights = reportNightsNewestFirst();
      const selected = selectedReportNight();

      const dateBtn = document.getElementById("reportDateBtn");
      if (dateBtn) {
        if (selected) {
          dateBtn.innerHTML = fmtReportDate(selected.start) +
            "<span class=\"np-dur\">" + fmtMinutes(selected.duration_min) +
            "</span>";
        } else {
          dateBtn.textContent = nights.length ? "Select night" : "No nights";
        }
      }
      const selIndex = selected ?
        nights.findIndex((night) => night === selected) : -1;
      const prevBtn = document.getElementById("reportPrevNight");
      const nextBtn = document.getElementById("reportNextNight");
      const latestBtn = document.getElementById("reportLatestNight");
      if (prevBtn) prevBtn.disabled = !(selIndex >= 0 &&
        selIndex < nights.length - 1);
      if (nextBtn) nextBtn.disabled = !(selIndex > 0);
      if (latestBtn) latestBtn.disabled = !(selIndex > 0);
      const pop = document.getElementById("reportCalPop");
      if (pop && pop.classList.contains("open")) renderReportCalendar();
      const displayable = reportResult &&
        (reportResult.state === "ready" || reportResult.state === "partial");
      const durationMin = displayable && reportResult.duration_min ?
        reportResult.duration_min : (selected ? selected.duration_min : 0);
      AirCANnect.ui.text("reportDuration", durationMin ? fmtMinutes(durationMin) : "--");
      AirCANnect.ui.text("reportAhi", displayable && Number.isFinite(Number(reportResult.ahi)) ?
        Number(reportResult.ahi).toFixed(1) : "--");
      AirCANnect.ui.text("reportRdi", displayable && Number.isFinite(Number(reportResult.rdi)) ?
        Number(reportResult.rdi).toFixed(1) : "--");
      AirCANnect.ui.text("reportPressure", displayable ? reportMetricValues(
        reportResult, ["average_pressure", "ipap_50", "ipap_95"], 1) : "--");
      AirCANnect.ui.text("reportLeak", displayable ? reportMetricValues(
        reportResult, ["average_leak", "leak_50", "leak_95"], 1) : "--");

      const tidalVolumeVisible = displayable && (
        Number.isFinite(Number(reportResult.tidal_volume_50)) ||
        Number.isFinite(Number(reportResult.tidal_volume_95)));
      renderOptionalReportMetric(
        "reportTidalVolumeMetric",
        "reportTidalVolume",
        tidalVolumeVisible,
        reportMetricValues(reportResult,
          ["tidal_volume_50", "tidal_volume_95"], 1000));

      const respiratoryRateVisible = displayable && (
        Number.isFinite(Number(reportResult.respiratory_rate_50)) ||
        Number.isFinite(Number(reportResult.respiratory_rate_95)));
      renderOptionalReportMetric(
        "reportRespiratoryRateMetric",
        "reportRespiratoryRate",
        respiratoryRateVisible,
        reportMetricValues(reportResult,
          ["respiratory_rate_50", "respiratory_rate_95"], 1));

      const minuteVentilationVisible = displayable && (
        Number.isFinite(Number(reportResult.minute_ventilation_50)) ||
        Number.isFinite(Number(reportResult.minute_ventilation_95)));
      renderOptionalReportMetric(
        "reportMinuteVentilationMetric",
        "reportMinuteVentilation",
        minuteVentilationVisible,
        reportMetricValues(reportResult,
          ["minute_ventilation_50", "minute_ventilation_95"], 1));

      const spo2Median = displayable &&
        Number.isFinite(Number(reportResult.spo2_median)) ?
        Number(reportResult.spo2_median).toFixed(0) + "%" : "--";
      const spo2Threshold = displayable &&
        Number.isFinite(Number(reportResult.spo2_threshold_minutes)) ?
        Math.round(Number(reportResult.spo2_threshold_minutes)) + "m" : "--";
      const spo2Visible = spo2Median !== "--" || spo2Threshold !== "--";
      renderOptionalReportMetric(
        "reportSpo2Metric",
        "reportSpo2Summary",
        spo2Visible,
        spo2Median + " / " + spo2Threshold);

      const csrVisible = displayable &&
        Number.isFinite(Number(reportResult.csr_minutes));
      renderOptionalReportMetric(
        "reportCsrMetric",
        "reportCsr",
        csrVisible,
        csrVisible ? Math.round(Number(reportResult.csr_minutes)) + "m" : "--");

      renderReportEventCounts(displayable ? reportResult : null);
      renderReportSessions(reportResult && reportResult.sessions ?
        reportResult.sessions : selected ? selected.sessions : []);
      updateReportZoomControls();
    }

    // Conditional GET by stable night id. Poll while the backend is building.
    function isTransientReportError(text) {
      try {
        const parsed = JSON.parse(text);
        const code = parsed && parsed.error ? parsed.error : "";
        return code === "report_queue_busy" ||
               code === "report_queue_unavailable" ||
               code === "report_stream_unavailable" ||
               code === "report_stream_slots_full" ||
               code === "stream_busy" ||
               code === "stream_slots_full" ||
               code === "response_alloc";
      } catch (error) {
        return false;
      }
    }

    function cancelReportLoadRequest() {
      if (!reportLoadAbortController) return;
      reportLoadAbortController.abort();
      reportLoadAbortController = null;
    }

    function cancelReportRangeRequest() {
      if (!reportRangeAbortController) return;
      reportRangeAbortController.abort();
      reportRangeAbortController = null;
    }

    function cancelReportRequests(preserveRange = false) {
      cancelReportLoadRequest();
      cancelReportRangeRequest();
      reportLoadToken++;
      reportRangeToken++;
      if (!preserveRange) reportRangeActiveKey = "";
    }

    function reportCompletionMatches(data, url) {
      if (!data || !url) return false;

      const request = new URL(url, window.location.href);
      const night = request.searchParams.get("night") || "";
      if (String(data.night || "") !== night) return false;
      return data.kind === "night";
    }

    async function waitForReportCompletion(url, afterSerial, timeoutMs) {
      await AirCANnect.snapshots.wait(
        "report",
        (data) => reportCompletionMatches(data, url),
        afterSerial,
        timeoutMs);
    }

    async function pollReportFetch(options) {
      const active = options.active || (() => true);
      const delay = options.delayMs || REPORT_POLL_DELAY_MS;
      const maxAttempts = options.maxAttempts || 1;
      const deadline = Date.now() + maxAttempts * delay;
      for (let attempt = 0; attempt < maxAttempts; attempt++) {
        if (!active()) return null;
        const reportSnapshot = AirCANnect.snapshots.read("report");
        let response;
        try {
          response = await options.request();
        } catch (error) {
          if (error && error.name === "AbortError") return null;
          await AirCANnect.time.delay(delay);
          continue;
        }
        if (!active()) return null;
        const action = await options.handle(response);
        if (!action) return null;
        if (action.done) return action.value;
        const remainingMs = deadline - Date.now();
        if (remainingMs <= 0) break;

        if (action.waitForReport && options.waitUrl) {
          await waitForReportCompletion(
            options.waitUrl,
            reportSnapshot.serial,
            Math.min(REPORT_SSE_FALLBACK_MS, remainingMs));
        } else {
          await AirCANnect.time.delay(
            Math.min(
              action.delayMs == null ? delay : action.delayMs,
              remainingMs));
        }
      }
      return options.timeoutValue === undefined ? null : options.timeoutValue;
    }

    function conditionalRequestOptions(cached, signal) {
      const headers = {};
      if (cached && cached.etag) headers["If-None-Match"] = cached.etag;
      return {cache: "no-cache", headers, signal};
    }

    function reportArtifactRevision(response) {
      return String(
        response.headers.get("X-Report-Source-Revision") || "").toLowerCase();
    }

    async function runReportFetchJobs(jobs, concurrency) {
      if (!jobs.length) return;

      let next = 0;
      const workers = [];
      const workerCount = Math.min(concurrency || 2, jobs.length);
      for (let i = 0; i < workerCount; i++) {
        workers.push((async () => {
          while (next < jobs.length) {
            const job = jobs[next++];
            await job();
          }
        })());
      }
      await Promise.all(workers);
    }

    function reportChartDefinition(key) {
      return reportChartDefs.find((definition) => definition.key === key) ||
        null;
    }

    function reportRangeWindow(lo, hi) {
      return Number.isFinite(lo) && Number.isFinite(hi) && hi > lo
        ? {from: lo, to: hi} : null;
    }

    function signalStoreMagic(bytes, expected) {
      if (bytes.length < expected.length) return false;
      for (let i = 0; i < expected.length; i++) {
        if (bytes[i] !== expected.charCodeAt(i)) return false;
      }
      return true;
    }

    function signalStoreMetricSource(valid, str, summary, index) {
      const bit = 1 << index;
      if (!(valid & bit)) return "";
      if (str & bit) return "str_edf";
      if (summary & bit) return "summary";
      return "calculated";
    }

    function signalStorePopcount(value) {
      let bits = Number(value) >>> 0;
      let count = 0;
      while (bits) {
        bits &= bits - 1;
        count++;
      }
      return count;
    }

    function decodeSignalStoreNight(buffer) {
      const invalid = {valid: false};
      const bytes = new Uint8Array(buffer);
      const view = new DataView(buffer);
      if (buffer.byteLength < SIGNAL_STORE_NIGHT_HEADER_BYTES ||
          !signalStoreMagic(bytes, "ACRNIG01") ||
          view.getUint16(8, true) !== 2 ||
          view.getUint16(10, true) !== SIGNAL_STORE_NIGHT_HEADER_BYTES ||
          view.getUint32(12, true) !== buffer.byteLength) {
        return invalid;
      }

      const generation = view.getUint32(20, true);
      const revisionValue = view.getBigUint64(24, true);
      const dayStart = Number(view.getBigInt64(32, true));
      const dayEnd = Number(view.getBigInt64(40, true));
      const sessionCount = view.getUint16(64, true);
      const trackCount = view.getUint16(66, true);
      const expectedSize = SIGNAL_STORE_NIGHT_HEADER_BYTES +
        sessionCount * SIGNAL_STORE_SESSION_BYTES +
        trackCount * SIGNAL_STORE_TRACK_BYTES;
      if (!generation || revisionValue === 0n || !(dayEnd > dayStart) ||
          expectedSize !== buffer.byteLength) {
        return invalid;
      }

      const sessions = [];
      let sessionOffset = SIGNAL_STORE_NIGHT_HEADER_BYTES;
      let previousEnd = 0;
      let durationMs = 0;
      for (let i = 0; i < sessionCount; i++) {
        const start = Number(view.getBigInt64(sessionOffset, true));
        const end = Number(view.getBigInt64(sessionOffset + 8, true));
        if (!(end > start) || start < dayStart || end > dayEnd ||
            (i && start < previousEnd)) {
          return invalid;
        }
        sessions.push({
          start,
          end,
          duration_min: Math.round((end - start) / 60000),
        });
        durationMs += end - start;
        previousEnd = end;
        sessionOffset += SIGNAL_STORE_SESSION_BYTES;
      }
      if (BigInt(Math.round(durationMs)) !== view.getBigUint64(48, true)) {
        return invalid;
      }

      const tracks = [];
      const tracksByName = {};
      let availableSignals = 0;
      for (let index = 0; index < trackCount; index++) {
        const offset = sessionOffset + index * SIGNAL_STORE_TRACK_BYTES;
        const signal = view.getUint8(offset);
        const name = SIGNAL_STORE_SIGNAL_NAMES[signal] || "";
        const interval = view.getUint32(offset + 12, true);
        const phase = view.getUint32(offset + 20, true);
        const blockSlots = view.getUint16(offset + 6, true);
        const firstBlock = Number(view.getBigInt64(offset + 24, true));
        if (!name || view.getUint8(offset + 1) !== 1 || !interval ||
            SIGNAL_STORE_BLOCK_MS % interval !== 0 || phase >= interval ||
            !blockSlots || blockSlots > SIGNAL_STORE_MAX_BLOCKS ||
            firstBlock % SIGNAL_STORE_BLOCK_MS !== 0) {
          return invalid;
        }

        const presentBlocks = bytes.slice(
          offset + 64, offset + 64 + SIGNAL_STORE_BITMAP_BYTES);
        const track = {
          metadataIndex: index,
          signal,
          name,
          unit: view.getUint8(offset + 2),
          lodMask: view.getUint8(offset + 3),
          trackIndex: view.getUint16(offset + 4, true),
          blockSlots,
          presentCount: view.getUint16(offset + 8, true),
          interval,
          scale: view.getFloat32(offset + 16, true),
          offset: view.getFloat32(offset + 80, true),
          missingValue: view.getInt16(offset + 84, true),
          phase,
          firstBlock,
          firstValid: Number(view.getBigInt64(offset + 32, true)),
          lastValid: Number(view.getBigInt64(offset + 40, true)),
          validSamples: view.getBigUint64(offset + 48, true),
          expectedSamples: view.getBigUint64(offset + 56, true),
          presentBlocks,
        };
        if (!Number.isFinite(track.scale) || !track.scale ||
            !Number.isFinite(track.offset) ||
            track.presentCount > track.blockSlots) {
          return invalid;
        }
        tracks.push(track);
        if (!tracksByName[name]) tracksByName[name] = [];
        tracksByName[name].push(track);
        availableSignals |= 1 << signal;
      }

      const requestedSignals = view.getUint32(80, true);
      const missingRequired = view.getUint32(84, true);
      const missingOptional = view.getUint32(88, true);
      const requestedEventMask = view.getUint8(92);
      const missingEventMask = view.getUint8(93);
      const availableEventMask = view.getUint8(68);
      const sourceFlags = view.getUint8(69);
      const metricValid = view.getUint32(96, true);
      const metricStr = view.getUint32(100, true);
      const metricSummary = view.getUint32(104, true);
      const metrics = [
        ["average_leak", 20, view.getInt32(108, true) / 1000],
        ["ahi", 0, view.getInt32(112, true) / 1000],
        ["oa_index", 1, view.getInt32(116, true) / 1000],
        ["ca_index", 2, view.getInt32(120, true) / 1000],
        ["ua_index", 3, view.getInt32(124, true) / 1000],
        ["hypopnea_index", 4, view.getInt32(128, true) / 1000],
        ["arousal_index", 5, view.getInt32(132, true) / 1000],
        ["mask_pressure_50", 6, view.getInt32(136, true) / 1000],
        ["average_pressure", 21, view.getInt32(76, true) / 1000],
        ["ipap_50", 22, view.getInt32(216, true) / 1000],
        ["ipap_95", 23, view.getInt32(220, true) / 1000],
        ["leak_50", 7, view.getInt32(140, true) / 1000],
        ["duration_min", 8, view.getUint32(144, true)],
        ["mask_pressure_95", 9, view.getInt32(148, true) / 1000],
        ["leak_95", 10, view.getInt32(152, true) / 1000],
        ["minute_ventilation_50", 11, view.getInt32(156, true) / 1000],
        ["minute_ventilation_95", 12, view.getInt32(160, true) / 1000],
        ["respiratory_rate_50", 13, view.getInt32(164, true) / 1000],
        ["respiratory_rate_95", 14, view.getInt32(168, true) / 1000],
        ["tidal_volume_50", 15, view.getInt32(172, true) / 1000],
        ["tidal_volume_95", 16, view.getInt32(176, true) / 1000],
        ["spo2_median", 17, view.getInt32(180, true) / 1000],
        ["spo2_threshold_minutes", 18, view.getUint32(184, true)],
        ["csr_minutes", 19, view.getUint32(188, true)],
      ];

      const result = {
        valid: true,
        state: missingRequired || missingEventMask ? "partial" : "ready",
        error: "",
        source_revision: revisionValue.toString(16).padStart(16, "0"),
        generation,
        sleep_day_epoch: view.getInt32(16, true),
        day_start: dayStart,
        day_end: dayEnd,
        start: sessions.length ? sessions[0].start : dayStart,
        end: sessions.length ? sessions[sessions.length - 1].end : dayEnd,
        duration_min: Math.round(durationMs / 60000),
        missing_required: signalStorePopcount(missingRequired),
        missing_streams: signalStorePopcount(
          missingRequired | missingOptional),
        streams: signalStorePopcount(requestedSignals),
        events_available: !!availableEventMask,
        requested_event_mask: requestedEventMask,
        missing_event_mask: missingEventMask,
        source_flags: sourceFlags,
        sessions,
        stream_details: [],
        hypopnea_count: view.getUint32(192, true),
        ca_count: view.getUint32(196, true),
        oa_count: view.getUint32(200, true),
        ua_count: view.getUint32(204, true),
        arousal_count: view.getUint32(208, true),
        csr_count: view.getUint32(212, true),
        store: {tracks, tracksByName},
      };
      metrics.forEach(([field, index, value]) => {
        if (!(metricValid & (1 << index))) return;
        result[field] = value;
        result[field + "_source"] = signalStoreMetricSource(
          metricValid, metricStr, metricSummary, index);
      });
      if (result.ahi !== undefined && result.arousal_index !== undefined) {
        result.rdi = result.ahi + result.arousal_index;
      }

      SIGNAL_STORE_SIGNAL_NAMES.forEach((name, signal) => {
        if (!name || !(requestedSignals & (1 << signal))) return;
        const signalTracks = tracksByName[name] || [];
        result.stream_details.push({
          kind: "series",
          name,
          source: name,
          required: signal === 0 || signal === 5,
          complete: !!(availableSignals & (1 << signal)),
          provider: signalTracks.length && (sourceFlags & 1) ? "edf" :
            (signalTracks.length ? "spool" : "missing"),
          has_edf: signalTracks.length > 0 && !!(sourceFlags & 1),
          has_spool: signalTracks.length > 0 && !!(sourceFlags & 8),
          low_res: signalTracks.length > 0 && signalTracks.every(
            (track) => track.interval >= 60000),
        });
      });
      return result;
    }

    async function pollSignalStoreNight(token, nightId, signal) {
      const url = "/api/report/result?night=" + encodeURIComponent(nightId);
      const timeoutValue = {status: 0, result: null};
      return pollReportFetch({
        active: () => token === reportLoadToken,
        waitUrl: url,
        maxAttempts: REPORT_RESULT_POLL_MAX_ATTEMPTS,
        delayMs: REPORT_POLL_DELAY_MS,
        timeoutValue,
        request: () => {
          const cached = lruGet(reportResultClientCache, url);
          return AirCANnect.http.request(
            url, conditionalRequestOptions(cached, signal));
        },
        handle: async (response) => {
          if (response.status === 304) {
            const cached = lruGet(reportResultClientCache, url);
            if (!cached) throw new Error("report cache revalidation failed");
            return {done: true, value: {
              status: 304,
              result: cached.decoded,
            }};
          }
          if (response.status === 200) {
            const buffer = await response.arrayBuffer();
            if (signal.aborted || token !== reportLoadToken) return null;

            const decoded = decodeSignalStoreNight(buffer);
            if (!decoded.valid) throw new Error("invalid report metadata");

            const revision = reportArtifactRevision(response);
            const generation = Number(
              response.headers.get("X-Report-Generation") || 0);
            if ((revision && revision !== decoded.source_revision) ||
                (generation && generation !== decoded.generation)) {
              throw new Error("report metadata identity mismatch");
            }
            lruSet(reportResultClientCache, url, {
              etag: response.headers.get("ETag") || "",
              decoded,
            }, REPORT_RESULT_CLIENT_CACHE_MAX);
            return {done: true, value: {status: 200, result: decoded}};
          }
          if (response.status === 202) {
            timeoutValue.status = 202;
            AirCANnect.ui.message(
              "reportMsg", "Preparing report...", true, true);
            return {done: false, waitForReport: true};
          }
          if (response.status === 404) {
            return {done: true, value: {status: 404, result: null}};
          }

          const text = await response.text();
          if (response.status === 503 && isTransientReportError(text)) {
            return {done: false};
          }
          throw new Error(text || "report metadata request failed");
        },
      });
    }

    function reportSignalTracks(name, store = reportSignalStore) {
      return store && store.tracksByName && store.tracksByName[name] || [];
    }

    function signalStoreChartParts(definition) {
      if (!definition) return [];
      if (definition.type === "events") {
        return reportResult && reportResult.events_available ? ["events"] : [];
      }
      return (definition.series || [definition])
        .map((series) => series.key)
        .filter((name) => reportSignalTracks(name).length > 0);
    }

    function signalStoreExpandedChartKeys() {
      const visible = new Set(visibleReportChartOrder());
      return reportChartPreferences.order.filter((key) =>
        visible.has(key) && !reportChartPreferences.collapsed.has(key));
    }

    function signalStoreBaseLoadKeys(keys) {
      const result = Array.from(keys || []);
      if (result.some((key) => key !== "events") &&
          !result.includes("events") && reportResult.events_available) {
        result.unshift("events");
      }
      return result;
    }

    function signalStoreBlockPresent(track, slot) {
      return slot >= 0 && slot < track.blockSlots &&
        !!(track.presentBlocks[Math.floor(slot / 8)] & (1 << (slot % 8)));
    }

    function signalStoreLevel(track, spanMs) {
      const chartHost = document.getElementById("reportCharts");
      const width = Math.max(320,
        chartHost ? chartHost.clientWidth : window.innerWidth || 800);
      const targetCells = width * 2;
      const candidates = [{name: "raw", interval: track.interval}];
      if (track.lodMask & 1) candidates.push({name: "1s", interval: 1000});
      if (track.lodMask & 2) candidates.push({name: "10s", interval: 10000});
      candidates.sort((a, b) => a.interval - b.interval);
      return candidates.find((candidate) =>
        spanMs / candidate.interval <= targetCells) ||
        candidates[candidates.length - 1];
    }

    function signalStoreCacheKey(track, level, blockStart, context) {
      return [context.nightId, context.generation, track.signal,
        track.interval, track.trackIndex, level.name, blockStart].join(":");
    }

    function signalStoreTouchBlock(key, track, context) {
      const entry = reportSignalBlockCache.get(key);
      if (!entry) return null;

      if ((!entry.closed && entry.revision !== context.revision) ||
          entry.scale !== track.scale || entry.offset !== track.offset ||
          entry.missingValue !== track.missingValue ||
          entry.phase !== track.phase) {
        reportSignalBlockCache.delete(key);
        return null;
      }

      reportSignalBlockCache.delete(key);
      reportSignalBlockCache.set(key, entry);
      return entry;
    }

    function trimSignalStoreBlockCache() {
      let bytes = 0;
      reportSignalBlockCache.forEach((entry) => {
        bytes += entry.buffer.byteLength +
          SIGNAL_STORE_BLOCK_CACHE_ENTRY_OVERHEAD;
      });
      while (bytes > SIGNAL_STORE_BLOCK_CACHE_MAX_BYTES) {
        const key = reportSignalBlockCache.keys().next().value;
        if (key === undefined) break;
        bytes -= reportSignalBlockCache.get(key).buffer.byteLength +
          SIGNAL_STORE_BLOCK_CACHE_ENTRY_OVERHEAD;
        reportSignalBlockCache.delete(key);
      }
    }

    function signalStoreFirstSample(track, blockStart) {
      let remainder = blockStart % track.interval;
      if (remainder < 0) remainder += track.interval;
      let delta = track.phase - remainder;
      if (delta < 0) delta += track.interval;
      return blockStart + delta;
    }

    function decodeSignalStoreBlock(buffer, byteOffset, track, level,
                                    blockStart, blockBytes, lo, hi) {
      const view = new DataView(buffer, byteOffset, blockBytes);
      const points = [];
      const cells = SIGNAL_STORE_BLOCK_MS / level.interval;
      const firstSample = level.name === "raw"
        ? signalStoreFirstSample(track, blockStart) : blockStart;
      const firstCell = Math.max(0, level.name === "raw"
        ? Math.ceil((lo - firstSample) / level.interval)
        : Math.ceil((lo - firstSample) / level.interval) - 1);
      const endCell = Math.min(cells,
        Math.floor((hi - firstSample) / level.interval) + 1);
      // A leading missing cell must break the line from the preceding block.
      let haveValue = true;

      for (let i = firstCell; i < endCell; i++) {
        const offset = level.name === "raw" ? i * 2 : i * 4;
        const t = firstSample + i * level.interval;
        const minimum = view.getInt16(offset, true);
        const maximum = level.name === "raw"
          ? minimum : view.getInt16(offset + 2, true);
        if (minimum === track.missingValue ||
            maximum === track.missingValue ||
            t >= blockStart + SIGNAL_STORE_BLOCK_MS) {
          if (haveValue) points.push({gap: true, t});
          haveValue = false;
          continue;
        }

        const min = minimum * track.scale + track.offset;
        const max = maximum * track.scale + track.offset;
        if (level.name === "raw") {
          points.push({t, value: min});
        } else {
          points.push({
            t,
            end: t + level.interval,
            min: Math.min(min, max),
            max: Math.max(min, max),
            value: (min + max) / 2,
            envelope: true,
          });
        }
        haveValue = true;
      }
      return points;
    }

    async function fetchSignalStoreBlocks(track, level, from, to, context) {
      const blockCount = (to - from) / SIGNAL_STORE_BLOCK_MS;
      let url = "/api/report/plot?night=" +
        encodeURIComponent(context.nightId) + "&part=signal&track=" +
        track.metadataIndex + "&from=" + from + "&to=" + to +
        "&level=" + encodeURIComponent(level.name);
      const loaded = await pollReportFetch({
        active: context.active,
        waitUrl: url,
        maxAttempts: REPORT_SIGNAL_POLL_MAX_ATTEMPTS,
        delayMs: REPORT_POLL_DELAY_MS,
        request: () => AirCANnect.http.request(url, {
          cache: "no-store",
          signal: context.signal,
        }),
        handle: async (response) => {
          if (response.status === 202) {
            return {done: false, waitForReport: true};
          }
          if (response.status !== 200 && response.status !== 204) {
            const text = await response.text();
            if (response.status === 503 && isTransientReportError(text)) {
              return {done: false};
            }
            throw new Error(text || "report signal request failed");
          }

          const revision = reportArtifactRevision(response);
          const generation = Number(
            response.headers.get("X-Report-Generation") || 0);
          const responseTrack = Number(
            response.headers.get("X-Report-Track"));
          const present = response.headers.get("X-Report-Present-Blocks") || "";
          const interval = Number(
            response.headers.get("X-Report-Interval-Ms") || 0);
          const envelope = response.headers.get("X-Report-Envelope") === "1";
          if ((revision && revision !== context.revision) ||
              generation !== context.generation ||
              responseTrack !== track.trackIndex ||
              present.length !== blockCount || /[^01]/.test(present) ||
              interval !== level.interval ||
              envelope !== (level.name !== "raw")) {
            throw new Error("report signal identity mismatch");
          }

          const buffer = response.status === 204
            ? new ArrayBuffer(0) : await response.arrayBuffer();
          if (!context.active()) return null;

          const blockBytes = SIGNAL_STORE_BLOCK_MS / level.interval *
            (envelope ? 4 : 2);
          const presentCount = Array.from(present)
            .filter((value) => value === "1").length;
          if (buffer.byteLength !== presentCount * blockBytes) {
            throw new Error("invalid report signal payload");
          }

          for (let i = 0; i < blockCount; i++) {
            const blockStart = from + i * SIGNAL_STORE_BLOCK_MS;
            const slot = (blockStart - track.firstBlock) / SIGNAL_STORE_BLOCK_MS;
            if ((present[i] === "1") !== signalStoreBlockPresent(track, slot)) {
              throw new Error("report signal coverage mismatch");
            }
          }

          let packedOffset = 0;
          for (let i = 0; i < blockCount; i++) {
            if (present[i] !== "1") continue;

            const blockStart = from + i * SIGNAL_STORE_BLOCK_MS;
            const key = signalStoreCacheKey(track, level, blockStart, context);
            reportSignalBlockCache.set(key, {
              buffer: buffer.slice(packedOffset, packedOffset + blockBytes),
              revision: context.revision,
              closed: blockStart + SIGNAL_STORE_BLOCK_MS <= track.lastValid,
              scale: track.scale,
              offset: track.offset,
              missingValue: track.missingValue,
              phase: track.phase,
            });
            packedOffset += blockBytes;
          }
          trimSignalStoreBlockCache();
          return {done: true, value: true};
        },
      });
      if (!loaded && context.active()) {
        throw new Error("report signal not ready");
      }
      return loaded;
    }

    async function ensureSignalStoreTrackBlocks(track, level, from, to,
                                                context) {
      if (!context.active()) return;

      const runs = [];
      let run = null;
      for (let block = from; block < to; block += SIGNAL_STORE_BLOCK_MS) {
        const slot = (block - track.firstBlock) / SIGNAL_STORE_BLOCK_MS;
        const key = signalStoreCacheKey(track, level, block, context);
        if (!signalStoreBlockPresent(track, slot)) continue;

        if (signalStoreTouchBlock(key, track, context)) {
          run = null;
          continue;
        }

        if (!run || block - run.from >=
            SIGNAL_STORE_MAX_BLOCKS * SIGNAL_STORE_BLOCK_MS) {
          run = {from: block, to: block + SIGNAL_STORE_BLOCK_MS};
          runs.push(run);
        } else {
          run.to = block + SIGNAL_STORE_BLOCK_MS;
        }
      }

      await runReportFetchJobs(runs.map((run) => () =>
        fetchSignalStoreBlocks(
          track, level, run.from, run.to, context)), 1);
    }

    function appendSignalStorePoints(target, source, blockStart) {
      if (!source.length) {
        if (target.length && !target[target.length - 1].gap) {
          target.push({gap: true, t: blockStart});
        }
        return;
      }
      source.forEach((point) => {
        const previous = target.length ? target[target.length - 1] : null;
        if (point.gap && previous && previous.gap) return;
        if (previous && !point.gap && !previous.gap &&
            point.t === previous.t) {
          target[target.length - 1] = point;
          return;
        }
        target.push(point);
      });
    }

    async function loadSignalStoreTrack(track, lo, hi, context) {
      const trackEnd = track.firstBlock +
        track.blockSlots * SIGNAL_STORE_BLOCK_MS;
      const from = Math.max(track.firstBlock,
        Math.floor(lo / SIGNAL_STORE_BLOCK_MS) * SIGNAL_STORE_BLOCK_MS);
      const to = Math.min(trackEnd,
        Math.ceil(hi / SIGNAL_STORE_BLOCK_MS) * SIGNAL_STORE_BLOCK_MS);
      if (!(to > from)) return [];

      const level = signalStoreLevel(track, hi - lo);
      await ensureSignalStoreTrackBlocks(track, level, from, to, context);
      if (!context.active()) return [];

      const points = [];
      for (let block = from; block < to; block += SIGNAL_STORE_BLOCK_MS) {
        const slot = (block - track.firstBlock) / SIGNAL_STORE_BLOCK_MS;
        const present = signalStoreBlockPresent(track, slot);
        const entry = present ? signalStoreTouchBlock(
          signalStoreCacheKey(track, level, block, context), track, context) : null;

        if (present && !entry) throw new Error("report signal block unavailable");

        const visible = entry ? decodeSignalStoreBlock(
          entry.buffer, 0, track, level, block, entry.buffer.byteLength, lo, hi) : [];

        appendSignalStorePoints(points, visible, Math.max(block, lo));
      }

      if (context.prefetchJobs) {
        const margin = SIGNAL_STORE_PREFETCH_BLOCKS * SIGNAL_STORE_BLOCK_MS;
        const ranges = [
          {from: Math.max(track.firstBlock, from - margin), to: from},
          {from: to, to: Math.min(trackEnd, to + margin)},
        ];
        ranges.filter((range) => range.to > range.from).forEach((range) => {
          context.prefetchJobs.push(() => ensureSignalStoreTrackBlocks(
            track, level, range.from, range.to, context));
        });
      }
      return points;
    }

    function mergeSignalStoreTracks(series) {
      const points = series.flat().sort((a, b) => a.t - b.t);
      const merged = [];
      points.forEach((point) => {
        const previous = merged.length ? merged[merged.length - 1] : null;
        if (point.gap && previous && previous.gap) return;
        if (previous && !point.gap && !previous.gap &&
            point.t === previous.t) {
          merged[merged.length - 1] = point;
        } else {
          merged.push(point);
        }
      });
      return merged;
    }

    function decodeSignalStoreEvents(buffer, expected) {
      const bytes = new Uint8Array(buffer);
      const view = new DataView(buffer);
      if (buffer.byteLength < SIGNAL_STORE_EVENT_HEADER_BYTES ||
          !signalStoreMagic(bytes, "ACREVT01") ||
          view.getUint16(8, true) !== 1 ||
          view.getUint16(10, true) !== SIGNAL_STORE_EVENT_HEADER_BYTES ||
          view.getUint32(12, true) !== buffer.byteLength ||
          view.getUint32(20, true) !== expected.generation ||
          view.getBigUint64(24, true).toString(16).padStart(16, "0") !==
            expected.revision) {
        throw new Error("invalid report events");
      }

      const blockCount = view.getUint16(40, true);
      const eventCount = view.getUint32(44, true);
      const recordsOffset = SIGNAL_STORE_EVENT_HEADER_BYTES + blockCount * 8;
      if (!blockCount || blockCount > SIGNAL_STORE_MAX_BLOCKS ||
          recordsOffset + eventCount * 16 !== buffer.byteLength) {
        throw new Error("invalid report events");
      }

      const events = [];
      for (let i = 0; i < eventCount; i++) {
        const offset = recordsOffset + i * 16;
        events.push({
          t: Number(view.getBigInt64(offset, true)),
          duration: view.getInt32(offset + 8, true),
          code: view.getUint16(offset + 12, true),
          flags: view.getUint16(offset + 14, true),
        });
      }
      return events;
    }

    async function loadSignalStoreEvents(context) {
      if (!context.active()) return [];

      const key = [context.nightId, context.generation,
        context.revision, "events"].join(":");
      const cached = lruGet(reportEventClientCache, key);
      if (cached) return cached;

      const url = "/api/report/plot?night=" +
        encodeURIComponent(context.nightId) + "&part=events";
      const events = await pollReportFetch({
        active: context.active,
        waitUrl: url,
        maxAttempts: REPORT_SIGNAL_POLL_MAX_ATTEMPTS,
        delayMs: REPORT_POLL_DELAY_MS,
        request: () => AirCANnect.http.request(url, {
          cache: "no-store",
          signal: context.signal,
        }),
        handle: async (response) => {
          if (response.status === 202) {
            return {done: false, waitForReport: true};
          }
          if (response.status !== 200) {
            const text = await response.text();
            if (response.status === 503 && isTransientReportError(text)) {
              return {done: false};
            }
            throw new Error(text || "report events request failed");
          }
          return {done: true, value: decodeSignalStoreEvents(
            await response.arrayBuffer(), context)};
        },
      });
      if (!context.active()) return [];
      if (!events) throw new Error("report events not ready");

      lruSet(reportEventClientCache, key, events,
        SIGNAL_STORE_EVENT_CACHE_MAX);
      return events;
    }

    async function fetchSignalStoreChart(definition, lo, hi, context) {
      if (definition.type === "events") {
        return {events: await loadSignalStoreEvents(context), series: {}};
      }

      const decoded = {events: [], series: {}};
      for (const series of definition.series || [definition]) {
        const tracks = reportSignalTracks(series.key, context.store);
        const values = [];
        for (const track of tracks) {
          values.push(await loadSignalStoreTrack(
            track, lo, hi, context));
          if (!context.active()) return null;
        }
        if (tracks.length) {
          decoded.series[series.key] = mergeSignalStoreTracks(values);
        }
      }
      return decoded;
    }

    function publishSignalStoreBaseChart(key, decoded, render = true) {
      const definition = reportChartDefinition(key);
      if (!definition || !decoded) return;
      if (definition.type === "events") {
        reportBaseEvents = decoded.events || [];
        if (!reportZoom || !signalStoreRangeChartReady("events")) {
          reportEvents = reportBaseEvents;
        }
      } else {
        Object.keys(decoded.series || {}).forEach((name) => {
          reportBaseSeries[name] = decoded.series[name];
          if (!reportZoom || !signalStoreRangeChartReady(key)) {
            reportSeries[name] = decoded.series[name];
          }
        });
      }
      reportBaseLoadedCharts.add(key);
      if (render && !updateRenderedReportChart(key)) renderReportCharts();
    }

    function loadSignalStoreBaseChart(key, token, signal) {
      if (reportBaseLoadedCharts.has(key)) return Promise.resolve(true);
      const pending = reportBaseChartPromises.get(key);
      if (pending) return pending;

      const definition = reportChartDefinition(key);
      const range = reportRange();
      if (!definition || !validReportRange(range)) return Promise.resolve(false);
      const context = {
        nightId: reportCurrentNightId,
        revision: reportCurrentRevision,
        generation: reportCurrentGeneration,
        store: reportSignalStore,
        signal,
        active: () => !signal.aborted && token === reportLoadToken &&
          context.nightId === reportCurrentNightId &&
          context.generation === reportCurrentGeneration &&
          context.revision === reportCurrentRevision,
      };
      const promise = fetchSignalStoreChart(
        definition, range.start, range.end, context)
        .then((decoded) => {
          if (!decoded || !context.active()) return false;
          publishSignalStoreBaseChart(key, decoded);
          return true;
        })
        .finally(() => {
          if (reportBaseChartPromises.get(key) === promise) {
            reportBaseChartPromises.delete(key);
          }
        });
      reportBaseChartPromises.set(key, promise);
      return promise;
    }

    async function loadSignalStoreBase(token, nightId, signal) {
      activateSignalStoreNight(
        nightId,
        reportResult.source_revision,
        reportResult.generation,
        reportResult.store);
      renderReportSummary();
      renderReportCharts();

      const keys = signalStoreBaseLoadKeys(signalStoreExpandedChartKeys());
      await runReportFetchJobs(keys.map((key) => () =>
        loadSignalStoreBaseChart(key, token, signal)), 2);
      return token === reportLoadToken;
    }

    async function ensureSignalStoreChartLoaded(key) {
      if (!reportSignalStore || !reportCurrentNightId) return;
      const token = reportLoadToken;
      let controller = reportLoadAbortController;
      let ownsController = false;
      if (!controller) {
        controller = new AbortController();
        reportLoadAbortController = controller;
        ownsController = true;
      }
      try {
        const keys = signalStoreBaseLoadKeys([key]);
        await runReportFetchJobs(keys.map((chartKey) =>
          () => loadSignalStoreBaseChart(
            chartKey, token, controller.signal)), 2);
        if (reportZoom && token === reportLoadToken) {
          ensureSignalStoreRangeLoaded(
            reportZoom.start, reportZoom.end, [key]);
        }
      } catch (error) {
        if (token === reportLoadToken && !controller.signal.aborted) {
          AirCANnect.ui.message("reportMsg", error.message, false, true);
        }
      } finally {
        if (ownsController && reportLoadAbortController === controller) {
          reportLoadAbortController = null;
        }
      }
    }

    function signalStoreRangeKey(from, to) {
      return [reportCurrentNightId, reportCurrentGeneration, from, to].join(":");
    }

    function currentSignalStoreRangeKey() {
      if (!reportZoom || !reportCurrentNightId || !reportCurrentGeneration) {
        return "";
      }
      const range = reportRangeWindow(reportZoom.start, reportZoom.end);
      return range ? signalStoreRangeKey(range.from, range.to) : "";
    }

    function signalStoreRangeChartReady(key) {
      const current = currentSignalStoreRangeKey();
      return !!(current && reportRangeView && reportRangeView.key === current &&
        reportRangeView.loadedCharts.has(key));
    }

    function applySignalStoreRangeChart(entry, key) {
      if (!entry || entry.key !== currentSignalStoreRangeKey()) return;
      const definition = reportChartDefinition(key);
      if (!definition) return;
      if (definition.type === "events") {
        reportEvents = entry.events || reportBaseEvents;
      } else {
        (definition.series || [definition]).forEach((series) => {
          if (Object.prototype.hasOwnProperty.call(entry.series, series.key)) {
            reportSeries[series.key] = entry.series[series.key];
          }
        });
      }
    }

    function startSignalStoreRangeWorker(entry) {
      if (!entry || entry.promise || !entry.requestedCharts.size) return;
      const controller = reportRangeAbortController || new AbortController();
      reportRangeAbortController = controller;
      const token = ++reportRangeToken;
      const context = {
        nightId: reportCurrentNightId,
        revision: reportCurrentRevision,
        generation: reportCurrentGeneration,
        store: reportSignalStore,
        prefetchJobs: [],
        signal: controller.signal,
        active: () => !controller.signal.aborted &&
          token === reportRangeToken &&
          entry.key === reportRangeActiveKey &&
          context.nightId === reportCurrentNightId &&
          context.generation === reportCurrentGeneration &&
          context.revision === reportCurrentRevision,
      };
      const promise = (async () => {
        while (context.active()) {
          const keys = Array.from(entry.requestedCharts)
            .filter((key) => !entry.loadedCharts.has(key));
          entry.requestedCharts.clear();
          if (!keys.length) break;

          await runReportFetchJobs(keys.map((key) => async () => {
            const definition = reportChartDefinition(key);
            if (!definition) return;
            const decoded = await fetchSignalStoreChart(
              definition, entry.from, entry.to, context);
            if (!decoded || !context.active()) return;

            if (definition.type === "events") {
              entry.events = decoded.events;
            } else {
              Object.assign(entry.series, decoded.series);
            }
            entry.loadedCharts.add(key);
            applySignalStoreRangeChart(entry, key);
            if (!updateRenderedReportChart(key)) renderReportCharts();
          }), 2);
        }

        // All requested charts are published before adjacent blocks compete for I/O.
        if (context.active()) {
          void runReportFetchJobs(context.prefetchJobs, 1).catch((error) => {
            if (context.active()) console.warn("Report prefetch failed", error);
          });
        }
      })().catch((error) => {
        if (context.active()) {
          AirCANnect.ui.message("reportMsg", error.message, false, true);
        }
      }).finally(() => {
        const restart = context.active() && entry.requestedCharts.size > 0;
        if (entry.promise === promise) entry.promise = null;
        if (restart) startSignalStoreRangeWorker(entry);
      });
      entry.promise = promise;
    }

    function ensureSignalStoreRangeLoaded(lo, hi, requestedKeys) {
      if (!reportCurrentNightId || !reportCurrentGeneration) return;
      const range = reportRangeWindow(lo, hi);
      if (!range) return;
      const key = signalStoreRangeKey(range.from, range.to);
      if (reportRangeActiveKey !== key) {
        cancelReportRangeRequest();
        reportRangeActiveKey = key;
        reportRangeView = {
          key,
          from: range.from,
          to: range.to,
          series: {},
          events: reportBaseEvents,
          loadedCharts: new Set(),
          requestedCharts: new Set(),
          promise: null,
        };
        reportSeries = {...reportBaseSeries};
        reportEvents = reportBaseEvents;
      }

      const entry = reportRangeView;
      const keys = requestedKeys || signalStoreExpandedChartKeys();
      keys.forEach((chartKey) => {
        if (entry.loadedCharts.has(chartKey)) {
          applySignalStoreRangeChart(entry, chartKey);
        } else {
          entry.requestedCharts.add(chartKey);
        }
      });
      startSignalStoreRangeWorker(entry);
    }

    async function loadSelectedReportNight(preserveView = false) {
      cancelReportLoadRequest();
      const night = selectedReportNight();
      if (!night) {
        resetReportData();
        renderReportSummary();
        return;
      }

      const controller = new AbortController();
      reportLoadAbortController = controller;
      const nightId = reportNightLoadKey(night);
      const keepView = preserveView && nightId === reportCurrentNightId &&
        !!reportResult;
      const savedZoom = keepView ? reportZoom : null;
      const savedHiddenSessions = keepView ? [...reportHiddenSessions] : [];
      const token = ++reportLoadToken;
      if (!keepView) {
        resetReportData();
        renderReportSummary();
      }

      AirCANnect.ui.message("reportMsg", "Loading report...", true, true);
      try {
        const res = await pollSignalStoreNight(
          token, nightId, controller.signal);
        if (!res || token !== reportLoadToken) return;
        if (res.status === 404) {
          AirCANnect.ui.message("reportMsg", "Night not found", false, true);
          return;
        }
        if (res.status === 202 && !res.result) {
          AirCANnect.ui.message("reportMsg", "Preparing report...", true, true);
          return;
        }
        if ((res.status !== 200 && res.status !== 304) || !res.result) {
          AirCANnect.ui.message("reportMsg", "Report not ready", false, true);
          renderReportSummary();
          return;
        }
        if (res.result.state !== "ready" && res.result.state !== "partial") {
          AirCANnect.ui.message(
            "reportMsg", res.result.error || "Report incomplete", false, true);
          renderReportSummary();
          return;
        }

        const changed = !keepView ||
          res.result.source_revision !== reportCurrentRevision ||
          res.result.generation !== reportCurrentGeneration;
        if (changed) {
          if (keepView) resetReportData();
          reportResult = res.result;
          reportZoom = savedZoom;
          savedHiddenSessions.forEach((key) => reportHiddenSessions.add(key));

          const loaded = await loadSignalStoreBase(
            token, nightId, controller.signal);
          if (!loaded || token !== reportLoadToken) return;
        } else {
          reportBaseChartPromises.clear();
          if (reportRangeView) reportRangeView.promise = null;

          await runReportFetchJobs(signalStoreExpandedChartKeys().map((key) =>
            () => ensureSignalStoreChartLoaded(key)), 2);
          if (token !== reportLoadToken) return;
        }

        renderReportSummary();
        AirCANnect.ui.message("reportMsg",
          reportResult.state === "partial"
            ? "Report loaded (incomplete - some data missing)"
            : "Report loaded", true);
      } catch (error) {
        if (token !== reportLoadToken) return;
        AirCANnect.ui.message("reportMsg", error.message, false, true);
      } finally {
        if (reportLoadAbortController === controller) {
          reportLoadAbortController = null;
        }
      }
    }

    function scheduleReportPoll(loadNight) {
      if (reportPollTimer) clearTimeout(reportPollTimer);
      reportPollTimer = setTimeout(() => loadReportSummary(loadNight), 750);
    }

    async function loadReportSummary(loadNight) {
      try {
        const headers = {};
        if (reportSummaryEtag) {
          headers["If-None-Match"] = reportSummaryEtag;
        }
        const response = await AirCANnect.http.request("/api/report/summary",
          {cache: "no-cache", headers});
        if (response.status === 202) {
          AirCANnect.ui.message("reportMsg", "Preparing report index...", true, true);
          scheduleReportPoll(loadNight);
          return;
        }
        if (response.status === 200) {
          reportSummary = await response.json();
          reportSummaryEtag = response.headers.get("ETag") || "";
        } else if (response.status !== 304 || !reportSummary) {
          throw new Error(await response.text() || "report summary failed");
        }

        renderReportSummary();
        if (!reportNightsNewestFirst().length) {
          AirCANnect.ui.message("reportMsg", "No nights", true);
          return;
        }
        if (loadNight) await loadSelectedReportNight();
      } catch (error) {
        AirCANnect.ui.message("reportMsg", error.message, false, true);
      }
    }

    async function refreshReportSummary(loadNight) {
      await loadReportSummary(loadNight !== false);
    }

    function reportCacheUrlNight(url) {
      try {
        return new URL(url, window.location.href).searchParams.get("night") || "";
      } catch (error) {
        return "";
      }
    }

    function invalidateReportNightCache(nightId, rebuild = true) {
      for (const key of reportResultClientCache.keys()) {
        if (reportCacheUrlNight(key) === nightId) {
          reportResultClientCache.delete(key);
        }
      }
      for (const key of reportSignalBlockCache.keys()) {
        if (rebuild && key.startsWith(nightId + ":")) {
          reportSignalBlockCache.delete(key);
        }
      }
      for (const key of reportEventClientCache.keys()) {
        if (key.startsWith(nightId + ":")) reportEventClientCache.delete(key);
      }
    }

    function handleReportCompletion(data) {
      const serial = Number(data && data.serial) || 0;
      if (!serial) return;

      const completionKey = [
        serial,
        data.night || "",
        data.kind || "",
        data.from || "",
        data.to || "",
        data.success ? 1 : 0,
        data.forced ? 1 : 0,
        data.generation || "",
        data.error || "",
      ].join(":");
      if (completionKey === reportHandledCompletionKey) return;
      reportHandledCompletionKey = completionKey;
      if (data.kind !== "night" || !data.night) return;

      const nightId = String(data.night);
      const active = AirCANnect.pages.isActive("report");
      const selected = selectedReportNight();
      const selectedNightId = selected ? String(selected.id) : "";
      if (!data.success) {
        if (active && !reportResult && nightId === selectedNightId &&
            data.error !== "cancelled") {
          AirCANnect.ui.message(
            "reportMsg", data.error || "Report failed", false, true);
        }
        return;
      }
      const current = nightId === reportCurrentNightId;
      const reload = active && (current ||
        (!reportResult && nightId === selectedNightId));
      invalidateReportNightCache(nightId, !!data.forced);
      if (current) cancelReportRequests(!data.forced);
      if (reload) loadSelectedReportNight(current && !!reportResult);
    }

    AirCANnect.events.subscribe("report", handleReportCompletion);
    AirCANnect.actions.register("report.step-night", (_event, element) =>
      stepReportNight(Number(element.dataset.value)));
    AirCANnect.actions.register("report.toggle-calendar", () =>
      toggleReportCalendar());
    AirCANnect.actions.register("report.latest", () =>
      selectLatestReportNight());
    AirCANnect.actions.register("report.step-month", (_event, element) =>
      stepReportCalMonth(Number(element.dataset.value)));
    AirCANnect.actions.register("report.zoom", (_event, element) =>
      zoomReportWindow(Number(element.dataset.value)));
    AirCANnect.actions.register("report.reset-zoom", () => resetReportZoom());
    AirCANnect.pages.onLoad("report", (refresh) => {
      if (refresh) {
        refreshReportSummary(true);
      } else {
        loadReportSummary(true);
        scheduleReportDrawAfterReveal();
      }
    });
    AirCANnect.pages.onLeave("report", () => cancelReportRequests());
    window.addEventListener("resize", () => scheduleReportDraw());

    let reportSummary = null;
    let reportPollTimer = null;
    let reportLoadToken = 0;
    let reportLoadAbortController = null;
    let reportSummaryEtag = "";
    let reportResult = null;
    const reportResultClientCache = new Map();
    let reportSeries = {};
    let reportEvents = [];
    const reportHiddenSessions = new Set();
    let reportDrawItems = [];
    let reportDrawPending = false;
    let reportDrawRetryCount = 0;
    let reportResizeObserver = null;
    let reportZoom = null;
    let reportBaseSeries = {};
    let reportBaseEvents = [];
    let reportSignalStore = null;
    const reportBaseLoadedCharts = new Set();
    const reportBaseChartPromises = new Map();
    let reportCurrentNightId = "";
    let reportCurrentRevision = "";
    let reportCurrentGeneration = 0;
    let reportHandledCompletionKey = "";
    const reportSignalBlockCache = new Map();
    const reportEventClientCache = new Map();
    let reportRangeView = null;
    let reportRangeActiveKey = "";
    let reportRangeToken = 0;
    let reportRangeAbortController = null;
    let reportHoverTime = null;
    let reportDrag = null;
    let reportSelectedNightId = "";
    let reportCalView = null;

    const SVG_NS = "http:" + "/" + "/www.w3.org/2000/svg";
    const REPORT_RESULT_CLIENT_CACHE_MAX = 8;
    const SIGNAL_STORE_BLOCK_MS = 15 * 60 * 1000;
    const SIGNAL_STORE_NIGHT_HEADER_BYTES = 224;
    const SIGNAL_STORE_SESSION_BYTES = 16;
    const SIGNAL_STORE_TRACK_BYTES = 88;
    const SIGNAL_STORE_EVENT_HEADER_BYTES = 96;
    const SIGNAL_STORE_BITMAP_BYTES = 16;
    const SIGNAL_STORE_MAX_BLOCKS = 128;
    const SIGNAL_STORE_BLOCK_CACHE_MAX_BYTES = 8 * 1024 * 1024;
    const SIGNAL_STORE_BLOCK_CACHE_ENTRY_OVERHEAD = 256;
    const SIGNAL_STORE_EVENT_CACHE_MAX = 8;
    const SIGNAL_STORE_PREFETCH_BLOCKS = 2;
    const SIGNAL_STORE_SIGNAL_NAMES = [
      "flow",
      "inspiratory_pressure",
      "expiratory_pressure",
      "leak",
      "minute_ventilation",
      "mask_pressure",
      "inspiratory_duration",
      "respiratory_rate",
      "ie_ratio",
      "flow_limitation",
      "",
      "snore",
      "tidal_volume",
      "spo2",
      "pulse",
    ];
    const REPORT_RESULT_POLL_MAX_ATTEMPTS = 160;
    const REPORT_SIGNAL_POLL_MAX_ATTEMPTS = 120;
    const REPORT_POLL_DELAY_MS = 300;
    const REPORT_SSE_FALLBACK_MS = 1000;
    const REPORT_CHART_PREFERENCES_KEY = "aircannect.reportCharts.v1";
    const reportChartDefs = [
      {key: "events", title: "Event Flags", type: "events"},
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
      {key: "snore", title: "Snore", color: "#38bdf8", unit: ""},
      {
        key: "tidal_volume",
        title: "Tidal Volume",
        color: "#c084fc",
        unit: "L",
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
      {
        key: "spo2",
        title: "SpO2",
        color: "#22c55e",
        unit: "%",
        optional: true,
      },
      {
        key: "pulse",
        title: "Pulse",
        color: "#ef4444",
        unit: "bpm",
        optional: true,
      },
    ];
    let reportChartPreferences = {
      order: reportChartDefs.map((definition) => definition.key),
      collapsed: new Set(),
    };
    loadReportChartPreferences();

    const reportEventDefs = [
      {code: 7, key: "CSR", label: "CSR", color: "#2563eb"},
      {code: 3, key: "CA", label: "Central Apnea", color: "#d946ef"},
      {code: 4, key: "OA", label: "Obstructive Apnea", color: "#22d3ee"},
      {code: 5, key: "UA", label: "Apnea", color: "#a78bfa"},
      {code: 2, key: "H", label: "Hypopnea", color: "#fb923c"},
      {code: 6, key: "Ar", label: "Arousal", color: "#cbd5e1"},
    ];
    const reportEventCountFields = [
      {key: "CA", field: "ca_count"},
      {key: "OA", field: "oa_count"},
      {key: "UA", field: "ua_count"},
      {key: "H", field: "hypopnea_count"},
      {key: "Ar", field: "arousal_count"},
    ];
})();
