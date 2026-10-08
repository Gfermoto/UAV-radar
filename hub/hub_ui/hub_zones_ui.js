/* Hub zones / feeders settings UI (0.1.9+) */

let zonesDraft = null;

function _zid() {
  return "z" + Math.random().toString(36).slice(2, 8);
}

function _fid() {
  return "f" + Math.random().toString(36).slice(2, 8);
}

function ensureZonesDraft(z) {
  z = z || {};
  const feeders = Array.isArray(z.feeders) && z.feeders.length
    ? z.feeders.map((f) => ({ ...f }))
    : [{
        id: "default",
        name: "Default",
        url: "http://192.168.1.117/skyaware/data/aircraft.json",
        enabled: false,
        poll_interval_s: 5,
        stale_s: 30,
      }];
  const zones = Array.isArray(z.zones) && z.zones.length
    ? z.zones.map((x) => ({ ...x }))
    : [{
        id: "default",
        name: "Default",
        lat: null,
        lon: null,
        radius_m: 10000,
        feeder_id: feeders[0].id,
        filter_det: false,
        filter_hb: false,
        filter_mel: false,
        forward_mel: null,
        alert_mute: false,
      }];
  zonesDraft = {
    feeders,
    zones,
    node_zone: { ...(z.node_zone || {}) },
    mel_lan_only_global: z.mel_lan_only_global === true,
    gps_override_max_age_s: z.gps_override_max_age_s || 60,
  };
  return zonesDraft;
}

function feederCardHtml(f, idx) {
  const id = escapeHtml(f.id || "");
  return (
    '<article class="z-card" data-feeder-idx="' + idx + '">' +
      '<div class="z-card-top">' +
        '<input class="z-name" data-k="name" value="' + escapeHtml(f.name || f.id || "") + '" placeholder="Имя" />' +
        '<label class="toggle-row tight"><input type="checkbox" data-k="enabled"' + (f.enabled ? " checked" : "") + ' /><span>опрашивать</span></label>' +
        '<button type="button" class="icon-btn danger" data-rm-feeder title="Удалить">×</button>' +
      '</div>' +
      '<label class="z-full">Адрес списка самолётов' +
        '<input class="z-url" data-k="url" value="' + escapeHtml(f.url || "") + '" placeholder="http://…/aircraft.json" />' +
      '</label>' +
      '<div class="z-grid-2">' +
        '<label>id<input data-k="id" value="' + id + '" /></label>' +
        '<label>считается старым, с<input type="number" data-k="stale_s" min="5" value="' + escapeHtml(String(f.stale_s != null ? f.stale_s : 30)) + '" /></label>' +
      '</div>' +
    '</article>'
  );
}

function zoneCardHtml(z, feeders, idx) {
  const opts = (feeders || []).map((f) => {
    const sel = (z.feeder_id || "") === f.id ? " selected" : "";
    return '<option value="' + escapeHtml(f.id) + '"' + sel + ">" + escapeHtml(f.name || f.id) + "</option>";
  }).join("");
  return (
    '<article class="z-card zone" data-zone-idx="' + idx + '">' +
      '<div class="z-card-top">' +
        '<input class="z-name" data-k="name" value="' + escapeHtml(z.name || z.id || "") + '" placeholder="Имя площадки" />' +
        '<button type="button" class="icon-btn danger" data-rm-zone title="Удалить">×</button>' +
      '</div>' +
      '<div class="z-grid-2">' +
        '<label>id<input data-k="id" value="' + escapeHtml(z.id || "") + '" /></label>' +
        '<label>Источник<select data-k="feeder_id"><option value="">— без самолётов —</option>' + opts + "</select></label>" +
      "</div>" +
      '<div class="z-grid-3">' +
        '<label>широта<input type="number" step="0.0001" data-k="lat" value="' + escapeHtml(z.lat != null ? z.lat : "") + '" placeholder="55.75" /></label>' +
        '<label>долгота<input type="number" step="0.0001" data-k="lon" value="' + escapeHtml(z.lon != null ? z.lon : "") + '" placeholder="37.61" /></label>' +
        '<label title="Круг, внутри которого самолёт может остановить отправку в облако">радиус, м<input type="number" min="100" step="100" data-k="radius_m" value="' + escapeHtml(String(z.radius_m != null ? z.radius_m : 1500)) + '" /></label>' +
      "</div>" +
      '<div class="z-adsb-filters">' +
        '<div class="z-adsb-filters-h">Если в этом круге гражданский самолёт — не отправлять в облако.</div>' +
        '<div class="z-filters">' +
          '<label class="toggle-row tight"><input type="checkbox" data-k="filter_det"' + (z.filter_det ? " checked" : "") + " /><span>детекция</span></label>" +
          '<label class="toggle-row tight"><input type="checkbox" data-k="filter_hb"' + (z.filter_hb ? " checked" : "") + " /><span>пульс</span></label>" +
          '<label class="toggle-row tight"><input type="checkbox" data-k="filter_mel"' + (z.filter_mel ? " checked" : "") + " /><span>спектрограмма</span></label>" +
        "</div>" +
      "</div>" +
    "</article>"
  );
}

