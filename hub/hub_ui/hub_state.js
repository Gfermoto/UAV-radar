/* Hub UI shared mutable state (Wave E) */
const $ = (id) => document.getElementById(id);
let selected = null;
let feedTransport = "all";
let feedKind = "all";
/** "" / "all" = все узлы; иначе канонический node_id (как PB). */
let viewNode = "all";
let lastSnap = null;
let lastToastKey = "";
let pendingMelFile = null;
/** null = snapshot; array = GET /api/mel with date filter. */
let lastMelDisk = null;
let lastTickOkAt = 0;
let tickFailStreak = 0;
let authWaiters = [];
const tokenKey = "nevod_mock_token";
// Engineer unlock — только RAM; F5 сбрасывает.
let hubUnlockTok = "";
let hubEngRevealed = false;
try { sessionStorage.removeItem("nevod_hub_unlock"); } catch (_) {}
