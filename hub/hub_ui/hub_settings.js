const CLOUD_ORIGIN_DEFAULT = "https://nevod.endorphine.agency";
let cloudFwdHasToken = false;
let _cloudFwdProbeCache = null;
let _cloudFwdProbeFetchAt = 0;

function _fwdProbeFields(fwd) {
  if (!fwd || typeof fwd !== "object") return null;
  const hasProbe =
    fwd.probe_ok === true ||
    fwd.probe_ok === false ||
    (fwd.probe_at != null && fwd.probe_at !== "");
  if (!hasProbe) return null;
  return {
    probe_ok: fwd.probe_ok,
    probe_at: fwd.probe_at,
    probe_error: fwd.probe_error || "",
    probe_http: fwd.probe_http,
  };
}

/** Dashboard forward без probe — подмешать кэш или /api/hub/cloud. */
function mergeHubForward(dashFwd) {
  const a = dashFwd && typeof dashFwd === "object" ? dashFwd : {};
  if (a.probe_ok === true || a.probe_ok === false) return a;
  const cached = _fwdProbeFields(_cloudFwdProbeCache);
  if (cached) return Object.assign({}, a, cached);
  return a;
}

function _cloudFwdStatusTone(fwd, src) {
  if (!fwd || fwd.enabled === false) return "muted";
  if (fwd.last_error === "cabinet_key_missing") return "err";
  if (src === "none") return "muted";
  if (src === "factory") {
    if (fwd.probe_ok === true) return "ok";
    if (fwd.probe_ok === false) return "err";
    return "warn";
  }
  if (fwd.probe_ok === true) return "ok";
  if (fwd.probe_ok === false) return "err";
  return "warn";
}

let _cloudFwdProbeInflight = null;
async function refreshCloudFwdProbe() {
  const now = Date.now();
  if (now - _cloudFwdProbeFetchAt < 8000) return null;
  if (_cloudFwdProbeInflight) return _cloudFwdProbeInflight;
  _cloudFwdProbeFetchAt = now;
  _cloudFwdProbeInflight = hubCloudFetch("/api/hub/cloud", { unlock: !!hubUnlockTok })
    .then((j) => {
      _cloudFwdProbeInflight = null;
      if (!j || !j.forward) return null;
      const p = _fwdProbeFields(j.forward);
      if (p) _cloudFwdProbeCache = Object.assign({}, j.forward, p);
      return j.forward;
    })
    .catch(() => {
      _cloudFwdProbeInflight = null;
      return null;
    });
  return _cloudFwdProbeInflight;
}

function setHubEngMode(on) {
  document.body.classList.toggle("hub-eng", !!on);
}

function setEngOriginEnabled(on) {
  const st = $("engUnlockStatus");
  if (st) {
    st.textContent = on ? "открыто" : "закрыто";
    st.classList.toggle("on", !!on);
  }
}

function showCloudUrlFields(on) {
  const box = $("cloudUrlFields");
  const btn = $("cloudOriginSave");
  const setPw = $("engSetPwBtn");
  const close = $("cloudUrlClose");
  const open = $("engUnlockBtn");
  if (box) box.hidden = !on;
  if (btn) btn.hidden = !on;
  if (setPw) setPw.hidden = !on;
  if (close) close.hidden = !on;
  if (open) open.hidden = !!on;
}

function revealEngMenus() {
  hubEngRevealed = true;
  const box = $("cloudUrlSecret");
  if (box) box.classList.add("on");
  showCloudUrlFields(!!hubUnlockTok);
  if (hubUnlockTok) refreshCloudPanel();
}

function hideEngMenus() {
  hubEngRevealed = false;
  hubUnlockTok = "";
  setEngOriginEnabled(false);
  setHubEngMode(false);
  showCloudUrlFields(false);
  const box = $("cloudUrlSecret");
  if (box) box.classList.remove("on");
}

function syncSettingsGear() {
  const gear = $("btnSettings");
  if (gear) gear.hidden = false;
}