function collectFeedersFromDom() {
  const out = [];
  document.querySelectorAll("#feederCards .z-card").forEach((card) => {
    const get = (k) => card.querySelector("[data-k=\"" + k + "\"]");
    const idEl = get("id");
    const en = get("enabled");
    const url = get("url");
    const name = get("name");
    const stale = get("stale_s");
    const id = ((idEl && idEl.value) || "").trim() || _fid();
    out.push({
      id,
      name: ((name && name.value) || id).trim(),
      url: ((url && url.value) || "").trim(),
      enabled: !!(en && en.checked),
      poll_interval_s: 5,
      stale_s: Number((stale && stale.value) || 30),
    });
  });
  return out;
}

function collectZonesFromDom() {
  const out = [];
  document.querySelectorAll("#zoneCards .z-card").forEach((card) => {
    const get = (k) => card.querySelector("[data-k=\"" + k + "\"]");
    const id = (((get("id") || {}).value) || "").trim() || _zid();
    const latV = (get("lat") || {}).value;
    const lonV = (get("lon") || {}).value;
    const fid = ((get("feeder_id") || {}).value || "").trim();
    const prev = ((zonesDraft && zonesDraft.zones) || []).find((x) => x.id === id) || {};
    out.push({
      id,
      name: (((get("name") || {}).value) || id).trim(),
      lat: latV !== "" && latV != null ? Number(latV) : null,
      lon: lonV !== "" && lonV != null ? Number(lonV) : null,
      radius_m: Number(((get("radius_m") || {}).value) || 10000),
      feeder_id: fid || null,
      filter_det: !!(get("filter_det") && get("filter_det").checked),
      filter_hb: !!(get("filter_hb") && get("filter_hb").checked),
      filter_mel: !!(get("filter_mel") && get("filter_mel").checked),
      forward_mel: Object.prototype.hasOwnProperty.call(prev, "forward_mel")
        ? prev.forward_mel
        : null,
      alert_mute: false,
    });
  });
  return out;
}

function collectNodeZoneFromDom() {
  const out = {};
  document.querySelectorAll("#nodeZoneList select[data-node]").forEach((sel) => {
    const nid = sel.getAttribute("data-node");
    if (!nid) return;
    out[nid] = (sel.value || "").trim() || "_none";
  });
  return out;
}

function paintFeederCards() {
  const box = $("feederCards");
  if (!box || !zonesDraft) return;
  box.innerHTML = zonesDraft.feeders.map((f, i) => feederCardHtml(f, i)).join("") ||
    '<div class="empty">Нет feeders</div>';
  box.querySelectorAll("[data-rm-feeder]").forEach((btn) => {
    btn.onclick = () => {
      const card = btn.closest(".z-card");
      const idx = Number(card && card.getAttribute("data-feeder-idx"));
      if (!Number.isFinite(idx)) return;
      if (zonesDraft.feeders.length <= 1) {
        flash("Нужен хотя бы один feeder", false);
        return;
      }
      zonesDraft.feeders.splice(idx, 1);
      paintFeederCards();
      paintZoneCards();
    };
  });
}

