function flash(msg, ok) {
  const el = $("flash");
  if (!el) return;
  el.hidden = false;
  el.classList.toggle("bad", ok === false);
  el.classList.toggle("ok", ok !== false);
  el.textContent = msg;
  el.classList.add("show");
  clearTimeout(flash._t);
  flash._t = setTimeout(() => {
    el.classList.remove("show");
  }, 4200);
}

async function tick() {
  try {
    const headers = {};
    const tok = sessionStorage.getItem(tokenKey);
    if (tok) headers.Authorization = "Bearer " + tok;
    const r = await fetch("/api/dashboard", {
      cache: "no-store",
      headers,
    });
    if (r.status === 401) {
      sessionStorage.removeItem(tokenKey);
      location.href = "/";
      return;
    }
    if (!r.ok) {
      tickFailStreak++;
      setLiveStatus(lastTickOkAt ? "stale" : "error", lastTickOkAt);
      if (tickFailStreak === 1 || tickFailStreak % 5 === 0) {
        flash("dashboard HTTP " + r.status + " — повтор…", false);
      }
      return;
    }
    lastSnap = await r.json();
    tickFailStreak = 0;
    lastTickOkAt = Date.now();
    setLiveStatus("ok", lastTickOkAt);
    renderHero(lastSnap);
    renderChecklist(lastSnap);
    renderTimeline(lastSnap);
    renderFeed(lastSnap);
    renderMqtt(lastSnap);
    renderAdsbOps(lastSnap);
    renderWebhooks(lastSnap);
    await paintMelDisk(lastSnap);
    renderNodes(lastSnap);
    renderDetail(lastSnap);
    if (typeof renderHubMap === "function") renderHubMap(lastSnap);
    const hub = lastSnap.hub || {};
    const fwd = mergeHubForward(hub.forward || {});
    paintCloudFwd(fwd, {
      unlocked: !!hubUnlockTok,
      unlock_set: !!(hub.engineer && hub.engineer.unlock_set),
    });
    if (
      fwd.has_token &&
      fwd.probe_ok !== true &&
      fwd.probe_ok !== false &&
      typeof refreshCloudFwdProbe === "function"
    ) {
      refreshCloudFwdProbe().then((rich) => {
        if (!rich || !lastSnap) return;
        paintCloudFwd(mergeHubForward(Object.assign({}, hub.forward || {}, rich)), {
          unlocked: !!hubUnlockTok,
          unlock_set: !!(hub.engineer && hub.engineer.unlock_set),
        });
      });
    }
    if ($("hubVer") && lastSnap.version) {
      $("hubVer").textContent = "v" + lastSnap.version + " · Hub";
    }
  } catch (e) {
    tickFailStreak++;
    setLiveStatus(lastTickOkAt ? "stale" : "error", lastTickOkAt);
    if (tickFailStreak === 1 || tickFailStreak % 5 === 0) {
      flash("dashboard offline: " + (e && e.message ? e.message : e), false);
    }
    console.warn(e);
  }
}

if ($("showStale")) $("showStale").onchange = () => { if (lastSnap) { renderNodes(lastSnap); renderDetail(lastSnap); } };
$("toastClose").onclick = () => $("toast").classList.remove("show");
$("toastOpen").onclick = () => { if (pendingMelFile) openMelSpectrogram(pendingMelFile); };
function stopMelAudio() {
  const audio = $("melAudio");
  if (!audio) return;
  audio.pause();
  audio.removeAttribute("src");
  audio.load();
}
$("melModalClose").onclick = () => { stopMelAudio(); $("melModal").classList.remove("open"); };
$("melModal").onclick = (e) => { if (e.target === $("melModal")) { stopMelAudio(); $("melModal").classList.remove("open"); } };
if ($("melPrev")) $("melPrev").onclick = () => melPagerStep(-1);
if ($("melNext")) $("melNext").onclick = () => melPagerStep(1);
if ($("authModal")) {
  $("authModal").onclick = (e) => { if (e.target === $("authModal")) closeAuthModal(null); };
}
if ($("authTokenCancel")) $("authTokenCancel").onclick = () => closeAuthModal(null);
if ($("authTokenSave")) $("authTokenSave").onclick = () => {
  const v = ($("authTokenInput") && $("authTokenInput").value) || "";
  closeAuthModal(v);
};
if ($("authTokenInput")) {
  $("authTokenInput").addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      e.preventDefault();
      closeAuthModal($("authTokenInput").value || "");
    } else if (e.key === "Escape") {
      e.preventDefault();
      closeAuthModal(null);
    }
  });
}
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape") {
    const auth = $("authModal");
    if (auth && auth.classList.contains("open")) closeAuthModal(null);
    return;
  }
  const modal = $("melModal");
  if (!modal || !modal.classList.contains("open")) return;
  if (e.key === "ArrowLeft") { e.preventDefault(); melPagerStep(-1); }
  if (e.key === "ArrowRight") { e.preventDefault(); melPagerStep(1); }
});