function showView(name) {
  const ops = $("viewOps");
  const map = $("viewMap");
  const set = $("viewSettings");
  const gear = $("btnSettings");
  const btnOps = $("btnOps");
  const btnMap = $("btnMap");
  if (ops) ops.classList.toggle("view-hidden", name !== "ops");
  if (map) map.classList.toggle("view-hidden", name !== "map");
  if (set) set.classList.toggle("view-hidden", name !== "settings");
  if (gear) gear.classList.toggle("active", name === "settings");
  if (btnOps) btnOps.classList.toggle("active", name === "ops");
  if (btnMap) btnMap.classList.toggle("active", name === "map");
  document.body.classList.toggle("hub-map-open", name === "map");
  if (name === "settings") enterSettingsTab();
  if (name === "map" && typeof hubMapShow === "function") hubMapShow();
}

async function enterSettingsTab() {
  showSettingsBody(true);
  syncSettingsGear();
  await loadSettingsAlerts();
  await loadMapWxSettings();
  await loadZonesSettings();
}

function showSettingsBody(on) {
  const body = $("settingsBody");
  if (body) body.classList.toggle("view-hidden", !on);
  if ($("alertCfgStatus")) $("alertCfgStatus").textContent = on ? "ok" : "—";
}

async function saveUiLogin() {
  const user = ($("uiNewUser") && $("uiNewUser").value || "").trim();
  const current_password = ($("uiCurrentPw") && $("uiCurrentPw").value) || "";
  const a = ($("uiNewPw") && $("uiNewPw").value) || "";
  const b = ($("uiNewPw2") && $("uiNewPw2").value) || "";
  if (a !== b) { flash("Пароли не совпадают", false); return; }
  if (a.length < 8) { flash("Пароль ≥ 8 символов", false); return; }
  const r = await fetch("/api/hub/ui/password", {
    method: "POST",
    credentials: "same-origin",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ user, password: a, current_password }),
    cache: "no-store",
  });
  if (r.status === 401) {
    location.href = "/";
    return;
  }
  const j = await r.json().catch(() => ({}));
  if (!r.ok) {
    flash(j.error === "bad_login" ? "Неверный текущий пароль" : (j.error || ("HTTP " + r.status)), false);
    return;
  }
  ["uiNewUser", "uiCurrentPw", "uiNewPw", "uiNewPw2"].forEach((id) => {
    if ($(id)) $(id).value = "";
  });
  flash("Пароль входа сохранён", true);
}

async function loadSettingsAlerts() {
  const j = await hubCloudFetch("/api/hub/webhooks");
  if (j && j.webhooks) fillAlertForm(j.webhooks);
}

async function loadMapWxSettings() {
  const j = await hubCloudFetch("/api/hub/map_wx");
  const st = $("mapWxStatus");
  if (!st) return;
  if (!j) {
    st.textContent = "нет связи";
    return;
  }
  if ($("ecowittShareUrl") && !$("ecowittShareUrl").dataset.touched) {
    $("ecowittShareUrl").value = j.ecowitt_share_url || "";
  }
  if ($("ecowittRadiusKm") && !$("ecowittRadiusKm").dataset.touched) {
    $("ecowittRadiusKm").value = j.ecowitt_radius_km != null ? j.ecowitt_radius_km : 8;
  }
  const bits = [];
  if (j.ecowitt_configured) bits.push("Ecowitt " + (j.ecowitt_radius_km || 8) + " км");
  else bits.push("Ecowitt нет");
  if (j.configured) bits.push("ключ " + (j.key_hint || "задан") + (j.via === "env" ? " (env)" : ""));
  else bits.push("Open-Meteo free");
  st.textContent = bits.join(" · ");
}

async function saveMapWxKey(clear) {
  const body = {};
  if (clear === "eco") {
    body.ecowitt_share_url = "clear";
  } else if (clear) {
    body.api_key = "clear";
  } else {
    const eco = (($("ecowittShareUrl") && $("ecowittShareUrl").value) || "").trim();
    const key = (($("openMeteoKey") && $("openMeteoKey").value) || "").trim();
    let r = Number(($("ecowittRadiusKm") && $("ecowittRadiusKm").value) || 8);
    if (!(r >= 1 && r <= 50)) r = 8;
    body.ecowitt_radius_km = r;
    if (eco) body.ecowitt_share_url = eco;
    if (key) body.api_key = key;
    if (!eco && !key && $("ecowittRadiusKm")) {
      // allow saving radius alone when URL already stored
      body.ecowitt_radius_km = r;
    }
    if (!("ecowitt_share_url" in body) && !("api_key" in body)) {
      // still POST radius
    }
  }
  if (!clear && !("api_key" in body) && !("ecowitt_share_url" in body) && !("ecowitt_radius_km" in body)) {
    flash("Введите URL Ecowitt или ключ", false);
    return;
  }
  const j = await hubCloudFetch("/api/hub/map_wx", { method: "POST", body });
  if (!j) return;
  if ($("openMeteoKey")) $("openMeteoKey").value = "";
  if ($("ecowittShareUrl")) delete $("ecowittShareUrl").dataset.touched;
  if ($("ecowittRadiusKm")) delete $("ecowittRadiusKm").dataset.touched;
  await loadMapWxSettings();
  flash(clear === "eco" ? "Ecowitt сброшен" : (clear ? "Ключ сброшен" : "Погода сохранена"), true);
}

