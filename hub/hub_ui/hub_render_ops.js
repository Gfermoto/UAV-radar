/* Hub render — mqtt / adsb / webhooks / mel / detail (Wave E) */
function fillMqttForm(s) {
  const m = s.mqtt || {};
  if (document.activeElement && ["mqttHost","mqttPort","mqttUser","mqttPass"].includes(document.activeElement.id)) {
    // не затирать поля во время ввода
  } else {
    if ($("mqttHost") && !$("mqttHost").dataset.touched) $("mqttHost").value = m.host || "";
    if ($("mqttPort") && !$("mqttPort").dataset.touched) $("mqttPort").value = m.port || 1883;
    if ($("mqttUser") && !$("mqttUser").dataset.touched) $("mqttUser").value = m.username || "";
    if ($("mqttPass") && !$("mqttPass").dataset.touched) {
      $("mqttPass").placeholder = m.has_password ? "сохранён (оставьте пустым)" : "пусто = без пароля";
    }
  }
  const st = m.status || s.mqtt_status || "off";
  if ($("mqttCfgStatus")) $("mqttCfgStatus").textContent = st;
}

function renderMqtt(s) {
  // Конфиг брокера only — лента сообщений объединена в #feed.
  fillMqttForm(s);
}

function adsbSignalLabel(a) {
  const sig = a.signal || (a.enabled ? (a.fresh ? "live" : (a.stale ? "stale" : "waiting")) : "off");
  const map = {
    live: ["приём LIVE", "adsb-live", "ok"],
    stale: ["приём stale", "adsb-warn", "warn"],
    error: ["ошибка", "adsb-bad", "bad"],
    waiting: ["ожидание", "adsb-warn", "warn"],
    off: ["выкл", "", ""],
  };
  const row = map[sig] || map.waiting;
  return { sig, label: row[0], chip: row[1], banner: row[2] };
}

function renderAdsbHero(a) {
  a = a || {};
  const sAdsb = $("sAdsb");
  const sCircle = $("sAdsbCircle");
  const wrapA = $("statAdsb");
  const wrapC = $("statAdsbCircle");
  if (!sAdsb) return;
  const sl = adsbSignalLabel(a);
  const live = sl.sig === "live";
  if (wrapA) wrapA.classList.toggle("is-off", !live);
  if (wrapC) wrapC.classList.toggle("is-off", !live);
  if (!live) {
    sAdsb.textContent = "off";
    sAdsb.title = a.last_error ? String(a.last_error) : (sl.label || "");
    if (sCircle) sCircle.textContent = "—";
    return;
  }
  const health = a.last_health || {};
  const total = health.total_aircraft != null ? health.total_aircraft : "—";
  sAdsb.textContent = "LIVE · " + total;
  sAdsb.title = "Самолёты с приёмника площадки";
  if (sCircle) {
    if (a.positions_missing) sCircle.textContent = "нет lat/lon";
    else if (a.in_circle || a.civilian_in_radius) {
      const n = (a.meta || {}).adsb_count_in_radius;
      sCircle.textContent = "да · " + (n != null ? n : "≥1");
    } else sCircle.textContent = "пусто";
  }
}

