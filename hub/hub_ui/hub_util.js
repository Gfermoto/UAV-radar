function fmt(n) {
  if (n == null || Number.isNaN(n)) return "—";
  if (typeof n === "number") return Number.isInteger(n) ? String(n) : n.toFixed(3);
  return String(n);
}
/** Возраст / uptime: только целые секунды (без .000). */
function fmtSec(n) {
  if (n == null || Number.isNaN(Number(n))) return "—";
  return String(Math.round(Number(n)));
}
/** Uptime платы: 45 с · 12 м · 1ч 12м · 2д 3ч */
function fmtUptime(sec) {
  if (sec == null || Number.isNaN(Number(sec))) return null;
  let s = Math.max(0, Math.round(Number(sec)));
  const d = Math.floor(s / 86400); s %= 86400;
  const h = Math.floor(s / 3600); s %= 3600;
  const m = Math.floor(s / 60); s %= 60;
  if (d > 0) return h > 0 ? `${d}д ${h}ч` : `${d}д`;
  if (h > 0) return m > 0 ? `${h}ч ${m}м` : `${h}ч`;
  if (m > 0) return s > 0 && m < 10 ? `${m}м ${s}с` : `${m}м`;
  return `${s} с`;
}
/** Возраст последнего пакета на Hub. */
function fmtLinkAge(sec) {
  if (sec == null || Number.isNaN(Number(sec))) return null;
  const s = Math.max(0, Math.round(Number(sec)));
  if (s < 60) return `${s} с назад`;
  if (s < 3600) return `${Math.round(s / 60)} м назад`;
  if (s < 86400) return `${Math.round(s / 3600)} ч назад`;
  return `${Math.round(s / 86400)} д назад`;
}
function fmtBytes(b) {
  if (b == null) return "—";
  if (b < 1024) return b + " B";
  if (b < 1048576) return (b/1024).toFixed(1) + " KB";
  return (b/1048576).toFixed(2) + " MB";
}
/** ISO / epoch-ms → локальная дата-время. */
function fmtWhen(v) {
  if (v == null || v === "") return "—";
  if (typeof v === "number") {
    if (v < 1e12) return "нет NTP (uptime)";
    const y = new Date(v > 1e12 ? v : v * 1000).getFullYear();
    if (y < 2020) return "нет NTP (uptime)";
  }
  let d;
  if (typeof v === "number") d = new Date(v > 1e12 ? v : v * 1000);
  else d = new Date(String(v));
  if (Number.isNaN(d.getTime())) return String(v);
  if (d.getFullYear() < 2020) return "нет NTP (uptime)";
  const pad = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}