function fillAlertForm(wh) {
  if (!wh) return;
  if ($("alertMinThreat") && !$("alertMinThreat").dataset.touched)
    $("alertMinThreat").value = wh.min_threat != null ? wh.min_threat : 0.5;
  if ($("alertCooldown") && !$("alertCooldown").dataset.touched)
    $("alertCooldown").value = wh.cooldown_s != null ? wh.cooldown_s : 60;
  const tg = wh.telegram || {};
  if ($("tgEnabled") && !$("tgEnabled").dataset.touched) $("tgEnabled").checked = !!tg.enabled;
  if ($("tgChat") && !$("tgChat").dataset.touched) $("tgChat").value = tg.chat_id || "";
  const ift = wh.ifttt || {};
  if ($("iftttEnabled") && !$("iftttEnabled").dataset.touched) $("iftttEnabled").checked = !!ift.enabled;
  const em = wh.email || {};
  if ($("emailEnabled") && !$("emailEnabled").dataset.touched) $("emailEnabled").checked = !!em.enabled;
  if ($("smtpHost") && !$("smtpHost").dataset.touched) $("smtpHost").value = em.host || "";
  if ($("smtpPort") && !$("smtpPort").dataset.touched) $("smtpPort").value = em.port != null ? em.port : 587;
  if ($("smtpUser") && !$("smtpUser").dataset.touched) $("smtpUser").value = em.user || "";
  if ($("smtpFrom") && !$("smtpFrom").dataset.touched) $("smtpFrom").value = em.from_addr || "";
  if ($("smtpTo") && !$("smtpTo").dataset.touched) $("smtpTo").value = em.to || "";
  if ($("smtpTls") && !$("smtpTls").dataset.touched) $("smtpTls").checked = em.tls !== false;
}

async function saveAlerts() {
  const body = {
    min_threat: Number(($("alertMinThreat") && $("alertMinThreat").value) || 0.5),
    cooldown_s: Number(($("alertCooldown") && $("alertCooldown").value) || 60),
    telegram: {
      enabled: !!( $("tgEnabled") && $("tgEnabled").checked ),
      chat_id: ($("tgChat") && $("tgChat").value || "").trim(),
    },
    ifttt: {
      enabled: !!( $("iftttEnabled") && $("iftttEnabled").checked ),
    },
    email: {
      enabled: !!( $("emailEnabled") && $("emailEnabled").checked ),
      host: ($("smtpHost") && $("smtpHost").value || "").trim(),
      port: Number(($("smtpPort") && $("smtpPort").value) || 587),
      user: ($("smtpUser") && $("smtpUser").value || "").trim(),
      from_addr: ($("smtpFrom") && $("smtpFrom").value || "").trim(),
      to: ($("smtpTo") && $("smtpTo").value || "").trim(),
      tls: !!( $("smtpTls") && $("smtpTls").checked ),
    },
  };
  const tgTok = ($("tgToken") && $("tgToken").value) || "";
  if (tgTok.trim()) body.telegram.bot_token = tgTok.trim();
  const iftUrl = ($("iftttUrl") && $("iftttUrl").value) || "";
  if (iftUrl.trim()) body.ifttt.url = iftUrl.trim();
  const smtpPass = ($("smtpPass") && $("smtpPass").value) || "";
  if (smtpPass.trim()) body.email.password = smtpPass.trim();
  const j = await hubCloudFetch("/api/hub/webhooks", { method: "POST", body });
  if (!j) return;
  ["tgToken","iftttUrl","smtpPass"].forEach(id => { if ($(id)) $(id).value = ""; });
  ["alertMinThreat","alertCooldown","tgEnabled","tgChat","iftttEnabled","emailEnabled","smtpHost","smtpPort","smtpUser","smtpFrom","smtpTo","smtpTls"].forEach(id => {
    if ($(id)) delete $(id).dataset.touched;
  });
  fillAlertForm(j.webhooks || {});
  flash("Алерты сохранены", true);
  if (lastSnap) {
    lastSnap.hub = lastSnap.hub || {};
    lastSnap.hub.webhooks = j.webhooks || {};
    renderWebhooks(lastSnap);
  }
}

