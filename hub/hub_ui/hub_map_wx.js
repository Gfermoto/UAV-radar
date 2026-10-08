/* Hub map — weather HUD / Open-Meteo (Wave D) */
function wxKey(lat, lon) {
  return lat.toFixed(3) + "," + lon.toFixed(3);
}

function _wxFresh(rec) {
  return rec && !rec.fail && Date.now() - rec.t0 < HUB_WX_TTL_MS;
}

function _wxFailedRecently(rec) {
  return rec && rec.fail && Date.now() - rec.t0 < 45 * 1000;
}

/** Geofence (ADS-B filter / zone radius) — perfect circle, no acoustic weather model. */
function circleLatLngs(lat, lon, baseR) {
  const pts = [];
  for (let i = 0; i < HUB_POLY_N; i++) {
    const br = (i * 360) / HUB_POLY_N;
    pts.push(destPoint(lat, lon, br, baseR));
  }
  return pts;
}

function _lsFlag(key, fallback) {
  try {
    const v = localStorage.getItem(key);
    if (v === "0") return false;
    if (v === "1") return true;
  } catch (e) { /* ignore */ }
  return fallback;
}

function _setLsFlag(key, on) {
  try { localStorage.setItem(key, on ? "1" : "0"); } catch (e) { /* ignore */ }
}

function reliefWanted() {
  const el = $("hubMapRelief");
  if (el) return !!el.checked;
  return _lsFlag("hubMapRelief", true);
}

function requestWx(lat, lon, sampleM) {
  const mk = wxKey(lat, lon);
  const ek = mk + "|" + Math.round(sampleM);
  const met = _wxMet[mk];
  const el = _wxEl[ek];
  if (_wxFresh(met) && (_wxFresh(el) || _wxFailedRecently(el))) return { met, el: _wxFresh(el) ? el : null };
  if (_wxFailedRecently(met)) return { met: null, el: _wxFresh(el) ? el : null };
  if (_wxPending[ek]) return { met: _wxFresh(met) ? met : null, el: _wxFresh(el) ? el : null };
  _wxPending[ek] = true;
  const url =
    "/api/hub/meteo?lat=" + encodeURIComponent(lat.toFixed(4)) +
    "&lon=" + encodeURIComponent(lon.toFixed(4)) +
    "&sample_m=" + encodeURIComponent(String(Math.round(sampleM)));
  fetch(url, { credentials: "same-origin", headers: { Accept: "application/json" } })
    .then((r) => r.json().then((j) => ({ ok: r.ok, j })))
    .then(({ ok, j }) => {
      if (!ok || !j || !j.current) throw new Error("wx");
      const c = j.current;
      const daily = j.daily || {};
      _wxMet[mk] = {
        t: _num(c.temperature_2m),
        rh: _num(c.relative_humidity_2m),
        p: _num(c.surface_pressure),
        pLocalKpa: _num(c.pressure_local_kpa),
        windMs: _num(c.wind_speed_10m),
        windFrom: _num(c.wind_direction_10m),
        precipMm: _num(c.precipitation),
        gustMs: _num(c.wind_gusts_10m),
        isDay: c.is_day == null ? null : !!Number(c.is_day),
        sunrise: (daily.sunrise && daily.sunrise[0]) || null,
        sunset: (daily.sunset && daily.sunset[0]) || null,
        src: j.source || null,
        nowS: Date.now() / 1000,
        t0: Date.now(),
      };
      if (Array.isArray(j.elevation) && j.elevation.length) {
        _wxEl[ek] = {
          elev0: _num(j.elevation[0]),
          ring: j.elevation.slice(1).map(_num),
          t0: Date.now(),
        };
      } else if (!_wxEl[ek]) {
        _wxEl[ek] = { fail: true, t0: Date.now() };
      }
      _paintWxHud();
      if (lastSnap) renderHubMap(lastSnap);
    })
    .catch(() => {
      if (!_wxMet[mk] || !_wxFresh(_wxMet[mk])) _wxMet[mk] = { fail: true, t0: Date.now() };
      _paintWxHud();
    })
    .finally(() => { delete _wxPending[ek]; });
  return { met: _wxFresh(met) ? met : null, el: _wxFresh(el) ? el : null };
}

function _dirRu(deg) {
  if (deg == null) return "";
  const n = ((Math.round(deg / 45) % 8) + 8) % 8;
  return ["С", "СВ", "В", "ЮВ", "Ю", "ЮЗ", "З", "СЗ"][n];
}

/** Wind "from" — plain Russian genitive for lay HUD. */
function _windFromRu(deg) {
  if (deg == null) return "";
  const n = ((Math.round(deg / 45) % 8) + 8) % 8;
  return [
    "севера",
    "северо-востока",
    "востока",
    "юго-востока",
    "юга",
    "юго-запада",
    "запада",
    "северо-запада",
  ][n];
}

function _bearingToRu(deg) {
  if (deg == null) return "";
  const n = ((Math.round(deg / 45) % 8) + 8) % 8;
  return ["север", "северо-восток", "восток", "юго-восток", "юг", "юго-запад", "запад", "северо-запад"][n];
}

