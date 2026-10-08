/* Hub map — ISO 9613 envelope (Wave D) */
const _FOLIAGE = {
  63: [0, 0.02],
  125: [0, 0.03],
  250: [1, 0.04],
  500: [1, 0.05],
  1000: [1, 0.06],
  2000: [1, 0.08],
  4000: [2, 0.09],
  8000: [3, 0.12],
};
let _lastEnvelopeHud = null;
const _envByNid = {};
let _wxFocusNid = "";

function nearestOctaveHz(f) {
  const keys = Object.keys(_FOLIAGE).map(Number);
  let best = 500;
  let bd = 1e9;
  for (let i = 0; i < keys.length; i++) {
    const d = Math.abs(keys[i] - f);
    if (d < bd) {
      bd = d;
      best = keys[i];
    }
  }
  return best;
}

function alphaDbPerM(fHz, tC, rhPct, pKpa) {
  const f = fHz;
  const t = tC + 273.15;
  const p = pKpa;
  if (!(f > 0) || !(p > 0) || !(t > 0)) return 0;
  const T0 = 293.15;
  const T01 = 273.16;
  const P_REF = 101.325;
  const psat = Math.pow(10, -6.8346 * Math.pow(T01 / t, 1.261) + 4.6151);
  const h = rhPct * psat / (p / P_REF);
  const pr = p / P_REF;
  const fro = pr * (24 + 4.04e4 * h * (0.02 + h) / (0.391 + h));
  const frn =
    pr *
    Math.pow(t / T0, -0.5) *
    (9 + 280 * h * Math.exp(-4.17 * (Math.pow(t / T0, -1 / 3) - 1)));
  const aClass = 1.84e-11 * (P_REF / p) * Math.sqrt(t / T0);
  const aO = 0.01275 * Math.exp(-2239.1 / t) * fro / (fro * fro + f * f);
  const aN = 0.1068 * Math.exp(-3352.0 / t) * frn / (frn * frn + f * f);
  return 8.686 * f * f * (aClass + Math.pow(t / T0, -2.5) * (aO + aN));
}

function adivDb(dM) {
  return 20 * Math.log10(Math.max(1, dM)) + 11;
}

function foliageDb(pathM, fHz) {
  if (!(pathM >= 10)) return 0;
  const band = nearestOctaveHz(fHz);
  const shortDb = _FOLIAGE[band][0];
  const rate = _FOLIAGE[band][1];
  if (pathM <= 20) return shortDb;
  return rate * Math.min(pathM, 200);
}

function agrPorousDb(dM, hsM, hrM) {
  const d = Math.max(1, dM);
  const hs = hsM != null ? hsM : ISO_HS_M;
  const hr = hrM != null ? hrM : ISO_HR_M;
  const hm = (hs + hr) / 2;
  const agr = 4.8 - (2 * hm / d) * (17 + 300 / d);
  return agr < 0 ? 0 : agr;
}

function groundExtraDb(g, dM, hsM, hrM) {
  const gg = _clamp(g == null ? 1 : g, 0, 1);
  return (gg - 1) * agrPorousDb(dM, hsM, hrM);
}

function barrierDzDb(zM, fHz) {
  if (!(zM > 0) || !(fHz > 0)) return 0;
  const lam = 340 / fHz;
  const dz = 10 * Math.log10(3 + (20 / lam) * zM);
  if (!(dz > 0)) return 0;
  return Math.min(20, dz);
}

function rangeM(extraDb, fHz, tC, rhPct, pKpa) {
  const aref = adivDb(ISO_REF_M) + alphaDbPerM(fHz, 20, 70, 101.325) * ISO_REF_M;
  const target = aref - extraDb;
  let lo = ISO_DRAW_MIN_M;
  let hi = ISO_DRAW_MAX_M;
  for (let i = 0; i < 40; i++) {
    const mid = 0.5 * (lo + hi);
    const a = adivDb(mid) + alphaDbPerM(fHz, tC, rhPct, pKpa) * mid;
    if (a < target) lo = mid;
    else hi = mid;
  }
  return 0.5 * (lo + hi);
}

