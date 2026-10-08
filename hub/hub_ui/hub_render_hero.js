/* Hub render — hero / toast (Wave E) */
function showToast(t) {
  if (!t) return;
  const key = (t.ts || "") + "|" + (t.saved_file || "") + "|" + (t.node_id || "");
  if (key === lastToastKey) return;
  lastToastKey = key;
  pendingMelFile = t.saved_file || null;
  $("toastBody").textContent =
    `${t.node_id || "?"} · ${fmtBytes(t.payload_bytes)} · ${t.num_bands || "?"}×${t.num_frames || "?"} · ${t.via || ""}`;
  $("toast").classList.add("show");
  clearTimeout(showToast._tm);
  showToast._tm = setTimeout(() => $("toast").classList.remove("show"), 12000);
}

function renderHero(s) {
  $("sNodes").textContent = `${s.node_count || 0} / ${s.online_count || 0}`;
  const ids = s.online_nodes || [];
  const nodesEl = $("sNodes");
  if (nodesEl) {
    nodesEl.title = ids.length ? ("online: " + ids.join(", ")) : "нет online";
  }
  const readyTxt = s.ready_core_pb ? "READY" : "WAIT";
  const readyTitle = s.ready_core_pb
    ? "DET + Heartbeat + Mel (PB) приняты"
    : "Ждём полный набор PB: DET, Heartbeat и Mel";
  ["sReady", "sReadyLab"].forEach(id => {
    const el = $(id);
    if (!el) return;
    el.textContent = readyTxt;
    el.title = readyTitle;
    const base = id === "sReadyLab" ? "x" : "v";
    el.className = base + " " + (s.ready_core_pb ? "ok" : "warn");
  });
  if ($("sBytes")) $("sBytes").textContent = fmtBytes(s.total_bytes || 0);
  const mq = $("sMqtt");
  if (mq) {
    const st = s.mqtt_status || "off";
    mq.textContent = st.startsWith("connected") ? "ON" : (st === "off" ? "off" : st.split(":")[0]);
    mq.className = "v " + (st.startsWith("connected") ? "ok" : (st === "off" ? "warn" : "bad"));
  }
  const toast = s.last_mel_toast;
  const melEl = $("sMelAge");
  if (melEl) {
    if (!toast) {
      melEl.textContent = "нет";
      melEl.className = "x warn";
      melEl.title = "";
    } else {
      const age = fmtAge(toast.ts);
      melEl.textContent = age ? age + " назад" : (toast.node_id || "есть");
      melEl.className = "x ok";
      melEl.title = (toast.node_id || "") + (toast.ts ? " · " + toast.ts : "");
    }
  }
  if (toast) showToast(toast);
  const redisEl = $("sRedis");
  if (redisEl) {
    const r = (s.hub && s.hub.redis) || {};
    const base = redisEl.classList.contains("x") ? "x" : "v";
    if (!r.configured) {
      redisEl.textContent = "off";
      redisEl.className = base + " warn";
      redisEl.title = "HUB_REDIS_URL не задан — сессии только в RAM";
    } else if (r.ok) {
      redisEl.textContent = "OK";
      redisEl.className = base + " ok";
      redisEl.title = (r.host || "redis") + " · db " + (r.db != null ? r.db : "?");
    } else {
      redisEl.textContent = "FAIL";
      redisEl.className = base + " bad";
      redisEl.title = r.error || "Redis недоступен";
    }
  }
}

