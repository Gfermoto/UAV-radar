/* Hub map — Leaflet facade + shared map state (Wave D)
 *
 * @file hub_map.js
 * @brief Device/zone map; fitBounds to nodes+zones only (never ADS-B world zoom).
 * @note hubMapShow() sets _hubMapForceFit so opening the tab re-fits devices.
 */
const HUB_NODE_R_M = 500;
const HUB_RAY_M = 500;
const HUB_RAY_N = 20;
const HUB_AC_CAP = 48;
const HUB_WX_TTL_MS = 10 * 60 * 1000;
const HUB_RING_N = 16;
const HUB_POLY_N = 72;
let _hubMap = null;
let _hubMapReady = false;
let _hubMapFitKey = "";
let _hubMapForceFit = false;
/** Lab / Istra fallback when нет координат узлов и зон в snapshot. */
const HUB_MAP_FALLBACK_CENTER = [55.93391, 36.60942];
const HUB_MAP_FALLBACK_ZOOM = 11;
let _hubRelief = null;
const _hubNodeL = {};
const _hubTrackL = {};
const _hubZoneL = {};
const _hubAcL = {};
const _wxMet = {};
const _wxEl = {};
const _wxPending = {};
const HUB_OSM_TTL_MS = 30 * 60 * 1000;
const HUB_OSM_MAX_WAYS = 800;
const HUB_OSM_ENDPOINTS = [
  "https://overpass-api.de/api/interpreter",
  "https://lz4.overpass-api.de/api/interpreter",
];
let _osmGroups = null; // { roads, buildings, forest }
let _osmCache = { key: "", t0: 0, elements: null };
let _osmPending = false;
let _osmHint = "";
let _airGroup = null;
let _airportsData = null;
let _airportsLoading = false;

function _nidIcon(n, nid) {
  const thr = _num(n.threat);
  const confirmed = typeof isConfirmedAlarm === "function" ? isConfirmedAlarm(n) : false;
  let cls = "hub-map-nid";
  if (n.online) cls += " on";
  else cls += " off";
  // Полный thr/warn акцент только при confirmed; early/none — тише (candidate).
  if (confirmed && thr != null && thr >= 0.7) cls += " thr";
  else if (confirmed && thr != null && thr >= 0.3) cls += " warn";
  else if (!confirmed && thr != null && thr > 0) cls += " candidate";
  if (selected && canonNodeId(selected) === nid) cls += " sel";
  const lab = String((n && n.label) || "").trim();
  // Как в списке узлов: имя места · серийник (не только hex).
  const caption = lab ? (lab + " · " + nid) : nid;
  return L.divIcon({
    className: cls,
    html: '<span class="pin"></span><span class="id">' + escapeHtml(caption) + "</span>",
    iconSize: [168, 22],
    iconAnchor: [8, 11],
  });
}

function _popRow(k, vHtml) {
  if (vHtml == null || vHtml === "") return "";
  return "<div class='row'><span class='k'>" + escapeHtml(k) + "</span><span class='v'>" + vHtml + "</span></div>";
}