function paintZoneCards() {
  const box = $("zoneCards");
  if (!box || !zonesDraft) return;
  box.innerHTML = zonesDraft.zones.map((z, i) => zoneCardHtml(z, zonesDraft.feeders, i)).join("") ||
    '<div class="empty">Нет зон</div>';
  box.querySelectorAll("[data-rm-zone]").forEach((btn) => {
    btn.onclick = () => {
      const card = btn.closest(".z-card");
      const idx = Number(card && card.getAttribute("data-zone-idx"));
      if (!Number.isFinite(idx)) return;
      if (zonesDraft.zones.length <= 1) {
        flash("Нужна хотя бы одна зона", false);
        return;
      }
      zonesDraft.zones.splice(idx, 1);
      paintZoneCards();
      paintNodeZoneList();
    };
  });
}

function paintNodeZoneList() {
  const box = $("nodeZoneList");
  if (!box || !zonesDraft) return;
  const nodes = (typeof lastSnap !== "undefined" && lastSnap && lastSnap.nodes) || [];
  if (!nodes.length) {
    box.innerHTML = '<div class="empty">Нет узлов — появятся после первого пакета</div>';
    return;
  }
  const draftLabs = {};
  document.querySelectorAll("#nodeZoneList input[data-node-label]").forEach((inp) => {
    const k = inp.getAttribute("data-node-label");
    if (k) draftLabs[k] = inp.value;
  });
  box.innerHTML = nodes.map((n) => {
    const nid = n.node_id || "";
    const nz = zonesDraft.node_zone || {};
    const mapped = nz[nid] || nz[String(nid).toUpperCase()] ||
      (typeof canonNodeId === "function" ? nz[canonNodeId(nid)] : null);
    const cur = (mapped == null || mapped === "") ? "default" : mapped;
    const snapLab = String(n.label || "").trim();
    const inputLab = Object.prototype.hasOwnProperty.call(draftLabs, nid)
      ? draftLabs[nid]
      : snapLab;
    const noneSel = cur === "_none" ? " selected" : "";
    const opts = '<option value="_none"' + noneSel + ">вне зоны</option>" +
      zonesDraft.zones.map((z) => {
        const sel = z.id === cur ? " selected" : "";
        return '<option value="' + escapeHtml(z.id) + '"' + sel + ">" + escapeHtml(z.name || z.id) + "</option>";
      }).join("");
    return (
      '<div class="node-zone-row">' +
        '<div class="nz-id">' +
          (snapLab ? '<span class="nz-lab">' + escapeHtml(snapLab) + "</span>" : "") +
          '<span class="mono">' + escapeHtml(nid) + "</span>" +
        "</div>" +
        '<input class="nz-label" data-node-label="' + escapeHtml(nid) +
          '" maxlength="40" placeholder="Имя" value="' + escapeHtml(inputLab) + '" />' +
        '<select data-node="' + escapeHtml(nid) + '">' + opts + "</select>" +
      "</div>"
    );
  }).join("");
}

async function saveNodeLabelsFromDom() {
  const inputs = document.querySelectorAll("#nodeZoneList input[data-node-label]");
  for (const inp of inputs) {
    const nid = inp.getAttribute("data-node-label");
    if (!nid) continue;
    const label = (inp.value || "").trim();
    const j = await hubCloudFetch(
      "/api/hub/nodes/" + encodeURIComponent(nid) + "/label",
      { method: "POST", body: { label } }
    );
    if (!j) return false;
    if (lastSnap && lastSnap.nodes) {
      lastSnap.nodes.forEach((n) => {
        if (canonNodeId(n.node_id) === canonNodeId(nid)) n.label = j.label || "";
      });
    }
  }
  return true;
}

function paintZonesChips(z) {
  const chips = $("zonesCfgChips");
  if (!chips) return;
  z = z || zonesDraft || {};
  const feeders = z.feeders || [];
  const nfOn = feeders.filter((f) => f.enabled).length;
  const nz = (z.zones || []).length;
  chips.innerHTML =
    '<span class="chip ' + (nfOn ? "adsb-live" : "") + '">' +
      (nfOn ? (nfOn + " feeder · poll") : "poll off") + "</span>" +
    '<span class="chip">' + nz + " " + (nz === 1 ? "зона" : "зоны") + "</span>";
}

