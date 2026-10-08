#!/usr/bin/env python3
"""MQTT mirror subscriber for Nevod Hub (external broker)."""
from __future__ import annotations

import json
import os
import threading
import time
from pathlib import Path
from typing import Any

import hub_mel_io

MQTT_TOPICS_DEFAULT = (
    "nevod/+/detection",
    "nevod/+/heartbeat",
    "msh/+/2/e/#",
)


def topics_without_mesh_dup(topics: list[str] | tuple[str, ...], host: str, port: int) -> list[str]:
    """Mesh-клиент (MESH_MQTT_*) на том же брокере уже держит msh/…: вторая подписка удваивает события."""
    out = [str(t) for t in topics]
    mesh_host = os.environ.get("MESH_MQTT_HOST", "").strip().lower()
    if not mesh_host or mesh_host != str(host or "").strip().lower():
        return out
    try:
        mesh_port = int(os.environ.get("MESH_MQTT_PORT", "1883") or 1883)
    except ValueError:
        mesh_port = 1883
    if mesh_port != int(port or 1883):
        return out
    return [t for t in out if not t.startswith("msh/")]


TRACK_TOPIC = "nevod/hub/tracks/{track_id}"
ALERT_TOPIC = "nevod/hub/alert"
TRACK_RETAIN_MIN_S = 2.0
ALERT_COOLDOWN_S = 60.0


def _alert_due(prev: dict[str, Any], body: dict[str, Any], now: float) -> bool:
    if now - float(prev.get("alert") or 0.0) < ALERT_COOLDOWN_S:
        return False
    cls = str(body.get("class") or "")
    threat = float(body["threat"])
    prev_cls = prev.get("alerted_class")
    prev_threat = prev.get("alerted_threat")
    if prev_cls is None:
        return True
    if str(prev_cls) != cls:
        return True
    try:
        return threat >= float(prev_threat) + 0.15
    except (TypeError, ValueError):
        return True


def track_topics(track: dict[str, Any]) -> str:
    return TRACK_TOPIC.format(track_id=track["track_id"])


def alert_body(track: dict[str, Any], *, min_threat: float = 0.5) -> dict[str, Any] | None:
    """Короткий алерт. low / гражданский / слабая угроза наружу не идут."""
    if track.get("civil_possible"):
        return None
    estimate = track.get("estimate") if isinstance(track.get("estimate"), dict) else {}
    if estimate.get("quality") == "low":
        return None
    if estimate.get("method") not in ("bearing_intersection", "bearing_lsq", "bearing_mle"):
        return None
    try:
        threat = float(track.get("threat") or 0.0)
    except (TypeError, ValueError):
        return None
    if threat < min_threat:
        return None
    target = track.get("target") if isinstance(track.get("target"), dict) else {}
    return {
        "schema": "nevod.hub.alert.v1",
        "track_id": track.get("track_id"),
        "lat": estimate.get("lat"),
        "lon": estimate.get("lon"),
        "class": target.get("class"),
        "label_ru": target.get("label_ru"),
        "threat": threat,
        "quality": estimate.get("quality"),
        "n_nodes": track.get("n_nodes"),
        "ts": int(time.time() * 1000),
    }


def _mqtt_cfg_path(mel_dir: Path | None = None) -> Path:
    env = os.environ.get("MQTT_CFG_PATH", "").strip()
    if env:
        return Path(env)
    base = mel_dir if mel_dir is not None else hub_mel_io.MEL_DIR
    return Path(base) / "mqtt_mirror.json"


