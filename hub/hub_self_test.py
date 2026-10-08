#!/usr/bin/env python3
"""Characterization self-test for Nevod Hub (CLI: nevod_hub.py --self-test)."""
from __future__ import annotations


def run_self_test(port: int = 0) -> int:
    """Run full Hub characterization suite; return process exit code.

    Body executes with nevod_hub.__dict__ as globals so Handler/STATE mutations
    land on the composition-root module (same as pre-extract monolith).
    """
    import types

    import nevod_hub as hub

    _assert_track_pair()

    hub._post = _post  # type: ignore[attr-defined]
    hub._post_h = _post_h  # type: ignore[attr-defined]
    fn = types.FunctionType(
        _run_self_test_body.__code__,
        hub.__dict__,
        name="run_self_test",
        argdefs=_run_self_test_body.__defaults__,
        closure=_run_self_test_body.__closure__,
    )
    return fn(port)


def _assert_track_pair() -> None:
    """Два пеленга → один трек. Без сокета, до подъёма Hub."""
    from hub_tracks import TrackEngine

    eng = TrackEngine()
    eng.ingest(
        {
            "node_id": "A",
            "ts_ms": 1_000_000,
            "lat": 55.93391,
            "lon": 36.60942,
            "azimuth_deg": 33.69,
            "class_name": "drone",
            "p": 0.8,
            "threat": 0.9,
            "via": "mqtt_detection",
            "doa_confidence": 0.7,
        },
        now_ms=1_000_000,
    )
    eng.ingest(
        {
            "node_id": "B",
            "ts_ms": 1_001_000,
            "lat": 55.93391,
            "lon": 36.612634,
            "azimuth_deg": 326.31,
            "class_name": "drone",
            "p": 0.8,
            "threat": 0.9,
            "via": "mqtt_detection",
            "doa_confidence": 0.7,
        },
        now_ms=1_001_000,
    )
    tracks = eng.snapshot(1_001_000)
    if len(tracks) != 1 or tracks[0]["estimate"]["method"] != "bearing_intersection":
        raise SystemExit("track self-test: expected one bearing_intersection")


def _post(url: str, data: bytes, headers: dict, ctx) -> tuple:
    import json
    from urllib.request import Request, urlopen

    req = Request(url, data=data, headers=headers, method="POST")
    with urlopen(req, context=ctx, timeout=5) as resp:  # noqa: S310 lab tool
        body = resp.read()
        return resp.status, json.loads(body.decode("utf-8") or "{}")


def _post_h(url: str, data: bytes, headers: dict, ctx) -> tuple:
    import json
    from urllib.request import Request, urlopen

    req = Request(url, data=data, headers=headers, method="POST")
    with urlopen(req, context=ctx, timeout=5) as resp:  # noqa: S310 lab tool
        body = resp.read()
        return (
            resp.status,
            json.loads(body.decode("utf-8") or "{}"),
            resp.headers.get("Set-Cookie") or "",
        )


