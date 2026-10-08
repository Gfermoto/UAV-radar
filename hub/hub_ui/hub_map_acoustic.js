/* Hub map acoustic layers: OSM Overpass + OurAirports.
 * Depends: $, L, _num, _lsFlag, _setLsFlag, escapeHtml, _setHint, _hubMap
 */
(function (global) {
  const OSM_TTL = 30 * 60 * 1000;
  const OSM_FAIL_TTL = 12 * 1000;
  const NODE_PAD = 0.015; // ~1.7 km — локальный акустический контекст у узла
  const ENDPOINTS = ["/api/hub/overpass"];

  let osmCache = { key: "", t0: 0, elements: null, fail: false };
  let osmPending = false;
  let _forestPolys = [];
  let _buildingPolys = []; // { ring, heightM }
  let _roadPolys = [];
  let airGroup = null;
  let airportsData = null;
  let airportsLoading = false;
  let lastAirKey = "";

  function layerOn(domId, def) {
    const el = $(domId);
    if (el) return !!el.checked;
    return _lsFlag(domId, def);
  }

  function ensureGroups(map) {
    if (airGroup) return;
    map.createPane("airPane");
    map.getPane("airPane").style.zIndex = 380;
    airGroup = L.layerGroup();
  }

  function syncLayerVisibility(map) {
    if (!map || !airGroup) return;
    const on = layerOn("hubMapAirports", true);
    if (on && !map.hasLayer(airGroup)) airGroup.addTo(map);
    if (!on && map.hasLayer(airGroup)) map.removeLayer(airGroup);
  }

  function nodePoints(s) {
    const pts = [];
    const seen = [];
    const push = (lat, lon) => {
      if (seen.some((q) => Math.abs(q[0] - lat) < 0.01 && Math.abs(q[1] - lon) < 0.01)) return;
      seen.push([lat, lon]);
      pts.push([lat, lon]);
    };
    ((s && s.nodes) || []).forEach((n) => {
      const hb = (n && n.last_heartbeat) || {};
      const hp = hb.node_position || {};
      const candidates = [
        [hb.lat, hb.lon],
        [hp.lat, hp.lon],
      ];
      for (let i = 0; i < candidates.length; i++) {
        const lat = _num(candidates[i][0]);
        const lon = _num(candidates[i][1]);
        if (lat != null && lon != null) {
          push(lat, lon);
          break;
        }
      }
    });
    const zones = ((((s || {}).hub || {}).zones || {}).zones) || [];
    zones.forEach((z) => {
      const lat = _num(z.lat);
      const lon = _num(z.lon);
      if (lat != null && lon != null) push(lat, lon);
    });
    return pts;
  }

  /** Локальные bbox вокруг узлов (не один огромный прямоугольник на 50 км). */
  function nodeBoxes(s) {
    const pts = nodePoints(s);
    if (!pts.length) return null;
    const boxes = pts.map((p) => ({
      south: p[0] - NODE_PAD,
      west: p[1] - NODE_PAD,
      north: p[0] + NODE_PAD,
      east: p[1] + NODE_PAD,
    }));
    const key = boxes
      .map((b) => [b.south, b.west, b.north, b.east].map((x) => x.toFixed(3)).join(","))
      .sort()
      .join("|");
    return { boxes, key };
  }

  /** Объединённый bbox для фильтра аэродромов (шире OSM pad). */
  function airportsBbox(pts) {
    if (!pts.length) return null;
    const pad = 0.45; // ~50 km — крупные/средние аэродромы вокруг узлов
    let s0 = pts[0][0], n0 = pts[0][0], w0 = pts[0][1], e0 = pts[0][1];
    pts.forEach((p) => {
      s0 = Math.min(s0, p[0] - pad);
      n0 = Math.max(n0, p[0] + pad);
      w0 = Math.min(w0, p[1] - pad);
      e0 = Math.max(e0, p[1] + pad);
    });
    return { south: s0, west: w0, north: n0, east: e0 };
  }

  function wayLatLngs(el) {
    if (!el || !el.geometry || !el.geometry.length) return null;
    const ll = [];
    el.geometry.forEach((g) => {
      if (g && g.lat != null && g.lon != null) ll.push([g.lat, g.lon]);
    });
    return ll.length >= 2 ? ll : null;
  }

  function classifyWay(tags) {
    if (!tags) return null;
    if (tags.highway && /^(motorway|trunk|primary|secondary|tertiary)(_link)?$/.test(tags.highway)) {
      return "roads";
    }
    if (tags.building) return "buildings";
    if (tags.landuse === "forest" || tags.landuse === "wood" || tags.natural === "wood") {
      return "forest";
    }
    return null;
  }

  function distPointToSegM(lat, lon, a, b) {
    // equirectangular metres near mid-lat
    const midLat = ((a[0] + b[0]) / 2) * Math.PI / 180;
    const mx = 111320 * Math.cos(midLat);
    const my = 110540;
    const ax = a[1] * mx;
    const ay = a[0] * my;
    const bx = b[1] * mx;
    const by = b[0] * my;
    const px = lon * mx;
    const py = lat * my;
    const dx = bx - ax;
    const dy = by - ay;
    const len2 = dx * dx + dy * dy;
    let t = 0;
    if (len2 > 1e-6) t = Math.max(0, Math.min(1, ((px - ax) * dx + (py - ay) * dy) / len2));
    const qx = ax + t * dx;
    const qy = ay + t * dy;
    return Math.hypot(px - qx, py - qy);
  }

  function nearRoad(lat, lon, widthM) {
    const half = Math.max(2, (widthM || 7) / 2);
    for (let i = 0; i < _roadPolys.length; i++) {
      const poly = _roadPolys[i];
      for (let j = 1; j < poly.length; j++) {
        if (distPointToSegM(lat, lon, poly[j - 1], poly[j]) <= half) return true;
      }
    }
    return false;
  }

  function barrierPathDiffM(dBarrier, heightM, rangeM, hrM) {
    const R = Math.max(50, rangeM || 500);
    const H = Math.max(0, heightM);
    const Hr = hrM != null && hrM >= 0.3 ? hrM : 1.5;
    const Hs = 30;
    const dsr = Math.max(1, Math.min(dBarrier, R - 1));
    const dss = Math.max(1, R - dsr);
    const via = Math.hypot(dss, Hs - H) + Math.hypot(dsr, H - Hr);
    return Math.max(0, via - R);
  }

  function buildingHeightM(tags) {
    if (!tags) return 6; // suburban default 2 floors when OSM has no height
    const h = parseFloat(tags.height);
    if (Number.isFinite(h) && h > 0) return h;
    const levels = parseFloat(tags["building:levels"]);
    if (Number.isFinite(levels) && levels > 0) return levels * 3;
    return 6;
  }

  function roadWidthM(tags) {
    if (!tags) return 0;
    const w = parseFloat(tags.width);
    if (Number.isFinite(w) && w > 0) return w;
    const lanes = parseFloat(tags.lanes);
    if (Number.isFinite(lanes) && lanes > 0) return lanes * 3.5;
    return 0;
  }

  function indexOsmElements(elements) {
    _forestPolys = [];
    _buildingPolys = [];
    _roadPolys = [];
    (elements || []).forEach((el) => {
      if (el.type !== "way") return;
      const kind = classifyWay(el.tags);
      const ll = wayLatLngs(el);
      if (!ll || ll.length < 2) return;
      if (kind === "forest" && ll.length >= 3) _forestPolys.push(ll);
      else if (kind === "buildings" && ll.length >= 3) {
        _buildingPolys.push({ ring: ll, heightM: buildingHeightM(el.tags) });
      } else if (kind === "roads") {
        const w = roadWidthM(el.tags);
        if (w > 0) _roadPolys.push({ ll: ll, widthM: w });
      }
    });
  }

  function pointInRing(lat, lon, ring) {
    let inside = false;
    for (let i = 0, j = ring.length - 1; i < ring.length; j = i++) {
      const yi = ring[i][0];
      const xi = ring[i][1];
      const yj = ring[j][0];
      const xj = ring[j][1];
      const intersect =
        yi > lat !== yj > lat &&
        lon < ((xj - xi) * (lat - yi)) / (yj - yi + 1e-12) + xi;
      if (intersect) inside = !inside;
    }
    return inside;
  }

  /** ISO sector sample — always on when OSM cache ready (no checkbox gate). */
  function sectorObstacle(lat, lon, bearingDeg, maxM, hrM) {
    const out = {
      forestPathM: 0,
      buildingBlocked: false,
      buildingDistM: 999,
      barrierZ: 0,
      g: 1,
    };
    if (!osmCache.elements || osmCache.fail) return out;
    const max = Math.max(50, Math.min(maxM || 750, 1200));
    const step = 25;
    let hardHits = 0;
    let samples = 0;
    for (let d = step; d <= max; d += step) {
      const p =
        typeof destPoint === "function"
          ? destPoint(lat, lon, bearingDeg, d)
          : [lat, lon];
      for (let i = 0; i < _forestPolys.length; i++) {
        if (pointInRing(p[0], p[1], _forestPolys[i])) {
          out.forestPathM += step;
          break;
        }
      }
      const inRecv = d <= 45;
      const inSrc = d >= Math.max(50, max - 900);
      if (inRecv || inSrc) {
        samples += 1;
        let hard = false;
        for (let r = 0; r < _roadPolys.length; r++) {
          const road = _roadPolys[r];
          const half = Math.max(2, (road.widthM || 7) / 2);
          for (let s = 1; s < road.ll.length; s++) {
            if (distPointToSegM(p[0], p[1], road.ll[s - 1], road.ll[s]) <= half) {
              hard = true;
              break;
            }
          }
          if (hard) break;
        }
        if (!hard) {
          for (let j = 0; j < _buildingPolys.length; j++) {
            if (pointInRing(p[0], p[1], _buildingPolys[j].ring)) {
              hard = true;
              break;
            }
          }
        }
        if (hard) hardHits += 1;
      }
      for (let j = 0; j < _buildingPolys.length; j++) {
        const b = _buildingPolys[j];
        if (d <= 120 && pointInRing(p[0], p[1], b.ring)) {
          out.buildingBlocked = true;
          out.buildingDistM = Math.min(out.buildingDistM, d);
          const hBuild = b.heightM > 0 ? b.heightM : 6;
          const z = barrierPathDiffM(d, hBuild, max, hrM);
          out.barrierZ = Math.max(out.barrierZ, z);
          break;
        }
      }
    }
    if (samples > 0) out.g = 1 - hardHits / samples;
    return out;
  }

  function buildQl(boxes) {
    const parts = [];
    boxes.forEach((b) => {
      const bb = b.south + "," + b.west + "," + b.north + "," + b.east;
      parts.push('way["building"](' + bb + ");");
      parts.push('way["landuse"~"^(forest|wood)$"](' + bb + ");");
      parts.push('way["natural"="wood"](' + bb + ");");
      parts.push(
        'way["highway"~"^(motorway|trunk|primary|secondary|tertiary)(_link)?$"](' +
          bb +
          ");"
      );
    });
    return "[out:json][timeout:25];(" + parts.join("") + ");out body geom;";
  }

  function fetchOverpass(pack) {
    const q = buildQl(pack.boxes);
    const url = ENDPOINTS[0];
    fetch(url, {
      method: "POST",
      credentials: "same-origin",
      headers: { "Content-Type": "application/json", Accept: "application/json" },
      body: JSON.stringify({ data: q }),
    })
      .then((r) => r.json().then((j) => ({ ok: r.ok, status: r.status, j })))
      .then(({ ok, status, j }) => {
        if (!ok || !j || !Array.isArray(j.elements)) {
          osmCache = { key: pack.key, t0: Date.now(), elements: null, fail: true };
          return;
        }
        osmCache = { key: pack.key, t0: Date.now(), elements: j.elements, fail: false };
        indexOsmElements(j.elements);
      })
      .catch(() => {
        osmCache = { key: pack.key, t0: Date.now(), elements: null, fail: true };
      })
      .finally(() => {
        osmPending = false;
      });
  }

  function boxesForView(map, pack) {
    if (!pack || !pack.boxes || !pack.boxes.length) return pack;
    if (!map || typeof map.getCenter !== "function") return pack;
    const c = map.getCenter();
    const boxes = pack.boxes.slice().sort((a, b) => {
      const da = Math.abs((a.south + a.north) / 2 - c.lat) + Math.abs((a.west + a.east) / 2 - c.lng);
      const db = Math.abs((b.south + b.north) / 2 - c.lat) + Math.abs((b.west + b.east) / 2 - c.lng);
      return da - db;
    });
    const one = [boxes[0]];
    const key = one
      .map((b) => [b.south, b.west, b.north, b.east].map((x) => x.toFixed(3)).join(","))
      .join("|");
    return { boxes: one, key: key };
  }

  function requestOsm(map, s, force) {
    if (!map) return;
    ensureGroups(map);
    syncLayerVisibility(map);
    const pack = boxesForView(map, nodeBoxes(s));
    if (!pack) return;
    const age = Date.now() - (osmCache.t0 || 0);
    if (!force && osmCache.key === pack.key && osmCache.elements && age < OSM_TTL) {
      indexOsmElements(osmCache.elements);
      return;
    }
    if (!force && osmCache.key === pack.key && osmCache.fail && age < OSM_FAIL_TTL) return;
    if (osmPending) return;
    osmPending = true;
    fetchOverpass(pack);
  }

  function airIcon(name) {
    return L.divIcon({
      className: "hub-map-ad",
      html: '<span class="diamond"></span><span class="id">' + escapeHtml(name || "AD") + "</span>",
      iconSize: [100, 16],
      iconAnchor: [6, 8],
    });
  }

  function paintAirports(bbox) {
    if (!airGroup || !airportsData || !bbox) return;
    airGroup.clearLayers();
    let n = 0;
    airportsData.forEach((a) => {
      if (n >= 40) return;
      if (a.type === "heliport") return; // меньше шума на карте
      const lat = _num(a.lat);
      const lon = _num(a.lon);
      if (lat == null || lon == null) return;
      if (lat < bbox.south || lat > bbox.north || lon < bbox.west || lon > bbox.east) return;
      n++;
      const label = a.iata || a.icao || a.name || "AD";
      const m = L.marker([lat, lon], { icon: airIcon(label), zIndexOffset: 200 });
      m.bindPopup(escapeHtml([a.name, a.icao, a.iata, a.type].filter(Boolean).join(" · ")));
      m.addTo(airGroup);
    });
  }

  function loadAirports(map, s) {
    ensureGroups(map);
    const pts = nodePoints(s);
    const bbox = airportsBbox(pts);
    if (!bbox) return;
    const airKey = [bbox.south, bbox.west, bbox.north, bbox.east].map((x) => x.toFixed(2)).join(",");
    function paintAndShow() {
      paintAirports(bbox);
      syncLayerVisibility(map);
    }
    if (airportsData) {
      // Всегда перекрашиваем: иначе после clear/смены зума маркеры «теряются».
      lastAirKey = airKey;
      paintAndShow();
      return;
    }
    if (airportsLoading) return;
    airportsLoading = true;
    lastAirKey = airKey;
    fetch("/hub_ui/airports_ru.json")
      .then((r) => {
        if (!r.ok) throw new Error("airports");
        return r.json();
      })
      .then((j) => {
        airportsData = Array.isArray(j) ? j : j.airports || [];
        paintAndShow();
      })
      .catch(() => {
        airportsData = [];
      })
      .finally(() => {
        airportsLoading = false;
      });
  }

  function onMapReady(map) {
    ensureGroups(map);
    syncLayerVisibility(map);
  }

  function onRender(map, s, force) {
    if (!map) return;
    requestOsm(map, s, !!force);
    loadAirports(map, s);
    syncLayerVisibility(map);
  }

  function bindExtraToggles(onChange) {
    ["hubMapAirports"].forEach((id) => {
      const el = $(id);
      if (!el || el._hubOsmBound) return;
      el.checked = _lsFlag(id, true);
      el.addEventListener("change", () => {
        _setLsFlag(id, el.checked);
        if (typeof onChange === "function") onChange();
        if (typeof _hubMap !== "undefined" && _hubMap) syncLayerVisibility(_hubMap);
      });
      el._hubOsmBound = true;
    });
  }

  global.HubMapAcoustic = {
    onMapReady,
    onRender,
    bindExtraToggles,
    syncLayerVisibility,
    sectorObstacle,
  };
})(typeof window !== "undefined" ? window : this);