function nodePopupHtml(n, nid, ll, az) {
  const h = (n && n.last_heartbeat) || {};
  const d = (n && (n.joined_detection || n.last_detection)) || {};
  const raw = (n && n.last_detection) || {};
  const bits = [];
  const lab = String((n && n.label) || "").trim();
  bits.push("<div class='nid'>" + escapeHtml(lab ? (lab + " · " + nid) : nid) + "</div>");
  if (lab) bits.push("<div class='coords' style='opacity:.85'>серийник " + escapeHtml(nid) + "</div>");
  bits.push("<div class='coords'>" + ll[0].toFixed(5) + ", " + ll[1].toFixed(5) + "</div>");
  const staleGps =
    typeof nodeMapPosStaleLabel === "function" ? nodeMapPosStaleLabel(n) : null;
  if (staleGps) bits.push(_popRow("GPS", escapeHtml(staleGps)));
  const src = ((h.node_position || {}).source) || ((raw.node_position || {}).source) || "";
  if (src) bits.push(_popRow("GPS", escapeHtml(src)));
  if (az != null) {
    const ph = typeof nodeAzimuthPhase === "function" ? nodeAzimuthPhase(n) : null;
    let label = "азимут";
    if (ph && ph.phase === "afterglow" && ph.age_s != null) {
      label = "азимут · " + Math.round(ph.age_s) + " с назад";
    } else if (ph && ph.phase === "hot") {
      label = "азимут · live";
    }
    bits.push(_popRow(label, escapeHtml(az.toFixed(1) + "°")));
  }
  const inst =
    _num(n.install_height_m) ??
    _num(h.install_height_m) ??
    _num((h.node_position || {}).install_height_m) ??
    (_num(h.alt_m) != null && _num(h.alt_m) <= 40 ? _num(h.alt_m) : null);
  if (inst != null && inst > 0) {
    bits.push(_popRow("установка", escapeHtml(fmt(inst) + " м над землёй")));
  }
  let ground = _num(n.ground_elev_m);
  if (ground == null) {
    const mk = wxKey(ll[0], ll[1]);
    Object.keys(_wxEl).forEach((k) => {
      if (k.indexOf(mk) === 0 && _wxEl[k] && _wxEl[k].elev0 != null && !_wxEl[k].fail) {
        ground = _num(_wxEl[k].elev0);
      }
    });
  }
  if (ground != null) {
    bits.push(_popRow("рельеф", escapeHtml(fmt(ground) + " м")));
    const tip = _num(n.tip_alt_m) ?? (inst != null && inst > 0 ? ground + inst : null);
    if (tip != null) {
      bits.push(_popRow("tip 3D", escapeHtml(fmt(tip) + " м абс.")));
    }
  }
  const up = fmtUptime(h.uptime_s);
  if (up) bits.push(_popRow("uptime", escapeHtml(up)));
  const noise = _num(h.noise_dbfs);
  if (noise != null) bits.push(_popRow("шум", escapeHtml(noise.toFixed(1) + " dBFS")));
  const splFast = _num(h.spl_fast);
  const splDbfs = _num(h.spl_dbfs);
  if (splFast != null) bits.push(_popRow("SPL", escapeHtml(fmt(splFast))));
  else if (splDbfs != null) bits.push(_popRow("fast", escapeHtml(splDbfs.toFixed(1) + " dBFS")));
  const confirmed = typeof isConfirmedAlarm === "function" ? isConfirmedAlarm(n) : false;
  if (n.threat != null) {
    const thrCol = confirmed ? threatColor(n.threat) : "var(--muted)";
    bits.push(_popRow(
      "угроза",
      "<span style='color:" + thrCol + "'>" + escapeHtml(fmt(n.threat)) + "</span>"
    ));
  }
  const blob = Object.assign({}, raw, d);
  const clsLab =
    typeof classOperatorLabel === "function"
      ? classOperatorLabel(blob, n)
      : classRuOf(blob) || d.class_name || raw.class_name;
  const conf = d.confidence ?? d.p ?? raw.confidence;
  const detBits = [];
  if (clsLab) detBits.push(escapeHtml(fmt(clsLab)));
  if (conf != null) detBits.push(escapeHtml(fmt(conf)));
  const tier = String(d.alarm_tier || raw.alarm_tier || "").toLowerCase();
  if (tier === "confirmed") detBits.push("подтверждено");
  else if (tier === "early") detBits.push("раннее");
  if (d.early_warning || raw.early_warning) detBits.push("приближение");
  if (detBits.length) bits.push(_popRow("детекция", detBits.join(" · ")));
  return "<div class='hub-map-pop'>" + bits.join("") + "</div>";
}

function _bindMapToggles() {
  const relief = $("hubMapRelief");
  if (relief && !relief._hubBound) {
    relief.checked = _lsFlag("hubMapRelief", true);
    relief.addEventListener("change", () => {
      _setLsFlag("hubMapRelief", relief.checked);
      _syncOverlays();
    });
    relief._hubBound = true;
  }
  if (typeof HubMapAcoustic !== "undefined" && HubMapAcoustic.bindExtraToggles) {
    HubMapAcoustic.bindExtraToggles(() => {
      _syncOverlays();
      if (lastSnap) HubMapAcoustic.onRender(_hubMap, lastSnap, true);
    });
  }
}

