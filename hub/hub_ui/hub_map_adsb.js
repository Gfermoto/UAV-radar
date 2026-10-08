/* Hub map — ADS-B icons / HUD (Wave D) */
function _altLabel(alt) {
  const a = _num(alt);
  if (a == null) return "";
  // dump1090 / adsb.lol: alt_baro обычно в футах
  if (Math.abs(a) >= 500) return "FL" + Math.round(a / 100);
  return Math.round(a) + " ft";
}

function _distLabel(distM) {
  const d = _num(distM);
  if (d == null) return "";
  if (d >= 1000) return (Math.round(d / 100) / 10) + " км";
  return Math.round(d) + " м";
}

function _acIcon(ac) {
  const hex = String(ac.hex || "?").toUpperCase();
  const flight = String(ac.flight || "").trim();
  const call = flight || hex;
  const inR = !!ac.in_radius;
  const track = _num(ac.track != null ? ac.track : ac.true_heading);
  const rot = track != null ? Math.round(track) : 0;
  const altL = _altLabel(ac.alt_baro);
  // Top-down «самолётик»; 0° = нос вверх = север (как track/heading).
  const svg =
    '<svg class="plane-svg" viewBox="0 0 24 24" aria-hidden="true">' +
    '<path d="M12 2.2 L13.2 9.2 L20.5 14.2 L19.6 15.6 L13.2 13.2 L13.2 18.6 L15.8 20.4 L15.8 21.6 L12 20.2 L8.2 21.6 L8.2 20.4 L10.8 18.6 L10.8 13.2 L4.4 15.6 L3.5 14.2 L10.8 9.2 Z"/>' +
    "</svg>";
  return L.divIcon({
    className: "hub-map-ac" + (inR ? " in" : ""),
    html:
      '<span class="plane" style="transform:rotate(' + rot + 'deg)">' + svg + "</span>" +
      '<span class="lbl">' +
        '<span class="id">' + escapeHtml(call) + "</span>" +
        (altL ? '<span class="alt">' + escapeHtml(altL) + "</span>" : "") +
      "</span>",
    iconSize: [112, 28],
    iconAnchor: [9, 9],
  });
}

function _acPopupHtml(ac) {
  const hex = String(ac.hex || "?").toUpperCase();
  const flight = String(ac.flight || "").trim();
  const bits = [];
  bits.push("<div class='nid'>" + escapeHtml(flight || hex) + "</div>");
  if (flight) bits.push("<div class='coords'>" + escapeHtml(hex) + "</div>");
  const altL = _altLabel(ac.alt_baro);
  const gs = _num(ac.gs);
  const track = _num(ac.track != null ? ac.track : ac.true_heading);
  const row1 = [];
  if (altL) row1.push(altL);
  if (gs != null) row1.push(Math.round(gs) + " kt");
  if (track != null) row1.push(Math.round(track) + "°");
  if (row1.length) bits.push(_popRow("полёт", row1.join(" · ")));
  const distL = _distLabel(ac.dist_m);
  if (distL) {
    bits.push(
      _popRow(
        "до зоны",
        distL + (ac.in_radius ? " · в круге глушения" : "")
      )
    );
  } else if (ac.in_radius) {
    bits.push(_popRow("зона", "в круге глушения"));
  }
  const src = String(ac.source || "").trim();
  if (src) bits.push(_popRow("источник", src));
  return "<div class='hub-map-pop'>" + bits.join("") + "</div>";
}

function _paintAdsbHud(s) {
  const el = $("hubMapAdsb");
  if (!el) return;
  const a = (((s || {}).hub || {}).adsb) || {};
  const acs = a.aircraft || [];
  const fb = a.fallback || {};
  const meta = a.meta || {};
  let src = "выкл";
  if (fb.active) src = "adsb.lol";
  else if (a.signal === "live" || a.fresh) src = "local";
  else if (a.enabled) src = "ожидание";
  const n = acs.length;
  const inR =
    meta.adsb_count_in_radius != null
      ? meta.adsb_count_in_radius
      : acs.filter((x) => x && x.in_radius).length;
  const r = _num(a.radius_m);
  const rL = r != null ? Math.round(r) + " м" : "—";
  const mute =
    a.filter_det || a.filter_hb || a.filter_mel
      ? "глуш. " +
        [a.filter_det ? "DET" : "", a.filter_hb ? "HB" : "", a.filter_mel ? "Mel" : ""]
          .filter(Boolean)
          .join("/")
      : "без глуш.";
  el.style.display = "block";
  el.textContent =
    "ADS-B · " + src + " · " + n + " · в круге " + inR + " · R " + rL + " · " + mute;
}

