const board = document.querySelector("#board");
const installer = document.querySelector("esp-web-install-button");
const connect = installer.querySelector("button");
const download = document.querySelector("#download");

function selectBoard() {
  const profile = board.selectedOptions[0];
  connect.disabled = !profile.value || !customElements.get("esp-web-install-button");
  download.hidden = !profile.value;

  if (profile.value) {
    installer.setAttribute("manifest", profile.value);
    download.href = profile.dataset.zip;
  } else {
    installer.removeAttribute("manifest");
    download.removeAttribute("href");
  }
}

board.addEventListener("change", selectBoard);
selectBoard();
import("./esp-web-tools/install-button.js").then(selectBoard).catch(() => {
  document.querySelector("#load-error").hidden = false;
});