def _run_self_test_body(port: int = 0) -> int:
    token = "lab-self-test-token-32chars!!"
    # Lab unlock path used by unlock self-test (mirrors IoT hardcoded PW).
    os.environ["HUB_LAB_DEFAULT_UNLOCK"] = "1"
    os.environ["HUB_DASHBOARD_OPEN"] = "1"
    os.environ["HUB_FORWARD_ALLOW_PRIVATE"] = "1"
    os.environ["HUB_FORWARD_ALLOW_LOOPBACK"] = "1"
    DASHBOARD_OPEN = True  # lab self-test: open viewer APIs (prod default is closed)
    global STATE, MEL_DIR, MEL_SAVE
    # MEL_* aliases track hub_mel_io module globals
    mel_tmp = Path(tempfile.mkdtemp(prefix="nevod_mel_test_"))
    prev_dir, prev_save = MEL_DIR, MEL_SAVE
    hub_mel_io.MEL_DIR = mel_tmp
    hub_mel_io.MEL_SAVE = True
    MEL_DIR = hub_mel_io.MEL_DIR
    MEL_SAVE = hub_mel_io.MEL_SAVE
    global STORE, WEBHOOKS, FORWARDER, ADSB, ZONES, DEVICE_TOKENS, HUB_UNLOCK_SESSIONS, HUB_SETTINGS_SESSIONS
    store_tmp = Path(tempfile.mkdtemp(prefix="nevod_hub_store_"))
    STORE = HubStore(store_tmp / "hub.sqlite")
    hub_auth.bind_store(STORE)
    WEBHOOKS = WebhookHub(STORE)
    ZONES = ZoneRegistry(STORE)
    FORWARDER = HubForwarder(STORE)
    FORWARDER.base = ""  # self-test не ставит пакеты в очередь на заводской URL
    FORWARDER.zones = ZONES
    ADSB = AdsbService(STORE, mqtt_creds=lambda: {}, zones=ZONES)
    FORWARDER.adsb = ADSB
    DEVICE_TOKENS = {}
    HUB_UNLOCK_SESSIONS.clear()
    HUB_SETTINGS_SESSIONS.clear()
    HUB_UI_SESSIONS.clear()
    STATE = State(token)
    httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    # HubContext on the server so Handler.ctx prefers it over re-capturing globals.
    from hub_context import HubContext as _HubContext
    from hub_runtime import bind_services as _bind_services

    _bind_services(
        STATE=STATE,
        STORE=STORE,
        WEBHOOKS=WEBHOOKS,
        FORWARDER=FORWARDER,
        ADSB=ADSB,
        ZONES=ZONES,
        DEVICE_TOKENS=DEVICE_TOKENS,
    )
    httpd.ctx = _HubContext.from_module(sys.modules.get("nevod_hub") or sys.modules[__name__])  # type: ignore[attr-defined]
    port = httpd.server_address[1]
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    base = f"http://127.0.0.1:{port}"
    auth = {"Authorization": f"Bearer {token}"}
    ts = int(time.time() * 1000)
    errors: list[str] = []

    def check(name: str, fn) -> None:
        try:
            fn()
            print(f"  OK  {name}")
        except Exception as e:  # noqa: BLE001
            errors.append(f"{name}: {e}")
            print(f"  FAIL {name}: {e}")

    def t_pb_det() -> None:
        raw = pb_encode(
            **{
                "1:str": "nevod-AABBCC",
                "2:u64": ts,
                "3:str": "UAV",
                "4:i32": 1,
                "5:f32": 0.9,
                "6:f32": 0.8,
                "10:f32": 42.0,
            }
        )
        code, ack = _post(
            base + "/api/v1/pb/ReportDetection",
            raw,
            {**auth, "Content-Type": "application/x-protobuf"},
            None,
        )
        assert code == 200 and ack.get("accepted") is True

    def t_pb_det_rpm() -> None:
        raw = pb_encode(
            **{
                "1:str": "nevod-RPM01",
                "2:u64": ts,
                "3:str": "ice_uav",
                "4:i32": 2,
                "5:f32": 0.88,
                "6:f32": 0.85,
                "9:f32": 240.0,
                "24:f32": 2400.0,
                "25:i32": 2,
                "26:bool": True,
            }
        )
        meta = validate_detection_pb(raw)
        assert meta.get("rpm_valid") is True, meta
        assert "rotor_hz" not in meta, meta
        assert abs(float(meta.get("rpm")) - 2400.0) < 1e-2, meta
        assert int(meta.get("blade_count")) == 2, meta
        assert abs(float(meta.get("bpf_hz")) - 240.0) < 1e-2, meta
        code, ack = _post(
            base + "/api/v1/pb/ReportDetection",
            raw,
            {**auth, "Content-Type": "application/x-protobuf"},
            None,
        )
        assert code == 200 and ack.get("accepted") is True
        node = next(n for n in STATE.snapshot()["nodes"] if n["node_id"] == "RPM01")
        det = node.get("last_detection") or {}
        assert det.get("rpm_valid") is True, det
        assert int(det.get("blade_count")) == 2, det
        assert abs(float(det.get("rpm")) - 2400.0) < 1e-2, det

    def t_pb_hb() -> None:
        raw = pb_encode(
            **{
                "1:str": "nevod-AABBCC",
                "2:u64": ts,
                "3:str": "ok",
                "5:f32": -48.0,
                "6:i32": 12,
                "10:f64": 50.45,
                "11:f64": 30.52,
                "12:f32": -32.0,
            }
        )
        code, ack = _post(
            base + "/api/v1/pb/ReportHeartbeat",
            raw,
            {**auth, "Content-Type": "application/x-protobuf"},
            None,
        )
        assert code == 200 and ack.get("accepted") is True


    def t_join_det_hb() -> None:
        snap = STATE.snapshot()
        node = next(n for n in snap["nodes"] if n["node_id"] == "AABBCC")
        jd = node.get("joined_detection") or node.get("last_detection") or {}
        assert jd.get("joined") is True, jd
        assert abs(float(jd.get("spl_fast")) - (-32.0)) < 1e-5, jd
        assert abs(float(jd.get("noise_dbfs")) - (-48.0)) < 1e-5, jd
        assert abs(float(jd.get("lat") or 0) - 50.45) < 1e-6, jd
        assert abs(float(node.get("threat")) - 0.8) < 1e-5, node
        # HB must not carry threat
        assert (node.get("last_heartbeat") or {}).get("threat") is None

    def t_second_node() -> None:
        raw = pb_encode(
            **{
                "1:str": "nevod-DDEEFF",
                "2:u64": ts,
                "3:str": "ok",
                "5:f32": -50.0,
                "6:i32": 99,
                "12:f32": -36.0,
            }
        )
        code, ack = _post(
            base + "/api/v1/pb/ReportHeartbeat",
            raw,
            {**auth, "Content-Type": "application/x-protobuf"},
            None,
        )
        assert code == 200 and ack.get("accepted") is True

    def _mel_raw(node_id: str, stamp: int) -> bytes:
        bands, frames = 64, 401
        data = bytes((i % 256) for i in range(bands * frames))
        return pb_encode(
            **{
                "1:str": node_id[:16],
                "2:u64": stamp,
                "3:u32": bands,
                "4:u32": frames,
                "5:bytes": data,
                "6:f32": -80.0,
                "7:f32": 0.0,
            }
        )

    def t_pb_mel() -> None:
        code, ack = _post(
            base + "/api/v1/pb/UploadMel",
            _mel_raw("nevod-AABBCC", ts),
            {**auth, "Content-Type": "application/x-protobuf"},
            None,
        )
        assert code == 200 and ack.get("accepted") is True
        assert ack.get("saved_file"), ack
        assert (MEL_DIR / ack["saved_file"]).is_file()

    def t_pb_mel_gzip() -> None:
        import gzip

        bands, frames = 64, 401
        raw_u8 = bytes((i % 256) for i in range(bands * frames))
        gz = gzip.compress(raw_u8, compresslevel=6)
        assert len(gz) < len(raw_u8)
        body = pb_encode(
            **{
                "1:str": "nevod-AABBCC",
                "2:u64": ts + 50,
                "3:u32": bands,
                "4:u32": frames,
                "5:bytes": gz,
                "6:f32": -80.0,
                "7:f32": 0.0,
                "8:u32": 1,
            }
        )
        code, ack = _post(
            base + "/api/v1/pb/UploadMel",
            body,
            {**auth, "Content-Type": "application/x-protobuf"},
            None,
        )
        assert code == 200 and ack.get("accepted") is True
        saved = ack.get("saved_file")
        assert saved, ack
        path = MEL_DIR / saved
        assert path.is_file()
        assert path.stat().st_size == bands * frames
        meta = json.loads((MEL_DIR / f"{saved}.meta.json").read_text())
        assert meta.get("num_frames") == frames
        assert meta.get("data_encoding") == 1 or meta.get("encoding") == "uint8_minmax_gzip"

    def t_pb_mel_gzip_delta() -> None:
        import gzip

        bands, frames = 64, 401
        # Smooth spectrogram-like data → delta compresses well.
        raw = bytearray(bands * frames)
        for f in range(frames):
            for b in range(bands):
                raw[f * bands + b] = (b * 3 + f) & 0xFF
        delta = bytearray(raw)
        for i in range(bands, len(delta)):
            delta[i] = (raw[i] - raw[i - bands]) & 0xFF
        gz = gzip.compress(bytes(delta), compresslevel=9)
        body = pb_encode(
            **{
                "1:str": "nevod-AABBCC",
                "2:u64": ts + 60,
                "3:u32": bands,
                "4:u32": frames,
                "5:bytes": gz,
                "6:f32": -80.0,
                "7:f32": 0.0,
                "8:u32": 2,
            }
        )
        code, ack = _post(
            base + "/api/v1/pb/UploadMel",
            body,
            {**auth, "Content-Type": "application/x-protobuf"},
            None,
        )
        assert code == 200 and ack.get("accepted") is True
        saved = ack.get("saved_file")
        assert saved
        got = (MEL_DIR / saved).read_bytes()
        assert got == bytes(raw)
        meta = json.loads((MEL_DIR / f"{saved}.meta.json").read_text())
        assert meta.get("data_encoding") == 2

    def t_pb_mel_gzip_delta_u4_rejected() -> None:
        """enc=3 is lossy on real Mel — mock must reject, not save garbage."""
        import gzip

        bands, frames = 64, 401
        raw = bytearray(bands * frames)
        for f in range(frames):
            for b in range(bands):
                raw[f * bands + b] = (80 + b + f) & 0xFF
        delta = bytearray(raw)
        for i in range(bands, len(delta)):
            delta[i] = (raw[i] - raw[i - bands]) & 0xFF
        packed = bytearray((bands * frames + 1) // 2)
        for i, v in enumerate(delta):
            n = (v >> 4) & 0x0F
            if (i & 1) == 0:
                packed[i // 2] = n << 4
            else:
                packed[i // 2] |= n
        gz = gzip.compress(bytes(packed), compresslevel=9)
        body = pb_encode(
            **{
                "1:str": "nevod-AABBCC",
                "2:u64": ts + 90,
                "3:u32": bands,
                "4:u32": frames,
                "5:bytes": gz,
                "6:f32": -80.0,
                "7:f32": 0.0,
                "8:u32": 3,
            }
        )
        try:
            _post(
                base + "/api/v1/pb/UploadMel",
                body,
                {**auth, "Content-Type": "application/x-protobuf"},
                None,
            )
            raise AssertionError("expected HTTPError 400 for enc=3")
        except HTTPError as e:
            assert e.code == 400
            err_body = e.read().decode("utf-8", errors="replace")
            assert "mel_enc3_lossy_disabled" in err_body, err_body

    def t_pb_mel_gzip_delta_realistic() -> None:
        """Full-range uint8 Mel + enc=2 must roundtrip lossless (viewer OK)."""
        import gzip

        bands, frames = 64, 401
        raw = bytearray(bands * frames)
        state = [120.0] * bands
        for f in range(frames):
            for b in range(bands):
                state[b] = max(0.0, min(255.0, state[b] * 0.9 + ((b + f) % 17)))
                raw[f * bands + b] = int(state[b])
        delta = bytearray(raw)
        for i in range(bands, len(delta)):
            delta[i] = (raw[i] - raw[i - bands]) & 0xFF
        gz = gzip.compress(bytes(delta), compresslevel=9)
        body = pb_encode(
            **{
                "1:str": "nevod-AABBCC",
                "2:u64": ts + 91,
                "3:u32": bands,
                "4:u32": frames,
                "5:bytes": gz,
                "6:f32": -80.0,
                "7:f32": 0.0,
                "8:u32": 2,
            }
        )
        code, ack = _post(
            base + "/api/v1/pb/UploadMel",
            body,
            {**auth, "Content-Type": "application/x-protobuf"},
            None,
        )
        assert code == 200 and ack.get("accepted") is True
        saved = ack.get("saved_file")
        assert saved
        got = (MEL_DIR / saved).read_bytes()
        assert got == bytes(raw)
        meta = json.loads((MEL_DIR / f"{saved}.meta.json").read_text())
        assert meta.get("data_encoding") == 2

    def t_mel_skip_emu() -> None:
        before = len(list(MEL_DIR.glob("mel_*.bin")))
        code, ack = _post(
            base + "/api/v1/pb/UploadMel",
            _mel_raw("nevod-AABBCC", ts + 1),
            {
                **auth,
                "Content-Type": "application/x-protobuf",
                "X-Nevod-Source": "demo-feeder",
            },
            None,
        )
        assert code == 200 and ack.get("accepted") is True
        assert not ack.get("saved_file"), ack
        code2, ack2 = _post(
            base + "/api/v1/pb/UploadMel",
            _mel_raw("nevod-DEMO01", ts + 2),
            {**auth, "Content-Type": "application/x-protobuf"},
            None,
        )
        assert code2 == 200 and ack2.get("accepted") is True
        assert not ack2.get("saved_file"), ack2
        assert len(list(MEL_DIR.glob("mel_*.bin"))) == before

    def t_legacy_nodes_gone() -> None:
        try:
            _post(
                base + "/nodes/mel",
                b"\x00\x01",
                {**auth, "Content-Type": "application/octet-stream"},
                None,
            )
            raise AssertionError("expected HTTPError 410")
        except HTTPError as e:
            assert e.code == 410

    def t_reject() -> None:
        try:
            _post(
                base + "/api/v1/pb/UploadMel",
                b"\x00\x01",
                {**auth, "Content-Type": "application/x-protobuf"},
                None,
            )
            raise AssertionError("expected HTTPError")
        except HTTPError as e:
            assert e.code == 400

    def t_reject_short_mel() -> None:
        bands, frames = 64, 173
        data = bytes((i % 256) for i in range(bands * frames))
        body = pb_encode(
            **{
                "1:str": "nevod-AABBCC",
                "2:u64": ts + 3,
                "3:u32": bands,
                "4:u32": frames,
                "5:bytes": data,
                "6:f32": -80.0,
                "7:f32": 0.0,
            }
        )
        try:
            _post(
                base + "/api/v1/pb/UploadMel",
                body,
                {**auth, "Content-Type": "application/x-protobuf"},
                None,
            )
            raise AssertionError("expected HTTPError 400 for short Mel")
        except HTTPError as e:
            assert e.code == 400
            err = e.read().decode("utf-8", errors="replace")
            assert "bad_num_frames" in err, err

    def t_reject_uptime_ts() -> None:
        raw = pb_encode(
            **{
                "1:str": "nevod-AABBCC",
                "2:u64": 9190341,
                "3:str": "ok",
                "5:f32": -48.0,
                "6:i32": 12,
                "12:f32": -32.0,
            }
        )
        try:
            _post(
                base + "/api/v1/pb/ReportHeartbeat",
                raw,
                {**auth, "Content-Type": "application/x-protobuf"},
                None,
            )
            raise AssertionError("expected HTTPError 400 for uptime ts")
        except HTTPError as e:
            assert e.code == 400

    def t_ingest_timeline() -> None:
        snap = STATE.snapshot()
        tl = snap.get("ingest_timeline") or {}
        assert tl.get("slots") == 288, tl
        hb_sum = sum(tl.get("hb") or [])
        det_sum = sum(tl.get("det") or [])
        mel_sum = sum(tl.get("mel") or [])
        mqtt_hb_sum = sum(tl.get("mqtt_hb") or [])
        mqtt_det_sum = sum(tl.get("mqtt_det") or [])
        assert hb_sum + det_sum + mel_sum + mqtt_hb_sum + mqtt_det_sum >= 1, tl
        by = snap.get("ingest_timeline_by_node") or {}
        assert isinstance(by, dict), by
        assert any(
            sum(
                (v.get("hb") or [])
                + (v.get("det") or [])
                + (v.get("mel") or [])
                + (v.get("mqtt_hb") or [])
                + (v.get("mqtt_det") or [])
            )
            for v in by.values()
        ), by
        nid = next(iter(by))
        one = by[nid]
        assert one.get("node_id") == nid, one
        assert sum(one.get("hb") or []) + sum(one.get("det") or []) + sum(
            one.get("mel") or []
        ) + sum(one.get("mqtt_hb") or []) + sum(one.get("mqtt_det") or []) >= 1, one

    print("self-test nevod_hub")
    try:
        def t_webhook_ssrf() -> None:
            ok, reason = webhook_url_allowed("http://127.0.0.1/x")
            assert ok is False, reason
            ok2, _ = webhook_url_allowed("https://example.com/hooks/nevod")
            assert ok2 is True

        def t_webhook_telegram_ifttt() -> None:
            assert WEBHOOKS is not None
            wh = WEBHOOKS
            captured: dict[str, Any] = {}

            class _Resp:
                status = 200

                def __enter__(self):
                    return self

                def __exit__(self, *a):
                    return False

                def read(self):
                    return b'{"ok":true}'

            real_urlopen = urllib.request.urlopen

            def fake_urlopen(req, timeout=8):  # noqa: ARG001
                captured["url"] = getattr(req, "full_url", None) or req.get_full_url()
                captured["data"] = req.data
                return _Resp()

            urllib.request.urlopen = fake_urlopen  # type: ignore[assignment]
            try:
                wh.tg_enabled = True
                wh.tg_token = "test-bot-token"
                wh.tg_chat = "-100123"
                wh.ifttt_enabled = False
                wh.email_enabled = False
                wh.cooldown_s = 0
                wh.min_threat = 0.1
                wh._last_fire.clear()
                res = wh.on_joined_detection(
                    {
                        "node_id": "nevod-AABBCC",
                        "joined": True,
                        "threat": 0.88,
                        "class_name": "ice_uav",
                        "azimuth_deg": 42.0,
                        "timestamp_ms": ts,
                    }
                )
                assert res and res[0]["ok"] and res[0]["channel"] == "telegram"
                assert "api.telegram.org/bottest-bot-token/sendMessage" in captured["url"]
                body = json.loads(captured["data"].decode())
                assert body["chat_id"] == "-100123"
                assert "ice_uav" in body["text"] and "0.88" in body["text"]
                # token must not appear in delivery log detail
                assert "test-bot-token" not in (res[0].get("detail") or "")
                pub = wh.public_config()
                assert pub["telegram"]["has_token"] is True
                assert "test-bot-token" not in json.dumps(pub)
            finally:
                urllib.request.urlopen = real_urlopen  # type: ignore[assignment]
                wh.tg_enabled = False
                wh.tg_token = ""
                wh.tg_chat = ""

            # IFTTT: публичный URL (loopback всегда blocked даже при allow_private)
            ok_priv, _ = webhook_url_allowed(
                "http://192.168.1.10/hook", allow_private=True
            )
            assert ok_priv is True
            ok_loop, reason_loop = webhook_url_allowed(
                "http://127.0.0.1/hook", allow_private=True
            )
            assert ok_loop is False and "blocked_target_ip" in reason_loop, reason_loop

            captured2: dict[str, Any] = {}

            def fake_urlopen2(req, timeout=8):  # noqa: ARG001
                captured2["url"] = getattr(req, "full_url", None) or req.get_full_url()
                captured2["data"] = req.data
                return _Resp()

            real_uo = wh._urlopen
            wh._urlopen = fake_urlopen2  # type: ignore[method-assign]
            try:
                wh.ifttt_enabled = True
                wh.tg_enabled = False
                wh.email_enabled = False
                wh.allow_private = False
                wh.ifttt_url = "https://example.com/trigger/nevod/with/key"
                wh.cooldown_s = 0
                wh._last_fire.clear()
                res2 = wh.on_joined_detection(
                    {
                        "node_id": "nevod-AABBCC",
                        "joined": True,
                        "threat": 0.9,
                        "class_name": "jet_uav",
                        "azimuth_deg": 10,
                        "timestamp_ms": ts,
                    }
                )
                assert res2 and res2[0]["ok"] and res2[0]["channel"] == "ifttt", res2
                assert "example.com/trigger/nevod" in captured2["url"]
                payload = json.loads(captured2["data"].decode())
                assert payload["value1"] == "nevod-AABBCC"
                assert "jet_uav" in payload["value2"]
            finally:
                wh._urlopen = real_uo  # type: ignore[method-assign]
                wh.ifttt_enabled = False
                wh.ifttt_url = ""
                wh.allow_private = False

        def t_webhook_email_format() -> None:
            assert WEBHOOKS is not None
            wh = WEBHOOKS
            sent: dict[str, Any] = {}

            class _SMTP:
                def __init__(self, host, port, timeout=15):  # noqa: ARG002
                    sent["host"] = host
                    sent["port"] = port

                def __enter__(self):
                    return self

                def __exit__(self, *a):
                    return False

                def starttls(self, context=None):  # noqa: ARG002
                    sent["tls"] = True

                def login(self, user, password):
                    sent["user"] = user
                    sent["pass_len"] = len(password or "")

                def send_message(self, msg):
                    sent["subject"] = msg["Subject"]
                    sent["to"] = msg["To"]
                    sent["from"] = msg["From"]
                    sent["body"] = msg.get_content()

            real_smtp = smtplib.SMTP
            smtplib.SMTP = _SMTP  # type: ignore[misc,assignment]
            try:
                wh.email_enabled = True
                wh.smtp_host = "smtp.example.test"
                wh.smtp_port = 587
                wh.smtp_user = "hub@example.test"
                wh.smtp_pass = "secret-pass"
                wh.smtp_from = "hub@example.test"
                wh.smtp_to = "ops@example.test"
                wh.smtp_tls = True
                wh.cooldown_s = 0
                wh.min_threat = 0.1
                wh._last_fire.clear()
                res = wh.on_joined_detection(
                    {
                        "node_id": "nevod-AABBCC",
                        "joined": True,
                        "threat": 0.77,
                        "class_name": "drone",
                        "timestamp_ms": ts,
                    }
                )
                assert res and res[0]["ok"] and res[0]["channel"] == "email"
                assert "drone" in sent["subject"] and "0.77" in sent["subject"]
                assert sent["to"] == "ops@example.test"
                assert "secret-pass" not in json.dumps(wh.public_config())
                assert "secret-pass" not in (res[0].get("detail") or "")
            finally:
                smtplib.SMTP = real_smtp  # type: ignore[misc,assignment]
                wh.email_enabled = False
                wh.smtp_host = ""
                wh.smtp_pass = ""
                wh.smtp_to = ""

        def t_hub_dashboard() -> None:
            req = urllib.request.Request(base + "/api/dashboard", headers=auth)
            with urllib.request.urlopen(req, timeout=5) as r:
                snap = json.loads(r.read().decode())
                assert r.status == 200
            assert snap.get("service") == "nevod_hub", snap.get("service")
            assert "hub" in snap
            # PB origin never in public dashboard
            assert not (snap.get("hub") or {}).get("forward", {}).get("base")

        def t_cloud_token_and_locked_origin() -> None:
            assert FORWARDER is not None
            code_u, ack_u = _post(
                base + "/api/hub/cloud/unlock",
                json.dumps({"password": HUB_UNLOCK_HARDCODED_PW}).encode(),
                {**auth, "Content-Type": "application/json"},
                None,
            )
            assert code_u == 200 and ack_u.get("token"), ack_u
            unlock = ack_u["token"]
            code, ack = _post(
                base + "/api/hub/cloud/token",
                json.dumps({"token": "cloud-uplink-token-xyz"}).encode(),
                {
                    **auth,
                    "Content-Type": "application/json",
                    "X-Hub-Unlock": unlock,
                },
                None,
            )
            assert code == 200 and ack.get("forward", {}).get("has_token") is True
            # without unlock header — origin empty
            req = urllib.request.Request(base + "/api/hub/cloud", headers=auth)
            with urllib.request.urlopen(req, timeout=5) as r:
                pub = json.loads(r.read().decode())
            assert pub.get("forward", {}).get("base") in ("", None)
            assert pub.get("engineer", {}).get("unlocked") is False
            code3, ack3 = _post(
                base + "/api/hub/cloud/origin",
                json.dumps({"base": "https://example.com"}).encode(),
                {
                    **auth,
                    "Content-Type": "application/json",
                    "X-Hub-Unlock": unlock,
                },
                None,
            )
            assert code3 == 200
            assert ack3.get("forward", {}).get("base") == "https://example.com"
            # locked without header
            try:
                _post(
                    base + "/api/hub/cloud/origin",
                    json.dumps({"base": "https://evil.example"}).encode(),
                    {**auth, "Content-Type": "application/json"},
                    None,
                )
                raise AssertionError("expected 403 cloud_locked")
            except HTTPError as e:
                assert e.code == 403

        def t_webhook_cfg_api() -> None:
            assert WEBHOOKS is not None
            code0, ack0 = _post(
                base + "/api/hub/webhooks",
                json.dumps({"telegram": {"enabled": True, "chat_id": "1"}}).encode(),
                {**auth, "Content-Type": "application/json"},
                None,
            )
            assert code0 == 200, ack0
            code_u, ack_u = _post(
                base + "/api/hub/cloud/unlock",
                json.dumps({"password": HUB_UNLOCK_HARDCODED_PW}).encode(),
                {**auth, "Content-Type": "application/json"},
                None,
            )
            assert code_u == 200 and ack_u.get("token"), ack_u
            unlock = ack_u["token"]
            code2, ack2 = _post(
                base + "/api/hub/webhooks",
                json.dumps(
                    {
                        "min_threat": 0.4,
                        "cooldown_s": 30,
                        "telegram": {
                            "enabled": True,
                            "chat_id": "-10042",
                            "bot_token": "ui-saved-token",
                        },
                    }
                ).encode(),
                {
                    **auth,
                    "Content-Type": "application/json",
                    "X-Hub-Unlock": unlock,
                },
                None,
            )
            assert code2 == 200, ack2
            pub = ack2.get("webhooks") or {}
            assert pub.get("telegram", {}).get("enabled") is True
            assert pub.get("telegram", {}).get("chat_id") == "-10042"
            assert pub.get("telegram", {}).get("has_token") is True
            assert "ui-saved-token" not in json.dumps(pub)
            assert WEBHOOKS.tg_token == "ui-saved-token"

        def t_ui_login_and_closed_dashboard() -> None:
            import nevod_hub as _hub_mod

            _hub_mod.DASHBOARD_OPEN = True
            req0 = urllib.request.Request(base + "/")
            with urllib.request.urlopen(req0, timeout=5) as r:
                html0 = r.read().decode("utf-8")
                assert r.status == 200
            assert "btnMap" not in html0
            assert 'id="uGo"' in html0
            req_d = urllib.request.Request(base + "/api/dashboard")
            try:
                with urllib.request.urlopen(req_d, timeout=5) as r:
                    raise AssertionError(f"expected_401 got {r.status}")
            except HTTPError as e:
                assert e.code == 401, e.code
            req_ok = urllib.request.Request(base + "/api/dashboard", headers=auth)
            with urllib.request.urlopen(req_ok, timeout=5) as r:
                assert r.status == 200
            code, ack, sc = _post_h(
                base + "/api/hub/ui/setup",
                json.dumps({"user": "hub_admin", "password": "password1"}).encode(),
                {"Content-Type": "application/json"},
                None,
            )
            assert code == 200 and ack.get("configured") is True, ack
            try:
                _post(
                    base + "/api/hub/settings/unlock",
                    json.dumps({"password": ""}).encode(),
                    {**auth, "Content-Type": "application/json"},
                    None,
                )
                raise AssertionError("expected 410")
            except HTTPError as e:
                assert e.code == 410
            cookie = (sc.split(";")[0] if sc else "")
            assert cookie.startswith("nevod_hub_ui="), sc
            req_c = urllib.request.Request(
                base + "/api/dashboard", headers={"Cookie": cookie}
            )
            with urllib.request.urlopen(req_c, timeout=5) as r:
                assert r.status == 200
            req_html = urllib.request.Request(base + "/", headers={"Cookie": cookie})
            with urllib.request.urlopen(req_html, timeout=5) as r:
                html1 = r.read().decode("utf-8")
            assert "btnMap" in html1
            try:
                _post(
                    base + "/api/hub/ui/login",
                    json.dumps({"user": "hub_admin", "password": "wrong___x"}).encode(),
                    {"Content-Type": "application/json"},
                    None,
                )
                raise AssertionError("expected 401 bad_login")
            except HTTPError as e:
                assert e.code == 401
            _post(
                base + "/api/hub/ui/logout",
                b"{}",
                {"Content-Type": "application/json", "Cookie": cookie},
                None,
            )
            try:
                with urllib.request.urlopen(req_c, timeout=5) as r:
                    raise AssertionError(f"expected_401 after logout got {r.status}")
            except HTTPError as e:
                assert e.code == 401, e.code

        def t_node_label_api() -> None:
            assert STORE is not None and STATE is not None
            code0, ack0 = _post(
                base + "/api/hub/nodes/AABBCC/label",
                json.dumps({"label": "Гараж"}).encode(),
                {**auth, "Content-Type": "application/json"},
                None,
            )
            assert code0 == 200, ack0
            assert ack0.get("label") == "Гараж"
            code, ack = _post(
                base + "/api/hub/cloud/unlock",
                json.dumps({"password": HUB_UNLOCK_HARDCODED_PW}).encode(),
                {**auth, "Content-Type": "application/json"},
                None,
            )
            assert code == 200 and ack.get("token"), ack
            unlock = ack["token"]
            code2, ack2 = _post(
                base + "/api/hub/nodes/nevod-AABBCC/label",
                json.dumps({"label": "Гараж"}).encode(),
                {
                    **auth,
                    "Content-Type": "application/json",
                    "X-Hub-Unlock": unlock,
                },
                None,
            )
            assert code2 == 200, ack2
            assert ack2.get("node_id") == "AABBCC"
            assert ack2.get("label") == "Гараж"
            try:
                _post(
                    base + "/api/hub/nodes/AABBCC/label",
                    json.dumps({}).encode(),
                    {
                        **auth,
                        "Content-Type": "application/json",
                        "X-Hub-Unlock": unlock,
                    },
                    None,
                )
                raise AssertionError("expected 400 label_required")
            except HTTPError as e:
                assert e.code == 400
            try:
                _post(
                    base + "/api/hub/nodes/AABBCC/label",
                    json.dumps({"label": "x" * 41}).encode(),
                    {
                        **auth,
                        "Content-Type": "application/json",
                        "X-Hub-Unlock": unlock,
                    },
                    None,
                )
                raise AssertionError("expected 400 label_too_long")
            except HTTPError as e:
                assert e.code == 400
            STATE._node("AABBCC")
            row = next(
                n for n in STATE.snapshot()["nodes"] if n["node_id"] == "AABBCC"
            )
            assert row.get("label") == "Гараж"

        def t_forward_e2e_local() -> None:
            """Store-and-forward e2e against local fake Cloud (no real uplink)."""
            assert FORWARDER is not None and STATE is not None and STORE is not None
            got: list[tuple[str, bytes]] = []

            class _Cloud(BaseHTTPRequestHandler):
                def do_POST(self):  # noqa: N802
                    n = int(self.headers.get("Content-Length", "0"))
                    body = self.rfile.read(n)
                    auth_h = self.headers.get("Authorization") or ""
                    if auth_h != "Bearer fwd-e2e-token":
                        self.send_response(401)
                        self.end_headers()
                        return
                    got.append((self.path, body))
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.end_headers()
                    self.wfile.write(b'{"accepted":true}')

                def log_message(self, *a):  # noqa: ARG002
                    return

            srv = ThreadingHTTPServer(("127.0.0.1", 0), _Cloud)
            port = srv.server_address[1]
            th = threading.Thread(target=srv.serve_forever, daemon=True)
            th.start()
            try:
                FORWARDER.set_token("fwd-e2e-token")
                FORWARDER.set_origin(
                    base=f"http://127.0.0.1:{port}",
                    ca_file="",
                    forward_mel=False,
                )
                raw = pb_encode(
                    **{
                        "1:str": "nevod-AABBCC",
                        "2:u64": ts + 50,
                        "3:str": "UAV",
                        "4:i32": 1,
                        "5:f32": 0.91,
                        "6:f32": 0.85,
                    }
                )
                FORWARDER.maybe_enqueue("/api/v1/pb/ReportDetection", raw)
                # wait worker
                deadline = time.time() + 8.0
                while time.time() < deadline and not got:
                    time.sleep(0.2)
                assert got, "forward worker did not deliver"
                assert got[0][0] == "/api/v1/pb/ReportDetection"
                assert got[0][1] == raw
                st = FORWARDER.public_dict(include_origin=True)
                assert st.get("queue_depth", 0) == 0
                assert st.get("enabled") is True
            finally:
                FORWARDER.set_origin(base="", ca_file="", forward_mel=False)
                FORWARDER.set_token("")
                srv.shutdown()
                srv.server_close()

        def t_forward_mel_lan_only() -> None:
            """Микрофон решает Mel: Hub ставит пакет в очередь даже если старая галка выключена."""
            assert FORWARDER is not None and STORE is not None
            FORWARDER.set_token("fwd-mel-policy")
            FORWARDER.set_origin(
                base="http://127.0.0.1:9",
                ca_file="",
                forward_mel=False,
            )
            FORWARDER.mel_lan_only = True
            before = STORE.forward_stats()["queue_depth"]
            FORWARDER.maybe_enqueue("/api/v1/pb/UploadMel", b"\x0a\x04test")
            after = STORE.forward_stats()["queue_depth"]
            assert after == before + 1, "Mel from the device must enqueue"
            FORWARDER.set_origin(base="", ca_file="", forward_mel=False)
            FORWARDER.set_token("")

        def t_forward_backpressure() -> None:
            """Queue capped by queue_max; oldest dropped."""
            assert FORWARDER is not None and STORE is not None
            FORWARDER.set_token("fwd-bp")
            FORWARDER.set_origin(base="http://127.0.0.1:9", ca_file="", forward_mel=False)
            old_max = FORWARDER.queue_max
            FORWARDER.queue_max = 5
            # Stop worker from draining while we fill (disable by clearing base temporarily after enqueue)
            for i in range(12):
                STORE.enqueue_forward(f"/api/v1/pb/ReportHeartbeat", bytes([i]), max_depth=5)
            st = STORE.forward_stats()
            assert st["queue_depth"] == 5, st
            assert st["dropped_total"] >= 7, st
            FORWARDER.queue_max = old_max
            # drain leftover rows so other tests stay clean
            while True:
                items = STORE.dequeue_due(limit=50)
                if not items:
                    break
                for it in items:
                    STORE.forward_ok(it["id"])
            FORWARDER.set_origin(base="", ca_file="", forward_mel=False)
            FORWARDER.set_token("")

        def t_mqtt_hb_health() -> None:
            # Slim PB Heartbeat reserved rssi/heap — diag arrives via MQTT v1 JSON.
            assert STATE is not None
            body = json.dumps(
                {
                    "schema": "nevod.heartbeat.v1",
                    "node_id": "nevod-AABBCC",
                    "status": "ok",
                    "ts": "2026-08-12T09:00:00Z",
                    "fw": {"version": "0.12.13-ota", "xvf": "1.0.8", "nn": "5"},
                    "health": {
                        "uptime_s": 120,
                        "rssi_dbm": -62,
                        "free_heap_bytes": 42000,
                        "free_heap_min": 18000,
                        "temp_c": 41.2,
                    },
                }
            ).encode()
            STATE.note_mqtt("nevod/nevod-AABBCC/heartbeat", body)
            snap = STATE.snapshot()
            node = next(n for n in snap["nodes"] if n["node_id"] == "AABBCC")
            h = node.get("health") or {}
            assert h.get("rssi_dbm") == -62, h
            assert h.get("free_heap") == 42000, h
            assert h.get("free_heap_min") == 18000, h
            assert h.get("weak_rssi") is False, h
            assert h.get("heap_low") is False, h
            assert node.get("firmware_version") == "0.12.13-ota", node
            assert node.get("xvf_firmware_version") == "1.0.8", node
            assert node.get("nn_model_version") == "5", node
            assert node.get("last_heartbeat", {}).get("timestamp_ms"), node.get(
                "last_heartbeat"
            )
            assert any(
                e.get("key") == "mqtt_heartbeat"
                and e.get("node_id") == "AABBCC"
                and e.get("transport") == "mqtt"
                and e.get("kind") == "hb"
                and e.get("via_label") == "MQTT · HB"
                and e.get("summary")
                for e in snap.get("recent") or []
            ), snap.get("recent")
            tl = snap.get("ingest_timeline") or {}
            assert sum(tl.get("mqtt_hb") or []) >= 1, tl
            by_n = (snap.get("ingest_timeline_by_node") or {}).get("AABBCC") or {}
            assert sum(by_n.get("mqtt_hb") or []) >= 1, by_n
            # weak RSSI badge
            STATE.note_mqtt(
                "nevod/nevod-AABBCC/heartbeat",
                json.dumps(
                    {
                        "schema": "nevod.heartbeat.v1",
                        "node_id": "nevod-AABBCC",
                        "fw": {"version": "0.12.13-ota"},
                        "health": {"rssi_dbm": -80, "free_heap_bytes": 12000},
                    }
                ).encode(),
            )
            node2 = next(
                n
                for n in STATE.snapshot()["nodes"]
                if n["node_id"] == "AABBCC"
            )
            h2 = node2.get("health") or {}
            assert h2.get("weak_rssi") is True, h2
            assert h2.get("heap_low") is True, h2
            assert h2.get("heap_cliff") is True, h2

        def t_mqtt_detection_parity() -> None:
            assert STATE is not None
            # HB first (join window) then MQTT DET — same UI path as PB.
            STATE.note_mqtt(
                "nevod/nevod-MQTT01/heartbeat",
                json.dumps(
                    {
                        "schema": "nevod.heartbeat.v1",
                        "node_id": "nevod-MQTT01",
                        "ts": "2026-08-12T10:00:00Z",
                        "status": "ok",
                        "spl_fast": 55.0,
                        "noise_dbfs": -42.0,
                        "fw": {"version": "0.12.14-ota"},
                        "health": {"uptime_s": 30, "rssi_dbm": -50, "free_heap_bytes": 40000},
                        "node_position": {"lat": 55.75, "lon": 37.61},
                    }
                ).encode(),
            )
            STATE.note_mqtt(
                "nevod/nevod-MQTT01/detection",
                json.dumps(
                    {
                        "schema": "nevod.detection.v1",
                        "msg_id": "01TESTULIDMQTTDET000000",
                        "node_id": "nevod-MQTT01",
                        "ts": "2026-08-12T10:00:05Z",
                        "target": {"class": "drone", "class_id": 1, "p": 0.88},
                        "detection": {
                            "class": "drone",
                            "class_id": 1,
                            "p": 0.88,
                            "threat": 0.77,
                            "early_warning": True,
                        },
                        "bearing": {"azimuth_deg": 123, "confidence": 0.6},
                        "fw": {"version": "0.12.14-ota"},
                        "extensions": {"hps": {"bpf_hz": 240.0}},
                    }
                ).encode(),
            )
            snap = STATE.snapshot()
            node = next(n for n in snap["nodes"] if n["node_id"] == "MQTT01")
            assert node.get("firmware_version") == "0.12.14-ota", node
            det = node.get("last_detection") or {}
            assert det.get("via") == "mqtt_detection", det
            assert det.get("threat") == 0.77, det
            assert det.get("class_name") == "drone", det
            assert det.get("azimuth_deg") == 123, det
            assert det.get("joined") is True, det
            assert det.get("spl_fast") == 55.0, det
            assert any(
                (
                    e.get("key") == "episode"
                    and e.get("node_id") == "MQTT01"
                    and e.get("kind") == "det"
                    and (e.get("meta") or {}).get("via") in ("mqtt", "mixed")
                )
                or (e.get("key") == "mqtt_detection" and e.get("node_id") == "MQTT01")
                for e in snap.get("recent") or []
            ), snap.get("recent")
            assert (node.get("active_episode") or {}).get("hop_count", 0) >= 1, node.get(
                "active_episode"
            )
            mq = snap.get("mqtt_recent") or []
            assert any(r.get("key") == "mqtt_detection" for r in mq), mq
            by_n = (snap.get("ingest_timeline_by_node") or {}).get("MQTT01") or {}
            assert sum(by_n.get("mqtt_det") or []) >= 1, by_n
            assert sum(by_n.get("mqtt_hb") or []) >= 1, by_n

        check("webhook_ssrf", t_webhook_ssrf)
        check("webhook_telegram_ifttt", t_webhook_telegram_ifttt)
        check("webhook_email_format", t_webhook_email_format)
        check("hub_dashboard", t_hub_dashboard)
        check("cloud_token_locked_origin", t_cloud_token_and_locked_origin)
        check("ui_login_and_closed_dashboard", t_ui_login_and_closed_dashboard)
        check("webhook_cfg_api", t_webhook_cfg_api)
        check("node_label_api", t_node_label_api)
        check("forward_e2e_local", t_forward_e2e_local)
        check("forward_mel_lan_only", t_forward_mel_lan_only)
        check("forward_backpressure", t_forward_backpressure)
        def t_mqtt_pb_same_node() -> None:
            assert STATE is not None
            # MQTT nevod-XX + PB XX → один узел, оба транспорта.
            STATE.note_mqtt(
                "nevod/746E8C/heartbeat",
                json.dumps(
                    {
                        "schema": "nevod.heartbeat.v1",
                        "node_id": "nevod-746E8C",
                        "ts": "2026-08-12T11:00:00Z",
                        "status": "ok",
                        "device_type": "diy",
                        "fw": {"version": "0.12.13-ota"},
                        "health": {
                            "uptime_s": 10,
                            "rssi_dbm": -40,
                            "free_heap_bytes": 35000,
                            "cpu_mhz": 240,
                            "infer_ms": 12,
                        },
                        "node_position": {
                            "lat": 55.9,
                            "lon": 36.6,
                            "install_height_m": 6.0,
                            "alt_ref": "agl",
                            "fix": "3d",
                            "valid": True,
                        },
                    }
                ).encode(),
            )
            ts = int(time.time() * 1000)
            raw = pb_encode(
                **{
                    "1:str": "746E8C",
                    "2:u64": ts,
                    "3:str": "ok",
                    "5:f32": -70.0,
                    "6:i32": 99,
                    "12:f32": 50.0,
                }
            )
            # inject via mark path used by HTTP handler
            meta = validate_heartbeat_pb(raw)
            STATE.mark("pb_heartbeat", ok=True, meta=meta, transport="pb", body_len=len(raw))
            snap = STATE.snapshot()
            ids = [n["node_id"] for n in snap["nodes"] if "746E8C" in n["node_id"]]
            assert ids == ["746E8C"], ids
            node = next(n for n in snap["nodes"] if n["node_id"] == "746E8C")
            assert set(node.get("transports") or []) >= {"mqtt", "pb"}, node
            hb = node.get("last_heartbeat") or {}
            # last write is PB — firmware from earlier MQTT must stick; MQTT-only
            # health (rssi/heap/gps_fix) must NOT ride along as live via=pb.
            assert node.get("firmware_version") == "0.12.13-ota", node
            assert hb.get("via") == "pb_heartbeat", hb
            assert hb.get("health_source") == "pb", hb
            assert hb.get("rssi_dbm") is None, hb
            assert hb.get("free_heap") is None, hb
            assert hb.get("gps_fix") is None, hb
            health = node.get("health") or {}
            assert health.get("rssi_dbm") is None, health
            assert health.get("free_heap") is None, health
            assert health.get("free_heap_min") is None, health
            # Re-send MQTT after PB to verify full parse retained
            STATE.note_mqtt(
                "nevod/746E8C/heartbeat",
                json.dumps(
                    {
                        "schema": "nevod.heartbeat.v1",
                        "node_id": "nevod-746E8C",
                        "ts": "2026-08-12T11:00:10Z",
                        "status": "degraded",
                        "device_type": "diy",
                        "noise_dbfs": -74.0,
                        "spl_fast": -74.5,
                        "fw": {"version": "0.12.13-ota"},
                        "health": {
                            "uptime_s": 20,
                            "rssi_dbm": -23,
                            "free_heap_bytes": 37000,
                            "temp_c": 42.0,
                            "cpu_mhz": 240,
                            "cpu_load": 0.5,
                            "infer_ms": 15,
                            "arena_used_bytes": 100000,
                        },
                        "node_position": {
                            "lat": 55.9,
                            "lon": 36.6,
                            "install_height_m": 6.0,
                            "alt_ref": "agl",
                            "source": "manual_gnss",
                            "fix": "3d",
                            "valid": True,
                        },
                    }
                ).encode(),
            )
            node = next(n for n in STATE.snapshot()["nodes"] if n["node_id"] == "746E8C")
            hb = node.get("last_heartbeat") or {}
            assert hb.get("via") == "mqtt_heartbeat", hb
            assert hb.get("device_type") == "diy", hb
            assert hb.get("cpu_mhz") == 240, hb
            assert hb.get("infer_ms") == 15, hb
            assert hb.get("install_height_m") == 6.0, hb
            assert hb.get("alt_m") == 6.0, hb
            assert isinstance(hb.get("health"), dict) and hb["health"].get("cpu_mhz") == 240, hb
            assert isinstance(hb.get("node_position"), dict), hb
            assert isinstance(hb.get("fw"), dict) and hb["fw"].get("version") == "0.12.13-ota", hb
            assert len([n for n in STATE.snapshot()["nodes"] if n["node_id"] in ("746E8C", "nevod-746E8C")]) == 1

        def t_pb_hb_firmware_version() -> None:
            ts_ms = int(time.time() * 1000)
            raw = pb_encode(
                **{
                    "1:str": "nevod-FW1301",
                    "2:u64": ts_ms,
                    "3:str": "ok",
                    "5:f32": -50.0,
                    "6:i32": 9,
                    "12:f32": -30.0,
                    "13:str": "0.12.14-ota",
                }
            )
            meta = validate_heartbeat_pb(raw)
            assert meta.get("firmware_version") == "0.12.14-ota", meta
            assert "xvf_firmware_version" not in meta, meta
            raw_xvf = pb_encode(
                **{
                    "1:str": "nevod-FW1301",
                    "2:u64": ts_ms,
                    "3:str": "ok",
                    "5:f32": -50.0,
                    "6:i32": 9,
                    "12:f32": -30.0,
                    "13:str": "0.12.14-ota",
                    "14:str": "1.0.8",
                }
            )
            meta_xvf = validate_heartbeat_pb(raw_xvf)
            assert meta_xvf.get("xvf_firmware_version") == "1.0.8", meta_xvf
            raw_nn = pb_encode(
                **{
                    "1:str": "nevod-FW1301",
                    "2:u64": ts_ms,
                    "3:str": "ok",
                    "5:f32": -50.0,
                    "6:i32": 9,
                    "12:f32": -30.0,
                    "13:str": "0.12.14-ota",
                    "14:str": "1.0.8",
                    "15:str": "5",
                }
            )
            meta_nn = validate_heartbeat_pb(raw_nn)
            assert meta_nn.get("nn_model_version") == "5", meta_nn
            code, ack = _post(
                base + "/api/v1/pb/ReportHeartbeat",
                raw,
                {**auth, "Content-Type": "application/x-protobuf"},
                None,
            )
            assert code == 200 and ack.get("accepted") is True
            node = next(n for n in STATE.snapshot()["nodes"] if n["node_id"] == "FW1301")
            assert node.get("firmware_version") == "0.12.14-ota", node
            assert not node.get("xvf_firmware_version"), node
            code_xvf, ack_xvf = _post(
                base + "/api/v1/pb/ReportHeartbeat",
                raw_xvf,
                {**auth, "Content-Type": "application/x-protobuf"},
                None,
            )
            assert code_xvf == 200 and ack_xvf.get("accepted") is True
            node = next(n for n in STATE.snapshot()["nodes"] if n["node_id"] == "FW1301")
            assert node.get("xvf_firmware_version") == "1.0.8", node
            code_nn, ack_nn = _post(
                base + "/api/v1/pb/ReportHeartbeat",
                raw_nn,
                {**auth, "Content-Type": "application/x-protobuf"},
                None,
            )
            assert code_nn == 200 and ack_nn.get("accepted") is True
            node = next(n for n in STATE.snapshot()["nodes"] if n["node_id"] == "FW1301")
            assert node.get("nn_model_version") == "5", node

        def t_adsb_filter_suppress() -> None:
            assert ADSB is not None and FORWARDER is not None
            ADSB.apply(
                {
                    "enabled": False,
                    "filter_det": True,
                    "filter_hb": False,
                    "filter_mel": False,
                    "radius_m": 50000,
                    "site_lat": 55.75,
                    "site_lon": 37.61,
                },
                persist=False,
            )
            with ADSB._lock:
                ADSB._last_ok_at = time.time()
                ADSB._last_meta = {
                    "civilian_in_radius": True,
                    "adsb_radius_m": 50000,
                    "adsb_count_in_radius": 2,
                    "adsb_nearest_dist_m": 1200.0,
                }
                ADSB._feeder_cache["default"] = {
                    "ok_at": time.time(),
                    "stale_s": 30,
                    "positions": [(55.75, 37.61)],
                    "error": "",
                    "url": "http://example.test/aircraft.json",
                }
            assert ADSB.should_suppress_forward("det") is True
            assert ADSB.should_suppress_forward("hb") is False
            with ADSB._lock:
                ADSB._last_ok_at = time.time() - 9999
                ADSB._feeder_cache["default"]["ok_at"] = time.time() - 9999
            assert ADSB.should_suppress_forward("det") is False
            ADSB.apply({"filter_det": False}, persist=False)

        def t_adsb_ssrf_and_auth() -> None:
            assert ADSB is not None
            import nevod_hub as _hub_mod

            _hub_mod.DASHBOARD_OPEN = True
            try:
                ADSB.apply({"url": "http://127.0.0.1/evil"}, persist=False)
                raise AssertionError("ssrf_expected")
            except ValueError as e:
                assert "adsb_ssrf" in str(e), e
            # dashboard without cookie/Bearer → 401 (HUB_DASHBOARD_OPEN ignored)
            req = urllib.request.Request(base + "/api/dashboard")
            try:
                with urllib.request.urlopen(req, timeout=5) as r:
                    raise AssertionError(f"expected_401 got {r.status}")
            except HTTPError as e:
                assert e.code == 401, e.code
            req_ok = urllib.request.Request(base + "/api/dashboard", headers=auth)
            with urllib.request.urlopen(req_ok, timeout=5) as r:
                assert r.status == 200, r.status
                dash = json.loads(r.read().decode("utf-8") or "{}")
                assert "nodes" in dash and "hub" in dash
                assert "zones" in (dash.get("hub") or {})
            # still 401 if DASHBOARD_OPEN flipped off — same closed default
            prev_open = os.environ.get("HUB_DASHBOARD_OPEN")
            os.environ["HUB_DASHBOARD_OPEN"] = "0"
            try:
                _hub_mod.DASHBOARD_OPEN = False
                req2 = urllib.request.Request(base + "/api/dashboard")
                try:
                    with urllib.request.urlopen(req2, timeout=5) as r:
                        raise AssertionError(f"expected_401 got {r.status}")
                except HTTPError as e:
                    assert e.code == 401, e.code
            finally:
                if prev_open is None:
                    os.environ.pop("HUB_DASHBOARD_OPEN", None)
                else:
                    os.environ["HUB_DASHBOARD_OPEN"] = prev_open
                _hub_mod.DASHBOARD_OPEN = True
            # unlock without configured hash → 403 (not 500)
            prev = os.environ.pop("HUB_LAB_DEFAULT_UNLOCK", None)
            prev_pw = os.environ.pop("HUB_ENGINEER_PASSWORD", None)
            try:
                if STORE is not None:
                    STORE.set_meta("engineer_unlock_hash", "")
                req_u = urllib.request.Request(
                    base + "/api/hub/cloud/unlock",
                    data=json.dumps({"password": "anything1"}).encode(),
                    headers={**auth, "Content-Type": "application/json"},
                    method="POST",
                )
                try:
                    with urllib.request.urlopen(req_u, timeout=5) as r:
                        raise AssertionError(f"expected_403 got {r.status}")
                except HTTPError as e:
                    body = e.read().decode("utf-8", errors="replace")
                    ack_u = json.loads(body or "{}")
                    assert e.code == 403 and ack_u.get("error") == "unlock_not_configured", (
                        e.code,
                        ack_u,
                    )
            finally:
                if prev is not None:
                    os.environ["HUB_LAB_DEFAULT_UNLOCK"] = prev
                else:
                    os.environ["HUB_LAB_DEFAULT_UNLOCK"] = "1"
                if prev_pw is not None:
                    os.environ["HUB_ENGINEER_PASSWORD"] = prev_pw

        def t_mqtt_detection_full() -> None:
            assert STATE is not None
            STATE.note_mqtt(
                "nevod/FULL01/detection",
                json.dumps(
                    {
                        "schema": "nevod.detection.v1",
                        "msg_id": "01FULLDET00000000000000",
                        "node_id": "nevod-FULL01",
                        "ts": "2026-08-12T11:05:00Z",
                        "window": {"start": "2026-08-12T11:04:55Z", "end": "2026-08-12T11:05:00Z", "sec": 5},
                        "target": {"class": "drone", "class_id": 1, "p": 0.9},
                        "detection": {
                            "class": "drone",
                            "class_ru": "коптер",
                            "class_id": 1,
                            "p": 0.9,
                            "threat": 0.8,
                            "confirmed": True,
                            "probs": {"background": 0.1, "drone": 0.9},
                            "n_of_m": [3, 5],
                            "n_of_m_hits": 3,
                        },
                        "bearing": {"azimuth_deg": 45, "elevation_deg": 0, "confidence": 0.7, "beam_width_deg": 24},
                        "model": {"name": "nevod_esp32s3_int8"},
                        "fw": {"version": "0.12.13-ota", "uptime_s": 100},
                        "extensions": {
                            "device_type": "diy",
                            "detection_layers": ["tflite", "hps"],
                            "fusion": {"decision": "drone", "threat": 0.8, "early_warning": False},
                            "hps": {"bpf_hz": 180.0, "score": 0.6},
                            "rpm": {
                                "rpm": 1800.0,
                                "blade_count": 2,
                                "valid": True,
                            },
                        },
                    }
                ).encode(),
            )
            node = next(n for n in STATE.snapshot()["nodes"] if n["node_id"] == "FULL01")
            det = node.get("last_detection") or {}
            assert det.get("class_ru") == "коптер", det
            assert det.get("confirmed") is True, det
            assert det.get("fusion_decision") == "drone", det
            assert det.get("hps_score") == 0.6, det
            assert det.get("rpm_valid") is True, det
            assert abs(float(det.get("rpm")) - 1800.0) < 1e-2, det
            assert int(det.get("blade_count")) == 2, det
            assert det.get("detection_layers") == ["tflite", "hps"], det
            assert isinstance(det.get("detection"), dict), det
            assert isinstance(det.get("extensions"), dict), det
            assert isinstance(det.get("model"), dict), det

        def t_zones_gps_and_mel() -> None:
            assert ZONES is not None and ADSB is not None and FORWARDER is not None
            ZONES.apply_patch(
                {
                    "mel_lan_only_global": True,
                    "gps_override_max_age_s": 60,
                    "feeders": [
                        {
                            "id": "default",
                            "name": "lab",
                            "url": "http://185.218.111.196:9999/skyaware/data/aircraft.json",
                            "enabled": False,
                            "poll_interval_s": 5,
                            "stale_s": 30,
                        }
                    ],
                    "zones": [
                        {
                            "id": "default",
                            "name": "yard",
                            "lat": 55.75,
                            "lon": 37.61,
                            "radius_m": 10000,
                            "feeder_id": "default",
                            "filter_det": True,
                            "filter_hb": False,
                            "filter_mel": False,
                            "forward_mel": None,
                        },
                        {
                            "id": "dacha",
                            "name": "dacha",
                            "lat": 56.0,
                            "lon": 38.0,
                            "radius_m": 8000,
                            "feeder_id": "default",
                            "filter_det": False,
                            "forward_mel": True,
                        },
                    ],
                    "node_zone": {"NODEA": "default", "NODEB": "dacha"},
                },
                persist=False,
            )
            assert ZONES.effective_forward_mel("NODEA") is False
            assert ZONES.effective_forward_mel("NODEB") is True
            z = ZONES.zone_for_node("NODEA")
            assert z and z["id"] == "default"
            clat, clon, src = ZONES.resolve_center(
                z, lat=55.751, lon=37.615, timestamp_ms=int(time.time() * 1000)
            )
            assert src == "gps" and abs(clat - 55.751) < 1e-6
            clat2, clon2, src2 = ZONES.resolve_center(
                z, lat=55.751, lon=37.615, timestamp_ms=int((time.time() - 600) * 1000)
            )
            assert src2 == "zone" and abs(clat2 - 55.75) < 1e-6
            # feeder cache: aircraft near zone center → suppress DET
            with ADSB._lock:
                ADSB._feeder_cache["default"] = {
                    "ok_at": time.time(),
                    "stale_s": 30,
                    "positions": [(55.75, 37.61)],
                    "error": "",
                    "url": "http://example.test/aircraft.json",
                }
            assert ADSB.should_suppress_forward("det", node_id="NODEA") is True
            assert ADSB.should_suppress_forward("det", node_id="NODEB") is False
            ZONES.apply_patch(
                {"node_zone": {"NODEA": "_none", "NODEB": "dacha"}}, persist=False
            )
            assert ZONES.zone_for_node("NODEA") is None
            assert ADSB.should_suppress_forward("det", node_id="NODEA") is False
            # Mel forward gate via FORWARDER
            FORWARDER.base = "https://cloud.example"
            FORWARDER.token = "tok"
            before = STORE.forward_stats().get("queue_depth", 0) if STORE else 0
            # Продукт: Mel с микрофона всегда в очередь (см. forward_mel_lan_only).
            FORWARDER.maybe_enqueue(
                "/api/v1/pb/UploadMel", b"x", node_id="NODEA"
            )
            mid = STORE.forward_stats().get("queue_depth", 0) if STORE else 0
            assert mid == before + 1
            FORWARDER.maybe_enqueue(
                "/api/v1/pb/UploadMel", b"y", node_id="NODEB"
            )
            after = STORE.forward_stats().get("queue_depth", 0) if STORE else 0
            assert after == mid + 1

        check("mqtt_hb_health", t_mqtt_hb_health)
        check("mqtt_detection_parity", t_mqtt_detection_parity)
        check("mqtt_pb_same_node", t_mqtt_pb_same_node)
        check("pb_hb_firmware_version", t_pb_hb_firmware_version)
        check("adsb_filter_suppress", t_adsb_filter_suppress)
        check("adsb_ssrf_and_auth", t_adsb_ssrf_and_auth)
        check("zones_gps_and_mel", t_zones_gps_and_mel)
        check("mqtt_detection_full", t_mqtt_detection_full)
        check("pb_detection", t_pb_det)
        check("pb_detection_rpm", t_pb_det_rpm)
        check("pb_heartbeat", t_pb_hb)
        check("join_det_hb", t_join_det_hb)
        check("pb_mel", t_pb_mel)
        check("pb_mel_gzip", t_pb_mel_gzip)
        check("pb_mel_gzip_delta", t_pb_mel_gzip_delta)
        check("pb_mel_gzip_delta_u4_rejected", t_pb_mel_gzip_delta_u4_rejected)
        check("pb_mel_gzip_delta_realistic", t_pb_mel_gzip_delta_realistic)
        check("mel_skip_emu", t_mel_skip_emu)
        check("legacy_nodes_gone", t_legacy_nodes_gone)
        check("second_node", t_second_node)
        check("reject_bad_mel", t_reject)
        check("reject_short_mel", t_reject_short_mel)
        check("reject_uptime_ts", t_reject_uptime_ts)
        check("ingest_timeline", t_ingest_timeline)

        def t_hub_context_and_http_mro() -> None:
            assert getattr(httpd, "ctx", None) is not None
            names = [c.__name__ for c in Handler.__mro__]
            for need in (
                "HubAuthUiHttp",
                "HubMelHttp",
                "HubAdminHttp",
                "HubMqttHttp",
                "HubPbHttp",
            ):
                assert need in names, names
            # Mixin globals rebound to composition root
            assert Handler._handle_pb_ingest.__globals__ is not None
            assert "STATE" in Handler._handle_pb_ingest.__globals__

        check("hub_context_and_http_mro", t_hub_context_and_http_mro)
        snap = STATE.snapshot()
        assert snap["ready_core_pb"], snap
        assert snap["node_count"] >= 2, snap
        assert snap["mel_save_enabled"] is True
        assert any(f.get("meta", {}).get("node_id") == "AABBCC" for f in snap["mel_saved"]), snap["mel_saved"]
        assert not any("DEMO" in (f.get("meta") or {}).get("node_id", "") for f in snap["mel_saved"])
        node = next(n for n in snap["nodes"] if n["node_id"] == "AABBCC")
        assert node["last_detection"], node
        assert node["last_detection"].get("azimuth_deg") is not None
        assert node["last_heartbeat"], node
        assert node["last_mel"] and node["last_mel"].get("band_means"), node
        assert len(node["last_mel"]["band_means"]) == 64
        assert len(node["series"]["threat"]) >= 1
        assert snap.get("last_mel_toast"), snap
        assert any(c["id"] == "pb_mel" and c.get("label") for c in snap.get("checklist", []))
        grid = decode_mel_uint8(bytes([0, 255] + [128] * 62), 64, 1, 0.0, 10.0)
        assert len(grid) == 1 and len(grid[0]) == 64
        assert abs(grid[0][0] - 0.0) < 1e-6 and abs(grid[0][1] - 10.0) < 1e-6
        # MEL.zip includes decoded heat PNG (stdlib zlib, WebUI palette)
        png = hub_mel_io.render_mel_png(
            bytes([0, 255] + [128] * 62), 64, 1, 0.0, 10.0, scale=2
        )
        assert png[:8] == b"\x89PNG\r\n\x1a\n" and len(png) > 64
        zbytes = hub_mel_io.build_mel_archive_zip(mel_tmp)
        import io as _io
        import zipfile as _zf

        with _zf.ZipFile(_io.BytesIO(zbytes), "r") as zf:
            names = set(zf.namelist())
        assert any(n.endswith(".bin") for n in names), names
        assert any(n.endswith(".png") for n in names), names
        assert any(n.endswith(".meta.json") for n in names), names
    finally:
        httpd.shutdown()
        hub_mel_io.MEL_DIR = prev_dir
        hub_mel_io.MEL_SAVE = prev_save
        MEL_DIR, MEL_SAVE = prev_dir, prev_save
        try:
            if STORE is not None:
                STORE.close()
        except Exception:
            pass
        for p in mel_tmp.glob("*"):
            try:
                p.unlink()
            except OSError:
                pass
        try:
            mel_tmp.rmdir()
        except OSError:
            pass
        try:
            (store_tmp / "hub.sqlite").unlink(missing_ok=True)
            store_tmp.rmdir()
        except OSError:
            pass
    if errors:
        print("FAILED:", "; ".join(errors))
        return 1
    print("ALL PASSED")
    return 0