$("btnReset").onclick = async () => {
  if (!confirm("Удалить устройства, ленту и спектрограммы с этого Hub?")) return;
  const r = await fetch("/api/lab/reset", {
    method: "POST",
    credentials: "same-origin",
  });
  if (r.status === 401) {
    flash("Войдите снова", false);
    return;
  }
  const j = await r.json().catch(() => ({}));
  selected = null;
  viewNode = "all";
  lastToastKey = "";
  flash("Стенд сброшен · Mel удалено: " + (j.mel_removed != null ? j.mel_removed : "?"), r.ok);
  await tick();
};
function melDateQuery() {
  const from = ($("melFrom") && $("melFrom").value) || "";
  const to = ($("melTo") && $("melTo").value) || "";
  const node = (($("melNode") && $("melNode").value) || "").trim();
  const sort = (($("melSort") && $("melSort").value) || "date");
  const q = [];
  if (from) q.push("from=" + encodeURIComponent(from));
  if (to) q.push("to=" + encodeURIComponent(to));
  if (node) q.push("node=" + encodeURIComponent(node));
  if (sort && sort !== "date") q.push("sort=" + encodeURIComponent(sort));
  return q.length ? ("?" + q.join("&")) : "";
}

function melZipFilename(kind) {
  const from = ($("melFrom") && $("melFrom").value) || "";
  const to = ($("melTo") && $("melTo").value) || "";
  if (!from && !to) return kind + ".zip";
  const a = (from || "start").replace(/-/g, "");
  const b = (to || "end").replace(/-/g, "");
  return kind + "-" + a + "-" + b + ".zip";
}

async function paintMelDisk(s) {
  const q = melDateQuery();
  if (!q) {
    lastMelDisk = null;
    renderMelFiles(s);
    return;
  }
  const headers = {};
  const tok = sessionStorage.getItem(tokenKey);
  if (tok) headers.Authorization = "Bearer " + tok;
  try {
    const r = await fetch("/api/mel" + q, { cache: "no-store", credentials: "same-origin", headers });
    if (r.status === 401) {
      sessionStorage.removeItem(tokenKey);
      location.href = "/";
      return;
    }
    const j = await r.json().catch(() => ({}));
    if (!r.ok) {
      flash("Mel даты: " + (j.error || ("HTTP " + r.status)), false);
      lastMelDisk = [];
      renderMelFiles({ mel_saved: [] });
      return;
    }
    lastMelDisk = j.files || [];
    renderMelFiles({ mel_saved: lastMelDisk });
  } catch (e) {
    flash("Mel даты: " + (e && e.message ? e.message : e), false);
    renderMelFiles(s);
  }
}

async function downloadMelZip(labeled) {
  const q = melDateQuery();
  const url = labeled ? ("/api/mel/archive-gt.zip" + q) : ("/api/mel/archive.zip" + q);
  const name = melZipFilename(labeled ? "MEL-gt" : "MEL");
  flash(labeled ? "Собираю Mel+WAV…" : "Собираю Mel ZIP…", true);
  try {
    const n = await downloadUrl(url, name);
    flash((labeled ? "Mel+WAV скачан (" : "Mel ZIP скачан (") + fmtBytes(n) + ")", true);
  } catch (e) {
    flash((labeled ? "Mel+WAV: " : "Mel ZIP: ") + (e && e.message ? e.message : e), false);
  }
}