function envelopeAt(bearingDeg, ctx) {
  const fHz = ctx.f_hz != null ? ctx.f_hz : ISO_F_HZ;
  const tC = ctx.t_c != null ? ctx.t_c : 20;
  const rh = ctx.rh_pct != null ? ctx.rh_pct : 70;
  const pKpa = ctx.p_kpa != null ? ctx.p_kpa : 101.325;
  const forest = ctx.forest_path_m || 0;
  const g = ctx.g != null ? ctx.g : 1;
  const z = ctx.barrier_z_m || 0;
  const hs = ctx.hs_m != null ? ctx.hs_m : ISO_HS_M;
  let hr = ctx.hr_m != null ? ctx.hr_m : ISO_HR_M;
  if (!(hr >= 0.3)) hr = ISO_HR_M;
  if (hr > 80) hr = 80;
  const aFol = foliageDb(forest, fHz);
  let aBar = barrierDzDb(z, fHz);
  if (aBar > 0) aBar = Math.max(0, aBar - agrPorousDb(ISO_REF_M, hs, hr));
  const aGr = aBar > 0 ? 0 : groundExtraDb(g, ISO_REF_M, hs, hr);
  const extra = aGr + aFol + aBar;
  const r = rangeM(extra, fHz, tC, rh, pKpa);
  return {
    r_m: r,
    f_hz: fHz,
    a_div_db: adivDb(r),
    a_atm_db: alphaDbPerM(fHz, tC, rh, pKpa) * r,
    a_gr_db: aGr,
    a_bar_db: aBar,
    a_fol_db: aFol,
    extra_db: extra,
    bearing_deg: bearingDeg,
    hr_m: hr,
    hs_m: hs,
  };
}

function elevAtBearing(el, bearingDeg) {
  if (!el || !el.ring || !el.ring.length || el.elev0 == null) return el && el.elev0;
  const n = el.ring.length;
  const x = ((((bearingDeg % 360) + 360) % 360) * n) / 360;
  const i0 = Math.floor(x) % n;
  const i1 = (i0 + 1) % n;
  const t = x - Math.floor(x);
  const a = el.ring[i0];
  const b = el.ring[i1];
  if (a == null || b == null) return el.elev0;
  return a * (1 - t) + b * t;
}

/** Path-length difference for a thin screen (ISO 9613-2 geometry for Dz). */
function barrierPathDiffM(dBarrier, heightM, rangeM, hrM) {
  const R = Math.max(50, rangeM || ISO_REF_M);
  const H = Math.max(0, heightM);
  const Hr = hrM != null && hrM >= 0.3 ? hrM : ISO_HR_M;
  const dsr = Math.max(1, Math.min(dBarrier, R - 1));
  const dss = Math.max(1, R - dsr);
  const via = Math.hypot(dss, ISO_HS_M - H) + Math.hypot(dsr, H - Hr);
  return Math.max(0, via - R);
}

function nodeAltM(n) {
  if (!n) return null;
  const h = (n && n.last_heartbeat) || {};
  const d = (n && n.last_detection) || {};
  const j = (n && n.joined_detection) || {};
  const hp = h.node_position || {};
  const dp = d.node_position || {};
  const jp = j.node_position || {};
  return (
    _num(h.install_height_m) ??
    _num(hp.install_height_m) ??
    _num(j.install_height_m) ??
    _num(jp.install_height_m) ??
    _num(d.install_height_m) ??
    _num(dp.install_height_m) ??
    _num(h.alt_m) ??
    _num(hp.alt_m) ??
    _num(j.alt_m) ??
    _num(jp.alt_m) ??
    _num(d.alt_m) ??
    _num(dp.alt_m)
  );
}

/**
 * Mic height AGL for ISO Hr.
 * New FW: install_height_m (AGL). Legacy: alt_m only if ≤40 (was often MSL).
 */