function melLanOnlyFromUi() {
  if ($("melForwardCloud")) return !$("melForwardCloud").checked;
  if ($("melLanOnlyGlobal")) return !!$("melLanOnlyGlobal").checked;
  return !!(zonesDraft && zonesDraft.mel_lan_only_global);
}

function paintMelPolicy() {
  if (!zonesDraft) return;
  if ($("melForwardCloud") && !$("melForwardCloud").dataset.touched) {
    $("melForwardCloud").checked = !zonesDraft.mel_lan_only_global;
  }
  if ($("melLanOnlyGlobal") && !$("melLanOnlyGlobal").dataset.touched) {
    $("melLanOnlyGlobal").checked = !!zonesDraft.mel_lan_only_global;
  }
  const box = $("melZoneOverrides");
  if (box) {
    const zones = zonesDraft.zones || [];
    if (!zones.length) {
      box.innerHTML = "";
    } else {
      box.innerHTML =
        '<div class="mel-ov-h">Для площадки</div>' +
        zones.map((z) => {
          const fm = z.forward_mel;
          const v = fm === true ? "1" : fm === false ? "0" : "";
          return (
            '<div class="mel-ov-row">' +
              '<span class="mono">' + escapeHtml(z.name || z.id) + "</span>" +
              '<select data-mel-zone="' + escapeHtml(z.id) + '">' +
                '<option value=""' + (v === "" ? " selected" : "") + ">как у всех</option>" +
                '<option value="1"' + (v === "1" ? " selected" : "") + ">в облако</option>" +
                '<option value="0"' + (v === "0" ? " selected" : "") + ">оставить здесь</option>" +
              "</select>" +
            "</div>"
          );
        }).join("");
    }
  }
  if ($("melPolicyStatus")) {
    $("melPolicyStatus").textContent = zonesDraft.mel_lan_only_global
      ? "спектрограммы остаются здесь"
      : "спектрограммы уходят в облако";
  }
  const adv = $("melZoneAdv");
  if (adv) {
    const n = (zonesDraft.zones || []).length;
    adv.hidden = n < 2;
  }
}

function paintZonesForm(z) {
  ensureZonesDraft(z);
  if ($("gpsOverrideAge") && !$("gpsOverrideAge").dataset.touched) {
    $("gpsOverrideAge").value = zonesDraft.gps_override_max_age_s || 60;
  }
  paintFeederCards();
  paintZoneCards();
  paintNodeZoneList();
  paintZonesChips(zonesDraft);
  paintMelPolicy();
  const nOn = zonesDraft.feeders.filter((f) => f.enabled).length;
  if ($("zonesCfgStatus")) {
    $("zonesCfgStatus").textContent =
      (nOn ? ("poll · " + nOn + " feeder") : "poll выкл") +
      " · " + zonesDraft.zones.length + " зона";
  }
}

async function loadZonesSettings() {
  const j = await hubCloudFetch("/api/hub/zones");
  if (!j) return;
  paintZonesForm(j.zones || {});
}

async function saveZonesSettings() {
  if (!zonesDraft) ensureZonesDraft({});
  // keep Mel policy from current draft / Mel panel
  const melLan = ($("melForwardCloud") || $("melLanOnlyGlobal"))
    ? melLanOnlyFromUi()
    : !!zonesDraft.mel_lan_only_global;
  collectMelOverridesIntoDraft();
  zonesDraft.feeders = collectFeedersFromDom();
  zonesDraft.zones = collectZonesFromDom();
  // re-apply mel overrides after collect (by id)
  collectMelOverridesIntoDraft();
  zonesDraft.node_zone = collectNodeZoneFromDom();
  zonesDraft.mel_lan_only_global = melLan;
  zonesDraft.gps_override_max_age_s = Number(($("gpsOverrideAge") && $("gpsOverrideAge").value) || 60);
  if (!(await saveNodeLabelsFromDom())) return;
  const j = await hubCloudFetch("/api/hub/zones", {
    method: "POST",
    body: zonesDraft,
  });
  if (!j) return;
  ["gpsOverrideAge", "melForwardCloud", "melLanOnlyGlobal"].forEach((id) => {
    const el = $(id);
    if (el) delete el.dataset.touched;
  });
  paintZonesForm(j.zones || {});
  flash("Площадки сохранены", true);
  if (typeof tick === "function") tick();
}

