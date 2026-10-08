/* Hub render — nodes / checklist (Wave E) */
function forwardRecForNode(n, s) {
  const fwd = (s && s.hub && s.hub.forward) || {};
  const map = fwd.nodes || {};
  const raw = String((n && n.node_id) || "");
  const want = typeof canonNodeId === "function" ? canonNodeId(raw) : raw;
  if (want && map[want]) return { fwd: fwd, rec: map[want] };
  if (raw && map[raw]) return { fwd: fwd, rec: map[raw] };
  const hit = Object.keys(map).find(function (k) {
    return (typeof canonNodeId === "function" ? canonNodeId(k) : k) === want;
  });
  return { fwd: fwd, rec: hit ? map[hit] : null };
}

function partialLoraMel(n) {
  const mel = n && n.lora_mel_rx;
  if (!mel || typeof mel !== "object") return null;
  const got = Number(mel.got || 0);
  const total = Number(mel.n || 0);
  if (!total || got >= total) return null;
  return { got: got, n: total };
}

/** Три состояния кабинета на карточке. READY и проба ключа сюда не входят. */
function cabinetStatus(n, s) {
  const found = forwardRecForNode(n, s);
  const fwd = found.fwd;
  const rec = found.rec;
  const err = rec && rec.last_error ? String(rec.last_error) : "";
  const okAt = rec && rec.last_ok_at;
  const mel = partialLoraMel(n);
  const melTxt = mel ? ("спектр LoRa " + mel.got + "/" + mel.n) : "";
  if (err) {
    return { text: "облако отказало", tip: err, color: "var(--bad)" };
  }
  if (mel && !okAt) {
    return {
      text: melTxt,
      tip: "Кадр ещё не собран и в кабинет не отправлен",
      color: "var(--warn)",
    };
  }
  if (okAt && mel) {
    return {
      text: "кабинет принял · " + melTxt,
      tip: "Последняя пересылка прошла. Новый спектр LoRa ещё собирается и в кабинет не ушёл",
      color: "var(--ok)",
    };
  }
  if (okAt) {
    return {
      text: "кабинет принял",
      tip: "Последняя пересылка этого узла в кабинет прошла",
      color: "var(--ok)",
    };
  }
  if (fwd && fwd.last_error === "cabinet_key_missing") {
    return {
      text: "ключа кабинета нет",
      tip: "LoRa без ключа в кабинет не уходит",
      color: "var(--bad)",
    };
  }
  return {
    text: "в кабинет ещё не уходило",
    tip: "Hub видит узел. Успешной пересылки этого узла ещё не было",
    color: "var(--muted)",
  };
}

function cabinetChip(n, s) {
  const st = cabinetStatus(n, s);
  return `<span title="${escapeHtml(st.tip)}" style="color:${st.color}">${escapeHtml(st.text)}</span>`;
}

function visibleNodes(s) {
  const showStale = $("showStale") && $("showStale").checked;
  const nodes = [...(s.nodes || [])];
  nodes.sort((a, b) => {
    if (!!a.online !== !!b.online) return a.online ? -1 : 1;
    return (a.age_s ?? 1e9) - (b.age_s ?? 1e9);
  });
  if (showStale) return nodes;
  return nodes.filter(n => n.online || (n.age_s != null && n.age_s <= 3600));
}