function micHeightAgl(node, el) {
  const h = (node && node.last_heartbeat) || {};
  const pos = h.node_position || {};
  const inst =
    _num(h.install_height_m) ??
    _num(pos.install_height_m) ??
    null;
  if (inst != null && inst >= 0.3 && inst <= 80) return inst;
  const alt = nodeAltM(node);
  if (alt != null && alt >= 0.3 && alt <= 40) return alt; // legacy AGL-sized
  return ISO_HR_M;
}

function tipAltMsl(node, el) {
  const snap = _num(node && node.tip_alt_m);
  if (snap != null) return snap;
  const agl = micHeightAgl(node, el);
  const ground =
    _num(node && node.ground_elev_m) ??
    (el && el.elev0 != null ? _num(el.elev0) : null);
  if (ground == null || agl == null) return null;
  if (agl === ISO_HR_M && nodeAltM(node) == null) return null;
  return ground + agl;
}

function radiusAt(baseR, bearingDeg, met, el, lat, lon, node) {
  const fHz = ISO_F_HZ;
  let tC = 20;
  let rh = 70;
  let pKpa = 101.325;
  if (met) {
    if (met.t != null) tC = met.t;
    if (met.rh != null) rh = met.rh;
    if (met.pLocalKpa != null) pKpa = met.pLocalKpa;
    else if (met.p != null) pKpa = met.p / 10;
  }
  const hr = micHeightAgl(node, el);
  let forest = 0;
  let g = 1;
  let zBuild = 0;
  let buildNoH = false;
  if (
    lat != null &&
    lon != null &&
    typeof HubMapAcoustic !== "undefined" &&
    HubMapAcoustic.sectorObstacle
  ) {
    const obs = HubMapAcoustic.sectorObstacle(lat, lon, bearingDeg, baseR * 1.5, hr);
    forest = obs.forestPathM || 0;
    if (obs.g != null) g = obs.g;
    if (obs.barrierZ != null && obs.barrierZ > 0) zBuild = obs.barrierZ;
    else if (obs.buildingBlocked) buildNoH = true;
  }
  // DEM ring alone is not an along-ray profile — do not invent terrain z (was jagged).
  const env = envelopeAt(bearingDeg, {
    f_hz: fHz,
    t_c: tC,
    rh_pct: rh,
    p_kpa: pKpa,
    forest_path_m: forest,
    g: g,
    barrier_z_m: zBuild,
    hr_m: hr,
    hs_m: ISO_HS_M,
  });
  env.build_no_height = buildNoH;
  env.hr_m = hr;
  _lastEnvelopeHud = env;
  return env.r_m;
}

function circularSmooth(arr, k) {
  const n = arr.length;
  const out = new Array(n);
  for (let i = 0; i < n; i++) {
    let s = 0;
    let w = 0;
    for (let j = -k; j <= k; j++) {
      const ww = k + 1 - Math.abs(j);
      s += arr[(i + j + n) % n] * ww;
      w += ww;
    }
    out[i] = s / w;
  }
  return out;
}

function blobLatLngs(lat, lon, baseR, met, el, node) {
  const raw = [];
  for (let i = 0; i < HUB_POLY_N; i++) {
    const br = (i * 360) / HUB_POLY_N;
    raw.push(radiusAt(baseR, br, met, el, lat, lon, node));
  }
  const sm = circularSmooth(raw, 2);
  const pts = [];
  for (let i = 0; i < HUB_POLY_N; i++) {
    pts.push(destPoint(lat, lon, (i * 360) / HUB_POLY_N, sm[i]));
  }
  return pts;
}

function rayLatLngs(lat, lon, az, rangeM) {
  const pts = [];
  for (let i = 0; i <= HUB_RAY_N; i++) {
    pts.push(destPoint(lat, lon, az, (i / HUB_RAY_N) * rangeM));
  }
  return pts;
}

function rayTickLatLngs(lat, lon, az, rangeM) {
  const ticks = [];
  for (let d = HUB_RAY_TICK_M; d <= rangeM; d += HUB_RAY_TICK_M) {
    ticks.push({ ll: destPoint(lat, lon, az, d), m: d });
  }
  return ticks;
}

