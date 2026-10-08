/* Hub map — geo helpers (Wave D) */
function _num(x) {
  const n = Number(x);
  return Number.isFinite(n) ? n : null;
}

function _clamp(x, lo, hi) {
  return x < lo ? lo : x > hi ? hi : x;
}

function destPoint(lat, lon, bearingDeg, distM) {
  const R = 6371000;
  const br = (bearingDeg * Math.PI) / 180;
  const φ1 = (lat * Math.PI) / 180;
  const λ1 = (lon * Math.PI) / 180;
  const δ = distM / R;
  const φ2 = Math.asin(
    Math.sin(φ1) * Math.cos(δ) + Math.cos(φ1) * Math.sin(δ) * Math.cos(br)
  );
  const λ2 =
    λ1 +
    Math.atan2(
      Math.sin(br) * Math.sin(δ) * Math.cos(φ1),
      Math.cos(δ) - Math.sin(φ1) * Math.sin(φ2)
    );
  return [ (φ2 * 180) / Math.PI, ((((λ2 * 180) / Math.PI) + 540) % 360) - 180 ];
}

function _pair(lat, lon) {
  const a = _num(lat);
  const b = _num(lon);
  if (a == null || b == null) return null;
  if (Math.abs(a) > 90 || Math.abs(b) > 180) return null;
  if (a === 0 && b === 0) return null;
  return [a, b];
}

function _msgTsMs(obj) {
  if (!obj) return null;
  const ms = _num(obj.timestamp_ms);
  if (ms != null && ms > 0) return ms > 1e12 ? ms : ms * 1000;
  if (obj.ts) {
    const t = Date.parse(String(obj.ts));
    if (Number.isFinite(t)) return t;
  }
  if (obj.recv_at) {
    const t = Date.parse(String(obj.recv_at));
    if (Number.isFinite(t)) return t;
  }
  return null;
}

function _viaIsLora(n) {
  const d = (n && n.last_detection) || {};
  const j = (n && n.joined_detection) || {};
  const via = String(d.via || j.via || "").toLowerCase();
  if (via === "lora_gw") return true;
  const tr = (n && n.transports) || [];
  return tr.some((x) => String(x).toLowerCase() === "lora");
}

function nodeMapPos(n) {
  const hb = (n && n.last_heartbeat) || {};
  const d = (n && n.last_detection) || {};
  const j = (n && n.joined_detection) || {};
  const hp = hb.node_position || {};
  const dp = d.node_position || {};
  const jp = j.node_position || {};
  const hbPos = _pair(hb.lat, hb.lon) || _pair(hp.lat, hp.lon);
  const detPos =
    _pair(j.lat, j.lon) ||
    _pair(jp.lat, jp.lon) ||
    _pair(d.lat, d.lon) ||
    _pair(dp.lat, dp.lon);
  // via=lora_gw: свежий DET с координатами не должен проигрывать старому HB.
  if (_viaIsLora(n) && detPos && hbPos) {
    const hbTs = _msgTsMs(hb);
    const detTs = _msgTsMs(d) || _msgTsMs(j);
    if (detTs != null && hbTs != null && detTs > hbTs) return detPos;
  }
  // HB/manual_gnss first — DET может тащить чужой/устаревший node_position.
  return hbPos || detPos;
}

/** HB-позиция показана, но DET свежее — «позиция устарела» (координаты не выдумываем). */
function nodeMapPosStaleLabel(n) {
  if (!_viaIsLora(n)) return null;
  const hb = (n && n.last_heartbeat) || {};
  const d = (n && n.last_detection) || {};
  const j = (n && n.joined_detection) || {};
  const hp = hb.node_position || {};
  const hbPos = _pair(hb.lat, hb.lon) || _pair(hp.lat, hp.lon);
  if (!hbPos) return null;
  const shown = nodeMapPos(n);
  if (!shown) return null;
  const hbTs = _msgTsMs(hb);
  const detTs = _msgTsMs(d) || _msgTsMs(j);
  if (detTs == null || hbTs == null || detTs <= hbTs) return null;
  if (shown[0] === hbPos[0] && shown[1] === hbPos[1]) return "позиция устарела";
  return null;
}

