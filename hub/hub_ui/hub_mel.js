function drawMel1d(canvas, means) {
  if (!canvas || !means || !means.length) return;
  const { ctx, w, h } = fitCanvas(canvas, 96);
  ctx.clearRect(0, 0, w, h);
  const min = Math.min(...means), max = Math.max(...means);
  const span = (max - min) || 1;
  const bw = w / means.length;
  means.forEach((v, i) => {
    const t = (v - min) / span;
    const barH = Math.max(2, t * (h - 6));
    ctx.fillStyle = `hsl(${160 - t * 90} 55% ${35 + t * 30}%)`;
    ctx.fillRect(i * bw, h - barH, Math.max(1, bw - 0.5), barH);
  });
}

/** WebUI-matching blue→cyan→yellow→red (full spectrum; not green→yellow). */
function melHeatRgb(t) {
  t = Math.max(0, Math.min(1, t));
  let r = 0, g = 0, b = 0;
  if (t < 0.25) {
    const u = t / 0.25;
    b = Math.floor(80 + u * 175);
  } else if (t < 0.5) {
    const u = (t - 0.25) / 0.25;
    g = Math.floor(u * 220);
    b = 255;
  } else if (t < 0.75) {
    const u = (t - 0.5) / 0.25;
    r = Math.floor(u * 255);
    g = Math.floor(220 - u * 40);
  } else {
    const u = (t - 0.75) / 0.25;
    r = 255;
    g = Math.floor(180 - u * 180);
  }
  return `rgb(${r},${g},${b})`;
}

/** Display-only scale (p5–p95). Does not change meta data_min/max / float recovery. */
function melVisualScale(grid) {
  const vals = [];
  for (const row of grid) for (const v of row) {
    if (Number.isFinite(v)) vals.push(v);
  }
  if (!vals.length) return { dmin: 0, dmax: 1 };
  vals.sort((a, b) => a - b);
  const at = (q) => vals[Math.max(0, Math.min(vals.length - 1, Math.floor(q * (vals.length - 1))))];
  let dmin = at(0.05), dmax = at(0.95);
  if (!(dmax > dmin)) { dmin = vals[0]; dmax = vals[vals.length - 1]; }
  if (!(dmax > dmin)) dmax = dmin + 1e-3;
  return { dmin, dmax };
}

/**
 * @param {number[][]} grid
 * @param {{dmin?: number, dmax?: number, mode?: string}|null} scale
 *   mode "meta" → transport data_min/max; default → visual p5–p95 (float recovery unchanged)
 */
function drawMel2d(canvas, grid, scale) {
  if (!canvas || !grid || !grid.length) return;
  const frames = grid.length, bands = grid[0].length;
  const { ctx, w, h } = fitCanvas(canvas, 220);
  ctx.clearRect(0, 0, w, h);
  let min, max;
  if (scale && scale.mode === "meta" && Number.isFinite(scale.dmin) && Number.isFinite(scale.dmax) && scale.dmax > scale.dmin) {
    min = scale.dmin; max = scale.dmax;
  } else {
    const vis = melVisualScale(grid);
    min = vis.dmin; max = vis.dmax;
  }
  const span = (max - min) || 1;
  const cw = w / frames, ch = h / bands;
  for (let f = 0; f < frames; f++) {
    for (let b = 0; b < bands; b++) {
      const t = (grid[f][b] - min) / span;
      ctx.fillStyle = melHeatRgb(t);
      // band 0 at bottom (low freq)
      ctx.fillRect(f * cw, h - (b + 1) * ch, Math.ceil(cw + 0.5), Math.ceil(ch + 0.5));
    }
  }
}

let _melPager = { files: [], idx: 0 };

function paintMelPager() {
  const n = _melPager.files.length;
  const i = _melPager.idx;
  const lab = $("melPagerIdx");
  if (lab) lab.textContent = n ? ((i + 1) + " / " + n) : "—";
  const solo = !n || n <= 1;
  if ($("melPrev")) $("melPrev").disabled = solo || i <= 0;
  if ($("melNext")) $("melNext").disabled = solo || i >= n - 1;
  const hint = $("melPagerHint");
  if (hint) {
    hint.textContent = solo
      ? "Одна спектрограмма в плейлисте. Откройте другую из ленты или «Mel на диске», или снимите фильтр узла."
      : "Листание: кнопки ниже или ← → на клавиатуре.";
  }
}

function melPagerStep(delta) {
  const modal = $("melModal");
  if (!modal || !modal.classList.contains("open")) return;
  const next = _melPager.idx + delta;
  if (next < 0 || next >= _melPager.files.length) return;
  openMelSpectrogram(_melPager.files[next]);
}