function _syncOverlays() {
  if (!_hubMap) return;
  if (_hubRelief) {
    if (reliefWanted()) {
      if (!_hubMap.hasLayer(_hubRelief)) _hubRelief.addTo(_hubMap);
    } else if (_hubMap.hasLayer(_hubRelief)) {
      _hubMap.removeLayer(_hubRelief);
    }
  }
  if (typeof HubMapAcoustic !== "undefined" && HubMapAcoustic.syncLayerVisibility) {
    HubMapAcoustic.syncLayerVisibility(_hubMap);
  }
}

function _ensureHubMap() {
  if (_hubMapReady) return !!_hubMap;
  const el = $("hubMap");
  if (!el || typeof L === "undefined") return false;
  _bindMapToggles();
  _hubMap = L.map(el, {
    zoomControl: true,
    attributionControl: false,
  });
  _hubMap.createPane("reliefPane");
  _hubMap.getPane("reliefPane").style.zIndex = 250;
  _hubMap.createPane("zonePane");
  _hubMap.getPane("zonePane").style.zIndex = 350;
  _hubMap.createPane("nodeCirclePane");
  _hubMap.getPane("nodeCirclePane").style.zIndex = 400;
  _hubMap.createPane("rayPane");
  _hubMap.getPane("rayPane").style.zIndex = 450;
  _hubMap.createPane("trackPane");
  _hubMap.getPane("trackPane").style.zIndex = 470;
  L.tileLayer("https://tile{s}.maps.2gis.com/tiles?x={x}&y={y}&z={z}", {
    subdomains: "0123",
    attribution: "",
    maxZoom: 18,
  }).addTo(_hubMap);
  _hubRelief = L.tileLayer(
    "https://server.arcgisonline.com/ArcGIS/rest/services/Elevation/World_Hillshade/MapServer/tile/{z}/{y}/{x}",
    {
      attribution: "",
      maxZoom: 16,
      opacity: 0.45,
      pane: "reliefPane",
    }
  );
  _hubMap.setView(HUB_MAP_FALLBACK_CENTER, HUB_MAP_FALLBACK_ZOOM);
  _hubMap.on("moveend", () => { _paintWxHud(); });
  _hubMapReady = true;
  if (typeof HubMapAcoustic !== "undefined" && HubMapAcoustic.onMapReady) {
    HubMapAcoustic.onMapReady(_hubMap);
  }
  _syncOverlays();
  return true;
}

function _prune(store, keep, drop) {
  Object.keys(store).forEach((k) => {
    if (keep.has(k)) return;
    drop(store[k]);
    delete store[k];
  });
}

function _dropGroup(g) {
  if (!g || !_hubMap) return;
  ["marker", "circle", "ray", "rayTicks", "wedge"].forEach((k) => {
    if (g[k]) _hubMap.removeLayer(g[k]);
  });
}

function _fitMapToBounds(bounds) {
  if (!_hubMap || !bounds || !bounds.isValid()) return false;
  const ne = bounds.getNorthEast();
  const sw = bounds.getSouthWest();
  if (ne.lat === sw.lat && ne.lng === sw.lng) {
    _hubMap.setView(ne, 14, { animate: false });
    return true;
  }
  _hubMap.fitBounds(bounds.pad(0.12), {
    maxZoom: 16,
    padding: [48, 48],
    animate: false,
  });
  return true;
}

function _defaultMapView(s) {
  if (!_hubMap) return;
  const zones = typeof hubMapZones === "function" ? hubMapZones(s) : [];
  if (zones.length) {
    const pts = zones.map((z) => [z.lat, z.lon]);
    _fitMapToBounds(L.latLngBounds(pts));
    return;
  }
  _hubMap.setView(HUB_MAP_FALLBACK_CENTER, HUB_MAP_FALLBACK_ZOOM, { animate: false });
}

