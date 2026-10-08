/* Hub render — ingest timeline (Wave E) */
function drawIngestTimeline(canvas, tl) {
  if (!canvas || !tl) return;
  const { ctx, w, h } = fitCanvas(canvas, 140);
  ctx.clearRect(0, 0, w, h);
  const slots = tl.slots || 288;
  const colors = {
    hb: "#4ecdc4",
    det: "#d4a017",
    mel: "#9b6bff",
    mqtt_hb: "#7ec8e3",
    mqtt_det: "#e07a3d",
  };
  const padL = 4;
  const padR = 4;
  const plotW = Math.max(1, w - padL - padR);
  const slotW = plotW / slots;
  const y0 = 4;
  const gap = 2;
  const layers = [
    { key: "mel", data: tl.mel || [] },
    { key: "det", data: tl.det || [] },
    { key: "hb", data: tl.hb || [] },
    { key: "mqtt_det", data: tl.mqtt_det || [] },
    { key: "mqtt_hb", data: tl.mqtt_hb || [] },
  ];
  const bandH = Math.max(4, Math.floor((h - 8 - gap * (layers.length - 1)) / layers.length));
  layers.forEach((l, i) => { l.y = y0 + i * (bandH + gap); });
  ctx.fillStyle = "#1a2621";
  layers.forEach(l => ctx.fillRect(padL, l.y, plotW, bandH));
  layers.forEach(l => {
    ctx.fillStyle = colors[l.key];
    for (let i = 0; i < slots; i++) {
      const n = l.data[i] || 0;
      if (!n) continue;
      const x = padL + i * slotW;
      const barW = Math.max(1, slotW - 0.5);
      ctx.fillRect(x, l.y, barW, bandH);
    }
  });
}

function timelineFor(s) {
  const want = viewNode && viewNode !== "all" ? canonNodeId(viewNode) : "";
  if (!want) return s.ingest_timeline;
  const by = (s.ingest_timeline_by_node || {})[want];
  if (by) return by;
  const tl = s.ingest_timeline || {};
  const slots = tl.slots || 288;
  return {
    slots,
    bucket_s: tl.bucket_s,
    window_s: tl.window_s,
    hb: Array(slots).fill(0),
    det: Array(slots).fill(0),
    mel: Array(slots).fill(0),
    mqtt_hb: Array(slots).fill(0),
    mqtt_det: Array(slots).fill(0),
    node_id: want,
  };
}

function nodeIdsFromSnap(s) {
  const ids = new Set();
  (s.nodes || []).forEach((n) => {
    const id = canonNodeId(n.node_id);
    if (id) ids.add(id);
  });
  Object.keys(s.ingest_timeline_by_node || {}).forEach((id) => {
    const c = canonNodeId(id);
    if (c) ids.add(c);
  });
  (s.recent || []).forEach((e) => {
    const id = canonNodeId(e.node_id);
    if (id) ids.add(id);
  });
  return [...ids].sort();
}

function fillViewNodeSelects(s) {
  const ids = nodeIdsFromSnap(s);
  const cur = canonNodeId(viewNode) || "all";
  const keep = cur === "all" || ids.includes(cur) ? (cur === "" ? "all" : cur) : "all";
  viewNode = keep;
  const html = ['<option value="all">все устройства</option>']
    .concat(ids.map((n) => {
      const row = (s.nodes || []).find((x) => canonNodeId(x.node_id) === n);
      const cap = nodeOptionCaption(n, row && row.label);
      return `<option value="${escapeHtml(n)}">${escapeHtml(cap)}</option>`;
    }))
    .join("");
  const sig = "all|" + ids.map((n) => {
    const row = (s.nodes || []).find((x) => canonNodeId(x.node_id) === n);
    return n + "=" + String((row && row.label) || "");
  }).join("|");
  ["tlNode", "feedNode"].forEach((id) => {
    const el = $(id);
    if (!el) return;
    if (el.dataset.ids !== sig) {
      el.dataset.ids = sig;
      el.innerHTML = html;
    }
    if (el.value !== keep) el.value = keep;
  });
}

function bindViewNode() {
  ["tlNode", "feedNode"].forEach((id) => {
    const el = $(id);
    if (!el || el.dataset.bound) return;
    el.dataset.bound = "1";
    el.onchange = () => {
      viewNode = el.value || "all";
      if (!lastSnap) return;
      fillViewNodeSelects(lastSnap);
      renderTimeline(lastSnap);
      renderFeed(lastSnap);
    };
  });
}

function renderTimeline(s) {
  bindViewNode();
  fillViewNodeSelects(s);
  drawIngestTimeline($("cTimeline"), timelineFor(s));
}