function collectMelOverridesIntoDraft() {
  if (!zonesDraft) return;
  document.querySelectorAll("#melZoneOverrides select[data-mel-zone]").forEach((sel) => {
    const zid = sel.getAttribute("data-mel-zone");
    const z = (zonesDraft.zones || []).find((x) => x.id === zid);
    if (!z) return;
    const v = sel.value;
    z.forward_mel = v === "1" ? true : v === "0" ? false : null;
  });
}

async function saveMelPolicy(opts) {
  const quiet = !!(opts && opts.quiet);
  if (!zonesDraft) {
    await loadZonesSettings();
    if (!zonesDraft) return false;
  }
  zonesDraft.mel_lan_only_global = melLanOnlyFromUi();
  collectMelOverridesIntoDraft();
  const j = await hubCloudFetch("/api/hub/zones", {
    method: "POST",
    body: {
      mel_lan_only_global: zonesDraft.mel_lan_only_global,
      zones: zonesDraft.zones,
      feeders: zonesDraft.feeders,
      node_zone: zonesDraft.node_zone,
      gps_override_max_age_s: zonesDraft.gps_override_max_age_s,
    },
  });
  if (!j) return false;
  ["melForwardCloud", "melLanOnlyGlobal"].forEach((id) => {
    if ($(id)) delete $(id).dataset.touched;
  });
  paintZonesForm(j.zones || {});
  if (!quiet) flash("Mel policy сохранена", true);
  if (typeof tick === "function") tick();
  return true;
}

function renderZoneOpsStrip(s) {
  const strip = $("zoneOpsStrip");
  if (!strip) return;
  const z = ((s && s.hub) || {}).zones || {};
  const zones = z.zones || [];
  const feeders = z.feeders || [];
  const adsb = ((s && s.hub) || {}).adsb || {};
  const fcache = adsb.feeders || {};
  if (!zones.length && !feeders.length) {
    strip.innerHTML = "";
    return;
  }
  strip.innerHTML = zones.map((zone) => {
    const fid = zone.feeder_id || "";
    const fc = fcache[fid] || {};
    const live = fc.fresh ? "live" : (fid ? "stale" : "off");
    const cls = live === "live" ? "ok" : live === "stale" ? "warn" : "";
    const rKm = zone.radius_m != null ? (Math.round(zone.radius_m / 100) / 10) + " км" : "—";
    const filters = [];
    if (zone.filter_det) filters.push("−DET");
    if (zone.filter_hb) filters.push("−HB");
    if (zone.filter_mel) filters.push("−Mel");
    return (
      '<div class="zone-pill ' + cls + '">' +
        '<span class="zp-name">' + escapeHtml(zone.name || zone.id) + "</span>" +
        '<span class="zp-meta">' + escapeHtml(rKm) +
          (fid ? " · " + escapeHtml(fid) : " · без ADS-B") +
          (filters.length ? " · " + filters.join(" ") : "") +
        "</span>" +
      "</div>"
    );
  }).join("");
}

function paintFwdMeter(fwd) {
  fwd = fwd || {};
  const depth = Number(fwd.queue_depth || 0);
  const max = Math.max(1, Number(fwd.queue_max || 256));
  const pct = Math.min(100, Math.round((depth / max) * 100));
  if ($("fwdQueueDepth")) {
    $("fwdQueueDepth").textContent = depth + " / " + max;
  }
  if ($("fwdQueueBar")) {
    $("fwdQueueBar").style.width = pct + "%";
    $("fwdQueueBar").classList.toggle("hot", depth > max * 0.6);
  }
  if ($("fwdQueueHint")) {
    $("fwdQueueHint").textContent = depth
      ? "есть события — уйдут, когда облако снова на связи"
      : "буфер пуст";
  }
}