function renderNodes(s) {
  const list = $("nodeList");
  const nodes = visibleNodes(s);
  if (!nodes.length) {
    list.innerHTML = emptyOnboardHtml("Устройств пока нет");
    bindEmptyCa(list);
    return;
  }
  const online = nodes.find(n => n.online);
  if (!selected || !nodes.find(n => n.node_id === selected)) {
    selected = (online || nodes[0]).node_id;
  }
  list.innerHTML = "";
  nodes.forEach(n => {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "node-item" + (n.node_id === selected ? " active" : "");
    const linkSec = n.age_s != null ? fmtSec(n.age_s) : null;
    const fw = n.firmware_version ? String(n.firmware_version) : null;
    const hbVia = (n.last_heartbeat && n.last_heartbeat.via) ? viaLabel(n.last_heartbeat.via) : null;
    const detVia = (n.last_detection && n.last_detection.via) ? viaLabel(n.last_detection.via) : null;
    const lab = String(n.label || "").trim();
    const idLine = lab
      ? `<span class="nm">${escapeHtml(lab)}</span><span class="nid">${escapeHtml(n.node_id)}</span>`
      : `<span class="nid">${escapeHtml(n.node_id)}</span>`;
    const detBlob = Object.assign({}, n.last_detection || {}, n.joined_detection || {});
    const classOp =
      typeof classOperatorLabel === "function" ? classOperatorLabel(detBlob, n) : null;
    const alarmOk = typeof isConfirmedAlarm === "function" ? isConfirmedAlarm(n) : false;
    b.innerHTML = `
      <div class="id"><span class="dot ${n.online ? "on" : "off"}"></span>${idLine} ${transportChips(n.transports)}</div>
      <div class="meta">
        <span>${n.online ? "online" : "offline"} · <span title="Возраст последнего пакета на Hub. Сбрасывается при HB/DET — это НЕ uptime.">пакет ${linkSec != null ? escapeHtml(linkSec) + " с" : "—"}</span></span>
        <span style="color:${alarmOk ? threatColor(n.threat) : "var(--muted)"}">thr ${escapeHtml(fmt(n.threat))}</span>
        ${classOp ? `<span title="класс / ступень тревоги">${escapeHtml(classOp)}</span>` : ""}
        ${loiterChip(n)}
      </div>
      <div class="meta" style="margin-top:4px">
        ${fw ? `<span title="прошивка ESP">fw ${escapeHtml(fw)}</span>` : "<span>fw —</span>"}
        ${n.xvf_firmware_version ? `<span title="XVF Seeed">xvf ${escapeHtml(String(n.xvf_firmware_version))}</span>` : ""}
        ${n.nn_model_version ? `<span title="NN модель">nn ${escapeHtml(String(n.nn_model_version))}</span>` : ""}
        ${cabinetChip(n, s)}
        ${hbVia ? `<span title="последний пульс">HB ${escapeHtml(hbVia)}</span>` : ""}
        ${detVia ? `<span title="последняя детекция">DET ${escapeHtml(detVia)}</span>` : ""}
        ${(() => {
          const ae = n.active_episode;
          if (!ae || !ae.open) return "";
          const hops = ae.hop_count != null ? ae.hop_count : "?";
          const mels = ae.mel_count ? ` · ${ae.mel_count} mel` : "";
          const cls = ae.class_name ? ` ${escapeHtml(String(ae.class_name))}` : "";
          return `<span title="активный эпизод DET/Mel" class="chip">эпизод ${escapeHtml(String(hops))} hops${escapeHtml(String(mels))}${cls}</span>`;
        })()}
      </div>`;
    b.onclick = () => {
      selected = n.node_id;
      renderDetail(s);
      renderNodes(s);
      if (typeof renderHubMap === "function") renderHubMap(s);
    };
    list.appendChild(b);
  });
}

function renderChecklist(s) {
  const box = $("checklist");
  if (!box) return;
  box.innerHTML = "";
  (s.checklist || []).forEach(c => {
    const d = document.createElement("div");
    d.className = "check " + (c.ok ? "ok" : (c.count ? "bad" : ""));
    d.innerHTML = `<div class="n">${escapeHtml(c.label || c.id)}</div>
      <div class="h">${escapeHtml(c.hint || "")}</div>
      <div class="s">${c.ok ? "OK" : escapeHtml(c.last_error || "ожидание")} · ${escapeHtml(c.count || 0)}</div>`;
    box.appendChild(d);
  });
  const miss = $("checklistMiss");
  if (!miss) return;
  const coreIds = ["pb_detection", "pb_heartbeat", "pb_mel"];
  const missing = (s.checklist || [])
    .filter(c => coreIds.includes(c.id) && !c.ok)
    .map(c => c.label || c.id);
  if (!s.ready_core_pb && missing.length) {
    miss.style.display = "block";
    miss.textContent = "Core PB WAIT — не хватает: " + missing.join(", ");
  } else {
    miss.style.display = "none";
    miss.textContent = "";
  }
}