async function hubCloudFetch(path, { method = "GET", body = null, unlock = false } = {}) {
  const headers = {};
  const tok = sessionStorage.getItem(tokenKey);
  if (tok) headers.Authorization = "Bearer " + tok;
  if (body != null) headers["Content-Type"] = "application/json";
  if (unlock && hubUnlockTok) headers["X-Hub-Unlock"] = hubUnlockTok;
  const r = await fetch(path, {
    method,
    headers,
    credentials: "same-origin",
    body: body != null ? JSON.stringify(body) : undefined,
    cache: "no-store",
  });
  if (r.status === 401) {
    sessionStorage.removeItem(tokenKey);
    flash("Сессия истекла — войдите снова", false);
    location.href = "/";
    return null;
  }
  const j = await r.json().catch(() => ({}));
  if (r.status === 403) {
    if (j.error === "cloud_locked" || j.error === "bad_unlock_password") {
      hubUnlockTok = "";
      setEngOriginEnabled(false);
      showSettingsBody(false);
      syncSettingsGear();
    }
  }
  if (!r.ok) {
    const errMap = {
      bad_unlock_password: "Неверный пароль",
      unlock_not_configured: "Сначала задайте пароль",
      unlock_password_invalid: "Пароль: минимум 8 символов, без «:»",
      cloud_locked: "Сначала пароль",
      bad_login: "Неверный текущий пароль",
    };
    flash(errMap[j.error] || j.error || ("HTTP " + r.status), false);
    return null;
  }
  return j;
}

function paintCloudFwd(fwd, eng) {
  fwd = mergeHubForward(fwd || {});
  const probeSnap = _fwdProbeFields(fwd);
  if (probeSnap) _cloudFwdProbeCache = Object.assign({}, fwd, probeSnap);

  const src = (fwd && fwd.token_source) || (fwd && fwd.has_token ? "user" : "none");
  cloudFwdHasToken = src === "user";
  const st = $("cloudFwdStatus");
  if (st) {
    let tip = "Cloud token (LoRa) — только MQTT→Cloud forward";
    let text = "—";
    const tone = _cloudFwdStatusTone(fwd, src);
    if (fwd && fwd.enabled === false) {
      text = "origin не задан";
      tip = "Задайте адрес Cloud в инженерном меню";
    } else if (src === "user") {
      if (fwd.probe_ok === true) {
        text = "Cloud принял токен";
        tip = "POST ReportHeartbeat — Bearer принят облаком";
      } else if (fwd.probe_ok === false) {
        const err = fwd.probe_error || "";
        if (err === "unauthorized") {
          text = "Cloud отказал (401)";
          tip = "Bearer отклонён — проверьте токен клиента";
        } else if (err === "no_url") {
          text = "сохранён · нет origin";
          tip = "Токен в Hub есть, адрес Cloud пуст";
        } else if (err === "no_token") {
          text = "токен не задан";
          tip = "Сохраните Bearer в поле выше";
        } else {
          text = "Cloud недоступен";
          tip = err ? ("probe: " + err) : "сеть/TLS к Cloud";
        }
      } else {
        text = "сохранён · проверка Cloud…";
        tip = "Токен в Hub; ждём ответ Cloud (или нажмите «Сохранить токен»)";
      }
    } else if (src === "factory") {
      text = "ключ свежей прошивки";
      tip = "Свой ключ не задан — LoRa идёт с тем же ключом, что у свежепрошитого узла";
    } else if (fwd && fwd.last_error === "cabinet_key_missing") {
      text = "ключа нет";
      tip = "Нет ключа на Hub и нет ключа свежей прошивки в образе";
    } else {
      text = "ключа нет";
      tip = "Задайте ключ кабинета или соберите образ с ключом свежей прошивки";
    }
    st.textContent = text;
    st.title = tip;
    st.classList.remove("cloud-fwd-ok", "cloud-fwd-err", "cloud-fwd-warn", "cloud-fwd-muted");
    st.classList.add(
      tone === "ok" ? "cloud-fwd-ok" :
      tone === "err" ? "cloud-fwd-err" :
      tone === "warn" ? "cloud-fwd-warn" : "cloud-fwd-muted"
    );
  }
  if (typeof paintFwdMeter === "function") paintFwdMeter(fwd);
  if (eng && eng.unlocked && $("cloudOrigin") && !$("cloudOrigin").dataset.touched) {
    $("cloudOrigin").value = (fwd && fwd.base) || CLOUD_ORIGIN_DEFAULT;
  }
  if (eng && eng.unlocked && $("cloudCa") && !$("cloudCa").dataset.touched) {
    $("cloudCa").value = (fwd && fwd.ca_file) || "";
  }
  if (eng) setEngOriginEnabled(!!eng.unlocked);
}

