    function fmtDuration(seconds) {
      const value = Number(seconds);
      if (!Number.isFinite(value) || value < 0) return "";
      if (value < 90) return Math.ceil(value) + "s";
      if (value < 3600) return Math.ceil(value / 60) + "m";
      const hours = Math.floor(value / 3600);
      const minutes = Math.ceil((value - hours * 3600) / 60);
      return hours + "h " + minutes + "m";
    }

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
        pad2(date.getMonth() + 1) + "-" +
        pad2(date.getDate());
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
          reportPlotSection(item.key));
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
        ensureReportChartLoaded(key);
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

    loadReportChartPreferences();

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
      return pad2(date.getHours()) + ":" + pad2(date.getMinutes());
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
      reportBasePlotIndex = null;
      reportBaseLoadedCharts.clear();
      reportBaseChartPromises.clear();
      reportCurrentNightId = "";
      reportCurrentRevision = "";
      reportCurrentPlotEtag = "";
      reportRangeCache.clear();
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

    function activateReportBasePlot(nightId, revision, etag, index) {
      cancelReportRangeRequest();
      reportSeries = {};
      reportEvents = [];
      reportBaseSeries = {};
      reportBaseEvents = [];
      reportBasePlotIndex = index || null;
      reportBaseLoadedCharts.clear();
      reportBaseChartPromises.clear();
      reportCurrentNightId = String(nightId || "");
      reportCurrentRevision = String(revision || "");
      reportCurrentPlotEtag = String(etag || "");
      reportRangeCache.clear();
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
      if (measuredWidth <= 0 && reportTabActive()) measuredWidth = 800;
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
      return Math.max(4, Math.min(12, Math.round(graphW / 110)));
    }

    // Vertical gridlines + time labels along the x-axis, shared by every chart
    // so they line up. Drawn before the data so traces/marks sit on top.
    function drawReportTimeAxis(svg, pad, graphW, graphH, height, start, end) {
      const ticks = reportTimeTickCount(graphW);
      for (let i = 0; i <= ticks; i++) {
        const frac = i / ticks;
        const x = pad.left + graphW * frac;
        svg.appendChild(svgNode("line", {
          x1: x.toFixed(1),
          y1: String(pad.top),
          x2: x.toFixed(1),
          y2: (pad.top + graphH).toFixed(1),
          stroke: "#1f2633",
          "stroke-width": "1",
          "stroke-dasharray": "2 5",
        }));
        svg.appendChild(svgText(fmtReportTime(start + (end - start) * frac),
          x, height - 5, {"text-anchor": "middle"}));
      }
    }

    function drawReportChart(svg,
                             seriesList,
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
      svg._geom = {pad, graphW, width, height};
      const cursor = svgNode("line", {
        x1: "0", x2: "0",
        y1: String(pad.top), y2: (pad.top + graphH).toFixed(1),
        stroke: "#7c8595", "stroke-width": "1", "stroke-dasharray": "3 3",
        "pointer-events": "none", visibility: "hidden",
      });
      svg.appendChild(cursor);
      svg._cursor = cursor;
      svg._timeTooltip = createReportTimeTooltip(svg);
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
      const cursor = svgNode("line", {
        x1: "0", x2: "0",
        y1: String(pad.top), y2: (pad.top + graphH).toFixed(1),
        stroke: "#7c8595", "stroke-width": "1", "stroke-dasharray": "3 3",
        "pointer-events": "none", visibility: "hidden",
      });
      svg.appendChild(cursor);
      svg._cursor = cursor;
      svg._timeTooltip = createReportTimeTooltip(svg);
      return true;
    }

    function fmtReportClock(ms) {
      const d = new Date(Number(ms));
      if (Number.isNaN(d.getTime())) return "--";
      return pad2(d.getHours()) + ":" + pad2(d.getMinutes()) + ":" +
        pad2(d.getSeconds());
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
        reportZoom = {start: lo, end: hi};
        renderReportCharts();
      }
    }
    document.addEventListener("mouseup", reportFinishDrag);

    function validReportRange(range) {
      return range && Number.isFinite(range.start) && Number.isFinite(range.end) &&
        range.end - range.start > 60000;
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
      renderReportCharts();
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
      renderReportCharts();
    }

    document.addEventListener("keydown", (ev) => {
      if (!reportTabActive() || !reportZoom) return;
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
      if (retry && reportTabActive()) {
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
      if (!reportChartPreferences.collapsed.has(definition.key) &&
          (!reportBaseLoadedCharts.has(definition.key) ||
           (reportZoom && !currentReportRangeChartReady(definition.key)))) {
        const badge = document.createElement("span");
        badge.className = "report-res-badge";
        badge.textContent = "loading";
        name.appendChild(badge);
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
        type: "events",
        canvas,
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
      if (reportZoom) ensureReportRangeLoaded(reportZoom.start, reportZoom.end);
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
        const availableParts = reportChartPartNames(def, reportBasePlotIndex);
        const seriesList = seriesDefs.map((seriesDef) => ({
          label: seriesDef.label || def.title,
          color: seriesDef.color || def.color,
          points: (reportSeries[seriesDef.key] || []).slice()
            .sort((a, b) => a.t - b.t),
        })).filter((series) => series.points.length > 0);
        if (!seriesList.length) {
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
          if (availableParts.length &&
              !reportBaseLoadedCharts.has(def.key)) {
            cnote.textContent = "loading...";
          } else {
            cnote.textContent =
              reportResult && reportResult.missing_required > 0
                ? "backfilling..."
                : "not retained for this night";
          }
          ctitle.appendChild(cnote);
          appendReportChartActions(ctitle, def.key);
          card.appendChild(ctitle);
          if (reportChartPreferences.collapsed.has(def.key)) {
            card.classList.add("collapsed");
          }
          container.appendChild(card);
          return;
        }
        const extentSeriesList = seriesDefs.map((seriesDef) => {
          const base = reportBaseSeries[seriesDef.key] || [];
          const source = base.length ? base : reportSeries[seriesDef.key] || [];
          return source.filter((p) =>
            p && !p.gap && reportPointOverlapsAnyRange(p, sessionRanges));
        }).filter((points) => points.length > 0);
        const extent = reportSeriesExtent(extentSeriesList);

        const card = document.createElement("div");
        card.className = "report-chart";
        const title = document.createElement("div");
        title.className = "report-chart-title";
        const name = document.createElement("span");
        name.textContent = def.title;
        const rangePending = !!reportZoom &&
          !currentReportRangeChartReady(def.key);
        if (rangePending) {
          const badge = document.createElement("span");
          badge.className = "report-res-badge";
          badge.textContent = "loading";
          name.appendChild(badge);
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
          type: "series",
          canvas,
          seriesList,
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
      up("reportDuration", durationMin ? fmtMinutes(durationMin) : "--");
      up("reportAhi", displayable && Number.isFinite(Number(reportResult.ahi)) ?
        Number(reportResult.ahi).toFixed(1) : "--");
      const averagePressure = reportResult ?
        (reportResult.average_pressure ?? reportResult.mask_pressure_50) : null;
      const averageLeak = reportResult ?
        (reportResult.average_leak ?? reportResult.leak_50) : null;
      up("reportMaskPress50",
        displayable && Number.isFinite(Number(averagePressure)) ?
          Number(averagePressure).toFixed(1) : "--");
      up("reportLeak50",
        displayable && Number.isFinite(Number(averageLeak)) ?
          Number(averageLeak).toFixed(1) : "--");
      renderReportEventCounts(displayable ? reportResult : null);
      renderReportSessions(reportResult && reportResult.sessions ?
        reportResult.sessions : selected ? selected.sessions : []);
      updateReportZoomControls();

      if (nights.length) {
        msg("reportMsg", "Summary loaded", true);
      } else {
        const element = document.getElementById("reportMsg");
        if (element) element.className = "msg";
      }
    }

    // Conditional GET by stable night id. Poll while the backend is building.
    function isTransientReportError(text) {
      try {
        const parsed = JSON.parse(text);
        const code = parsed && parsed.error ? parsed.error : "";
        return code === "report_queue_busy" ||
               code === "artifact_stream_unavailable" ||
               code === "response_alloc";
      } catch (error) {
        return false;
      }
    }

    function delayMs(ms) {
      return new Promise((resolve) => setTimeout(resolve, ms));
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

    function cancelReportRequests() {
      cancelReportLoadRequest();
      cancelReportRangeRequest();
      reportLoadToken++;
      reportRangeToken++;
      reportRangeActiveKey = "";
    }

    async function pollReportFetch(options) {
      const active = options.active || (() => true);
      const delay = options.delayMs || REPORT_POLL_DELAY_MS;
      const maxAttempts = options.maxAttempts || 1;
      for (let attempt = 0; attempt < maxAttempts; attempt++) {
        if (!active()) return null;
        let response;
        try {
          response = await options.request();
        } catch (error) {
          if (error && error.name === "AbortError") return null;
          await delayMs(delay);
          continue;
        }
        if (!active()) return null;
        const action = await options.handle(response);
        if (!action) return null;
        if (action.done) return action.value;
        await delayMs(action.delayMs == null ? delay : action.delayMs);
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

    async function pollReportResult(token, nightId, signal) {
      const url = "/api/report/result?night=" + encodeURIComponent(nightId);
      return pollReportFetch({
        active: () => token === reportLoadToken,
        maxAttempts: REPORT_RESULT_POLL_MAX_ATTEMPTS,
        delayMs: REPORT_POLL_DELAY_MS,
        timeoutValue: {status: 0, etag: "", result: null},
        request: () => {
          const cached = lruGet(reportResultClientCache, url);
          return fetch(url, conditionalRequestOptions(cached, signal));
        },
        handle: async (resp) => {
          if (resp.status === 304) {
            const cached = lruGet(reportResultClientCache, url);
            if (!cached) throw new Error("result cache revalidation failed");
            return {done: true, value: {
              status: 304,
              etag: cached.etag,
              result: cached.decoded,
            }};
          }
          if (resp.status === 200) {
            const decoded = decodeReportResultBinary(await resp.arrayBuffer());
            if (!decoded.valid) throw new Error("invalid report result");

            const etag = resp.headers.get("ETag") || "";
            const revision = reportArtifactRevision(resp);
            if (revision && revision !== decoded.source_revision) {
              throw new Error("report result revision mismatch");
            }
            lruSet(reportResultClientCache, url, {etag, decoded, revision},
              REPORT_RESULT_CLIENT_CACHE_MAX);
            return {done: true, value: {
              status: 200,
              etag,
              result: decoded,
            }};
          }
          if (resp.status === 202) {
            msg("reportMsg", "Preparing report...", true, true);
            return {done: false};
          }
          if (resp.status === 404) {
            return {done: true, value: {
              status: 404,
              etag: "",
              result: null,
            }};
          }
          const text = await resp.text();
          if (resp.status === 503 && isTransientReportError(text)) {
            return {done: false};
          }
          throw new Error(text);
        },
      });
    }

    function reportPlotUrl(nightId, from, to, part, name) {
      let url = "/api/report/plot?night=" + encodeURIComponent(nightId);
      if (Number.isFinite(from) && Number.isFinite(to)) {
        url += "&from=" + encodeURIComponent(from) +
          "&to=" + encodeURIComponent(to);
      }
      if (part) url += "&part=" + encodeURIComponent(part);
      if (name) url += "&name=" + encodeURIComponent(name);
      return url;
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

    async function pollReportPlotPart(url, active, maxAttempts, delay, signal,
                                      decode) {
      return pollReportFetch({
        active,
        maxAttempts,
        delayMs: delay,
        request: () => {
          const cached = lruGet(reportPlotClientCache, url);
          return fetch(url, conditionalRequestOptions(cached, signal));
        },
        handle: async (response) => {
          if (response.status === 304) {
            const cached = lruGet(reportPlotClientCache, url);
            if (!cached) throw new Error("plot cache revalidation failed");
            return {done: true, value: cached};
          }
          if (response.status === 200) {
            const revision = reportArtifactRevision(response);
            const decoded = decode(await response.arrayBuffer());
            if (!decoded.valid) throw new Error("invalid plot response");

            const entry = {
              etag: response.headers.get("ETag") || "",
              revision,
              decoded,
            };
            lruSet(reportPlotClientCache, url, entry,
              REPORT_PLOT_CLIENT_CACHE_MAX);
            return {done: true, value: entry};
          }
          if (response.status === 202) return {done: false};
          if (response.status === 404) throw new Error("plot not found");

          const text = await response.text();
          if (response.status === 503 && isTransientReportError(text)) {
            return {done: false};
          }
          throw new Error(text || "plot request failed");
        },
      });
    }

    function reportChartDefinition(key) {
      return reportChartDefs.find((definition) => definition.key === key) ||
        null;
    }

    function reportPlotSection(name, index) {
      const source = index || reportBasePlotIndex;
      return source && source.sections ? source.sections[name] || null : null;
    }

    function reportChartPartNames(definition, index) {
      if (!definition) return [];
      if (definition.type === "events") {
        return reportResult && reportResult.events_available &&
          reportPlotSection("events", index) ? ["events"] : [];
      }
      return (definition.series || [definition])
        .map((series) => series.key)
        .filter((name) => reportPlotSection(name, index));
    }

    function reportExpandedChartKeys() {
      return reportChartPreferences.order.filter((key) =>
        !reportChartPreferences.collapsed.has(key));
    }

    async function fetchReportChartData(index, definition, context) {
      const decoded = {events: [], series: {}};
      const partNames = reportChartPartNames(definition, index);
      for (const name of partNames) {
        if (!context.active()) return null;
        const section = reportPlotSection(name, index);
        const part = name === "events" ? "events" : "series";
        const url = reportPlotUrl(context.nightId,
          context.from, context.to, part, part === "series" ? name : "");
        const fetched = await pollReportPlotPart(
          url,
          context.active,
          context.maxAttempts,
          context.delay,
          context.signal,
          (buffer) => part === "events"
            ? decodeReportPlotEvents(buffer, index, section)
            : decodeReportPlotSeries(buffer, index, section));
        if (!fetched || !context.active()) return null;
        if (fetched.revision && fetched.revision !== context.revision) {
          throw new Error("report_revision_changed");
        }
        if (part === "events") {
          decoded.events = fetched.decoded.events;
        } else {
          decoded.series[name] = fetched.decoded.points;
        }
      }
      return decoded;
    }

    function publishReportBaseChart(key, decoded) {
      const definition = reportChartDefinition(key);
      if (!definition || !decoded) return;
      if (definition.type === "events") {
        reportBaseEvents = decoded.events || [];
        if (!reportZoom) reportEvents = reportBaseEvents;
      } else {
        Object.keys(decoded.series || {}).forEach((name) => {
          reportBaseSeries[name] = decoded.series[name];
          if (!reportZoom) reportSeries[name] = decoded.series[name];
        });
      }
      reportBaseLoadedCharts.add(key);
      renderReportCharts();
    }

    function loadReportBaseChart(key, token, signal) {
      if (reportBaseLoadedCharts.has(key)) return Promise.resolve(true);
      const pending = reportBaseChartPromises.get(key);
      if (pending) return pending;

      const definition = reportChartDefinition(key);
      const index = reportBasePlotIndex;
      if (!definition || !index) return Promise.resolve(false);
      const context = {
        nightId: reportCurrentNightId,
        revision: reportCurrentRevision,
        from: null,
        to: null,
        signal,
        maxAttempts: REPORT_PLOT_POLL_MAX_ATTEMPTS,
        delay: REPORT_POLL_DELAY_MS,
        active: () => token === reportLoadToken &&
          index === reportBasePlotIndex,
      };
      const promise = fetchReportChartData(index, definition, context)
        .then((decoded) => {
          if (!decoded || !context.active()) return false;
          publishReportBaseChart(key, decoded);
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

    async function fetchReportPlot(token, nightId, revision, signal) {
      const indexUrl = reportPlotUrl(nightId, null, null, "index");
      const fetched = await pollReportPlotPart(
        indexUrl,
        () => token === reportLoadToken,
        REPORT_PLOT_POLL_MAX_ATTEMPTS,
        REPORT_POLL_DELAY_MS,
        signal,
        decodeReportPlotIndex);
      if (!fetched || token !== reportLoadToken) return false;
      if (fetched.revision && fetched.revision !== revision) {
        return "revision_changed";
      }

      activateReportBasePlot(nightId, revision, fetched.etag,
        fetched.decoded);
      renderReportSummary();
      renderReportCharts();

      const jobs = reportExpandedChartKeys().map((key) =>
        () => loadReportBaseChart(key, token, signal));
      try {
        await runReportFetchJobs(jobs, 2);
      } catch (error) {
        if (error && error.message === "report_revision_changed") {
          return "revision_changed";
        }
        throw error;
      }
      return true;
    }

    async function ensureReportChartLoaded(key) {
      if (!reportBasePlotIndex || !reportCurrentNightId) return;
      const token = reportLoadToken;
      let controller = reportLoadAbortController;
      let ownsController = false;
      if (!controller) {
        controller = new AbortController();
        reportLoadAbortController = controller;
        ownsController = true;
      }
      try {
        await loadReportBaseChart(key, token, controller.signal);
        if (reportZoom && token === reportLoadToken) {
          ensureReportRangeLoaded(reportZoom.start, reportZoom.end, [key]);
        }
      } catch (error) {
        if (error && error.message === "report_revision_changed") {
          loadSelectedReportNight();
        }
      } finally {
        if (ownsController && reportLoadAbortController === controller) {
          reportLoadAbortController = null;
        }
      }
    }

    function reportRangeWindow(lo, hi) {
      if (!(hi > lo)) return null;

      const from = Math.floor(lo / REPORT_RANGE_TILE_MS) *
        REPORT_RANGE_TILE_MS;
      const to = Math.ceil(hi / REPORT_RANGE_TILE_MS) *
        REPORT_RANGE_TILE_MS;
      return to > from ? {from, to} : null;
    }

    function reportRangeCacheKey(nightId, revision, from, to) {
      return String(nightId) + ":" + String(revision) + ":" + from + ":" + to;
    }

    function currentReportRangeCacheKey() {
      if (!reportZoom || !reportCurrentNightId || !reportCurrentRevision) {
        return "";
      }
      const w = reportRangeWindow(reportZoom.start, reportZoom.end);
      if (!w) return "";
      return reportRangeCacheKey(reportCurrentNightId,
        reportCurrentRevision, w.from, w.to);
    }

    function reportRangeEntry(key, from, to) {
      let entry = lruGet(reportRangeCache, key);
      if (entry) return entry;
      entry = {
        from,
        to,
        indexes: null,
        series: {},
        events: [],
        loadedCharts: new Set(),
        requestedCharts: new Set(),
        promise: null,
      };
      lruSet(reportRangeCache, key, entry, REPORT_RANGE_CACHE_MAX);
      return entry;
    }

    function applyReportRangeChart(entry, key) {
      if (!entry || reportRangeActiveKey !== currentReportRangeCacheKey()) {
        return;
      }
      const definition = reportChartDefinition(key);
      if (!definition) return;
      if (definition.type === "events") {
        reportEvents = entry.events;
      } else {
        (definition.series || [definition]).forEach((series) => {
          if (Object.prototype.hasOwnProperty.call(entry.series, series.key)) {
            reportSeries[series.key] = entry.series[series.key];
          }
        });
      }
    }

    function currentReportRangeChartReady(key) {
      const cacheKey = currentReportRangeCacheKey();
      const entry = cacheKey ? reportRangeCache.get(cacheKey) : null;
      return !!(entry && entry.loadedCharts.has(key));
    }

    function ensureReportRangeLoaded(lo, hi, requestedKeys) {
      if (!reportCurrentNightId || !reportCurrentRevision) return;
      const w = reportRangeWindow(lo, hi);
      if (!w) return;
      const key = reportRangeCacheKey(reportCurrentNightId,
        reportCurrentRevision, w.from, w.to);
      if (reportRangeActiveKey !== key) {
        cancelReportRangeRequest();
        reportRangeActiveKey = key;
        reportSeries = {...reportBaseSeries};
        reportEvents = reportBaseEvents;
      }

      const entry = reportRangeEntry(key, w.from, w.to);
      const keys = requestedKeys || reportExpandedChartKeys();
      keys.forEach((chartKey) => {
        if (!entry.loadedCharts.has(chartKey)) {
          entry.requestedCharts.add(chartKey);
        } else {
          applyReportRangeChart(entry, chartKey);
        }
      });
      startReportRangeWorker(key, entry);
    }

    function mergeReportPlots(plots) {
      const merged = {series: {}, events: []};
      const eventKeys = new Set();

      plots.forEach((plot) => {
        Object.keys(plot.series || {}).forEach((name) => {
          if (!merged.series[name]) merged.series[name] = [];
          const target = merged.series[name];
          (plot.series[name] || []).forEach((point) => {
            const previous = target.length ? target[target.length - 1] : null;
            if (point.gap && previous && previous.gap) return;
            if (previous && !point.gap && !previous.gap &&
                point.t === previous.t && point.value === previous.value &&
                point.min === previous.min && point.max === previous.max) {
              return;
            }
            target.push(point);
          });
        });

        (plot.events || []).forEach((event) => {
          const key = [event.t, event.duration, event.code, event.flags].join(":");
          if (eventKeys.has(key)) return;
          eventKeys.add(key);
          merged.events.push(event);
        });
      });
      merged.events.sort((a, b) => a.t - b.t || a.code - b.code);
      return merged;
    }

    async function loadReportRangeIndexes(context, from, to) {
      const indexes = [];
      const jobs = [];
      for (let tileStart = from; tileStart < to;
           tileStart += REPORT_RANGE_TILE_MS) {
        const tileIndex = indexes.length;
        indexes.push(null);
        jobs.push(async () => {
          const tileEnd = tileStart + REPORT_RANGE_TILE_MS;
          const url = reportPlotUrl(context.nightId,
            tileStart, tileEnd, "index");
          const fetched = await pollReportPlotPart(
            url,
            context.active,
            REPORT_RANGE_POLL_MAX_ATTEMPTS,
            REPORT_RANGE_POLL_DELAY_MS,
            context.signal,
            decodeReportPlotIndex);
          if (!fetched || !context.active()) return;
          if (fetched.revision && fetched.revision !== context.revision) {
            throw new Error("report_revision_changed");
          }
          indexes[tileIndex] = {
            from: tileStart,
            to: tileEnd,
            index: fetched.decoded,
          };
        });
      }
      await runReportFetchJobs(jobs, 2);
      return indexes.every((item) => item) ? indexes : null;
    }

    async function loadReportRangeChart(entry, key, context) {
      const definition = reportChartDefinition(key);
      if (!definition || !entry.indexes) return false;

      const plots = [];
      for (const tile of entry.indexes) {
        const tileContext = {
          ...context,
          from: tile.from,
          to: tile.to,
        };
        const decoded = await fetchReportChartData(
          tile.index, definition, tileContext);
        if (!decoded || !context.active()) return false;
        plots.push(decoded);
      }

      const merged = mergeReportPlots(plots);
      if (definition.type === "events") {
        entry.events = merged.events;
      } else {
        (definition.series || [definition]).forEach((series) => {
          entry.series[series.key] = merged.series[series.key] || [];
        });
      }
      entry.loadedCharts.add(key);
      applyReportRangeChart(entry, key);
      renderReportCharts();
      return true;
    }

    function startReportRangeWorker(key, entry) {
      if (!entry || entry.promise) return;
      const nightId = reportCurrentNightId;
      const revision = reportCurrentRevision;
      let controller = reportRangeAbortController;
      if (!controller) controller = new AbortController();
      reportRangeAbortController = controller;
      const token = ++reportRangeToken;
      const context = {
        nightId,
        revision,
        signal: controller.signal,
        maxAttempts: REPORT_RANGE_POLL_MAX_ATTEMPTS,
        delay: REPORT_RANGE_POLL_DELAY_MS,
        active: () => token === reportRangeToken &&
          key === reportRangeActiveKey &&
          nightId === reportCurrentNightId &&
          revision === reportCurrentRevision,
      };
      const promise = (async () => {
        if (!entry.indexes) {
          entry.indexes = await loadReportRangeIndexes(
            context, entry.from, entry.to);
          if (!entry.indexes || !context.active()) return;
        }

        while (context.active()) {
          const keys = Array.from(entry.requestedCharts)
            .filter((chartKey) => !entry.loadedCharts.has(chartKey));
          entry.requestedCharts.clear();
          if (!keys.length) break;
          await runReportFetchJobs(keys.map((chartKey) =>
            () => loadReportRangeChart(entry, chartKey, context)), 2);
        }
      })().catch((error) => {
        if (error && error.message === "report_revision_changed" &&
            context.active()) {
          loadSelectedReportNight();
        }
      }).finally(() => {
        const restart = context.active() &&
          entry.requestedCharts.size > 0;
        if (entry.promise === promise) entry.promise = null;
        if (reportRangeAbortController === controller) {
          reportRangeAbortController = null;
        }
        if (restart) startReportRangeWorker(key, entry);
      });
      entry.promise = promise;
    }

    function reportCrc32(bytes, start, length) {
      let crc = 0xFFFFFFFF;
      const end = start + length;
      for (let offset = start; offset < end; offset++) {
        crc ^= bytes[offset];
        for (let bit = 0; bit < 8; bit++) {
          crc = (crc >>> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        }
      }
      return (crc ^ 0xFFFFFFFF) >>> 0;
    }

    function reportPopcount(value) {
      let bits = Number(value) >>> 0;
      let count = 0;
      while (bits) {
        bits &= bits - 1;
        count++;
      }
      return count;
    }

    function reportMetricSource(validMask, strMask, summaryMask, index) {
      const bit = 1 << index;
      if (!(validMask & bit)) return "";
      if (strMask & bit) return "str_edf";
      if (summaryMask & bit) return "summary";
      return "calculated";
    }

    function decodeReportResultBinary(buf) {
      const invalid = {valid: false};
      const dv = new DataView(buf);
      const bytes = new Uint8Array(buf);
      const headerBytes = 160;
      if (dv.byteLength < headerBytes ||
          dv.getUint32(0, true) !== 0x36524341 ||
          dv.getUint16(4, true) !== 1 ||
          dv.getUint16(6, true) !== headerBytes ||
          dv.getUint32(8, true) !== dv.byteLength) {
        return invalid;
      }

      const sessionCount = dv.getUint16(78, true);
      if (headerBytes + sessionCount * 16 !== dv.byteLength ||
          reportCrc32(bytes, 0, 152) !== dv.getUint32(152, true) ||
          reportCrc32(bytes, headerBytes,
            dv.byteLength - headerBytes) !== dv.getUint32(148, true)) {
        return invalid;
      }

      const revision = dv.getBigUint64(16, true);
      const dayStart = Number(dv.getBigInt64(24, true));
      const dayEnd = Number(dv.getBigInt64(32, true));
      const therapyStart = Number(dv.getBigInt64(40, true));
      const therapyEnd = Number(dv.getBigInt64(48, true));
      if (revision === 0n || !(dayEnd > dayStart)) return invalid;

      const sessions = [];
      let previousEnd = 0;
      for (let index = 0; index < sessionCount; index++) {
        const offset = headerBytes + index * 16;
        const start = Number(dv.getBigInt64(offset, true));
        const end = Number(dv.getBigInt64(offset + 8, true));
        if (!(end > start) || start < dayStart || end > dayEnd ||
            (index > 0 && start < previousEnd)) {
          return invalid;
        }
        sessions.push({
          start,
          end,
          duration_min: Math.round((end - start) / 60000),
        });
        previousEnd = end;
      }
      if ((sessionCount === 0 && (therapyStart || therapyEnd)) ||
          (sessionCount > 0 &&
           (sessions[0].start !== therapyStart ||
            sessions[sessionCount - 1].end !== therapyEnd))) {
        return invalid;
      }

      const requestedSignals = dv.getUint32(60, true);
      const availableSignals = dv.getUint32(64, true);
      const missingRequired = dv.getUint32(68, true);
      const missingOptional = dv.getUint32(72, true);
      const flags = dv.getUint16(76, true);
      const sourceFlags = dv.getUint8(82);
      const metricValid = dv.getUint16(84, true);
      const metricStr = dv.getUint16(86, true);
      const metricSummary = dv.getUint16(88, true);
      const metricOffsets = [92, 96, 100, 104, 108, 112, 116, 120];
      const metrics = metricOffsets.map((offset) =>
        dv.getInt32(offset, true) / 1000);
      const signalNames = [
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
        null,
        "snore",
        "tidal_volume",
        "spo2",
        "pulse",
      ];
      const streamDetails = [];
      signalNames.forEach((name, index) => {
        if (!name) return;
        const bit = 1 << index;
        if (!(requestedSignals & bit)) return;

        const complete = !!(availableSignals & bit);
        streamDetails.push({
          kind: "series",
          name,
          source: name,
          required: index === 0 || index === 5,
          complete,
          provider: complete && (sourceFlags & 1) ? "edf" :
            (complete ? "spool" : "missing"),
          has_edf: complete && !!(sourceFlags & 1),
          has_spool: complete && !!(sourceFlags & 8),
          low_res: false,
        });
      });

      const result = {
        valid: true,
        state: flags & 1 ? "ready" : "partial",
        error: "",
        source_revision: revision.toString(16).padStart(16, "0"),
        sleep_day_epoch: dv.getInt32(12, true),
        day_start: dayStart,
        day_end: dayEnd,
        start: therapyStart || dayStart,
        end: therapyEnd || dayEnd,
        duration_min: dv.getUint32(56, true),
        missing_required: reportPopcount(missingRequired),
        missing_streams: reportPopcount(missingRequired | missingOptional),
        streams: reportPopcount(requestedSignals),
        events_available: !!(flags & 2),
        requested_event_mask: dv.getUint8(80),
        missing_event_mask: dv.getUint8(81),
        source_flags: sourceFlags,
        sessions,
        stream_details: streamDetails,
        hypopnea_count: dv.getUint32(124, true),
        ca_count: dv.getUint32(128, true),
        oa_count: dv.getUint32(132, true),
        ua_count: dv.getUint32(136, true),
        arousal_count: dv.getUint32(140, true),
        csr_count: dv.getUint32(144, true),
      };
      const metricFields = [
        "ahi",
        "oa_index",
        "ca_index",
        "ua_index",
        "hypopnea_index",
        "arousal_index",
        "mask_pressure_50",
        "leak_50",
      ];
      metricFields.forEach((field, index) => {
        if (!(metricValid & (1 << index))) return;
        result[field] = metrics[index];
        result[field + "_source"] = reportMetricSource(
          metricValid, metricStr, metricSummary, index);
      });
      if (result.mask_pressure_50 !== undefined) {
        result.average_pressure = result.mask_pressure_50;
      }
      if (result.leak_50 !== undefined) {
        result.average_leak = result.leak_50;
      }
      return result;
    }

    function reportPlotSectionCrcValid(buffer, section) {
      if (!section || buffer.byteLength !== section.length) return false;
      return reportCrc32(new Uint8Array(buffer), 0, buffer.byteLength) ===
        section.crc32;
    }

    // Decode the fixed PLOT v6 directory. Stable section names are the only
    // identity consumed by WebUI; entry positions are deliberately ignored.
    function decodeReportPlotIndex(buffer) {
      const invalid = {valid: false, sections: {}};
      const PREFIX_BYTES = 1600;
      const HEADER_BYTES = 64;
      const ENTRY_BYTES = 48;
      const NAME_BYTES = 32;
      if (buffer.byteLength !== PREFIX_BYTES) return invalid;

      const bytes = new Uint8Array(buffer);
      const dv = new DataView(buffer);
      if (dv.getUint32(0, true) !== 0x42504341 ||
          dv.getUint16(4, true) !== 6 ||
          dv.getUint32(8, true) !== PREFIX_BYTES) {
        return invalid;
      }

      const expectedCrc = dv.getUint32(48, true);
      const crcBytes = bytes.slice();
      crcBytes.fill(0, 48, 52);
      if (reportCrc32(crcBytes, 0, crcBytes.length) !== expectedCrc) {
        return invalid;
      }

      const totalSize = dv.getUint32(12, true);
      const base = Number(dv.getBigInt64(16, true));
      const start = Number(dv.getBigInt64(24, true));
      const end = Number(dv.getBigInt64(32, true));
      const count = dv.getUint16(40, true);
      if (totalSize < PREFIX_BYTES || !(end > start) ||
          count < 1 || count > 32) {
        return invalid;
      }

      const decoder = new TextDecoder();
      const sections = {};
      const order = [];
      let expectedOffset = PREFIX_BYTES;
      for (let i = 0; i < count; i++) {
        const offset = HEADER_BYTES + i * ENTRY_BYTES;
        const nameBytes = bytes.subarray(offset + 4, offset + 4 + NAME_BYTES);
        const nul = nameBytes.indexOf(0);
        if (nul <= 0) return invalid;
        const name = decoder.decode(nameBytes.subarray(0, nul));
        if (!/^[a-z0-9_]+$/.test(name) || sections[name]) return invalid;

        const section = {
          kind: dv.getUint8(offset),
          encoding: dv.getUint8(offset + 1),
          flags: dv.getUint16(offset + 2, true),
          name,
          offset: dv.getUint32(offset + 36, true),
          length: dv.getUint32(offset + 40, true),
          crc32: dv.getUint32(offset + 44, true),
        };
        if (!section.length || section.offset !== expectedOffset ||
            section.length > totalSize - section.offset ||
            (i === 0 && (section.kind !== 0 || name !== "events")) ||
            (i > 0 && (section.kind !== 1 || section.encoding > 1))) {
          return invalid;
        }
        expectedOffset += section.length;
        sections[name] = section;
        order.push(name);
      }
      if (expectedOffset !== totalSize) return invalid;

      return {
        valid: true,
        base,
        start,
        end,
        totalSize,
        prefixCrc32: expectedCrc,
        sections,
        order,
      };
    }

    function decodeReportPlotEvents(buffer, index, section) {
      const invalid = {valid: false, events: []};
      if (!index || !reportPlotSectionCrcValid(buffer, section) ||
          buffer.byteLength < 4) {
        return invalid;
      }

      const dv = new DataView(buffer);
      const count = dv.getUint32(0, true);
      if (buffer.byteLength !== 4 + count * 16) return invalid;
      const events = [];
      let offset = 4;
      for (let i = 0; i < count; i++, offset += 16) {
        events.push({
          t: index.base + dv.getInt32(offset, true),
          duration: dv.getInt32(offset + 4, true),
          code: dv.getInt32(offset + 8, true),
          flags: dv.getInt32(offset + 12, true),
        });
      }
      return {valid: true, events};
    }

    function decodeReportPlotSeries(buffer, index, section) {
      const invalid = {valid: false, points: []};
      if (!index || !reportPlotSectionCrcValid(buffer, section) ||
          buffer.byteLength < 16) {
        return invalid;
      }

      const dv = new DataView(buffer);
      const points = [];
      let offset = 0;
      if (section.encoding === 0) {
        const seriesBaseDelta = dv.getInt32(offset, true); offset += 4;
        const timeUnitMs = dv.getUint32(offset, true); offset += 4;
        const valueScaleMilli = dv.getUint32(offset, true); offset += 4;
        const pointCount = dv.getUint32(offset, true); offset += 4;
        if (!timeUnitMs || !valueScaleMilli ||
            buffer.byteLength !== offset + pointCount * 4) {
          return invalid;
        }

        let lastPointT = index.base;
        for (let i = 0; i < pointCount; i++, offset += 4) {
          const timeIndex = dv.getUint16(offset, true);
          if (timeIndex === 0xFFFF) {
            points.push({gap: true, t: lastPointT});
            continue;
          }
          const t = index.base + seriesBaseDelta + timeIndex * timeUnitMs;
          const value = dv.getInt16(offset + 2, true) *
            valueScaleMilli / 1000;
          lastPointT = t;
          points.push({t, value});
        }
      } else if (section.encoding === 1) {
        const axisBaseDelta = dv.getInt32(offset, true); offset += 4;
        const bucketMs = dv.getUint32(offset, true); offset += 4;
        const valueScaleMilli = dv.getUint32(offset, true); offset += 4;
        const runCount = dv.getUint32(offset, true); offset += 4;
        if (!bucketMs || !valueScaleMilli) return invalid;

        for (let run = 0; run < runCount; run++) {
          if (offset + 6 > buffer.byteLength) return invalid;
          const startBucket = dv.getUint32(offset, true); offset += 4;
          const bucketCount = dv.getUint16(offset, true); offset += 2;
          if (offset + bucketCount * 4 > buffer.byteLength) return invalid;
          const runStart = index.base + axisBaseDelta +
            startBucket * bucketMs;
          if (run > 0) points.push({gap: true, t: runStart});
          for (let bucket = 0; bucket < bucketCount;
               bucket++, offset += 4) {
            const minimum = dv.getInt16(offset, true) *
              valueScaleMilli / 1000;
            const maximum = dv.getInt16(offset + 2, true) *
              valueScaleMilli / 1000;
            const t = runStart + bucket * bucketMs;
            points.push({
              t,
              end: t + bucketMs,
              min: Math.min(minimum, maximum),
              max: Math.max(minimum, maximum),
              value: (minimum + maximum) / 2,
              envelope: true,
            });
          }
        }
        if (offset !== buffer.byteLength) return invalid;
      } else {
        return invalid;
      }
      return {valid: true, points};
    }

    async function loadSelectedReportNight() {
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
      const token = ++reportLoadToken;
      resetReportData();
      renderReportSummary();
      msg("reportMsg", "Loading report...", true, true);
      try {
        for (let attempt = 0; attempt < 2; attempt++) {
          const res = await pollReportResult(
            token, nightId, controller.signal);
          if (!res || token !== reportLoadToken) return;
          if (res.status === 404) {
            msg("reportMsg", "Night not found", false, true);
            return;
          }
          if ((res.status !== 200 && res.status !== 304) || !res.result) {
            msg("reportMsg", "Report not ready", false, true);
            renderReportSummary();
            return;
          }
          reportResult = res.result;
          if (reportResult.state !== "ready" &&
              reportResult.state !== "partial") {
            msg("reportMsg", reportResult.error || "Report incomplete",
                false, true);
            renderReportSummary();
            return;
          }

          const plotStatus = await fetchReportPlot(
            token, nightId, reportResult.source_revision, controller.signal);
          if (!plotStatus || token !== reportLoadToken) return;
          if (plotStatus === "revision_changed") continue;

          renderReportSummary();
          renderReportCharts();
          msg("reportMsg",
              reportResult.state === "partial"
                ? "Report loaded (incomplete - some data missing)"
                : "Report loaded", true);
          return;
        }
        throw new Error("report changed while loading");
      } catch (error) {
        if (token !== reportLoadToken) return;
        msg("reportMsg", error.message, false, true);
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
        const response = await fetch("/api/report/summary",
          {cache: "no-cache", headers});
        if (response.status === 202) {
          msg("reportMsg", "Preparing report index...", true, true);
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
        if (loadNight) await loadSelectedReportNight();
      } catch (error) {
        msg("reportMsg", error.message, false, true);
      }
    }

    async function refreshReportSummary(loadNight) {
      await loadReportSummary(loadNight !== false);
    }
