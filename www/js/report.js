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
      loadSelectedReportNight(false, false);
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

    function reportTherapyIndex(night) {
      if (!night) return null;
      const value = Number(night.therapy_index);
      return Number.isInteger(value) && value >= 0 ? value : null;
    }

    function reportNightLoadKey(night) {
      if (!night) return "";
      const start = Number(night.start || 0);
      return String(start || night.id || "");
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
      reportCurrentIndex = -1;
      reportCurrentEtag = null;
      reportRangeCache.clear();
      reportRangeInFlightKey = "";
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

    function rememberReportPlotClientCache(etag) {
      if (!etag) return;
      lruSet(reportPlotClientCache, etag, {
        series: reportSeries,
        events: reportEvents,
      }, REPORT_PLOT_CLIENT_CACHE_MAX);
    }

    function activateReportBasePlot(index, etag, series, events) {
      reportSeries = series || {};
      reportEvents = events || [];
      reportBaseSeries = reportSeries;
      reportBaseEvents = reportEvents;
      reportCurrentIndex = Number.isFinite(Number(index)) ? Number(index) : -1;
      reportCurrentEtag = etag || null;
      reportRangeCache.clear();
      reportRangeInFlightKey = "";
      reportRangeToken++;
    }

    function restoreReportPlotClientCache(etag, index) {
      const cached = etag ? lruGet(reportPlotClientCache, etag) : null;
      if (!cached) return false;
      activateReportBasePlot(index, etag, cached.series, cached.events);
      return true;
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
      reportZoom = null;
      reportRangeInFlightKey = "";
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

    function renderReportEventFlags(container, range, ranges) {
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
      name.textContent = "Event Flags";
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
      card.appendChild(title);
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
        if (reportResult && reportResult.state === "incomplete") {
          const note = document.createElement("div");
          note.className = "report-chart-note";
          note.textContent =
            "Backfilling this night - charts will appear once cached.";
          container.appendChild(note);
        }
        updateReportZoomControls();
        return;
      }
      if (reportResult.state === "partial") {
        const pnote = document.createElement("div");
        pnote.className = "report-chart-note";
        pnote.textContent =
          "Incomplete night - best-effort from cached data; missing sources are marked below.";
        container.appendChild(pnote);
      }
      const range = reportZoom || reportRange();
      if (!(range.end > range.start)) {
        updateReportZoomControls();
        return;
      }
      if (reportZoom) ensureReportRangeLoaded(reportZoom.start, reportZoom.end);
      const zoomAwaitingRange = !!reportZoom && !currentReportRangeReady();
      const sessionRanges = reportVisibleSessionRanges();

      // Map each signal name -> whether it fell back to the low-res 1-min trend
      // (high-res aged out); used to badge the affected charts.
      const lowResByName = {};
      (reportResult.stream_details || []).forEach((s) => {
        if (s && s.name) lowResByName[s.name] = !!s.low_res;
      });

      renderReportEventFlags(container, range, sessionRanges);

      reportChartDefs.forEach((def) => {
        const seriesDefs = def.series || [def];
        const seriesList = seriesDefs.map((seriesDef) => ({
          label: seriesDef.label || def.title,
          color: seriesDef.color || def.color,
          points: (reportSeries[seriesDef.key] || []).slice()
            .sort((a, b) => a.t - b.t),
        })).filter((series) => series.points.length > 0);
        if (!seriesList.length) {
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
          cnote.textContent =
            reportResult && reportResult.missing_required > 0
              ? "backfilling..."
              : "not retained for this night";
          ctitle.appendChild(cnote);
          card.appendChild(ctitle);
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
        card.appendChild(title);
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
          rangePending: zoomAwaitingRange,
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
      const state = reportSummary && reportSummary.state ?
        String(reportSummary.state) : "idle";
      const nights = reportNightsNewestFirst();
      const selected = selectedReportNight();

      const dateBtn = document.getElementById("reportDateBtn");
      if (dateBtn) {
        if (selected) {
          dateBtn.innerHTML = fmtReportDate(selected.start) +
            "<span class=\"np-dur\">" + fmtMinutes(selected.duration_min) +
            "</span>";
        } else {
          dateBtn.textContent = state === "fetching" ? "Loading..." :
            (nights.length ? "Select night" : "No nights");
        }
      }
      const selIndex = selected ?
        nights.findIndex((night) => night === selected) : -1;
      const prevBtn = document.getElementById("reportPrevNight");
      const nextBtn = document.getElementById("reportNextNight");
      if (prevBtn) prevBtn.disabled = !(selIndex >= 0 &&
        selIndex < nights.length - 1);
      if (nextBtn) nextBtn.disabled = !(selIndex > 0);
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

      if (state === "fetching") {
        const spool = reportSummary && reportSummary.spool ? reportSummary.spool : {};
        const text = "Fetching Summary" +
          (spool.bytes ? " " + fmtBytes(spool.bytes) : "");
        msg("reportMsg", text, true, true);
      } else if (state === "error") {
        msg("reportMsg", reportSummary.error || "Summary refresh failed", false, true);
      } else if (nights.length) {
        msg("reportMsg", "Summary loaded", true);
      } else {
        const element = document.getElementById("reportMsg");
        if (element) element.className = "msg";
      }
    }

    // Conditional GET by stable night id: returns {status, etag, json}.
    // Polls while the backend is still building (202).
    function isTransientReportError(text) {
      try {
        const parsed = JSON.parse(text);
        const code = parsed && parsed.error ? parsed.error : "";
        return code === "report_cache_busy" ||
               code === "report_queue_full";
      } catch (error) {
        return false;
      }
    }

    function delayMs(ms) {
      return new Promise((resolve) => setTimeout(resolve, ms));
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

    async function pollReportResult(token, nightStart) {
      const url = "/api/report/result?night=" + encodeURIComponent(nightStart);
      return pollReportFetch({
        active: () => token === reportLoadToken,
        maxAttempts: REPORT_RESULT_POLL_MAX_ATTEMPTS,
        delayMs: REPORT_POLL_DELAY_MS,
        timeoutValue: {status: 0, etag: "", json: null},
        request: () => fetch(url),
        handle: async (resp) => {
          if (resp.status === 304) {
            return {done: false, delayMs: 50};
          }
          if (resp.status === 200) {
            const etag = (resp.headers.get("ETag") || "").replace(/"/g, "");
            const json = await resp.json();
            return {done: true, value: {status: 200, etag: etag, json: json}};
          }
          if (resp.status === 202) {
            msg("reportMsg", "Preparing report...", true, true);
            return {done: false};
          }
          if (resp.status === 404) {
            return {done: true, value: {status: 404, etag: "", json: null}};
          }
          const text = await resp.text();
          if (resp.status === 503 && isTransientReportError(text)) {
            return {done: false};
          }
          throw new Error(text);
        },
      });
    }

    function reportPlotUrl(index, version) {
      return "/api/report/plot?index=" + index +
        "&v=" + encodeURIComponent(version);
    }

    function emptyReportPlot() {
      return {valid: true, series: {}, events: []};
    }

    async function fetchReportPlotBuffer(index, version) {
      const response = await fetch(reportPlotUrl(index, version),
        {cache: "force-cache"});
      if (response.status === 200) {
        return {kind: "buffer", buffer: await response.arrayBuffer()};
      }
      if (response.status === 304) {
        return {kind: "not_modified"};
      }
      if (response.status === 202) {
        return {kind: "retry"};
      }
      if (response.status === 409) {
        let stale = null;
        try { stale = await response.json(); } catch (error) {}
        if (stale && stale.error === "stale_plot" && stale.etag) {
          return {kind: "stale", etag: stale.etag};
        }
      }
      if (response.status === 404) throw new Error("plot not found");
      const text = await response.text();
      if (response.status === 503 && isTransientReportError(text)) {
        return {kind: "retry"};
      }
      throw new Error(text);
    }

    async function fetchReportRangePayload(url) {
      const response = await fetch(url, {cache: "force-cache"});
      if (response.status === 204) {
        return {kind: "plot", decoded: emptyReportPlot()};
      }
      if (response.status === 200) {
        const decoded = decodeReportPlotBinary(await response.arrayBuffer());
        if (!decoded.valid) return {kind: "retry"};
        return {kind: "plot", decoded};
      }
      if (response.status === 202) {
        return {kind: "retry"};
      }
      if (response.status === 409) {
        return {kind: "stale"};
      }
      const text = await response.text();
      if (response.status === 503 && isTransientReportError(text)) {
        return {kind: "retry"};
      }
      return {kind: "stop"};
    }

    async function fetchReportPlotReload(index, version) {
      try {
        const response = await fetch(reportPlotUrl(index, version) +
          "&reload=" + Date.now(), {cache: "no-store"});
        if (response.status !== 200) return null;
        return response.arrayBuffer();
      } catch (error) {
        return null;
      }
    }

    async function fetchReportPlot(token, index, version) {
      // The plot is keyed by (index, version=etag); 202 means it's still building
      // (poll), 409 means this version is stale and the backend provides the
      // current etag to retry under a cache-correct URL.
      if (restoreReportPlotClientCache(version, index)) {
        return token === reportLoadToken ? version : "";
      }
      const fetched = await pollReportFetch({
        active: () => token === reportLoadToken,
        maxAttempts: REPORT_PLOT_POLL_MAX_ATTEMPTS,
        delayMs: REPORT_POLL_DELAY_MS,
        request: () => fetchReportPlotBuffer(index, version),
        handle: async (result) => {
          if (result.kind === "buffer") {
            return {done: true, value: {version, buffer: result.buffer}};
          }
          if (result.kind === "not_modified") {
            if (restoreReportPlotClientCache(version, index)) {
              return {done: true, value: {version, cached: true}};
            }
            const reloaded = await fetchReportPlotReload(index, version);
            if (reloaded) {
              return {done: true, value: {version, buffer: reloaded}};
            }
            return {done: false, delayMs: 50};
          }
          if (result.kind === "stale") {
            version = result.etag;
            return {done: false, delayMs: 50};
          }
          return {done: false};
        },
      });
      if (token !== reportLoadToken) return false;
      if (!fetched) throw new Error("plot not ready");
      if (fetched.cached) return token === reportLoadToken ? fetched.version : "";
      const decoded = decodeReportPlotBinary(fetched.buffer);
      if (!decoded.valid) throw new Error("invalid plot response");
      activateReportBasePlot(index, fetched.version, decoded.series, decoded.events);
      rememberReportPlotClientCache(fetched.version);
      return token === reportLoadToken ? fetched.version : "";
    }

    function reportRangeWindow(lo, hi) {
      if (!(hi > lo)) return null;

      const viewTiles = Math.max(1,
        Math.ceil((hi - lo) / REPORT_RANGE_TILE_MS));
      const maxTiles =
        Math.floor(REPORT_RANGE_MAX_WINDOW_MS / REPORT_RANGE_TILE_MS);
      let fetchTiles = viewTiles;
      if (viewTiles <= 4) {
        fetchTiles = Math.min(maxTiles, REPORT_RANGE_PAGE_TILES);
      } else if (viewTiles <= 6) {
        fetchTiles = Math.min(maxTiles, REPORT_RANGE_PAGE_TILES + 4);
      }

      if (fetchTiles <= viewTiles) {
        const from = Math.floor(lo / REPORT_RANGE_TILE_MS) *
          REPORT_RANGE_TILE_MS;
        const to = Math.ceil(hi / REPORT_RANGE_TILE_MS) *
          REPORT_RANGE_TILE_MS;
        return to > from ? {from, to} : null;
      }

      const centerTile = Math.floor(((lo + hi) / 2) /
        REPORT_RANGE_TILE_MS);
      const stepTiles = Math.max(1, Math.floor(fetchTiles / 2));
      const pageCenterTile =
        Math.floor(centerTile / stepTiles) * stepTiles +
        Math.floor(stepTiles / 2);
      let fromTile = pageCenterTile - Math.floor(fetchTiles / 2);
      let toTile = fromTile + fetchTiles;

      const loTile = Math.floor(lo / REPORT_RANGE_TILE_MS);
      const hiTile = Math.ceil(hi / REPORT_RANGE_TILE_MS);
      if (fromTile > loTile) {
        const shift = fromTile - loTile;
        fromTile -= shift;
        toTile -= shift;
      }
      if (toTile < hiTile) {
        const shift = hiTile - toTile;
        fromTile += shift;
        toTile += shift;
      }

      const from = fromTile * REPORT_RANGE_TILE_MS;
      const to = toTile * REPORT_RANGE_TILE_MS;
      return to > from ? {from, to} : null;
    }

    function reportRangeCacheKey(index, etag, from, to) {
      return String(index) + ":" + String(etag || "") + ":" + from + ":" + to;
    }

    function currentReportRangeCacheKey() {
      if (!reportZoom || reportCurrentEtag == null || reportCurrentIndex < 0) {
        return "";
      }
      const w = reportRangeWindow(reportZoom.start, reportZoom.end);
      if (!w) return "";
      return reportRangeCacheKey(reportCurrentIndex,
        reportCurrentEtag, w.from, w.to);
    }

    function currentReportRangeReady() {
      const key = currentReportRangeCacheKey();
      return !!key && reportRangeCache.has(key);
    }

    function activateReportRangeCache(key) {
      const cached = lruGet(reportRangeCache, key);
      if (!cached) return false;
      reportSeries = cached.series;
      reportEvents = cached.events;
      return true;
    }

    function ensureReportRangeLoaded(lo, hi) {
      if (reportCurrentEtag == null || reportCurrentIndex < 0) return;
      const w = reportRangeWindow(lo, hi);
      if (!w) return;
      const key = reportRangeCacheKey(reportCurrentIndex,
        reportCurrentEtag, w.from, w.to);
      if (activateReportRangeCache(key) || reportRangeInFlightKey === key) return;
      fetchReportRange(lo, hi);
    }

    async function fetchReportRange(lo, hi) {
      if (reportCurrentEtag == null || reportCurrentIndex < 0) return;
      const w = reportRangeWindow(lo, hi);
      if (!w) return;
      const etag = reportCurrentEtag;
      const index = reportCurrentIndex;
      const from = w.from;
      const to = w.to;
      const key = reportRangeCacheKey(index, etag, from, to);
      if (activateReportRangeCache(key)) {
        renderReportCharts();
        return;
      }
      if (reportRangeInFlightKey === key) return;
      reportRangeInFlightKey = key;
      const token = ++reportRangeToken;
      const url = "/api/report/plot?index=" + index +
        "&v=" + encodeURIComponent(etag) +
        "&from=" + encodeURIComponent(from) +
        "&to=" + encodeURIComponent(to);
      try {
        const decoded = await pollReportFetch({
          active: () => token === reportRangeToken &&
            etag === reportCurrentEtag &&
            index === reportCurrentIndex,
          maxAttempts: REPORT_RANGE_POLL_MAX_ATTEMPTS,
          delayMs: REPORT_RANGE_POLL_DELAY_MS,
          request: () => fetchReportRangePayload(url),
          handle: async (result) => {
            if (result.kind === "plot") {
              return {done: true, value: result.decoded};
            }
            if (result.kind === "stale" || result.kind === "stop") {
              return null;
            }
            return {done: false};
          },
        });
        if (!decoded || token !== reportRangeToken || etag !== reportCurrentEtag ||
            index !== reportCurrentIndex) {
          return;
        }
        if (!decoded.valid) {
          return;
        }
        lruSet(reportRangeCache, key,
          {series: decoded.series, events: decoded.events},
          REPORT_RANGE_CACHE_MAX);
        reportSeries = decoded.series;
        reportEvents = decoded.events;
        renderReportCharts();
      } finally {
        if (reportRangeInFlightKey === key) reportRangeInFlightKey = "";
      }
    }

    // Decode the binary plot blob (mirrors report_manager.cpp, little-endian).
    function decodeReportPlotBinary(buf) {
      const decoded = {valid: false, events: [], series: {}};
      const dv = new DataView(buf);
      // Header: magic u32, version u16, flags u16, base_ms i64 (16 bytes).
      if (dv.byteLength < 16 || dv.getUint32(0, true) !== 0x42504341) {
        return decoded;
      }
      if (dv.getUint16(4, true) !== 5) return decoded;  // PLOT_BIN_VERSION
      decoded.valid = true;
      const base = Number(dv.getBigInt64(8, true));
      const PLOT_GAP_INDEX = 0xFFFF;
      const SERIES_COMPACT = 0;
      const SERIES_ENVELOPE_RUNS = 1;
      let off = 16;
      // Every read below is bounds-checked: a truncated/corrupt blob bails
      // cleanly (no out-of-range throw, no half-record) so a later redraw never
      // sees partial state. Events are all-or-nothing; series stop at the last
      // whole one decoded.
      if (off + 4 > dv.byteLength) return decoded;
      const eventCount = dv.getUint32(off, true); off += 4;
      if (off + eventCount * 16 > dv.byteLength) return decoded;
      for (let i = 0; i < eventCount; i++) {
        decoded.events.push({
          t: base + dv.getInt32(off, true),
          duration: dv.getInt32(off + 4, true),
          code: dv.getInt32(off + 8, true),
          flags: dv.getInt32(off + 12, true),
        });
        off += 16;
      }
      const decoder = new TextDecoder();
      while (off + 6 <= dv.byteLength) {
        const nameLen = dv.getUint16(off, true); off += 2;
        if (off + nameLen + 4 > dv.byteLength) break;
        const name = decoder.decode(new Uint8Array(buf, off, nameLen));
        off += nameLen;
        const mode = dv.getUint8(off); off += 1;
        off += 1; // flags
        off += 2; // reserved
        const points = [];
        if (mode === SERIES_COMPACT) {
          if (off + 16 > dv.byteLength) break;
          const seriesBaseDelta = dv.getInt32(off, true); off += 4;
          const timeUnitMs = dv.getUint32(off, true); off += 4;
          const valueScaleMilli = dv.getUint32(off, true); off += 4;
          const pointCount = dv.getUint32(off, true); off += 4;
          if (!timeUnitMs || !valueScaleMilli) break;
          if (off + pointCount * 4 > dv.byteLength) break;
          let lastPointT = base;
          for (let p = 0; p < pointCount; p++) {
            const timeIndex = dv.getUint16(off, true);
            const rawValue = dv.getInt16(off + 2, true);
            off += 4;
            if (timeIndex === PLOT_GAP_INDEX) {
              points.push({gap: true, t: lastPointT});
              continue;
            }
            const t = base + seriesBaseDelta + timeIndex * timeUnitMs;
            const value = rawValue * valueScaleMilli / 1000;
            if (t > 0 && Number.isFinite(value)) {
              lastPointT = t;
              points.push({t, value});
            }
          }
        } else if (mode === SERIES_ENVELOPE_RUNS) {
          if (off + 16 > dv.byteLength) break;
          const axisBaseDelta = dv.getInt32(off, true); off += 4;
          const bucketMs = dv.getUint32(off, true); off += 4;
          const valueScaleMilli = dv.getUint32(off, true); off += 4;
          const runCount = dv.getUint32(off, true); off += 4;
          if (!bucketMs || !valueScaleMilli) break;
          let firstRun = true;
          for (let r = 0; r < runCount; r++) {
            if (off + 6 > dv.byteLength) break;
            const startBucket = dv.getUint32(off, true); off += 4;
            const bucketCount = dv.getUint16(off, true); off += 2;
            if (off + bucketCount * 4 > dv.byteLength) {
              off = dv.byteLength + 1;
              break;
            }
            const runStartT =
              base + axisBaseDelta + startBucket * bucketMs;
            if (!firstRun) points.push({gap: true, t: runStartT});
            firstRun = false;
            for (let b = 0; b < bucketCount; b++) {
              const minValue = dv.getInt16(off, true) *
                valueScaleMilli / 1000;
              const maxValue = dv.getInt16(off + 2, true) *
                valueScaleMilli / 1000;
              off += 4;
              const t = runStartT + b * bucketMs;
              if (!Number.isFinite(minValue) || !Number.isFinite(maxValue)) {
                continue;
              }
              points.push({
                t,
                end: t + bucketMs,
                min: Math.min(minValue, maxValue),
                max: Math.max(minValue, maxValue),
                value: (minValue + maxValue) / 2,
                envelope: true,
              });
            }
          }
          if (off > dv.byteLength) break;
        } else {
          break;
        }
        if (points.length) {
          decoded.series[name] = points;
        }
      }
      return decoded;
    }

    async function loadSelectedReportNight(force, refreshCache) {
      const night = selectedReportNight();
      if (!night) {
        resetReportData();
        renderReportSummary();
        return;
      }
      const nightId = reportNightLoadKey(night);
      if (!force && reportLoadedId === nightId &&
          reportResult && reportResult.state === "ready") {
        renderReportSummary();
        renderReportCharts();
        return;
      }
      const therapyIndex = reportTherapyIndex(night);
      if (therapyIndex === null) {
        msg("reportMsg", "Selected night has no report index", false, true);
        return;
      }
      const token = ++reportLoadToken;
      resetReportData();
      reportLoadedId = "";
      renderReportSummary();
      msg("reportMsg", "Loading report...", true, true);
      try {
        if (refreshCache) {
          const queued = await fetch("/api/report/result?night=" +
                      encodeURIComponent(Number(night.start || 0)) +
                      "&refresh_cache=1", {method: "POST"});
          if (!queued.ok && queued.status !== 202) {
            throw new Error(await queued.text());
          }
        }
        const res = await pollReportResult(token, Number(night.start || 0));
        if (!res || token !== reportLoadToken) return;
        if (res.status === 404) {
          msg("reportMsg", "Night not found", false, true);
          return;
        }
        if (res.status !== 200 || !res.json) {
          msg("reportMsg", "Report not ready", false, true);
          renderReportSummary();
          return;
        }
        reportResult = res.json;
        if (res.json.state !== "ready" && res.json.state !== "partial") {
          msg("reportMsg", res.json.error || "Report incomplete", false, true);
          renderReportSummary();
          return;
        }
        const plotEtag = await fetchReportPlot(token, therapyIndex, res.etag);
        if (!plotEtag) return;
        reportLoadedId = nightId;
        renderReportSummary();
        renderReportCharts();
        msg("reportMsg",
            res.json.state === "partial"
              ? "Report loaded (incomplete - some data missing)"
              : "Report loaded", true);
      } catch (error) {
        if (token !== reportLoadToken) return;
        msg("reportMsg", error.message, false, true);
      }
    }

    function scheduleReportPoll() {
      if (reportPollTimer) clearTimeout(reportPollTimer);
      reportPollTimer = setTimeout(() => loadReportSummary(false), 750);
    }

    async function loadReportSummary(autoRefresh) {
      try {
        const data = await (await api("/api/report/summary")).json();
        if (data.state === "busy") {
          msg("reportMsg", "Summary busy; retrying...", true, true);
          scheduleReportPoll();
          return;
        }
        reportSummary = data;
        renderReportSummary();
        if (data.state === "fetching") {
          scheduleReportPoll();
        } else if (autoRefresh && data.state === "idle") {
          await refreshReportSummary(false);
        } else if (autoRefresh && data.state === "ready") {
          await loadSelectedReportNight(false);
        }
      } catch (error) {
        msg("reportMsg", error.message, false, true);
      }
    }

    async function refreshReportSummary(force) {
      try {
        await api("/api/report/summary", {method: "POST"});
        await loadReportSummary(false);
        scheduleReportPoll();
      } catch (error) {
        msg("reportMsg", error.message, false, true);
      }
    }