function _fitIfNeeded(bounds, keyChanged) {
  const force = _hubMapForceFit || keyChanged;
  if (!force) return;
  if (_fitMapToBounds(bounds)) _hubMapForceFit = false;
}

function _setHint(text) {
  const h = $("hubMapHint");
  if (!h) return;
  if (!text) {
    h.style.display = "none";
    h.textContent = "";
    return;
  }
  h.style.display = "block";
  h.textContent = text;
}

function _polyStyle(color, fillOp, weight) {
  return {
    color: color,
    weight: weight,
    fillColor: color,
    fillOpacity: fillOp,
    smoothFactor: 1.1,
  };
}

function _bearingWedge(lat, lon, az, distM, halfDeg) {
  const pts = [[lat, lon]];
  const steps = 10;
  for (let i = 0; i <= steps; i++) {
    const bearing = az - halfDeg + (2 * halfDeg * i) / steps;
    pts.push(destPoint(lat, lon, bearing, distM));
  }
  return pts;
}

function _trackRing(lat, lon, ell) {
  const cross = Number(ell && ell.cross_m) || 40;
  const along = Number(ell && ell.along_m) || cross;
  const major = Number(ell && ell.major_bearing_deg) || 0;
  const pts = [];
  for (let i = 0; i < 32; i++) {
    const t = (i / 32) * Math.PI * 2;
    const a = along * Math.cos(t);
    const c = cross * Math.sin(t);
    const bearing = major + (Math.atan2(c, a) * 180) / Math.PI;
    pts.push(destPoint(lat, lon, bearing, Math.hypot(a, c)));
  }
  return pts;
}

function _trackPopup(tr) {
  const est = tr.estimate || {};
  const tgt = tr.target || {};
  const bits = [];
  const row = (k, v) => {
    if (v == null || v === "") return;
    bits.push("<div>" + escapeHtml(k) + ": " + escapeHtml(String(v)) + "</div>");
  };
  row("трек", tr.track_id);
  row("класс", tgt.label_ru || tgt.class);
  const alts = (tgt.alternatives || []).map((a) => a.class).filter(Boolean);
  if (alts.length) row("ещё", alts.join(", "));
  row("угроза", tr.threat);
  const how = {
    bearing_intersection: "два пеленга",
    bearing_mle: "пеленги, по углам",
    bearing_lsq: "пеленги, по метрам",
  };
  row("как посчитано", how[est.method] || est.method);
  row("угол", est.crossing_deg);
  row("база м", est.baseline_m);
  row("узлы", (tr.contributors || []).map((c) => c.node_id).filter(Boolean).join(", "));
  if (tr.civil_possible) row("ADS-B", "возможен гражданский");
  return "<div class='hub-map-pop'>" + bits.join("") + "</div>";
}