class MqttMirror:
    """Подписчик на внешний MQTT-брокер (не поднимаем свой Mosquitto)."""

    def __init__(self, state: Any, cfg_path: Path | None = None) -> None:
        self.state = state
        self.cfg_path = cfg_path or _mqtt_cfg_path()
        self._lock = threading.Lock()
        self.enabled = False
        self.host = ""
        self.port = 1883
        self.username = ""
        self.password = ""
        self.topics: list[str] = list(MQTT_TOPICS_DEFAULT)
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._client: Any = None
        self._mesh_stop = threading.Event()
        self._mesh_thread: threading.Thread | None = None
        self._mesh_client: Any = None
        self.mesh_status = "off"
        self._track_pub: dict[str, dict[str, float | str]] = {}
        self.load()

    def public_dict(self) -> dict[str, Any]:
        with self._lock:
            return {
                "enabled": self.enabled,
                "host": self.host,
                "port": self.port,
                "username": self.username,
                "has_password": bool(self.password),
                "topics": list(self.topics),
                "status": self.state.mqtt_status,
                "mesh": {
                    "host": os.environ.get("MESH_MQTT_HOST", "").strip(),
                    "status": self.mesh_status,
                    "topics": ["msh/+/2/e/#"],
                },
            }

    def publish_tracks(
        self,
        tracks: list[dict[str, Any]],
        dropped_ids: list[str],
        *,
        min_threat: float | None = None,
    ) -> None:
        """Retain состояния трека и редкий алерт. Свои топики подписку DET не кормят."""
        client = self._client
        if not self.enabled or client is None:
            return
        if min_threat is None:
            try:
                min_threat = float(os.environ.get("HUB_ALERT_MIN_THREAT", "0.5"))
            except (TypeError, ValueError):
                min_threat = 0.5
        now = time.time()
        for track in tracks:
            tid = str(track.get("track_id") or "")
            if not tid:
                continue
            prev = self._track_pub.get(tid) or {}
            last_pub = float(prev.get("pub") or 0.0)
            if now - last_pub >= TRACK_RETAIN_MIN_S:
                self._publish(client, track_topics(track), track, retain=True)
                prev["pub"] = now
            body = alert_body(track, min_threat=min_threat)
            if body is not None and _alert_due(prev, body, now):
                self._publish(client, ALERT_TOPIC, body, retain=False)
                prev["alert"] = now
                prev["alerted_class"] = str(body.get("class") or "")
                prev["alerted_threat"] = float(body["threat"])
            self._track_pub[tid] = prev
        for tid in dropped_ids:
            self._publish(client, TRACK_TOPIC.format(track_id=tid), b"", retain=True)
            self._track_pub.pop(tid, None)

    def _publish(self, client: Any, topic: str, payload: Any, *, retain: bool) -> None:
        try:
            if isinstance(payload, (bytes, bytearray)):
                body: bytes | str = bytes(payload)
            else:
                body = json.dumps(payload, separators=(",", ":"), ensure_ascii=False)
            client.publish(topic, body, qos=1, retain=retain)
        except Exception as exc:  # noqa: BLE001
            print(f"[hub] track mqtt {topic}: {exc}", flush=True)

    def load(self) -> None:
        try:
            if not self.cfg_path.is_file():
                return
            raw = json.loads(self.cfg_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            print(f"[mock] MQTT cfg read: {e}", flush=True)
            return
        with self._lock:
            self.enabled = bool(raw.get("enabled"))
            self.host = str(raw.get("host") or "").strip()
            try:
                self.port = int(raw.get("port") or 1883)
            except (TypeError, ValueError):
                self.port = 1883
            self.username = str(raw.get("username") or "").strip()
            self.password = str(raw.get("password") or "")
            topics = raw.get("topics")
            if isinstance(topics, list) and topics:
                self.topics = [str(t) for t in topics if str(t).strip()]
            else:
                self.topics = list(MQTT_TOPICS_DEFAULT)

    def save(self) -> None:
        with self._lock:
            payload = {
                "enabled": self.enabled,
                "host": self.host,
                "port": self.port,
                "username": self.username,
                "password": self.password,
                "topics": list(self.topics),
            }
        try:
            self.cfg_path.parent.mkdir(parents=True, exist_ok=True)
            self.cfg_path.write_text(
                json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            try:
                os.chmod(self.cfg_path, 0o600)
            except OSError:
                pass
        except OSError as e:
            print(f"[mock] MQTT cfg write: {e}", flush=True)

    def apply(self, body: dict[str, Any], *, connect: bool | None = None) -> dict[str, Any]:
        with self._lock:
            if "host" in body:
                self.host = str(body.get("host") or "").strip()
            if "port" in body and body.get("port") is not None:
                try:
                    self.port = max(1, min(65535, int(body["port"])))
                except (TypeError, ValueError) as e:
                    raise ValueError("bad_port") from e
            if "username" in body:
                # trim: leading space → broker auth "Unspecified error" (seen Hub UI)
                self.username = str(body.get("username") or "").strip()
            if "password" in body:
                pw = body.get("password")
                if pw is not None and str(pw) != "":
                    self.password = str(pw)
            if "topics" in body and isinstance(body["topics"], list) and body["topics"]:
                self.topics = [str(t).strip() for t in body["topics"] if str(t).strip()]
            if connect is None:
                if "enabled" in body:
                    self.enabled = bool(body["enabled"])
            else:
                self.enabled = bool(connect)
            want = self.enabled
            host = self.host
        self.save()
        if want:
            if not host:
                raise ValueError("host_required")
            self.start()
        else:
            self.stop()
        return self.public_dict()

    def seed_from_env(self, host: str, port: int, user: str = "", password: str = "") -> None:
        host = (host or "").strip()
        if not host:
            return
        with self._lock:
            if self.host:
                return  # сохранённый cfg важнее CLI
            self.host = host
            self.port = int(port) if port else 1883
            self.username = user or ""
            self.password = password or ""
            self.enabled = True
        self.save()
        self.start()

    def stop(self, *, clear_enabled: bool = True) -> None:
        self._stop.set()
        client = self._client
        self._client = None
        if client is not None:
            try:
                client.disconnect()
            except Exception:  # noqa: BLE001
                pass
            try:
                client.loop_stop()
            except Exception:  # noqa: BLE001
                pass
        t = self._thread
        if t and t.is_alive() and t is not threading.current_thread():
            t.join(timeout=2.0)
        self._thread = None
        if clear_enabled:
            with self._lock:
                self.enabled = False
            self.state.mqtt_status = "off"

    def start(self) -> bool:
        try:
            import paho.mqtt.client as mqtt  # type: ignore
        except ImportError:
            self.state.mqtt_status = "paho_missing"
            print("[mock] MQTT: pip install paho-mqtt для панели MQTT v1", flush=True)
            return False

        self.stop(clear_enabled=False)
        self._stop.clear()
        with self._lock:
            self.enabled = True
            host = self.host
            port = self.port
            username = self.username
            password = self.password
            topics = topics_without_mesh_dup(self.topics, host, port)
            self._run_gen = getattr(self, "_run_gen", 0) + 1
            run_gen = self._run_gen

        # Drop stale "connecting:old-host" from a previous worker immediately.
        self.state.mqtt_status = f"connecting:{host}:{port}"

        def on_connect(client, userdata, flags, rc, properties=None):  # noqa: ARG001
            if run_gen != getattr(self, "_run_gen", 0):
                return
            if rc == 0:
                for t in topics:
                    client.subscribe(t)
                self.state.mqtt_status = f"connected:{host}:{port}"
                print(f"[mock] MQTT subscribed {host}:{port} {topics}", flush=True)
            else:
                self.state.mqtt_status = f"connect_fail:{rc}"

        def on_message(client, userdata, msg):  # noqa: ARG001
            if run_gen != getattr(self, "_run_gen", 0):
                return
            topic = msg.topic or ""
            if "/2/e/" in topic:
                try:
                    import hub_mesh_adapter

                    if hub_mesh_adapter.apply_to_state(self.state, topic, msg.payload):
                        return
                except Exception as e:  # noqa: BLE001
                    print(f"[mock] mesh adapter: {e}", flush=True)
            self.state.note_mqtt(msg.topic, msg.payload)

        def on_disconnect(client, userdata, *args, **kwargs):  # noqa: ARG001
            if run_gen != getattr(self, "_run_gen", 0):
                return
            rc = args[0] if len(args) == 1 else (args[1] if len(args) > 1 else kwargs.get("rc", 0))
            if not self._stop.is_set():
                self.state.mqtt_status = f"disconnected:{rc}"

        try:
            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)  # type: ignore[attr-defined]
        except Exception:
            client = mqtt.Client()
        if username:
            client.username_pw_set(username, password or None)
        client.on_connect = on_connect
        client.on_message = on_message
        client.on_disconnect = on_disconnect
        self._client = client

        def _run() -> None:
            while not self._stop.is_set() and run_gen == getattr(self, "_run_gen", 0):
                try:
                    self.state.mqtt_status = f"connecting:{host}:{port}"
                    client.connect(host, port, keepalive=30)
                    client.loop_forever(retry_first_connection=True)
                except Exception as e:  # noqa: BLE001
                    if self._stop.is_set() or run_gen != getattr(self, "_run_gen", 0):
                        break
                    self.state.mqtt_status = f"error:{e}"
                    time.sleep(3)
                if self._stop.is_set() or run_gen != getattr(self, "_run_gen", 0):
                    break
                time.sleep(1)

        self._thread = threading.Thread(target=_run, daemon=True, name="mqtt-sub")
        self._thread.start()
        return True

    def start_mesh_from_env(self) -> bool:
        """Второй клиент: брокер меш-шлюза (MESH_MQTT_*), не mqtt_mirror.json."""
        host = os.environ.get("MESH_MQTT_HOST", "").strip()
        if not host:
            self.mesh_status = "off"
            if hasattr(self.state, "mesh_mqtt_status"):
                self.state.mesh_mqtt_status = "off"
            return False
        try:
            port = int(os.environ.get("MESH_MQTT_PORT", "1883") or 1883)
        except ValueError:
            port = 1883
        user = os.environ.get("MESH_MQTT_USER", "").strip()
        password = os.environ.get("MESH_MQTT_PASSWORD", "")
        try:
            import hub_mesh_adapter
            import paho.mqtt.client as mqtt  # type: ignore
        except ImportError:
            self.mesh_status = "paho_missing"
            if hasattr(self.state, "mesh_mqtt_status"):
                self.state.mesh_mqtt_status = "paho_missing"
            return False
        topics = list(hub_mesh_adapter.msh_topics())
        self._mesh_stop.set()
        old = self._mesh_client
        self._mesh_client = None
        if old is not None:
            try:
                old.disconnect()
            except Exception:  # noqa: BLE001
                pass
        self._mesh_stop = threading.Event()
        self.mesh_status = f"connecting:{host}:{port}"
        if hasattr(self.state, "mesh_mqtt_status"):
            self.state.mesh_mqtt_status = self.mesh_status

        def on_connect(client, userdata, flags, rc, properties=None):  # noqa: ARG001
            if rc == 0:
                for t in topics:
                    client.subscribe(t)
                self.mesh_status = f"connected:{host}:{port}"
                if hasattr(self.state, "mesh_mqtt_status"):
                    self.state.mesh_mqtt_status = self.mesh_status
                print(f"[hub] MESH MQTT subscribed {host}:{port} {topics}", flush=True)
            else:
                self.mesh_status = f"connect_fail:{rc}"
                if hasattr(self.state, "mesh_mqtt_status"):
                    self.state.mesh_mqtt_status = self.mesh_status

        def on_message(client, userdata, msg):  # noqa: ARG001
            topic = msg.topic or ""
            try:
                hub_mesh_adapter.apply_to_state(self.state, topic, msg.payload)
            except Exception as e:  # noqa: BLE001
                print(f"[hub] mesh adapter: {e}", flush=True)

        try:
            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)  # type: ignore[attr-defined]
        except Exception:
            client = mqtt.Client()
        if user:
            client.username_pw_set(user, password or None)
        client.on_connect = on_connect
        client.on_message = on_message
        self._mesh_client = client
        stop = self._mesh_stop

        def _run() -> None:
            while not stop.is_set():
                try:
                    client.connect(host, port, keepalive=30)
                    client.loop_forever(retry_first_connection=True)
                except Exception as e:  # noqa: BLE001
                    if stop.is_set():
                        break
                    self.mesh_status = f"error:{e}"
                    if hasattr(self.state, "mesh_mqtt_status"):
                        self.state.mesh_mqtt_status = self.mesh_status
                    time.sleep(3)
                if stop.is_set():
                    break
                time.sleep(1)

        self._mesh_thread = threading.Thread(target=_run, daemon=True, name="mesh-mqtt")
        self._mesh_thread.start()
        return True