function _wxSrcPlain(src) {
  const s = String(src || "");
  if (s.indexOf("ecowitt") >= 0) return "своя станция во дворе";
  if (s.indexOf("met.no") >= 0) return "прогноз met.no";
  if (s.indexOf("open-meteo") >= 0) return "прогноз погоды";
  return "";
}

function _wxCircleHint(env) {
  const extras = [];
  if (env) {
    if (env.a_fol_db > 0.05) extras.push("деревья");
    if (env.a_bar_db > 0.05) extras.push("здания");
    else if (env.build_no_height) extras.push("здания рядом");
  }
  let line = "Круг на карте — как далеко слышен гул дрона в этой погоде.";
  if (extras.length) line += " Форму чуть меняют " + extras.join(" и ") + ".";
  line += " Это не зона тревоги.";
  return line;
}

function _wxFocusNode() {
  const nodes = ((lastSnap && lastSnap.nodes) || []).filter((n) => nodeMapPos(n));
  if (!nodes.length) return null;
  if (selected) {
    const want = canonNodeId(selected);
    const hit = nodes.find((n) => canonNodeId(n.node_id) === want);
    if (hit) return hit;
  }
  if (_hubMap) {
    const c = _hubMap.getCenter();
    let best = null;
    let bestD = 1e99;
    nodes.forEach((n) => {
      const ll = nodeMapPos(n);
      const d = Math.hypot(ll[0] - c.lat, ll[1] - c.lng);
      if (d < bestD) {
        bestD = d;
        best = n;
      }
    });
    return best;
  }
  return nodes[0];
}

function _esc(s) {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;");
}

function _paintWxHud() {
  const el = $("hubMapWx");
  if (!el) return;
  const node = _wxFocusNode();
  const nid = node ? canonNodeId(node.node_id) : "";
  _wxFocusNid = nid;
  const ll = node ? nodeMapPos(node) : null;
  const mk = ll ? wxKey(ll[0], ll[1]) : "";
  const met = mk && _wxFresh(_wxMet[mk]) ? _wxMet[mk] : null;
  const env = nid && _envByNid[nid] ? _envByNid[nid] : _lastEnvelopeHud;
  const lab = node ? String((node.label || "").trim() || nid) : "";
  const focusHint = selected && nid && canonNodeId(selected) === nid
    ? "сейчас смотрим этот датчик"
    : "датчик ближе к центру карты";

  if (!node) {
    el.innerHTML = '<div class="wx-mute">Поставьте датчик на карту — здесь появится погода</div>';
    return;
  }
  if (!met) {
    const failed = mk && _wxMet[mk] && _wxMet[mk].fail;
    el.innerHTML =
      '<div class="wx-title">' + _esc(lab) + "</div>" +
      '<div class="wx-mute">' + _esc(focusHint) + "</div>" +
      '<div class="wx-row">' + (failed ? "Погоду сейчас не удалось получить" : "Загружаем погоду…") + "</div>";
    return;
  }

  const rows = [];
  rows.push('<div class="wx-title">' + _esc(lab) + "</div>");
  rows.push('<div class="wx-mute">' + _esc(focusHint) + "</div>");

  const lead = [];
  if (met.t != null) lead.push(Math.round(met.t) + "°C");
  if (met.rh != null) lead.push("влажность " + Math.round(met.rh) + "%");
  if (lead.length) rows.push('<div class="wx-lead">' + lead.join(", ") + "</div>");

  if (met.windMs != null && met.windFrom != null) {
    let wind;
    if (met.windMs < 0.3) {
      wind = "Штиль";
    } else {
      wind =
        "Ветер с " + _windFromRu(met.windFrom) + ", " +
        met.windMs.toFixed(1).replace(".0", "") + " м/с";
      if (met.gustMs != null && met.gustMs > met.windMs + 0.8) {
        wind += ", порывы до " + met.gustMs.toFixed(1).replace(".0", "") + " м/с";
      }
    }
    rows.push('<div class="wx-row">' + wind + "</div>");
  }

  if (met.precipMm != null && met.precipMm > 0.05) {
    const mm = met.precipMm.toFixed(1).replace(".0", "");
    rows.push(
      '<div class="wx-row">' +
        (met.precipMm < 1 ? "Слабый дождь" : "Дождь") +
        " · " + mm + " мм/ч</div>"
    );
  }

  const src = _wxSrcPlain(met.src);
  if (src) rows.push('<div class="wx-mute">Данные: ' + _esc(src) + "</div>");

  if (env && env.hr_m != null && Math.abs(env.hr_m - 1.5) > 0.15) {
    rows.push(
      '<div class="wx-mute">Микрофон ≈ ' +
        env.hr_m.toFixed(1).replace(".0", "") +
        " м над землёй (установка)</div>"
    );
  }

  rows.push('<div class="wx-hint">' + _esc(_wxCircleHint(env)) + "</div>");

  const azPhase = typeof nodeAzimuthPhase === "function" ? nodeAzimuthPhase(node) : null;
  const az = azPhase && azPhase.phase === "hot" ? nodeAzimuth(node) : null;
  if (az != null) {
    rows.push(
      '<div class="wx-row">Сигнал сейчас с ' +
        _esc(_bearingToRu(az)) +
        " (" + Math.round(az) + "°)</div>"
    );
  }

  el.innerHTML = rows.join("");
}

