/* Hub render — event feed (Wave E) */
function bindFeedFilters() {
  const box = $("feedFilters");
  if (!box || box.dataset.bound) return;
  box.dataset.bound = "1";
  box.querySelectorAll("button[data-ft]").forEach(btn => {
    btn.onclick = () => {
      feedTransport = btn.getAttribute("data-ft") || "all";
      box.querySelectorAll("button[data-ft]").forEach(b => b.classList.toggle("on", b === btn));
      if (lastSnap) renderFeed(lastSnap);
    };
  });
  box.querySelectorAll("button[data-fk]").forEach(btn => {
    btn.onclick = () => {
      feedKind = btn.getAttribute("data-fk") || "all";
      box.querySelectorAll("button[data-fk]").forEach(b => b.classList.toggle("on", b === btn));
      if (lastSnap) renderFeed(lastSnap);
    };
  });
}

function visibleFeedEvents(s) {
  const want = viewNode && viewNode !== "all" ? canonNodeId(viewNode) : "";
  let rows = ((s && s.recent) || []).slice(0, 80);
  if (want) {
    rows = rows.filter(e => canonNodeId(e.node_id) === want);
  }
  if (feedTransport !== "all") {
    rows = rows.filter(e => (e.transport || "") === feedTransport);
  }
  if (feedKind !== "all") {
    rows = rows.filter(e => (e.kind || "") === feedKind);
  }
  return rows;
}

function melNodeNeedle() {
  return (($("melNode") && $("melNode").value) || "").trim().toUpperCase().replace(/-/g, "");
}

function melSortMode() {
  return (($("melSort") && $("melSort").value) || "date");
}

function filterSortMelFiles(files) {
  const src = Array.isArray(files) ? files.slice() : [];
  const needle = melNodeNeedle();
  const viewWant = (typeof viewNode !== "undefined" && viewNode && viewNode !== "all")
    ? (typeof canonNodeId === "function" ? canonNodeId(viewNode) : String(viewNode))
    : "";
  let list = src.filter((f) => {
    const nid = String((f.meta || {}).node_id || f.file || "").toUpperCase().replace(/-/g, "");
    if (needle && nid.indexOf(needle) < 0) return false;
    if (viewWant && typeof canonNodeId === "function") {
      const cn = canonNodeId((f.meta && f.meta.node_id) || "");
      if (cn && cn !== viewWant && nid.indexOf(String(viewWant).toUpperCase()) < 0) return false;
    }
    return true;
  });
  if (melSortMode() === "node") {
    list.sort((a, b) => {
      const na = String((a.meta || {}).node_id || "").toUpperCase();
      const nb = String((b.meta || {}).node_id || "").toUpperCase();
      if (na !== nb) return na < nb ? -1 : 1;
      const ta = String(a.received_at || a.mtime || "");
      const tb = String(b.received_at || b.mtime || "");
      return ta < tb ? 1 : ta > tb ? -1 : 0;
    });
  } else {
    list.sort((a, b) => {
      const ta = String(a.received_at || a.mtime || "");
      const tb = String(b.received_at || b.mtime || "");
      return ta < tb ? 1 : ta > tb ? -1 : 0;
    });
  }
  return list;
}