function renderAdsbOps(s) {
  const a = ((s && s.hub) || {}).adsb || {};
  renderAdsbHero(a);
  if (typeof renderZoneOpsStrip === "function") renderZoneOpsStrip(s);
  const box = $("adsbOpsStatus");
  const chips = $("adsbOpsChips");
  const banners = $("adsbBanners");
  const list = $("adsbAcList");
  if (!box) return;
  const anyFeeder = !!(a.enabled || (a.feeders && Object.keys(a.feeders).length) ||
    ((((s.hub || {}).zones || {}).feeders || []).some((f) => f.enabled)));
  if (!anyFeeder) {
    if (chips) chips.innerHTML = '<span class="chip">ADS-B off</span>';
    if (banners) banners.innerHTML = "";
    if (list) list.innerHTML = "";
    box.innerHTML = "<div class='empty'>Выкл. Настройка зон в ⚙</div>";
    return;
  }
  const health = a.last_health || {};
  const meta = a.meta || {};
  const sl = adsbSignalLabel(a);
  const inCircle = !!(a.in_circle || a.civilian_in_radius);
  const chipHtml = [];
  chipHtml.push('<span class="chip ' + (sl.chip || "adsb") + '">сигнал · ' + sl.label + '</span>');
  if (a.positions_missing) {
    chipHtml.push('<span class="chip adsb-warn">координат нет</span>');
  } else if (inCircle) {
    chipHtml.push('<span class="chip adsb">в круге · ' + (meta.adsb_count_in_radius || 1) + '</span>');
  } else if (a.fresh) {
    chipHtml.push('<span class="chip">круг пуст</span>');
  }
  if (chips) chips.innerHTML = chipHtml.join("");

  if (banners) {
    const total = health.total_aircraft != null ? health.total_aircraft : "—";
    const withPos = health.aircraft_with_positions != null ? health.aircraft_with_positions : "—";
    const inR = meta.adsb_count_in_radius != null ? meta.adsb_count_in_radius : "—";
    const nearest = meta.adsb_nearest_dist_m != null ? (Math.round(meta.adsb_nearest_dist_m) + " м") : "—";
    const rKm = a.radius_m != null ? (Math.round(a.radius_m / 100) / 10) + " км" : "—";
    let circleCls = "warn", circleText = "нет данных";
    if (a.positions_missing) { circleCls = "warn"; circleText = "самолёты есть, lat/lon нет — круг не считается"; }
    else if (inCircle) { circleCls = "live"; circleText = "да · " + inR + " в радиусе " + rKm; }
    else if (a.fresh) { circleCls = "ok"; circleText = "нет · радиус " + rKm; }
    banners.innerHTML =
      '<div class="adsb-banner ' + escapeHtml(sl.banner || "") + '"><div class="k">Сигнал ADS-B</div><div class="v">' + escapeHtml(sl.label) +
        '</div><div style="margin-top:4px;color:var(--muted)">heard ' + escapeHtml(total) + ' · с координатами ' + escapeHtml(withPos) + '</div></div>' +
      '<div class="adsb-banner ' + escapeHtml(circleCls) + '"><div class="k">Самолёт в круге</div><div class="v">' +
        (inCircle ? "ДА" : (a.positions_missing ? "—" : "НЕТ")) +
        '</div><div style="margin-top:4px;color:var(--muted)">' + escapeHtml(circleText) +
        (nearest !== "—" ? " · nearest " + escapeHtml(nearest) : "") + '</div></div>';
  }

  box.innerHTML =
    cell("poll", a.running ? "running" : "stopped", a.running ? "join-ok" : "join-bad") +
    cell("site", a.site_id || "—") +
    cell("радиус", a.radius_m != null ? (Math.round(a.radius_m) + " м") : "—") +
    cell("site lat/lon", (a.site_lat != null && a.site_lon != null) ? (Number(a.site_lat).toFixed(3) + ", " + Number(a.site_lon).toFixed(3)) : "не задано", (a.site_lat != null ? "" : "join-bad")) +
    cell("filter DET/HB/MEL", [a.filter_det, a.filter_hb, a.filter_mel].map(x => x ? "ON" : "off").join("/")) +
    (a.last_error ? cell("err", a.last_error, "join-bad") : "");

  if (list) {
    const rows = a.aircraft || [];
    if (!rows.length) {
      list.innerHTML = a.fresh ? "<div class='empty'>В последнем poll пусто</div>" : "";
    } else {
      list.innerHTML =
        '<div class="row head"><div>HEX</div><div>рейс</div><div>alt</div><div>круг / dist</div></div>' +
        rows.slice(0, 12).map(ac => {
          const dist = ac.dist_m != null ? (Math.round(ac.dist_m / 100) / 10) + " км" : "—";
          const mark = ac.in_radius ? "В КРУГЕ" : (ac.lat != null ? dist : "нет pos");
          return '<div class="row' + (ac.in_radius ? " in" : "") + '">' +
            "<div>" + escapeHtml(ac.hex || "—") + "</div>" +
            "<div>" + escapeHtml(ac.flight || "—") + "</div>" +
            "<div>" + escapeHtml(ac.alt_baro != null ? ac.alt_baro : "—") + "</div>" +
            "<div>" + escapeHtml(mark) + "</div></div>";
        }).join("");
    }
  }
}