function _renderTracks(s, fitPts) {
  if (!_hubMap) return;
  const keep = new Set();
  ((s && s.tracks) || []).forEach((tr) => {
    const est = tr.estimate || {};
    const lat = _num(est.lat);
    const lon = _num(est.lon);
    const id = String(tr.track_id || "");
    if (!id || lat == null || lon == null) return;
    keep.add(id);
    fitPts.push([lat, lon]);
    const low = est.quality === "low";
    const civil = !!tr.civil_possible;
    const color = civil ? "#8a8f98" : "#d4a017";
    const opacity = tr.state === "stale" ? 0.35 : 0.85;
    const ring = _trackRing(lat, lon, est.ellipse || {});
    let g = _hubTrackL[id];
    if (!g) {
      g = {
        ell: L.polygon(ring, {
          pane: "trackPane",
          color: color,
          weight: 1.5,
          fillColor: color,
          fillOpacity: 0.18,
          dashArray: low ? "4 4" : null,
          interactive: false,
        }).addTo(_hubMap),
        lines: L.layerGroup().addTo(_hubMap),
        marker: L.marker([lat, lon], {
          pane: "trackPane",
          zIndexOffset: 550,
          icon: L.divIcon({
            className: "hub-track-icon",
            html: "<span></span>",
            iconSize: [12, 12],
            iconAnchor: [6, 6],
          }),
        }).addTo(_hubMap),
      };
      _hubTrackL[id] = g;
    }
    g.ell.setLatLngs(ring);
    g.ell.setStyle({
      color: color,
      fillColor: color,
      opacity: opacity,
      fillOpacity: tr.state === "stale" ? 0.08 : 0.18,
      dashArray: low ? "4 4" : null,
    });
    g.lines.clearLayers();
    (tr.contributors || []).forEach((c) => {
      const clat = _num(c.lat);
      const clon = _num(c.lon);
      if (clat == null || clon == null) return;
      L.polyline([[clat, clon], [lat, lon]], {
        pane: "trackPane",
        color: color,
        weight: 1,
        opacity: opacity * 0.7,
        dashArray: "2 6",
        interactive: false,
      }).addTo(g.lines);
    });
    g.marker.setLatLng([lat, lon]);
    const label = (tr.target && (tr.target.label_ru || tr.target.class)) || "БПЛА";
    const live = tr.state !== "stale";
    g.marker.setIcon(L.divIcon({
      className: "hub-track-icon" + (live ? " live" : ""),
      html: "<b>" + escapeHtml(label) + "</b><small>вероятно · " + escapeHtml(String(tr.n_nodes || "")) + "</small>",
      iconSize: [88, 28],
      iconAnchor: [44, 14],
    }));
    g.marker.setOpacity(opacity);
    const pop = _trackPopup(tr);
    if (g._pop !== pop) {
      g.marker.bindPopup(pop, { closeButton: true, maxWidth: 280 });
      g._pop = pop;
    }
  });
  Object.keys(_hubTrackL).forEach((id) => {
    if (keep.has(id)) return;
    const g = _hubTrackL[id];
    if (!g) return;
    _hubMap.removeLayer(g.ell);
    _hubMap.removeLayer(g.lines);
    _hubMap.removeLayer(g.marker);
    delete _hubTrackL[id];
  });
}