$("btnMelZip").onclick = async () => {
  await downloadMelZip(false);
};
if ($("btnMelZipDisk")) $("btnMelZipDisk").onclick = async () => { await downloadMelZip(false); };
if ($("btnMelGtZip")) $("btnMelGtZip").onclick = async () => { await downloadMelZip(true); };
if ($("btnMelGtZipLab")) $("btnMelGtZipLab").onclick = async () => { await downloadMelZip(true); };
if ($("melDateClear")) {
  $("melDateClear").onclick = () => {
    if ($("melFrom")) $("melFrom").value = "";
    if ($("melTo")) $("melTo").value = "";
    if ($("melNode")) $("melNode").value = "";
    if ($("melSort")) $("melSort").value = "date";
    if (lastSnap) paintMelDisk(lastSnap);
  };
}
["melFrom", "melTo", "melNode", "melSort"].forEach((id) => {
  if ($(id)) $(id).onchange = () => { if (lastSnap) paintMelDisk(lastSnap); };
});
if ($("melNode")) {
  $("melNode").oninput = () => { if (lastSnap) paintMelDisk(lastSnap); };
}
$("btnCa").onclick = async () => {
  try {
    const n = await downloadUrl("/ca.crt", "ca.crt");
    flash("CA скачан (" + fmtBytes(n) + ")", true);
  } catch (e) {
    flash("CA: " + (e && e.message ? e.message : e), false);
  }
};
$("btnClearMel").onclick = async () => {
  if (!confirm("Удалить все mel_*.bin на диске Mock?")) return;
  const r = await fetch("/api/mel/clear", {
    method: "POST",
    credentials: "same-origin",
  });
  if (r.status === 401) {
    flash("Войдите снова", false);
    return;
  }
  const j = await r.json().catch(() => ({}));
  lastToastKey = "";
  if (!r.ok) {
    flash(j.error || ("Стирание Mel: HTTP " + r.status), false);
    return;
  }
  flash("Mel удалено: " + (j.removed != null ? j.removed : "?"), true);
  await tick();
};

["mqttHost","mqttPort","mqttUser","mqttPass"].forEach(id => {
  const el = $(id);
  if (!el) return;
  el.addEventListener("input", () => { el.dataset.touched = "1"; });
});
$("mqttConnect").onclick = () => {
  const body = {
    enabled: true,
    host: $("mqttHost").value.trim(),
    port: Number($("mqttPort").value || 1883),
    username: ($("mqttUser").value || "").trim(),
  };
  const pw = ($("mqttPass") && $("mqttPass").value) || "";
  if (pw) body.password = pw;  // пусто = не менять сохранённый
  postMqtt(body);
};
$("mqttDisconnect").onclick = () => postMqtt({ enabled: false });