function renderWebhooks(s) {
  const hub = (s && s.hub) || {};
  const wh = hub.webhooks || {};
  const box = $("webhookStatus");
  if (box) {
    const tg = wh.telegram || {};
    const ift = wh.ifttt || {};
    const em = wh.email || {};
    const ch = (name, o) => {
      if (!o.enabled) return cell(name, "off");
      if (!o.configured) return cell(name, "ON · не настроено", "join-bad");
      return cell(name, "ON · ready", "join-ok");
    };
    box.innerHTML =
      ch("Telegram", tg) +
      ch("IFTTT", ift) +
      ch("Email", em) +
      cell("min threat", wh.min_threat != null ? fmt(wh.min_threat) : "—") +
      cell("cooldown с", wh.cooldown_s != null ? fmt(wh.cooldown_s) : "—");
  }
  const feed = $("webhookFeed");
  if (!feed) return;
  const rows = hub.webhook_deliveries || s.webhook_deliveries || [];
  if (!rows.length) {
    feed.innerHTML = "<div class='empty'>Появятся, когда сработает уведомление</div>";
    return;
  }
  feed.innerHTML = rows.slice(0, 40).map(r => {
    const cls = r.ok ? "ok" : "err";
    const det = escapeHtml((r.detail || "").slice(0, 80));
    return `<div class="row"><span class="${cls}">${escapeHtml(r.channel || "?")}</span> ${escapeHtml(r.node_id || "?")} · ${det}</div>`;
  }).join("");
}

async function ensureBearerToken() {
  let tok = sessionStorage.getItem(tokenKey);
  if (tok) return tok;
  tok = await openAuthModal();
  if (!tok) return null;
  tok = String(tok).trim().replace(/^Bearer\s+/i, "");
  if (!tok) return null;
  sessionStorage.setItem(tokenKey, tok);
  return tok;
}

async function postMqtt(body) {
  const j = await hubCloudFetch("/api/mqtt", { method: "POST", body });
  if (!j) return;
  ["mqttHost","mqttPort","mqttUser","mqttPass"].forEach(id => {
    if ($(id)) delete $(id).dataset.touched;
  });
  if ($("mqttPass")) $("mqttPass").value = "";
  tick();
}

function renderMelFiles(s) {
  const box = $("melFiles");
  const files = (typeof filterSortMelFiles === "function")
    ? filterSortMelFiles(s.mel_saved || [])
    : (s.mel_saved || []);
  if (!files.length) {
    box.innerHTML = "<div class='empty'>Спектрограмм пока нет</div>";
    return;
  }
  const cap = lastMelDisk ? 500 : 40;
  box.innerHTML = files.slice(0, cap).map(f => {
    const m = f.meta || {};
    const dim = (m.num_bands && m.num_frames) ? `${m.num_bands}×${m.num_frames}` : "";
    const received = fmtWhen(f.received_at || f.mtime);
    const captured = f.clock_ok === false
      ? (f.capture_note || "нет NTP (uptime)")
      : fmtWhen(f.captured_at || m.timestamp_ms);
    const cls = m.class_name ? ` · <b>${escapeHtml(m.class_name)}</b>` : "";
    const gt = m.gt_label ? ` · GT <b>${escapeHtml(m.gt_label)}</b>` : "";
    const file = String(f.file || "");
    const url = String(f.url || "");
    return `<div class="row">
      <span class="nid">${escapeHtml(received)}</span>
      · принято · захват ${escapeHtml(captured)}
      · ${escapeHtml(m.node_id || "—")}${cls}${gt} · ${escapeHtml(dim)} · ${escapeHtml(fmtBytes(f.size))}
      · <a href="#" data-mel-dl="${escapeHtml(file)}" data-mel-url="${escapeHtml(url)}">${escapeHtml(file)}</a>
      · <a href="#" data-mel="${escapeHtml(file)}">спектрограмма</a>
      · <a href="#" data-mel-del="${escapeHtml(file)}" style="color:var(--danger,#ef4444)">удалить</a>
    </div>`;
  }).join("");
  box.querySelectorAll("[data-mel-dl]").forEach(a => {
    a.onclick = async (ev) => {
      ev.preventDefault();
      const name = a.getAttribute("data-mel-dl");
      const url = a.getAttribute("data-mel-url");
      try {
        const n = await downloadUrl(url, name);
        flash("Скачано " + name + " (" + fmtBytes(n) + ")", true);
      } catch (e) {
        flash("Скачивание: " + (e && e.message ? e.message : e), false);
      }
    };
  });
  box.querySelectorAll("[data-mel]").forEach(a => {
    a.onclick = (ev) => { ev.preventDefault(); openMelSpectrogram(a.getAttribute("data-mel")); };
  });
  box.querySelectorAll("[data-mel-del]").forEach(a => {
    a.onclick = async (ev) => {
      ev.preventDefault();
      const name = a.getAttribute("data-mel-del");
      if (!name || !confirm("Удалить спектрограмму?\n" + name)) return;
      const r = await fetch("/api/mel/" + encodeURIComponent(name), {
        method: "DELETE",
        credentials: "same-origin",
      });
      if (r.status === 401) {
        flash("Войдите снова", false);
        return;
      }
      const j = await r.json().catch(() => ({}));
      if (!r.ok) {
        flash(j.error || ("Удаление: HTTP " + r.status), false);
        return;
      }
      flash("Удалено: " + (j.file || name), true);
      await tick();
    };
  });
}

