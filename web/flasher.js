// Web flasher logic shared by the Korean page (index.html) and the English page (en/index.html).
// Each page passes its own UI strings and the path back to the site root, where
// manifest.json and firmware/ live.
import { ESPLoader, Transport } from "https://cdn.jsdelivr.net/npm/esptool-js@0.7.1/+esm";
import CryptoJS from "https://cdn.jsdelivr.net/npm/crypto-js@4.2.0/+esm";

const REPO_URL = "https://github.com/firepooh/esp32s31-eth1g-iperf";
const EXPECTED_CHIP = "ESP32-S31";

export function init({ base, t }) {
  const $ = (id) => document.getElementById(id);
  const url = (path) => base + path;
  const logEl = $("log");
  const log = (s = "") => { logEl.textContent += s + "\n"; logEl.scrollTop = logEl.scrollHeight; };
  const terminal = {
    clean() { logEl.textContent = ""; },
    writeLine(d) { log(d); },
    write(d) { logEl.textContent += d; logEl.scrollTop = logEl.scrollHeight; },
  };

  // Skipping the stub talks to the chip's ROM loader directly, like esptool --no-stub.
  // The ROM loader supports compressed writes and MD5 checks; full erase needs the stub.
  class Loader extends ESPLoader {
    constructor(options, noStub) { super(options); this.noStub = noStub; }
    async runStub() {
      if (this.noStub) { this.info(t.romLoader); return this.chip; }
      return super.runStub();
    }
  }

  let items = [];
  let transport = null, esploader = null;

  if (!("serial" in navigator)) {
    $("nowebserial").hidden = false;
    $("connect").disabled = true;
  }

  const fmtSize = (n) => (n >= 1048576 ? (n / 1048576).toFixed(2) + " MiB" : (n / 1024).toFixed(1) + " KiB");
  const hex = (buf) => Array.from(new Uint8Array(buf), (b) => b.toString(16).padStart(2, "0")).join("");

  function setBusy(busy) {
    const connected = !!esploader;
    $("flash").disabled = busy || !connected || !items.length;
    $("erase").disabled = busy || !connected || esploader.noStub;
    $("disconnect").disabled = busy || !connected;
    $("connect").disabled = busy || connected || !("serial" in navigator);
    $("nostub").disabled = busy || connected;
  }

  function selected() { return items[$("ver").selectedIndex]; }

  // Date, size and download links of the selected version. No innerHTML.
  function showInfo() {
    const box = $("info");
    box.replaceChildren();
    const it = selected();
    if (!it) return;
    const parts = [];
    if (it.date) parts.push(it.date);
    if (it.size) parts.push(fmtSize(it.size));
    box.append(parts.join(" · ") + (parts.length ? " · " : ""));
    const dl = document.createElement("a");
    dl.href = url(it.file); dl.download = it.file.split("/").pop(); dl.textContent = t.download;
    const rel = document.createElement("a");
    rel.href = `${REPO_URL}/releases/tag/${encodeURIComponent(it.version)}`; rel.textContent = t.releaseNotes;
    box.append(dl, " · ", rel);
    if (it.sha256) {
      const sha = document.createElement("div");
      sha.textContent = "SHA-256: " + it.sha256;
      box.append(sha);
    }
  }

  async function loadManifest() {
    const sel = $("ver");
    try {
      const res = await fetch(url("manifest.json"), { cache: "no-store" });
      if (!res.ok) throw new Error("HTTP " + res.status);
      items = await res.json();
      sel.replaceChildren();
      if (!items.length) {
        sel.append(new Option(t.noFirmware));
        log(t.noFirmwareLog);
        return;
      }
      items.forEach((it, i) => {
        let label = it.version;
        if (i === 0) label += `  (${t.latest})`;
        if (it.prerelease) label += `  (${t.prerelease})`;
        sel.append(new Option(label, it.file));
      });
      showInfo();
    } catch (e) {
      sel.replaceChildren(new Option(t.listFailed));
      log(t.manifestFailed + e.message);
    } finally {
      setBusy(false);
    }
  }
  $("ver").addEventListener("change", showInfo);

  $("connect").addEventListener("click", async () => {
    setBusy(true);
    try {
      const port = await navigator.serial.requestPort({});
      transport = new Transport(port, true);
      const loader = new Loader({ transport, baudrate: parseInt($("baud").value, 10), terminal }, $("nostub").checked);
      const chip = await loader.main();
      log(t.connectedChip + chip);
      if (!String(chip).toUpperCase().includes(EXPECTED_CHIP)) {
        log(t.wrongChip(EXPECTED_CHIP));
        try { await transport.disconnect(); } catch (_) {}
        transport = null;
        return;
      }
      esploader = loader;
    } catch (e) {
      log(t.connectFailed + e.message);
      try { if (transport) await transport.disconnect(); } catch (_) {}
      transport = null;
    } finally {
      setBusy(false);
    }
  });

  $("flash").addEventListener("click", async () => {
    const it = selected();
    if (!it || !esploader) return;
    setBusy(true);
    $("prog").value = 0; $("progtxt").textContent = "0%";
    try {
      log(t.downloading + it.file);
      const res = await fetch(url(it.file), { cache: "no-store" });
      if (!res.ok) throw new Error(t.downloadHttp + res.status);
      const data = new Uint8Array(await res.arrayBuffer());
      if (it.sha256 && globalThis.crypto && crypto.subtle) {
        const got = hex(await crypto.subtle.digest("SHA-256", data));
        if (got !== it.sha256) throw new Error(t.shaMismatch);
        log(t.shaOk);
      }
      log(t.writing(data.length));
      await esploader.writeFlash({
        fileArray: [{ data, address: 0x0 }],
        flashMode: "keep", flashFreq: "keep", flashSize: "keep",
        eraseAll: false, compress: true,
        reportProgress: (_idx, written, total) => {
          const pct = Math.round((written / total) * 100);
          $("prog").value = pct; $("progtxt").textContent = pct + "%";
        },
        calculateMD5Hash: (image) => CryptoJS.MD5(CryptoJS.lib.WordArray.create(image)).toString(),
      });
      log(t.flashDone);
      try { await esploader.after("hard_reset"); } catch (e) { log(t.resetFailed); }
    } catch (e) {
      log(t.flashFailed + e.message);
    } finally {
      setBusy(false);
    }
  });

  $("erase").addEventListener("click", async () => {
    if (!esploader) return;
    if (!confirm(t.eraseConfirm)) return;
    setBusy(true);
    try {
      log(t.erasing);
      await esploader.eraseFlash();
      log(t.eraseDone);
    } catch (e) {
      log(t.eraseFailed + e.message);
    } finally {
      setBusy(false);
    }
  });

  $("disconnect").addEventListener("click", async () => {
    try { if (transport) await transport.disconnect(); } catch (_) {}
    transport = null; esploader = null;
    log(t.disconnected);
    setBusy(false);
  });

  loadManifest();
}
