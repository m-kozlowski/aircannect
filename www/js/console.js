(() => {
    "use strict";

    let consoleSeq = -1;
    let consoleEnd = 0;

    async function loadConsole(showError) {
      try {
        const response = await AirCANnect.http.requestOk("/api/console");
        const before = consoleSeq;
        renderConsole(await response.json());
        return consoleSeq !== before;
      } catch (error) {
        if (showError) {
          const output = document.getElementById("consoleLog");
          output.textContent += "\nERR: " + error.message + "\n";
          output.scrollTop = output.scrollHeight;
        }
      }
      return false;
    }

    function renderConsole(data) {
      const seq = Number(data && data.seq);
      if (!data || !Number.isFinite(seq) || seq <= consoleSeq) return;
      const output = document.getElementById("consoleLog");
      if (data.reset || data.log !== undefined) {
        output.textContent = data.log || "";
        consoleEnd = Number(data.end ?? output.textContent.length);
        consoleSeq = seq;
      } else if (data.append !== undefined) {
        const from = Number(data.from ?? consoleEnd);
        const to = Number(data.to ?? (from + (data.append || "").length));
        if (from !== consoleEnd) {
          if (to > consoleEnd) setTimeout(() => loadConsole(false), 0);
          return;
        }
        output.textContent += data.append || "";
        if (output.textContent.length > 4096) {
          output.textContent = output.textContent.slice(-4096);
        }
        consoleEnd = to;
        consoleSeq = seq;
      }
      output.scrollTop = output.scrollHeight;
    }

    async function sendConsoleCommand() {
      const input = document.getElementById("consoleInput");
      const command = input.value.trim();
      if (!command) return;
      input.value = "";

      try {
        const response = await AirCANnect.http.requestOk("/api/console", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({cmd: command}),
        });
        const data = await response.json();
        if (data.log !== undefined) renderConsole(data);
        setTimeout(() => loadConsole(true), 300);
      } catch (error) {
        const output = document.getElementById("consoleLog");
        output.textContent += "\nERR: " + error.message + "\n";
        output.scrollTop = output.scrollHeight;
      }
    }

    async function clearConsoleLog() {
      try {
        await AirCANnect.http.requestOk("/api/console/clear", {method: "POST"});
        const output = document.getElementById("consoleLog");
        output.textContent = "";
        consoleSeq = -1;
        consoleEnd = 0;
        output.scrollTop = 0;
      } catch (error) {
        const output = document.getElementById("consoleLog");
        output.textContent += "\nERR: " + error.message + "\n";
        output.scrollTop = output.scrollHeight;
      }
    }


    AirCANnect.actions.register("console.send", () => sendConsoleCommand());
    AirCANnect.actions.register("console.clear", () => clearConsoleLog());
    AirCANnect.pages.onLoad("console", (refresh) => {
      loadConsole();
      if (!refresh) {
        setTimeout(() => document.getElementById("consoleInput").focus(), 0);
      }
    });
    AirCANnect.events.subscribe("console", renderConsole);
})();