/** Нормализация азимута: 360→0; prefer bearing / raw (top-level иногда 360-stub). */
function _normAz(v) {
  const a = _num(v);
  if (a == null || !Number.isFinite(a)) return null;
  let x = a % 360;
  if (x < 0) x += 360;
  if (x === 360) x = 0;
  return x;
}

function nodeAzimuth(n) {
  const d = (n && n.last_detection) || {};
  const j = (n && n.joined_detection) || {};
  const ep = (n && n.active_episode) || {};
  const br = (d.bearing && typeof d.bearing === "object") ? d.bearing
    : (j.bearing && typeof j.bearing === "object") ? j.bearing : null;
  // Узел с bearing.n сам опускает azimuth_deg без пеленга: сырой отсчёт сканера — не луч.
  const honest = !!(br && br.n != null);
  return (
    _normAz(br && br.azimuth_deg) ??
    (honest ? null : _normAz(d.azimuth_deg_raw)) ??
    (honest ? null : _normAz(j.azimuth_deg_raw)) ??
    _normAz(d.azimuth_deg) ??
    _normAz(j.azimuth_deg) ??
    _normAz(ep.azimuth_deg)
  );
}

function nodeMirrorAmbiguity(n) {
  const d = (n && n.last_detection) || {};
  const j = (n && n.joined_detection) || {};
  const br = (d.bearing && typeof d.bearing === "object") ? d.bearing
    : (j.bearing && typeof j.bearing === "object") ? j.bearing : null;
  return !!(br && br.mirror_ambiguity);
}

/** Сдвиг маркеров с одинаковыми lat/lon (~25 м), иначе слипаются в одну точку. */
function _offsetOverlaps(items) {
  // items: [{ nid, ll:[lat,lon], n }]
  const buckets = {};
  items.forEach((it, idx) => {
    const key = it.ll[0].toFixed(5) + "," + it.ll[1].toFixed(5);
    (buckets[key] || (buckets[key] = [])).push(idx);
  });
  Object.keys(buckets).forEach((key) => {
    const idxs = buckets[key];
    if (idxs.length < 2) return;
    idxs.forEach((idx, i) => {
      const br = (i * 360) / idxs.length;
      items[idx].ll = destPoint(items[idx].ll[0], items[idx].ll[1], br, 28);
      items[idx].overlap = true;
    });
  });
}
function hubMapZones(s) {
  const zones = ((((s || {}).hub || {}).zones || {}).zones) || [];
  const out = [];
  zones.forEach((z) => {
    const lat = _num(z.lat);
    const lon = _num(z.lon);
    const r = _num(z.radius_m);
    if (lat == null || lon == null || r == null || r <= 0) return;
    if (Math.abs(lat) > 90 || Math.abs(lon) > 180) return;
    out.push({
      id: String(z.id || "zone"),
      name: String(z.name || z.id || "зона"),
      lat,
      lon,
      r,
    });
  });
  if (!out.length) {
    const a = ((s || {}).hub || {}).adsb || {};
    const lat = _num(a.site_lat);
    const lon = _num(a.site_lon);
    const r = _num(a.radius_m);
    if (lat != null && lon != null && r > 0) {
      out.push({
        id: "_site",
        name: String(a.site_id || "ADS-B"),
        lat,
        lon,
        r,
      });
    }
  }
  return out;
}

/* lockstep iot/tools/hub_iso9613.py — ISO 9613-1/2 envelope, NOT DET range */
const HUB_RAY_TICK_M = 100;
const ISO_DRAW_MIN_M = 50;
const ISO_DRAW_MAX_M = 2000;
const ISO_REF_M = 500;
const ISO_F_HZ = 500;
const ISO_HS_M = 30;
const ISO_HR_M = 1.5;