function renderDetail(s) {
  const n = (s.nodes || []).find(x => x.node_id === selected);
  const box = $("detail");
  if (!n) {
    const hasAny = (s.nodes || []).length > 0;
    box.innerHTML = hasAny
      ? "<div class='empty'>Выберите узел слева</div>"
      : emptyOnboardHtml("Устройств пока нет");
    bindEmptyCa(box);
    return;
  }
  const d = n.joined_detection || n.last_detection || {};
  const raw = n.last_detection || {};
  const h = n.last_heartbeat || {};
  const m = n.last_mel || {};
  const thr = n.threat != null ? n.threat : 0;
  const joinCls = d.joined ? "join-ok" : "join-bad";
  const joinLabel = d.joined ? "Склеено с HB" : ("Не склеено: " + (d.join_reason || "нет данных"));
  const className = d.class_name || raw.class_name;
  const detBlob = Object.assign({}, raw, d);
  const classRu = classRuOf(detBlob);
  const classOp =
    typeof classOperatorLabel === "function" ? classOperatorLabel(detBlob, n) : classRu;
  const layers = d.detection_layers || raw.detection_layers;
  const fusion = d.nn_raw_top || raw.nn_raw_top || d.fusion_decision || raw.fusion_decision || null;
  const rpmOk = !!(d.rpm_valid ?? raw.rpm_valid);
  const confirmed = d.confirmed ?? raw.confirmed;
  const alarmOk = typeof isConfirmedAlarm === "function" ? isConfirmedAlarm(n) : false;
  const early = !!(d.early_warning || raw.early_warning);
  const gpsLab = gpsPosLabel(h);
  const gpsStale =
    typeof nodeMapPosStaleLabel === "function" ? nodeMapPosStaleLabel(n) : null;
  const heapCur = (n.health && n.health.free_heap != null) ? fmtBytes(n.health.free_heap) : null;
  const heapMin = (n.health && n.health.free_heap_min != null) ? fmtBytes(n.health.free_heap_min) : null;
  const uptimeStr = fmtUptime(h.uptime_s);
  const lab = String(n.label || "").trim();
  const title = lab || n.node_id;
  const titleStyle = lab
    ? "font-size:1.2rem;font-weight:600"
    : "font-family:var(--mono);font-size:1.2rem";
  const idLine = lab
    ? `<div style="font-family:var(--mono);font-size:0.85rem;color:var(--muted);margin-top:4px">${escapeHtml(n.node_id)}</div>`
    : "";
  box.innerHTML = `
    <div>
      <div style="display:flex;justify-content:space-between;align-items:baseline;gap:12px;flex-wrap:wrap">
        <div>
          <div style="${titleStyle}">${escapeHtml(title)}</div>
          ${idLine}
          <div style="color:var(--muted);font-size:0.85rem;margin-top:4px;display:flex;gap:10px;flex-wrap:wrap;align-items:center">
            <span class="dot ${n.online ? "on" : "off"}"></span>
            <span>${n.online ? "online" : "offline"}</span>
            ${transportChips(n.transports)}
            ${cabinetChip(n, s)}
          </div>
          <div style="font-family:var(--mono);font-size:0.95rem;margin-top:10px">
            ${uptimeStr
              ? `<span title="Живёт с последней перезагрузки платы (HB.uptime_s). Не сбрасывается от пакетов."><span style="color:var(--muted);font-size:0.7rem;text-transform:uppercase;letter-spacing:0.06em">uptime</span> ${escapeHtml(uptimeStr)}</span>`
              : `<span style="color:var(--muted)">uptime —</span>`}
          </div>
        </div>
        <div style="text-align:right;min-width:140px">
          <div style="font-size:0.7rem;color:var(--muted);text-transform:uppercase">Угроза</div>
          <div style="font-family:var(--mono);font-size:1.6rem;color:${alarmOk ? threatColor(n.threat) : "var(--muted)"}">${fmt(n.threat)}</div>
          <div class="threat-bar${alarmOk ? "" : " candidate"}"><i style="width:${Math.min(100, thr*100)}%"></i></div>
        </div>
      </div>
    </div>
    <div>
      <div class="sec-h"><h2>Детекция</h2>${viaChip(d.via || raw.via)}${loiterChip(n)}${(d.civilian_in_radius || raw.civilian_in_radius) ? '<span class="chip adsb">гражданское в радиусе</span>' : ''}${n.active_episode && n.active_episode.open ? '<span class="chip" title="Hub coalesce">эпизод активен</span>' : ''}</div>
      <div class="kv">
        ${n.active_episode ? cellMaybe("эпизод hops", n.active_episode.hop_count) : ""}
        ${n.active_episode && n.active_episode.mel_count ? cellMaybe("эпизод mel", n.active_episode.mel_count) : ""}
        ${n.active_episode && Array.isArray(n.active_episode.classes_seen) && n.active_episode.classes_seen.length > 1 ? cellMaybe("классы", n.active_episode.classes_seen.join("→")) : ""}
        ${cellOrNa("класс", className != null ? fmt(className) : null)}
        ${cellOrNa("класс RU", classOp || classRu)}
        ${cellOrNa("class_id", d.class_id != null || raw.class_id != null ? fmt(d.class_id ?? raw.class_id) : null)}
        ${cellOrNa("уверенность", fmt(d.confidence ?? d.p ?? raw.confidence))}
        ${cellOrNa("NN raw", fusion)}
        ${(() => {
          const az = fmt(d.azimuth_deg ?? raw.azimuth_deg);
          if (az == null) return cellOrNa("азимут °", null);
          const ph = typeof nodeAzimuthPhase === "function" ? nodeAzimuthPhase(n) : null;
          if (ph && ph.phase === "afterglow" && ph.age_s != null) {
            return cell("азимут °", az + " · " + Math.round(ph.age_s) + " с назад");
          }
          if (ph && ph.phase === "hot") {
            return cell("азимут °", az + " · live");
          }
          return cell("азимут °", String(az));
        })()}
        ${cellOrNa("doa conf", fmt(d.doa_confidence ?? raw.doa_confidence))}
        ${(() => {
          const s = d.doa_sigma_deg ?? raw.doa_sigma_deg;
          const k = d.doa_n ?? raw.doa_n;
          if (s == null || k == null) return "";
          return cell("разброс пеленга", "±" + Math.round(Number(s)) + "° · " + k + " отсч.");
        })()}
        ${(() => {
          // Старые узлы слали true_north без поправки на установку.
          if ((d.doa_n ?? raw.doa_n) == null) return "";
          const ref = d.heading_ref ?? raw.heading_ref;
          if (ref === "board") return cell("азимут от", "оси платы", "muted");
          if (ref === "true_north") return cell("азимут от", "севера");
          return "";
        })()}
        ${cellOrNa("bpf Гц", fmt(d.bpf_hz ?? raw.bpf_hz))}
        ${rpmOk ? cell("RPM", fmt(d.rpm ?? raw.rpm) + " · " + fmt(d.blade_count ?? raw.blade_count) + " лоп.") : ""}
        ${confirmed == null && !(d.alarm_tier || raw.alarm_tier) ? "" : (() => {
          const tier = String(d.alarm_tier || raw.alarm_tier || "").toLowerCase();
          if (tier === "early") return cell("тревога", "раннее · кандидат", "join-ok");
          if (tier === "confirmed") return cell("тревога", "подтверждено", "join-ok");
          if (confirmed == null) return "";
          return cell("confirmed", confirmed ? "да" : "нет");
        })()}
        ${layersChipsHtml(layers) || cellOrNa("слои", null)}
        ${early ? cell("приближение", "Kalman", "join-ok") : ""}
        ${(d.civilian_in_radius || raw.civilian_in_radius) ? cell("ADS-B гражданское", "да · в радиусе") : ""}
      </div>
    </div>
    <div>
      <h2>Склейка DET ← HB</h2>
      <div class="kv">
        ${cell("статус", joinLabel, joinCls)}
        ${cellMaybe("|Δt| DET↔HB", d.hb_age_ms != null ? (Math.round(Number(d.hb_age_ms)) + " мс") : null)}
      </div>
    </div>
    <div>
      <div class="sec-h"><h2>Пульс узла (HB)</h2>${viaChip(h.via)}</div>
      <div class="kv">
        ${cellOrNa("статус", fmt(h.status))}
        ${cellOrNa("device", fmt(h.device_type))}
        ${cellOrNa("fw", fmt(h.firmware_version || (h.fw && h.fw.version) || n.firmware_version))}
        ${cellOrNa("RSSI dBm", (n.health && n.health.rssi_dbm != null) ? String(Math.round(n.health.rssi_dbm)) : null)}
        ${cellOrNa("free heap", heapCur)}
        ${cellOrNa("heap min (с boot)", heapMin)}
        ${cellOrNa("temp °C", h.temp_c != null ? Number(h.temp_c).toFixed(1) : null)}
        ${cellOrNa("cpu MHz", fmt(h.cpu_mhz))}
        ${cellOrNa("cpu load", h.cpu_load != null ? Number(h.cpu_load).toFixed(2) : null)}
        ${cellOrNa("infer ms", h.infer_ms != null ? fmtSec(h.infer_ms) : null)}
        ${cellOrNa("arena", h.arena_used_bytes != null ? fmtBytes(h.arena_used_bytes) : null)}
        ${h.spl_fast != null ? cellOrNa("SPL", fmt(h.spl_fast)) : cellOrNa("fast dBFS", fmt(h.spl_dbfs))}
        ${cellOrNa("шум dBFS", fmt(h.noise_dbfs))}
        ${cellOrNa("широта", h.lat != null ? Number(h.lat).toFixed(5) : null)}
        ${cellOrNa("долгота", h.lon != null ? Number(h.lon).toFixed(5) : null)}
        ${cellOrNa("высота м", h.alt_m != null ? (fmt(h.alt_m) + (h.alt_ref === "msl" ? " MSL" : (h.alt_ref === "agl" ? " AGL" : ""))) : null)}
        ${cellOrNa("позиция", gpsStale || gpsLab)}
      </div>
    </div>
    <div class="charts">
      <div class="chart-box"><div class="t">Угроза</div><canvas class="spark" id="cThreat"></canvas></div>
      <div class="chart-box"><div class="t">Уверенность</div><canvas class="spark" id="cConf"></canvas></div>
      <div class="chart-box"><div class="t">RSSI</div><canvas class="spark" id="cRssi"></canvas></div>
      <div class="chart-box"><div class="t">Heap</div><canvas class="spark" id="cHeap"></canvas></div>
    </div>
    <div class="mel-wrap">
      <div style="display:flex;justify-content:space-between;gap:10px;flex-wrap:wrap;align-items:center">
        <div class="sec-h" style="margin:0"><h2 style="margin:0">Спектрограмма Mel ${m.num_frames ? `(${m.num_bands}×${m.num_frames})` : ""}</h2>${viaChip(m.via || (m.num_frames ? "pb_mel" : ""))}</div>
        ${m.saved_file ? `<button type="button" id="btnMelOpen">Открыть спектрограмму</button>` : ""}
      </div>
      <div class="kv" style="margin:10px 0">
        ${cell("peak ~Гц", m.peak_valid === false ? "—" : fmt(m.peak_hz_approx))}
        ${cellMaybe("форма", m.spectrum_shape)}
        ${cell("mean", fmt(m.value_mean))}
        ${cell("dyn", fmt(m.dynamic_range_db))}
        ${cell("байты", fmt(m.payload_bytes))}
        ${cell("файл", m.saved_file ? m.saved_file : (m.save_skip || "—"))}
      </div>
      <canvas class="mel1d" id="cMel"></canvas>
      ${(!m.band_means) ? "<div class='empty' style='margin-top:8px'>Mel ещё не приходил</div>" : ""}
    </div>
  `;
  requestAnimationFrame(() => {
    spark($("cThreat"), (n.series || {}).threat, "#d4a017");
    spark($("cConf"), (n.series || {}).confidence, "#8aa396");
    spark($("cRssi"), (n.series || {}).rssi, "#4ecdc4");
    spark($("cHeap"), (n.series || {}).heap, "#8aa396");
    drawMel1d($("cMel"), m.band_means);
    const btn = $("btnMelOpen");
    if (btn && m.saved_file) btn.onclick = () => openMelSpectrogram(m.saved_file);
  });
}