async function openMelSpectrogram(file) {
  if (!file) return;
  pendingMelFile = file;
  const pl = (typeof melPlaylistFor === "function") ? melPlaylistFor(file) : { files: [file], idx: 0 };
  _melPager = pl;
  paintMelPager();
  $("melModalTitle").textContent = "Спектрограмма Mel · " + file;
  $("melModal").classList.add("open");
  $("melModalMeta").innerHTML = cell("файл", file) + cell("статус", "загрузка…");
  try {
    const headers = {};
    const tok = sessionStorage.getItem(tokenKey);
    if (tok) headers.Authorization = "Bearer " + tok;
    const metaName = file.endsWith(".meta.json") ? file : (file + ".meta.json");
    const binName = file.endsWith(".meta.json") ? file.replace(/\.meta\.json$/, "") : file;
    const [metaR, binR] = await Promise.all([
      fetch("/api/mel/" + encodeURIComponent(metaName), { cache: "no-store", credentials: "same-origin", headers }),
      fetch("/api/mel/" + encodeURIComponent(binName), { cache: "no-store", credentials: "same-origin", headers }),
    ]);
    if (metaR.status === 401 || binR.status === 401) {
      sessionStorage.removeItem(tokenKey);
      location.href = "/";
      return;
    }
    if (!metaR.ok) throw new Error("meta " + metaR.status);
    if (!binR.ok) throw new Error("bin " + binR.status);
    const meta = await metaR.json();
    const buf = new Uint8Array(await binR.arrayBuffer());
    const bands = Number(meta.num_bands) || 64;
    const frames = Number(meta.num_frames) || Math.floor(buf.length / bands);
    let dmin = Number(meta.data_min), dmax = Number(meta.data_max);
    if (!Number.isFinite(dmin) || !Number.isFinite(dmax) || !(dmax > dmin)) {
      // LoRa 0x14 не несёт floor; firmware SPAN = 8 (MEL_QUANT_SPAN_NAT)
      dmin = 0;
      dmax = 8;
    }
    const need = bands * frames;
    if (buf.length < need) throw new Error("короткий payload");
    const span = dmax - dmin;
    const grid = [];
    for (let f = 0; f < frames; f++) {
      const row = [];
      const base = f * bands;
      for (let b = 0; b < bands; b++) row.push(dmin + (buf[base + b] / 255) * span);
      grid.push(row);
    }
    const vis = melVisualScale(grid);
    $("melModalMeta").innerHTML =
      cell("узел", fmt(meta.node_id)) +
      cell("класс", fmt(meta.class_name || "—")) +
      cell("размер", bands + "×" + frames) +
      cell("encoding", fmt(meta.encoding)) +
      cell("meta min/max", fmt(dmin) + " / " + fmt(dmax)) +
      cell("view p5/p95", fmt(vis.dmin) + " / " + fmt(vis.dmax)) +
      cell("байты", fmtBytes(buf.length)) +
      cell("ts", fmt(meta.timestamp_ms));
    paintMelGt(meta);
    requestAnimationFrame(() =>
      drawMel2d($("melModalCanvas"), grid, vis)
    );
    const wavName = binName.replace(/\.bin$/i, ".wav");
    const wavUrl = "/api/mel/" + encodeURIComponent(wavName);
    const audio = $("melAudio");
    if (audio) {
      audio.pause();
      audio.src = wavUrl;
      audio.load();
    }
    const dl = $("melWavDl");
    if (dl) {
      dl.href = wavUrl;
      dl.setAttribute("download", wavName);
    }
  } catch (e) {
    $("melModalMeta").innerHTML = cell("ошибка", String(e.message || e), "join-bad");
    const audio = $("melAudio");
    if (audio) { audio.removeAttribute("src"); audio.load(); }
    paintMelGt(null);
  }
}

const MEL_GT_RU = {
  background: "Фон",
  drone: "Дрон",
  ice_uav: "ДВС-БПЛА",
  jet_uav: "ТРД-БПЛА",
};

function paintMelGt(meta) {
  const board = $("melBoardClass");
  if (board) board.textContent = "класс: " + (meta && meta.class_name ? meta.class_name : "—");
  const gt = (meta && meta.gt_label) || "";
  document.querySelectorAll("#melGtBar [data-gt]").forEach((btn) => {
    btn.classList.toggle("on", btn.getAttribute("data-gt") === gt);
  });
  const yours = $("melGtYours");
  if (yours) {
    const ru = MEL_GT_RU[gt];
    if (gt) {
      yours.hidden = false;
      yours.textContent = "ваша метка: " + (ru || gt);
    } else {
      yours.hidden = true;
      yours.textContent = "";
    }
  }
}

async function postMelGt(value) {
  const file = pendingMelFile;
  if (!file) return;
  const binName = file.endsWith(".meta.json") ? file.replace(/\.meta\.json$/, "") : file;
  const headers = { "Content-Type": "application/json" };
  const tok = sessionStorage.getItem(tokenKey);
  if (tok) headers.Authorization = "Bearer " + tok;
  const r = await fetch("/api/mel/" + encodeURIComponent(binName) + "/gt", {
    method: "POST",
    credentials: "same-origin",
    cache: "no-store",
    headers,
    body: JSON.stringify({ gt_label: value }),
  });
  if (r.status === 401) {
    sessionStorage.removeItem(tokenKey);
    location.href = "/";
    return;
  }
  const j = await r.json().catch(() => ({}));
  if (!r.ok) {
    if (typeof flash === "function") flash(j.error || ("GT HTTP " + r.status), false);
    return;
  }
  const boardEl = $("melBoardClass");
  const boardName = boardEl && boardEl.textContent
    ? boardEl.textContent.replace(/^класс:\s*/, "")
    : "—";
  paintMelGt({
    class_name: boardName === "—" ? "" : boardName,
    gt_label: j.gt_label,
  });
  if (typeof lastSnap !== "undefined" && lastSnap && typeof paintMelDisk === "function") {
    paintMelDisk(lastSnap);
  }
}

(function bindMelGt() {
  const bar = $("melGtBar");
  if (!bar) return;
  bar.querySelectorAll("[data-gt]").forEach((btn) => {
    btn.onclick = () => postMelGt(btn.getAttribute("data-gt"));
  });
  if ($("melGtClear")) $("melGtClear").onclick = () => postMelGt(null);
  const corr = $("melCorrectionsDl");
  if (corr) {
    corr.onclick = async (ev) => {
      if (typeof downloadUrl !== "function") return;
      ev.preventDefault();
      try {
        await downloadUrl("/api/mel/corrections.zip", "MEL-corrections.zip");
        if (typeof flash === "function") flash("Коррекции скачаны", true);
      } catch (e) {
        if (typeof flash === "function") flash("Коррекции: " + (e && e.message ? e.message : e), false);
      }
    };
  }
})();