async function refreshCloudPanel() {
  const j = await hubCloudFetch("/api/hub/cloud", { unlock: !!hubUnlockTok });
  if (!j) return;
  paintCloudFwd(j.forward || {}, j.engineer || {});
}

async function saveCloudToken() {
  return saveCloudUplink();
}

async function saveCloudUplink() {
  let t = (($("cloudToken") && $("cloudToken").value) || "").trim();
  if (t.startsWith("Bearer ")) t = t.slice(7).trim();
  if (t) {
    const tokJ = await hubCloudFetch("/api/hub/cloud/token", {
      method: "POST",
      body: { token: t },
    });
    if (!tokJ) return;
    if ($("cloudToken")) $("cloudToken").value = "";
    paintCloudFwd(tokJ.forward || {}, { unlocked: !!hubUnlockTok });
  } else {
    await refreshCloudPanel();
  }
  if (typeof saveMelPolicy === "function") {
    const ok = await saveMelPolicy({ quiet: true });
    if (!ok) return;
  }
  flash("Сохранено", true);
}

async function saveCloudOrigin() {
  if (!hubUnlockTok) { flash("Сначала пароль", false); return; }
  const base = (($("cloudOrigin") && $("cloudOrigin").value) || "").trim() || CLOUD_ORIGIN_DEFAULT;
  const originJ = await hubCloudFetch("/api/hub/cloud/origin", {
    method: "POST",
    body: { base },
    unlock: true,
  });
  if (!originJ) return;
  if ($("cloudOrigin")) delete $("cloudOrigin").dataset.touched;
  paintCloudFwd(originJ.forward || {}, { unlocked: true });
  flash("Адрес сохранён", true);
}

async function engUnlock(setPw) {
  const pw = ($("engPassword") && $("engPassword").value) || "";
  if (!pw) { flash("Введите пароль", false); return; }
  const path = setPw ? "/api/hub/cloud/unlock/set" : "/api/hub/cloud/unlock";
  const j = await hubCloudFetch(path, { method: "POST", body: { password: pw } });
  if (!j) return;
  hubUnlockTok = j.token || "";
  if ($("engPassword")) $("engPassword").value = "";
  setEngOriginEnabled(!!hubUnlockTok);
  showCloudUrlFields(!!hubUnlockTok);
  paintCloudFwd(j.forward || {}, { unlocked: !!hubUnlockTok });
  await refreshCloudPanel();
  flash(setPw ? "Пароль сохранён" : "Адрес облака открыт", true);
}

async function downloadUrl(url, filename) {
  // <a download> на self-signed HTTPS часто даёт «проверьте интернет».
  // revokeObjectURL сразу после click() тоже рвёт скачивание — откладываем.
  const r = await fetch(url, { cache: "no-store", credentials: "same-origin" });
  if (r.status === 401) throw new Error("войдите снова");
  if (!r.ok) {
    const j = await r.json().catch(() => ({}));
    throw new Error(j.detail || j.error || ("HTTP " + r.status));
  }
  const blob = await r.blob();
  if (!blob.size) throw new Error("пустой ответ");
  const obj = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = obj;
  a.download = filename || "download.bin";
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(obj), 60000);
  return blob.size;
}