function melPlaylistFor(file) {
  const want = viewNode && viewNode !== "all" ? canonNodeId(viewNode) : "";
  const dated = typeof lastMelDisk !== "undefined" && lastMelDisk;
  const rawRows = dated ? lastMelDisk : ((lastSnap && lastSnap.mel_saved) || []);
  const savedRows = filterSortMelFiles(rawRows);
  let fromDisk = savedRows.map((row) => String(row.file || "")).filter(Boolean);
  if (want) {
    const filtered = savedRows
      .filter((row) => canonNodeId((row.meta && row.meta.node_id) || "") === want)
      .map((row) => String(row.file || ""))
      .filter(Boolean);
    if (filtered.length) fromDisk = filtered;
  }
  if (dated) {
    const diskIdx = fromDisk.indexOf(file);
    if (diskIdx >= 0 && fromDisk.length > 0) {
      return { files: fromDisk, idx: diskIdx };
    }
    return { files: file ? [file] : [], idx: 0 };
  }

  const fromFeed = [];
  const seen = new Set();
  for (const e of visibleFeedEvents(lastSnap || {})) {
    const meta = e.meta || {};
    const files = [];
    if (meta.saved_file) files.push(String(meta.saved_file));
    if (Array.isArray(meta.mel_files)) {
      meta.mel_files.forEach((f) => { if (f) files.push(String(f)); });
    }
    const kd = e.kind || "";
    const isMelish = kd === "mel" || (e.key || "").includes("mel") || (e.key || "") === "episode";
    if (!isMelish) continue;
    for (const f of files) {
      if (!f || seen.has(f)) continue;
      seen.add(f);
      fromFeed.push(f);
    }
  }
  const feedIdx = fromFeed.indexOf(file);
  if (feedIdx >= 0 && fromFeed.length > 1) {
    return { files: fromFeed, idx: feedIdx };
  }
  const diskIdx = fromDisk.indexOf(file);
  if (diskIdx >= 0 && fromDisk.length > 0) {
    return { files: fromDisk, idx: diskIdx };
  }
  if (feedIdx >= 0) return { files: fromFeed, idx: feedIdx };
  return { files: file ? [file] : [], idx: 0 };
}

function renderFeed(s) {
  bindFeedFilters();
  bindViewNode();
  fillViewNodeSelects(s);
  const box = $("feed");
  if (!box) return;
  const rows = visibleFeedEvents(s);
  if (!rows.length) {
    const hasAny = (s.recent || []).length > 0;
    box.innerHTML = hasAny
      ? "<div class='empty'>Нет событий по фильтру — выберите «все».</div>"
      : "<div class='empty'>Событий пока нет. Когда узел выйдет на связь, лента оживет.</div>";
    return;
  }
  box.innerHTML = rows.map(e => {
    const tr = e.transport || "";
    const kd = e.kind || "";
    const trChip = tr === "mqtt"
      ? '<span class="chip mqtt">MQTT</span>'
      : (tr === "pb" ? '<span class="chip pb">PB</span>' : '<span class="chip">?</span>');
    const kdChip = (e.key === "episode")
      ? '<span class="chip kind">ЭПИЗОД</span>'
      : (kd
        ? `<span class="chip kind">${escapeHtml(kd.toUpperCase())}</span>`
        : '<span class="chip kind">RAW</span>');
    const civChip = (e.meta && e.meta.civilian_in_radius)
      ? '<span class="chip adsb">ADS-B</span>' : '';
    const nid = e.node_id ? `<span class="nid">${escapeHtml(e.node_id)}</span>` : "";
    const sum = escapeHtml(e.summary || e.error || e.key || "");
    const melFile = e.meta && (e.meta.saved_file || e.meta.last_mel_file)
      ? String(e.meta.saved_file || e.meta.last_mel_file) : "";
    const mel = melFile && (kd === "mel" || (e.key || "").includes("mel") || e.key === "episode")
      ? ` <a href="#" data-mel="${escapeHtml(melFile)}">spectrogram</a>` : "";
    const mark = e.ok === false ? '<span class="err">✗</span> ' : "";
    return `<div class="event-row">
      <div class="t">${escapeHtml(eventTimeShort(e.ts))}</div>
      <div class="chip-row">${trChip}${kdChip}${civChip}</div>
      <div class="sum">${mark}${nid}${sum}${mel}</div>
    </div>`;
  }).join("");
  box.querySelectorAll("[data-mel]").forEach(a => {
    a.onclick = (ev) => { ev.preventDefault(); openMelSpectrogram(a.getAttribute("data-mel")); };
  });
}