function renderHubMap(s) {
  const mapView = $("viewMap");
  if (!mapView || mapView.classList.contains("view-hidden")) return;
  if (!_ensureHubMap()) {
    _setHint(typeof L === "undefined" ? "Leaflet не загрузился (CDN)." : "Нет контейнера карты.");
    return;
  }
  const nodes = (s && s.nodes) || [];
  const keepN = new Set();
  /** Fit only devices + zones — never ADS-B (иначе zoom = весь земной шар). */
  const fitPts = [];
  const b = [];
  nodes.forEach((n) => {
    const nid = canonNodeId(n.node_id);
    const ll = nodeMapPos(n);
    if (!nid || !ll) return;
    keepN.add(nid);
    fitPts.push(ll);
    const wxRaw = requestWx(ll[0], ll[1], HUB_NODE_R_M * 0.6);
    const met = wxRaw.met ? Object.assign({}, wxRaw.met, { nowS: Date.now() / 1000 }) : null;
    const el = wxRaw.el;
    let pts;
    try {
      pts = blobLatLngs(ll[0], ll[1], HUB_NODE_R_M, met, el, n);
    } catch (err) {
      pts = circleLatLngs(ll[0], ll[1], HUB_NODE_R_M);
    }
    if (_lastEnvelopeHud) _envByNid[nid] = Object.assign({}, _lastEnvelopeHud);
    let g = _hubNodeL[nid];
    if (!g) {
      g = {
        circle: L.polygon(pts, Object.assign({ pane: "nodeCirclePane" }, _polyStyle("#3dba7a", 0.1, 1.3))).addTo(_hubMap),
        ray: L.polyline([ll, ll], {
          pane: "rayPane",
          color: "#d4a017",
          weight: 3,
          opacity: 0,
          interactive: false,
        }).addTo(_hubMap),
        rayTicks: L.layerGroup().addTo(_hubMap),
        marker: L.marker(ll, { icon: _nidIcon(n, nid), zIndexOffset: 600 }).addTo(_hubMap),
      };
      g.marker.on("click", () => {
        selected = n.node_id;
        if (lastSnap) {
          renderNodes(lastSnap);
          renderDetail(lastSnap);
          renderHubMap(lastSnap);
        }
      });
      _hubNodeL[nid] = g;
    } else {
      g.circle.setLatLngs(pts);
    }
    g.marker.setLatLng(ll);
    const lab = String((n && n.label) || "").trim();
    const tierSig = typeof alarmTierOf === "function" ? alarmTierOf(n) : "";
    const iconSig =
      nid + "|" + lab + "|" + (n.online ? "1" : "0") + "|" + String(_num(n.threat)) + "|" +
      tierSig + "|" +
      (selected && canonNodeId(selected) === nid ? "s" : "");
    if (g._iconSig !== iconSig) {
      g.marker.setIcon(_nidIcon(n, nid));
      g._iconSig = iconSig;
    }
    const az = nodeAzimuth(n);
    const azPh = typeof nodeAzimuthPhase === "function" ? nodeAzimuthPhase(n) : null;
    const showRay = az != null && azPh != null;
    if (!g.ray) {
      g.ray = L.polyline([ll, ll], {
        pane: "rayPane",
        color: "#d4a017",
        weight: 3,
        opacity: 0,
        interactive: false,
      }).addTo(_hubMap);
    }
    if (!g.rayTicks) g.rayTicks = L.layerGroup().addTo(_hubMap);
    if (!g.wedge) {
      g.wedge = L.polygon([ll, ll, ll], {
        pane: "rayPane",
        color: "#d4a017",
        weight: 0,
        fillColor: "#d4a017",
        fillOpacity: 0,
        interactive: false,
      }).addTo(_hubMap);
    }
    if (!showRay) {
      g.ray.setStyle({ opacity: 0 });
      g.ray.setLatLngs([ll, ll]);
      g.rayTicks.clearLayers();
      g.wedge.setStyle({ fillOpacity: 0 });
    } else {
      const hot = azPh.phase === "hot";
      const op = hot ? 0.95 : 0.42;
      const wt = hot ? 3 : 2;
      const col = hot ? "#d4a017" : "#a89050";
      try {
        g.ray.setLatLngs(rayLatLngs(ll[0], ll[1], az, HUB_RAY_M));
        g.ray.setStyle({ opacity: op, weight: wt, color: col });
        g.rayTicks.clearLayers();
        const showLbl = _hubMap && _hubMap.getZoom() >= 14;
        rayTickLatLngs(ll[0], ll[1], az, HUB_RAY_M).forEach((tk) => {
          const cm = L.circleMarker(tk.ll, {
            pane: "rayPane",
            radius: hot ? 3.5 : 2.5,
            color: col,
            weight: 1.2,
            fillColor: "#fff8e7",
            fillOpacity: hot ? 0.95 : 0.55,
            interactive: false,
          });
          if (showLbl) {
            cm.bindTooltip(tk.m + " м", {
              permanent: false,
              direction: "right",
              className: "hub-ray-tick",
            });
          }
          cm.addTo(g.rayTicks);
        });
      } catch (err) {
        g.ray.setLatLngs([ll, destPoint(ll[0], ll[1], az, HUB_RAY_M)]);
        g.ray.setStyle({ opacity: op, weight: wt, color: col });
      }
      g.wedge.setLatLngs(_bearingWedge(ll[0], ll[1], az, HUB_RAY_M, 12));
      g.wedge.setStyle({ fillColor: col, fillOpacity: hot ? 0.16 : 0.07 });
    }
    const popHtml = nodePopupHtml(n, nid, ll, showRay ? az : null);
    if (g._popHtml !== popHtml) {
      const pop = g.marker.getPopup();
      if (pop) pop.setContent(popHtml);
      else g.marker.bindPopup(popHtml, { closeButton: true, maxWidth: 280 });
      g._popHtml = popHtml;
    }
    b.push(ll);
    const gb = g.circle.getBounds();
    if (gb && gb.isValid()) {
      b.push(gb.getNorthEast());
      b.push(gb.getSouthWest());
      fitPts.push(gb.getNorthEast());
      fitPts.push(gb.getSouthWest());
    }
  });
  _prune(_hubNodeL, keepN, _dropGroup);
  _renderTracks(s, fitPts);

  const keepZ = new Set();
  hubMapZones(s).forEach((z) => {
    keepZ.add(z.id);
    const pts = circleLatLngs(z.lat, z.lon, z.r);
    let c = _hubZoneL[z.id];
    if (!c) {
      c = L.polygon(pts, Object.assign({ pane: "zonePane" }, _polyStyle("#6eb5ff", 0.055, 1.5))).addTo(_hubMap);
      _hubZoneL[z.id] = c;
    } else {
      c.setLatLngs(pts);
    }
    const tip =
      escapeHtml(z.name) +
      " · круг " +
      Math.round(z.r / 100) / 10 +
      " км (ADS-B геозона · глушение DET/HB/Mel при filter ON)";
    if (c._tip !== tip) {
      c.unbindTooltip();
      c.bindTooltip(tip, { sticky: true });
      c._tip = tip;
    }
    const zb = c.getBounds();
    if (zb && zb.isValid()) {
      fitPts.push(zb.getNorthEast());
      fitPts.push(zb.getSouthWest());
    }
  });
  _prune(_hubZoneL, keepZ, (c) => { if (c) _hubMap.removeLayer(c); });

  const acs = ((((s || {}).hub || {}).adsb || {}).aircraft) || [];
  const keepA = new Set();
  acs.slice(0, HUB_AC_CAP).forEach((ac) => {
    const lat = _num(ac.lat);
    const lon = _num(ac.lon);
    if (lat == null || lon == null) return;
    const id = String(ac.hex || (lat + "," + lon)).toLowerCase();
    keepA.add(id);
    let m = _hubAcL[id];
    if (!m) {
      m = L.marker([lat, lon], { icon: _acIcon(ac), zIndexOffset: 500 }).addTo(_hubMap);
      _hubAcL[id] = m;
    }
    m.setLatLng([lat, lon]);
    const acHtml = _acIcon(ac).options.html;
    if (m._acHtml !== acHtml) {
      m.setIcon(_acIcon(ac));
      m._acHtml = acHtml;
    }
    const acPop = _acPopupHtml(ac);
    if (m._popHtml !== acPop) {
      const pop = m.getPopup();
      if (pop) pop.setContent(acPop);
      else m.bindPopup(acPop, { closeButton: true, maxWidth: 260 });
      m._popHtml = acPop;
    }
    // ADS-B intentionally NOT in fitPts — иначе fitBounds = весь земной шар.
  });
  _prune(_hubAcL, keepA, (m) => { if (m) _hubMap.removeLayer(m); });

  _syncOverlays();
  _paintWxHud();
  _paintAdsbHud(s);
  if (typeof HubMapAcoustic !== "undefined" && HubMapAcoustic.onRender) {
    HubMapAcoustic.onRender(_hubMap, s);
  }

  if (!keepN.size && !keepZ.size) {
    _setHint("Нет координат: узлы без lat/lon и зона ADS-B без центра.");
    _hubMapFitKey = "";
    if (_hubMapForceFit) {
      _defaultMapView(s);
      _hubMapForceFit = false;
    }
  } else {
    _setHint("");
    const fitKey = [...keepN].sort().join(",") + "|" + [...keepZ].sort().join(",");
    const keyChanged = fitKey !== _hubMapFitKey;
    if (fitPts.length) _fitIfNeeded(L.latLngBounds(fitPts), keyChanged);
    _hubMapFitKey = fitKey;
  }
}

function hubMapShow() {
  _hubMapForceFit = true;
  requestAnimationFrame(() => {
    requestAnimationFrame(() => {
      if (_ensureHubMap() && _hubMap) _hubMap.invalidateSize();
      _syncOverlays();
      if (lastSnap) renderHubMap(lastSnap);
    });
  });
}