function threatColor(t) {
  if (t == null) return "var(--muted)";
  if (t >= 0.7) return "var(--bad)";
  if (t >= 0.3) return "var(--warn)";
  return "var(--ok)";
}
function escapeHtml(s) {
  return String(s == null ? "" : s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}
/** PB/topic 746E8C · MQTT body nevod-746E8C → один ключ (≤16, как hub_pb). */
const NODE_ID_RE_MAX = 16;
function canonNodeId(raw) {
  let s = String(raw == null ? "" : raw).trim();
  if (!s) return "";
  if (s.toLowerCase().startsWith("nevod-")) s = s.slice(6);
  return s.slice(0, NODE_ID_RE_MAX);
}
/** Список/dropdown: «Гараж · 746E8C» или только id. */
function nodeOptionCaption(nodeId, label) {
  const id = canonNodeId(nodeId) || String(nodeId || "");
  const lab = String(label == null ? "" : label).trim();
  return lab ? (lab + " · " + id) : id;
}
function cell(l, x, cls) {
  return `<div class="cell ${cls||""}"><div class="l">${escapeHtml(l)}</div><div class="x">${escapeHtml(x)}</div></div>`;
}
/** Пропуск пустых ячеек — сетка не прыгает от «—». */
function cellMaybe(l, x, cls) {
  if (x == null || x === "" || x === "—") return "";
  return cell(l, x, cls);
}
/** Тот же ряд, что у TCP: пустое → «нет данных», не вырезать карточку. */
function cellOrNa(l, x, cls) {
  if (x == null || x === "" || x === "—") return cell(l, "нет данных", cls || "muted");
  return cell(l, x, cls);
}
  /** UAV pipeline layers → short chips (skip legacy impulse "transient"). */
  function layersChipsHtml(layers) {
    if (!Array.isArray(layers) || !layers.length) return "";
    // Abbreviations only — full wire names overflow the kv cell.
    const abbr = {
      goertzel: { short: "Gz", title: "goertzel" },
      tflite: { short: "NN", title: "tflite" },
      hps: { short: "HPS", title: "hps" },
      fusion: { short: "Fus", title: "fusion" },
    };
    const chips = [];
    for (const raw of layers) {
      const k = String(raw || "").toLowerCase();
      if (!k || k === "transient") continue; // product: no gunshot/impulse layer
      const m = abbr[k];
      const label = m ? m.short : k;
      const title = m ? m.title : k;
      chips.push(
        `<span class="chip kind" title="${escapeHtml(title)}">${escapeHtml(label)}</span>`
      );
    }
    if (!chips.length) return "";
    return `<div class="cell cell-layers"><div class="l">слои</div><div class="x chip-row">${chips.join("")}</div></div>`;
  }
/** DoA/азимут на карте и в деталях — только при реальной детекции (не фон, не пустой last_detection). */
const HUB_EPISODE_GAP_MS = 10000; // = hub_episodes.GAP_MS
const HUB_AZ_AFTERGLOW_MS = 30000; // после close эпизода

function _nodeDetBlob(n) {
  const d = (n && (n.joined_detection || n.last_detection)) || {};
  const raw = (n && n.last_detection) || {};
  return Object.assign({}, raw, d);
}

/** Возраст last DET (мс). null если часов нет — без sticky forever. */
function nodeDetAgeMs(n) {
  const det = _nodeDetBlob(n);
  const recv = det.recv_at;
  if (recv != null && String(recv).trim() !== "") {
    const t = Date.parse(String(recv));
    if (Number.isFinite(t)) return Math.max(0, Date.now() - t);
  }
  const ts = det.timestamp_ms;
  if (ts != null && ts !== "") {
    const t = Number(ts);
    if (Number.isFinite(t) && t > 1e11) return Math.max(0, Date.now() - t);
  }
  return null;
}

/** Payload похож на реальную тревогу (без учёта возраста). */
function nodeDetPayloadReal(n) {
  const det = _nodeDetBlob(n);
  const cls = String(det.class_name || det.class || "").trim();
  const cid = det.class_id;
  if (cls === "background" || cid === 0) return false;
  if (det.early_warning) return true;
  if (det.confirmed === true) return true;
  if (cls && cls !== "background") return true;
  if (cid != null && Number(cid) !== 0) return true;
  const thrRaw = n && n.threat != null ? n.threat : det.threat;
  const thr = thrRaw == null || thrRaw === "" ? null : Number(thrRaw);
  if (thr != null && Number.isFinite(thr) && thr <= 0) return false;
  if (thr != null && Number.isFinite(thr) && thr > 0) return true;
  const confRaw = det.confidence != null ? det.confidence : det.p;
  const conf = confRaw == null || confRaw === "" ? null : Number(confRaw);
  if (conf != null && Number.isFinite(conf) && conf > 0 && cls) return true;
  const azCand =
    det.azimuth_deg != null
      ? det.azimuth_deg
      : det.azimuth_deg_raw != null
        ? det.azimuth_deg_raw
        : det.bearing && det.bearing.azimuth_deg;
  if (
    azCand != null &&
    Number.isFinite(Number(azCand)) &&
    thr != null &&
    Number.isFinite(thr) &&
    thr > 0
  ) {
    return true;
  }
  return false;
}

function nodeHasDetection(n) {
  const ep = n && n.active_episode;
  if (ep && ep.class_name && String(ep.class_name) !== "background") return true;
  if (ep && ep.azimuth_deg != null && Number.isFinite(Number(ep.azimuth_deg))) return true;
  return nodeDetPayloadReal(n);
}

/**
 * Фаза луча азимута: hot | afterglow | null.
 * hot — эпизод open или DET ≤ gap 10с; afterglow — ещё 30с; иначе off.
 */
function nodeAzimuthPhase(n) {
  const ep = n && n.active_episode;
  const epOpen = !!(ep && ep.open !== false);
  const real = nodeDetPayloadReal(n);
  if (!epOpen && !real) return null;
  // Явный фон / нулевой threat гасит сразу (даже если age свежий).
  const det = _nodeDetBlob(n);
  const cls = String(det.class_name || det.class || "").trim();
  if (cls === "background" || det.class_id === 0) return null;
  const thrRaw = n && n.threat != null ? n.threat : det.threat;
  const thr = thrRaw == null || thrRaw === "" ? null : Number(thrRaw);
  if (thr != null && Number.isFinite(thr) && thr <= 0 && !epOpen) return null;

  const age = nodeDetAgeMs(n);
  if (epOpen) {
    return { phase: "hot", age_s: age != null ? age / 1000 : null };
  }
  if (!real || age == null) return null;
  if (age <= HUB_EPISODE_GAP_MS) {
    return { phase: "hot", age_s: age / 1000 };
  }
  if (age <= HUB_EPISODE_GAP_MS + HUB_AZ_AFTERGLOW_MS) {
    return { phase: "afterglow", age_s: age / 1000 };
  }
  return null;
}

function nodeShowsAzimuth(n) {
  return nodeAzimuthPhase(n) != null;
}

/** Product RU: как UavLabels.h (PB не несёт class_ru). */
function classRuOf(det) {
  if (!det) return null;
  const direct = det.class_ru;
  if (direct != null && String(direct).trim() !== "") return String(direct);
  const name = String(det.class_name || det.class || "");
  if (name === "UAV") return "неизвестный БПЛА";
  const byName = {
    background: "фон",
    drone: "коптер",
    ice_uav: "ДВС-БПЛА",
    jet_uav: "ТРД-БПЛА",
  };
  if (byName[name]) return byName[name];
  const id = det.class_id;
  if (id === 0) return "фон";
  if (id === 1) return "коптер";
  if (id === 2) return "ДВС-БПЛА";
  if (id === 3) return "ТРД-БПЛА";
  return null;
}

/** alarm_tier узла: confirmed | early | none | "". */
function alarmTierOf(n) {
  const d = (n && (n.joined_detection || n.last_detection)) || {};
  const raw = (n && n.last_detection) || {};
  return String(d.alarm_tier || raw.alarm_tier || "").toLowerCase();
}

/** Полный акцент только при alarm_tier=confirmed; early/пусто — кандидат. */
function isConfirmedAlarm(n) {
  return alarmTierOf(n) === "confirmed";
}

/**
 * Подпись класса для оператора: «кандидат: коптер» vs «коптер».
 * early / missing / none → кандидат; confirmed → без префикса.
 */
function classOperatorLabel(det, n) {
  const base = classRuOf(det) || (det && (det.class_name || det.class)) || null;
  if (base == null || String(base).trim() === "") return null;
  const name = String(base);
  if (name === "фон" || name === "background") return name;
  if (isConfirmedAlarm(n || { last_detection: det, joined_detection: det })) {
    return name;
  }
  return "кандидат: " + name;
}
/**
 * Строка «основание» DET: ступень · тон · канал · n из m.
 * Винтовой класс без тона — «класс без тона»; у ТРД тона нет по природе, это не сбой.
 */
function detEvidenceLine(det, n) {
  if (!det) return null;
  const node = n || { last_detection: det, joined_detection: det };
  const tier = alarmTierOf(node);
  const parts = [];
  if (tier === "confirmed") parts.push("подтверждено");
  else if (tier === "early") parts.push("кандидат (раннее)");
  else parts.push("кандидат");
  const cid = det.class_id;
  const cname = String(det.class_name || det.class || "").toLowerCase();
  const isJet = cid === 3 || cname.indexOf("jet") === 0;
  const isProp = !isJet && (cid === 1 || cid === 2 || cname === "drone" || cname.indexOf("ice") === 0);
  const tone = det.tone_agreed;
  if (isJet) parts.push("тон: н/п (ТРД)");
  else if (tone === true) parts.push("тон: да");
  else if (tone === false) parts.push(isProp ? "тон: нет · класс без тона" : "тон: нет");
  else parts.push("тон: —");
  if (det.via) parts.push(viaLabel(det.via));
  const nm = det.n_of_m;
  if (Array.isArray(nm) && nm.length === 2 && nm[1] > 0) {
    parts.push(nm[0] + " из " + nm[1]);
  }
  return parts.join(" · ");
}

/** Высота с HB: AGL/MSL только при явном флаге, иначе «высота с узла». */
function heightLabel(h) {
  if (!h || h.alt_m == null || !isFinite(Number(h.alt_m))) return null;
  const v = Number(h.alt_m).toFixed(1) + " м";
  if (h.alt_ref === "agl") return { k: "высота AGL", v };
  if (h.alt_ref === "msl") return { k: "высота MSL", v };
  return { k: "высота с узла", v };
}

/** Позиция узла: manual_gnss ≠ GNSS fix. PB без source ≠ «ручная». */
function gpsPosLabel(h) {
  if (!h) return null;
  const src = String(h.gps_source || "");
  if (h.lat == null && h.lon == null && !h.gps_valid && (!h.gps_fix || h.gps_fix === "none")) {
    return null;
  }
  if (!src) {
    // Typical PB HB: lat/lon without source — do not claim NVS manual.
    const fix = h.gps_fix;
    if (fix && fix !== "none") return String(fix);
    if (h.lat != null || h.lon != null) return "координаты";
    return null;
  }
  const manual = src === "manual_gnss" || src.indexOf("manual") === 0;
  if (manual) return "ручная";
  const fix = h.gps_fix;
  if (fix && fix !== "none") return String(fix) + " · " + src;
  return src || null;
}
/** Только тревоги (не копии чисел). Числа — в kv детали. */
function alertBadges(n) {
  const h = (n && n.health) || {};
  const out = [];
  if (!n.online) {
    out.push(`<span class="badge bad" title="Нет пакетов в окне online">offline</span>`);
  }
  if (h.weak_rssi) {
    out.push(`<span class="badge warn" title="RSSI &lt; −75 dBm">weak RSSI</span>`);
  }
  if (h.heap_cliff) {
    out.push(`<span class="badge bad" title="heap cliff">heap cliff</span>`);
  } else if (h.heap_low) {
    out.push(`<span class="badge warn" title="heap &lt; 22 KB">heap low</span>`);
  }
  if (h.free_heap_min != null && Number(h.free_heap_min) < 8000) {
    out.push(`<span class="badge bad" title="free_heap_min с boot &lt; 8 KB">min heap</span>`);
  }
  return out.join("");
}
function viaLabel(via) {
  const m = {
    pb_detection: "PB · DET",
    pb_heartbeat: "PB · HB",
    pb_mel: "PB · MEL",
    mqtt_detection: "MQTT · DET",
    mqtt_heartbeat: "MQTT · HB",
    episode: "Эпизод",
  };
  if (!via) return "—";
  if (m[via]) return m[via];
  if (String(via).startsWith("mqtt")) return "MQTT";
  if (String(via).startsWith("pb")) return "PB";
  return String(via);
}
function transportChips(transports) {
  const set = new Set((transports || []).map(x => String(x).toLowerCase()));
  const bits = [];
  if (set.has("pb")) bits.push('<span class="chip pb" title="На этот Hub">PB</span>');
  if (set.has("mqtt")) bits.push('<span class="chip mqtt" title="MQTT на брокер этой площадки">MQTT</span>');
  if (set.has("lora")) bits.push('<span class="chip lora" title="До шлюза пакет шёл по LoRa">LoRa</span>');
  if (!bits.length) bits.push('<span class="chip" title="Ещё не было свежих пакетов">—</span>');
  return `<span class="chip-row">${bits.join("")}</span>`;
}
function loiterLabel(n) {
  if (!n || !n.loitering) return null;
  const s = Number(n.presence_s);
  if (Number.isFinite(s) && s > 0) return `зависание ${Math.floor(s / 60)} мин`;
  // LoRa compact: флаг без presence_s — не показывать минуты Hub.
  return "флаг, без длительности";
}
function loiterChip(n) {
  const lab = loiterLabel(n);
  if (!lab) return "";
  return `<span class="chip loiter" title="Цель держится у узла ≥5 мин (NN, разрывы <60 с). Фон LAeq заморожен; после 20 мин вето mel_cmin может снять публикацию.">${escapeHtml(lab)}</span>`;
}
function viaChip(via) {
  const lab = viaLabel(via);
  if (lab === "—") return '<span class="chip">—</span>';
  const cls = lab.startsWith("MQTT") ? "mqtt" : (lab.startsWith("PB") ? "pb" : "");
  return `<span class="chip ${cls}" title="via=${escapeHtml(via || "")}">${escapeHtml(lab)}</span>`;
}
function eventTimeShort(ts) {
  if (!ts) return "—";
  const d = new Date(String(ts));
  if (Number.isNaN(d.getTime())) return String(ts).slice(11, 19) || "—";
  const pad = (n) => String(n).padStart(2, "0");
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}
/** ISO / epoch → относительный возраст: "12 с", "3 м", "2 ч". */
function fmtAge(v) {
  if (v == null || v === "") return null;
  let ms;
  if (typeof v === "number") ms = v > 1e12 ? v : v * 1000;
  else {
    const d = new Date(String(v));
    if (Number.isNaN(d.getTime())) return null;
    ms = d.getTime();
  }
  const sec = Math.max(0, Math.round((Date.now() - ms) / 1000));
  if (sec < 60) return sec + " с";
  if (sec < 3600) return Math.round(sec / 60) + " м";
  if (sec < 86400) return Math.round(sec / 3600) + " ч";
  return Math.round(sec / 86400) + " д";
}
function setLiveStatus(state, atMs) {
  const live = $("liveStatus");
  const pulse = $("livePulse");
  const label = $("liveLabel");
  if (!live || !pulse || !label) return;
  live.classList.remove("ok", "stale", "error");
  pulse.classList.remove("stale", "error");
  live.classList.add(state);
  if (state !== "ok") pulse.classList.add(state);
  if (state === "ok") {
    const age = atMs ? Math.max(0, Math.round((Date.now() - atMs) / 1000)) : 0;
    label.textContent = age <= 1 ? "live" : "live · " + age + " с назад";
  } else if (state === "stale") {
    const age = atMs ? Math.max(0, Math.round((Date.now() - atMs) / 1000)) : 0;
    label.textContent = age ? ("stale · " + age + " с") : "stale";
  } else {
    label.textContent = "нет связи";
  }
}
function emptyOnboardHtml(title) {
  const eng = document.body.classList.contains("hub-eng");
  if (!eng) {
    return `<div class="empty">
    <div>${title}</div>
    <p class="hint">Когда устройство выйдет на связь, оно появится в списке и на карте.</p>
  </div>`;
  }
  return `<div class="empty">
    <div>${title}</div>
    <ol class="empty-steps">
      <li data-n="1">Узел шлёт DET/HB/Mel на этот Hub (<code>ingest_url</code> + токен клиента)</li>
      <li data-n="2"><button type="button" class="linkish" data-empty-ca>Скачать CA</button> и поставить на плату («Подтянуть CA»)</li>
      <li data-n="3">Чеклист в ⚙ Лаборатория: WAIT → чего не хватает</li>
    </ol>
  </div>`;
}
function bindEmptyCa(root) {
  if (!root) return;
  root.querySelectorAll("[data-empty-ca]").forEach(el => {
    el.onclick = () => { if ($("btnCa")) $("btnCa").click(); };
  });
}
function closeAuthModal(value) {
  const modal = $("authModal");
  if (modal) modal.classList.remove("open");
  const waiters = authWaiters.splice(0, authWaiters.length);
  waiters.forEach(fn => fn(value || null));
}
function openAuthModal() {
  const modal = $("authModal");
  const input = $("authTokenInput");
  if (!modal || !input) return Promise.resolve(null);
  const already = modal.classList.contains("open");
  if (!already) {
    modal.classList.add("open");
    input.value = "";
    setTimeout(() => input.focus(), 30);
  }
  return new Promise(resolve => { authWaiters.push(resolve); });
}

function fitCanvas(canvas, fallbackH) {
  // Measure content box via width:100% — parent.clientWidth includes padding and
  // used to set canvas px wider than the card (timeline/sparks overflow + unequal panels).
  const cssH = Math.max(1, Math.floor(fallbackH || 220));
  canvas.style.width = "100%";
  canvas.style.maxWidth = "100%";
  canvas.style.height = cssH + "px";
  void canvas.offsetWidth;
  let cssW = Math.floor(canvas.clientWidth);
  if (!cssW && canvas.parentElement) {
    const st = getComputedStyle(canvas.parentElement);
    const padX = (parseFloat(st.paddingLeft) || 0) + (parseFloat(st.paddingRight) || 0);
    cssW = Math.floor(canvas.parentElement.clientWidth - padX);
  }
  cssW = Math.max(1, cssW || 300);
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  canvas.width = Math.max(1, Math.floor(cssW * dpr));
  canvas.height = Math.max(1, Math.floor(cssH * dpr));
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w: cssW, h: cssH };
}

function spark(canvas, series, color) {
  if (!canvas) return;
  const { ctx, w, h } = fitCanvas(canvas, 72);
  ctx.clearRect(0, 0, w, h);
  if (!series || series.length < 2) {
    ctx.fillStyle = "#3a4a43"; ctx.fillRect(0, h/2, w, 1); return;
  }
  const min = Math.min(...series), max = Math.max(...series);
  const span = (max - min) || 1;
  ctx.beginPath();
  series.forEach((v, i) => {
    const x = (i / (series.length - 1)) * (w - 2) + 1;
    const y = h - 4 - ((v - min) / span) * (h - 8);
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.strokeStyle = color; ctx.lineWidth = 1.75; ctx.stroke();
}