["cloudOrigin"].forEach(id => {
  const el = $(id);
  if (!el) return;
  el.addEventListener("input", () => { el.dataset.touched = "1"; });
  el.addEventListener("change", () => { el.dataset.touched = "1"; });
});
(function(){
  let verClicks = 0, verTimer = null;
  const ver = $("hubVer");
  if (!ver) return;
  ver.addEventListener("click", function(){
    verClicks++;
    clearTimeout(verTimer);
    if (verClicks >= 5) {
      verClicks = 0;
      revealEngMenus();
      if (hubUnlockTok) refreshCloudPanel();
    }
    verTimer = setTimeout(function(){ verClicks = 0; }, 2000);
  });
})();
window.addEventListener("pageshow", () => {
  hideEngMenus();
  showView("ops");
});
if ($("btnOps")) $("btnOps").onclick = () => showView("ops");
if ($("btnMap")) $("btnMap").onclick = () => showView("map");
if ($("btnSettings")) $("btnSettings").onclick = () => {
  const set = $("viewSettings");
  const open = set && !set.classList.contains("view-hidden");
  showView(open ? "ops" : "settings");
};
if ($("btnLogout")) $("btnLogout").onclick = async () => {
  try {
    await fetch("/api/hub/ui/logout", { method: "POST", credentials: "same-origin", cache: "no-store" });
  } catch (_) { /* ignore */ }
  location.href = "/";
};
if ($("uiLoginSaveBtn")) $("uiLoginSaveBtn").onclick = () => saveUiLogin();
if ($("engUnlockBtn")) $("engUnlockBtn").onclick = () => engUnlock(false);
if ($("engSetPwBtn")) $("engSetPwBtn").onclick = () => engUnlock(true);
if ($("cloudUplinkSave")) $("cloudUplinkSave").onclick = () => saveCloudUplink();
if ($("cloudTokenSave")) $("cloudTokenSave").onclick = () => saveCloudUplink();
if ($("cloudOriginSave")) $("cloudOriginSave").onclick = () => saveCloudOrigin();
if ($("cloudUrlClose")) $("cloudUrlClose").onclick = () => hideEngMenus();
if ($("alertSaveBtn")) $("alertSaveBtn").onclick = () => saveAlerts();
if ($("mapWxSaveBtn")) $("mapWxSaveBtn").onclick = () => saveMapWxKey(false);
if ($("mapWxClearBtn")) $("mapWxClearBtn").onclick = () => saveMapWxKey(true);
if ($("mapWxClearEcoBtn")) $("mapWxClearEcoBtn").onclick = () => saveMapWxKey("eco");
["ecowittShareUrl", "ecowittRadiusKm"].forEach((id) => {
  const el = $(id);
  if (el) el.addEventListener("input", () => { el.dataset.touched = "1"; });
});
if ($("zonesSaveBtn")) $("zonesSaveBtn").onclick = () => saveZonesSettings();
if ($("zonesReloadBtn")) $("zonesReloadBtn").onclick = () => loadZonesSettings();
if ($("feederAddBtn")) $("feederAddBtn").onclick = () => {
  if (!zonesDraft) ensureZonesDraft({});
  zonesDraft.feeders = collectFeedersFromDom();
  zonesDraft.feeders.push({
    id: _fid(), name: "Feeder", url: "", enabled: false, poll_interval_s: 5, stale_s: 30,
  });
  paintFeederCards();
  paintZoneCards();
};
if ($("zoneAddBtn")) $("zoneAddBtn").onclick = () => {
  if (!zonesDraft) ensureZonesDraft({});
  zonesDraft.feeders = collectFeedersFromDom();
  zonesDraft.zones = collectZonesFromDom();
  const fid = (zonesDraft.feeders[0] && zonesDraft.feeders[0].id) || "default";
  zonesDraft.zones.push({
    id: _zid(), name: "Zone", lat: null, lon: null, radius_m: 10000,
    feeder_id: fid, filter_det: false, filter_hb: false, filter_mel: false,
    forward_mel: null, alert_mute: false,
  });
  paintZoneCards();
  paintNodeZoneList();
  paintMelPolicy();
};
["melForwardCloud", "melLanOnlyGlobal", "gpsOverrideAge"].forEach((id) => {
  const el = $(id);
  if (!el) return;
  el.addEventListener("input", () => { el.dataset.touched = "1"; });
  el.addEventListener("change", () => { el.dataset.touched = "1"; });
});
["alertMinThreat","alertCooldown","tgEnabled","tgChat","tgToken","iftttEnabled","iftttUrl","emailEnabled","smtpHost","smtpPort","smtpUser","smtpPass","smtpFrom","smtpTo","smtpTls"].forEach(id => {
  const el = $(id);
  if (!el) return;
  el.addEventListener("input", () => { el.dataset.touched = "1"; });
  el.addEventListener("change", () => { el.dataset.touched = "1"; });
});
hideEngMenus();
syncSettingsGear();
showView("ops");
// First paint before poll — onboarding empty states.
if ($("nodeList") && !$("nodeList").children.length) {
  $("nodeList").innerHTML = emptyOnboardHtml("Ждём первый пакет…");
  bindEmptyCa($("nodeList"));
}
if ($("detail") && !$("detail").children.length) {
  $("detail").innerHTML = emptyOnboardHtml("Нет устройств — дождитесь первого пакета");
  bindEmptyCa($("detail"));
}

if ($("btnDashboardJson")) {
  $("btnDashboardJson").onclick = async (ev) => {
    ev.preventDefault();
    try {
      const headers = {};
      const tok = sessionStorage.getItem(tokenKey);
      if (tok) headers.Authorization = "Bearer " + tok;
      const r = await fetch("/api/dashboard", {
        cache: "no-store",
        headers,
      });
      if (r.status === 401) {
        location.href = "/";
        return;
      }
      if (!r.ok) throw new Error("HTTP " + r.status);
      const blob = new Blob([JSON.stringify(await r.json(), null, 2)], { type: "application/json" });
      const obj = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = obj;
      a.download = "dashboard.json";
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(obj), 60000);
    } catch (e) {
      flash("dashboard.json: " + (e && e.message ? e.message : e), false);
    }
  };
}

// Не setInterval(1s): при медленном /api/dashboard запросы накладываются и душат Hub.
(function pollLoop() {
  const t0 = Date.now();
  Promise.resolve(tick()).finally(function () {
    const took = Date.now() - t0;
    const wait = Math.max(2500, 4000 - Math.min(took, 3500));
    setTimeout(pollLoop, wait);
  });
})();
