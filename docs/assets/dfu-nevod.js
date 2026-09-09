/**
 * NEVOD DIY — WebDFU wrapper for Seeed XVF3800 Upgrade (alt=1).
 * Uses vendored dfu.js (devanlai/webdfu, ISC). Never Factory (alt=0).
 */
(function (global) {
  "use strict";

  var VID = 0x2886;
  var PID = 0x001a;
  var ALT_UPGRADE = 1;
  var XFER = 4096;
  var FW_URL = "./flash/application_xvf3800_i2s_slave_v1.0.8_16k.bin";
  var EXPECT_SHA =
    "9dc3308a4db8570603bcc88103d2f0de0291cc92a384d2b25de6eef6f2d99eb8";

  function setStatus(el, msg) {
    if (el) el.textContent = msg;
  }

  function setProgress(el, done, total) {
    if (!el) return;
    if (total && total > 0) {
      var pct = Math.min(100, Math.floor((100 * done) / total));
      el.textContent = pct + "% (" + done + " / " + total + ")";
      el.style.width = pct + "%";
    } else {
      el.textContent = "";
      el.style.width = "0%";
    }
  }

  async function sha256Hex(buf) {
    var hash = await crypto.subtle.digest("SHA-256", buf);
    return Array.from(new Uint8Array(hash))
      .map(function (b) {
        return b.toString(16).padStart(2, "0");
      })
      .join("");
  }

  async function loadFirmware() {
    var res = await fetch(FW_URL, { cache: "no-cache" });
    if (!res.ok) throw new Error("Не удалось скачать образ DSP (" + res.status + ")");
    var buf = await res.arrayBuffer();
    var hex = await sha256Hex(buf);
    if (hex !== EXPECT_SHA) {
      throw new Error("SHA-256 образа не совпал (ожидали Seeed v1.0.8 slave 16k)");
    }
    return buf;
  }

  function pickUpgrade(interfaces) {
    for (var i = 0; i < interfaces.length; i++) {
      var a = interfaces[i].alternate;
      if (a && a.alternateSetting === ALT_UPGRADE) return interfaces[i];
    }
    return null;
  }

  async function flash(opts) {
    opts = opts || {};
    var statusEl = opts.statusEl;
    var progressEl = opts.progressEl;
    var buttonEl = opts.buttonEl;

    if (!navigator.usb) {
      throw new Error("Нужен Chrome или Edge с WebUSB (не Safari / Firefox / телефон)");
    }
    if (typeof dfu === "undefined") {
      throw new Error("dfu.js не загружен");
    }

    if (buttonEl) buttonEl.disabled = true;
    setProgress(progressEl, 0, 1);
    setStatus(statusEl, "Загрузка образа v1.0.8…");

    var firmware;
    try {
      firmware = await loadFirmware();
    } catch (e) {
      if (buttonEl) buttonEl.disabled = false;
      throw e;
    }

    setStatus(statusEl, "Выберите reSpeaker XVF3800 (DFU)…");
    var selected;
    try {
      selected = await navigator.usb.requestDevice({
        filters: [{ vendorId: VID, productId: PID }],
      });
    } catch (e) {
      if (buttonEl) buttonEl.disabled = false;
      if (e && e.name === "NotFoundError") {
        throw new Error("Устройство не выбрано. Mute+Reset, кабель у 3.5 mm, WinUSB (Zadig).");
      }
      throw e;
    }

    var interfaces = dfu.findDeviceDfuInterfaces(selected);
    var upgrade = pickUpgrade(interfaces);
    if (!upgrade) {
      if (buttonEl) buttonEl.disabled = false;
      throw new Error(
        "Нет слота Upgrade (alt=1). Mute+Reset до мигания Mute; не выбирайте Factory."
      );
    }

    var device = new dfu.Device(selected, upgrade);
    device.logInfo = function (msg) {
      setStatus(statusEl, String(msg));
    };
    device.logWarning = function (msg) {
      setStatus(statusEl, "⚠ " + msg);
    };
    device.logProgress = function (done, total) {
      setProgress(progressEl, done, total);
    };

    try {
      setStatus(statusEl, "Подключение Upgrade (alt=1)…");
      await device.open();
      setStatus(statusEl, "Прошивка… не вынимайте кабель");
      await device.do_download(XFER, firmware, true);
      setProgress(progressEl, firmware.byteLength, firmware.byteLength);
      setStatus(
        statusEl,
        "Готово. Выньте кабель у 3.5 mm на ~5 с и вставьте снова. ESP шьётся на XIAO."
      );
    } catch (e) {
      var msg = String(e && e.message ? e.message : e);
      // Write usually finished; browser/OS often cannot USB-reset XVF3800.
      if (/reset for manifestation|Unable to reset the device/i.test(msg)) {
        setProgress(progressEl, firmware.byteLength, firmware.byteLength);
        setStatus(
          statusEl,
          "Запись, скорее всего, прошла (сбой только USB reset). Выньте питание/USB у 3.5 mm на ~5 с и вставьте снова — не жмите Flash повторно."
        );
        return;
      }
      if (/timeout|Timeout|NETWORK_ERR|TransferError/i.test(msg)) {
        msg =
          "TIMEOUT / сбой передачи. Не жмите кнопку снова сразу: выньте USB ~10 с, снова Mute+Reset, одна попытка. " +
          msg;
      }
      setStatus(statusEl, msg);
      throw e;
    } finally {
      try {
        await device.close();
      } catch (_) {}
      if (buttonEl) buttonEl.disabled = false;
    }
  }

  function bind(buttonId, statusId, progressId) {
    var btn = document.getElementById(buttonId);
    var status = document.getElementById(statusId);
    var progress = document.getElementById(progressId);
    if (!btn) return;
    btn.addEventListener("click", function () {
      flash({ statusEl: status, progressEl: progress, buttonEl: btn }).catch(
        function () {}
      );
    });
  }

  global.NevodDfu = { flash: flash, bind: bind, FW_URL: FW_URL, EXPECT_SHA: EXPECT_SHA };
})(window);
