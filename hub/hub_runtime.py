"""Hub runtime: CheckItem, ingest histograms, NodeRecord, State."""
from __future__ import annotations

import json
import math
import os
import sys
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

import hub_episodes
import hub_redis
from hub_dem import attach_node_tips
from hub_episodes import (
    EpisodeRegistry,
    apply_detection,
    apply_mel,
    episode_event_dict,
    find_event_index,
)
from hub_events import VIA_LABELS, _event_meta, build_event_summary, enrich_event
from hub_forward import looks_pro
from hub_labels import attach_node_labels
from hub_auth import _unlock_hash_stored
from hub_mel_io import MEL_SAVE, clear_saved_mel, list_saved_mel
from hub_mqtt_parse import (
    _apply_firmware_version,
    _apply_nn_model_version,
    _apply_xvf_firmware_version,
    _mqtt_node_id,
    _parse_mqtt_detection,
    _parse_mqtt_heartbeat,
    join_detection_with_hb,
    lora_mqtt_pb,
    mqtt_is_lora,
)
from hub_pb import NODE_ID_RE_MAX, _canon_node_id
from hub_tracks import TrackEngine, civil_aircraft_from_adsb, observation_from_detection

SERVICES: dict[str, Any] = {}


def bind_services(**kwargs: Any) -> None:
    SERVICES.update(kwargs)


def _hub():
    """Процесс Hub — это `python3 nevod_hub.py`: модуль называется __main__, не nevod_hub."""
    mod = sys.modules.get("nevod_hub")
    if mod is not None:
        return mod
    main = sys.modules.get("__main__")
    if main is not None and hasattr(main, "_forward_enqueue"):
        return main
    return None


def _g(name: str, default=None):
    if name in SERVICES:
        return SERVICES[name]
    hub = _hub()
    if hub is not None and hasattr(hub, name):
        return getattr(hub, name)
    if default is not None:
        return default
    # Sensible empty defaults for collection-like services when unbound.
    if name in {"DEVICE_TOKENS"}:
        return {}
    return None

# Shared constants (kept here so State does not import nevod_hub at load time).
# Держать равным nevod_hub.HUB_VERSION.
HUB_VERSION = "0.2.4"
MQTT_TOPICS_DEFAULT = (
    "nevod/+/detection",
    "nevod/+/heartbeat",
    "msh/+/2/e/#",
)
INGEST_TIMELINE_WINDOW_S = 24 * 3600
INGEST_TIMELINE_BUCKET_S = 300
CHECK_LABELS: dict[str, str] = {
    "pb_detection": "Protobuf · детекция (событие)",
    "pb_heartbeat": "Protobuf · пульс узла (HB)",
    "pb_mel": "Protobuf · спектрограмма Mel",
    "mqtt_v1": "MQTT v1 · detection|heartbeat",
}
CHECK_HINTS: dict[str, str] = {
    "pb_detection": "На плате: Cloud ON + детекция. Event-only PB (без GPS/SPL).",
    "pb_heartbeat": "HB ~45с: status, SPL, noise, GPS, uptime (PB).",
    "pb_mel": "mel_upload≠0; ≥120с; mute при открытом WebUI.",
    "mqtt_v1": "На Mock: форма MQTT → host/port существующего брокера; на плате mqtt_enabled.",
}


def _forward_enqueue(*args, **kwargs):
    hub = _hub()
    fn = getattr(hub, "_forward_enqueue", None) if hub is not None else None
    if fn is not None:
        return fn(*args, **kwargs)
    print("[hub-fwd] lora drop: hub module not bound", flush=True)


def _unlock_hash_ok(*args, **kwargs):
    fn = getattr(_hub(), "_unlock_hash_ok", None)
    if fn is not None:
        return fn(*args, **kwargs)


@dataclass 
class CheckItem :
    key :str 
    ok :bool =False 
    count :int =0 
    last_error :str =""
    last_meta :dict [str ,Any ]=field (default_factory =dict )
    last_at :str =""


def _now_iso ()->str :
    return datetime .now (timezone .utc ).isoformat ()


def _iso_age_s (ts :str |None )->float |None :
    if not ts :
        return None 
    try :
        last =datetime .fromisoformat (str (ts ).replace ("Z","+00:00"))
        if last .tzinfo is None :
            last =last .replace (tzinfo =timezone .utc )
        return (datetime .now (timezone .utc )-last ).total_seconds ()
    except (TypeError ,ValueError ):
        return None 



_INGEST_KINDS =("hb","det","mel","mqtt_hb","mqtt_det")


def _ingest_zeros (slots :int )->dict [str ,list [int ]]:
    return {k :[0 ]*slots for k in _INGEST_KINDS }


def _ingest_hist_shell (
slots :int ,
now :float ,
node_id :str |None ,
bands :dict [str ,list [int ]],
)->dict [str ,Any ]:
    out :dict [str ,Any ]={
    "bucket_s":INGEST_TIMELINE_BUCKET_S ,
    "window_s":INGEST_TIMELINE_WINDOW_S ,
    "slots":slots ,
    "now":now ,
    "node_id":node_id ,
    }
    for k in _INGEST_KINDS :
        out [k ]=bands .get (k )or [0 ]*slots 
    return out 


def build_ingest_histograms (
ingest_log :list [dict [str ,Any ]],
*,
now :float |None =None ,
)->tuple [dict [str ,Any ],dict [str ,dict [str ,Any ]]]:
    """Один проход: агрегат + per-node 24h гистограммы."""
    slots =INGEST_TIMELINE_WINDOW_S //INGEST_TIMELINE_BUCKET_S 
    now_f =time .time ()if now is None else now 
    window_start =now_f -INGEST_TIMELINE_WINDOW_S 
    agg_b =_ingest_zeros (slots )
    buckets :dict [str ,dict [str ,list [int ]]]={}

    def ensure (nid :str )->dict [str ,list [int ]]:
        if nid not in buckets :
            buckets [nid ]=_ingest_zeros (slots )
        return buckets [nid ]

    for ev in ingest_log :
        t =ev .get ("t")
        if not isinstance (t ,(int ,float ))or t <window_start :
            continue 
        idx =int ((float (t )-window_start )/INGEST_TIMELINE_BUCKET_S )
        if idx ==slots :
            idx =slots -1 
        if idx <0 or idx >=slots :
            continue 
        kind =ev .get ("kind")
        if kind not in _INGEST_KINDS :
            continue 
        agg_b [kind ][idx ]+=1 
        nid =_canon_node_id (ev .get ("node_id")or "")
        if not nid :
            continue 
        ensure (nid )[kind ][idx ]+=1 

    agg =_ingest_hist_shell (slots ,now_f ,None ,agg_b )
    by ={
    nid :_ingest_hist_shell (slots ,now_f ,nid ,b )
    for nid ,b in buckets .items ()
    }
    return agg ,by 


def build_ingest_timeline (
ingest_log :list [dict [str ,Any ]],
node_id :str |None =None ,
)->dict [str ,Any ]:
    """24h sliding window, 5-min buckets (288 slots), counts by server receive time.

    ``node_id`` — канонический id (как PB ``746E8C``); None = все узлы.
    """
    agg ,by =build_ingest_histograms (ingest_log )
    if not node_id :
        return agg 
    want =_canon_node_id (node_id )
    if want in by :
        return by [want ]
    slots =INGEST_TIMELINE_WINDOW_S //INGEST_TIMELINE_BUCKET_S 
    return _ingest_hist_shell (slots ,agg ["now"],want ,_ingest_zeros (slots ))


def build_ingest_timelines_by_node (
ingest_log :list [dict [str ,Any ]],
)->dict [str ,dict [str ,Any ]]:
    """Per-node 24h histograms (same buckets as aggregate)."""
    _ ,by =build_ingest_histograms (ingest_log )
    return by 



# 2.5 × MQTT HB (60 s): loitering from firmware expires if HB stops.
LOITER_FRESH_S =150.0 


@dataclass 
class NodeRecord :
    node_id :str 
    first_seen :str 
    last_seen :str 
    transports :set [str ]=field (default_factory =set )
    transport_at :dict [str ,str ]=field (default_factory =dict )
    counts :dict [str ,int ]=field (default_factory =dict )
    last_detection :dict [str ,Any ]|None =None 
    last_heartbeat :dict [str ,Any ]|None =None 
    last_mel :dict [str ,Any ]|None =None 
    firmware_version :str |None =None 
    firmware_version_at :str |None =None 
    xvf_firmware_version :str |None =None 
    xvf_firmware_version_at :str |None =None 
    nn_model_version :str |None =None 
    nn_model_version_at :str |None =None 
    free_heap_min :float |None =None 
    threat_series :list [float ]=field (default_factory =list )
    rssi_series :list [float ]=field (default_factory =list )
    heap_series :list [float ]=field (default_factory =list )
    conf_series :list [float ]=field (default_factory =list )
    errors :list [dict [str ,Any ]]=field (default_factory =list )
    loitering :bool =False 
    presence_s :int |None =None 
    loitering_at :str |None =None 
    loitering_since :str |None =None 

    def note_loitering (self ,on :bool ,presence_s :int |None ,at :str )->None :
        if not on :
            self .loitering =False 
            self .presence_s =None 
            self .loitering_since =None 
            self .loitering_at =at 
            return 
        prev_age =_iso_age_s (self .loitering_at )if self .loitering_at else None 
        fresh =bool (
            self .loitering and prev_age is not None and prev_age <=LOITER_FRESH_S
        )
        if not fresh or not self .loitering_since :
            self .loitering_since =at 
        self .loitering =True 
        self .presence_s =presence_s 
        self .loitering_at =at 

    def loitering_view (self )->dict [str ,Any ]:
    # MQTT HB ~60 s carries the flag only while true; a stale flag must expire.
    # Compact LoRa несёт только флаг (без presence_s) — не синтезировать минуты
    # с часов Hub (loitering_since): иначе оператор видит «растущие» минуты устройства.
        age =_iso_age_s (self .loitering_at )
        live =bool (self .loitering and age is not None and age <=LOITER_FRESH_S )
        pres =self .presence_s 
        if live and pres is not None and age is not None :
            pres =int (pres +age )
        elif live :
            pres =None 
        return {"loitering":live ,"presence_s":pres if live else None ,"loitering_at":self .loitering_at }

    def push_series (self ,name :str ,val :float |None ,maxlen :int =90 )->None :
        if val is None or not math .isfinite (float (val )):
            return 
        series =getattr (self ,name )
        series .append (float (val ))
        del series [:-maxlen ]

    def touch_transport (self ,name :str ,at :str |None =None )->None :
        if not name :
            return 
        ts =at or _now_iso ()
        self .transports .add (name )
        self .transport_at [name ]=ts 

    def live_transports (self ,online_s :float =120.0 )->list [str ]:
        out :list [str ]=[]
        for name in sorted (self .transports ):
            win =240.0 if name =="lora"else online_s 
            age =_iso_age_s (self .transport_at .get (name ))
            if age is not None and age <=win :
                out .append (name )
        return out 

    def to_dict (self ,*,online_s :float =120.0 )->dict [str ,Any ]:
    # DIY ReportHeartbeat ~45s — окно 45s давало мерцание offline между тиками.
        age =None 
        try :
            last =datetime .fromisoformat (self .last_seen .replace ("Z","+00:00"))
            age =(datetime .now (timezone .utc )-last ).total_seconds ()
        except ValueError :
            age =None 
        threat =None 
        if self .last_detection and self .last_detection .get ("threat")is not None :
            threat =self .last_detection .get ("threat")
        joined =None 
        if self .last_detection :
            joined =join_detection_with_hb (self .last_detection ,self .last_heartbeat )
        hb_via =(self .last_heartbeat or {}).get ("via")
        ext =(self .last_heartbeat or {}).get ("extensions")or {}
        if isinstance (ext ,dict )and str (ext .get ("via")or "").strip ()=="lora_gw":
            hb_via ="lora_gw"
        lora_age =_iso_age_s (self .transport_at .get ("lora"))
        win =240.0 if (hb_via =="lora_gw"or (lora_age is not None and lora_age <=240.0 ))else online_s 
        online =age is not None and age <=win 
        hb =self .last_heartbeat or {}
        # Live RSSI/heap only from the transport that last wrote HB. Series keep
        # history for charts; PB must not present stale MQTT numbers as current.
        pb_live =hb .get ("health_source")=="pb"or hb .get ("via")=="pb_heartbeat"
        if pb_live :
            rssi =None 
            heap =None 
            heap_min =None 
            if hb .get ("rssi_dbm")is not None :
                try :
                    rssi =float (hb ["rssi_dbm"])
                except (TypeError ,ValueError ):
                    rssi =None 
            if hb .get ("free_heap")is not None :
                try :
                    heap =float (hb ["free_heap"])
                except (TypeError ,ValueError ):
                    heap =None 
                    # PB wire has no free_heap_min — do not show stale MQTT watermark.
            if hb .get ("free_heap_min")is not None :
                try :
                    heap_min =float (hb ["free_heap_min"])
                except (TypeError ,ValueError ):
                    heap_min =None 
        else :
            rssi =self .rssi_series [-1 ]if self .rssi_series else None 
            if rssi is None and hb .get ("rssi_dbm")is not None :
                try :
                    rssi =float (hb ["rssi_dbm"])
                except (TypeError ,ValueError ):
                    rssi =None 
            heap =self .heap_series [-1 ]if self .heap_series else None 
            if heap is None and hb .get ("free_heap")is not None :
                try :
                    heap =float (hb ["free_heap"])
                except (TypeError ,ValueError ):
                    heap =None 
            heap_min =self .free_heap_min 
            if heap_min is None and hb .get ("free_heap_min")is not None :
                try :
                    heap_min =float (hb ["free_heap_min"])
                except (TypeError ,ValueError ):
                    heap_min =None 
        health ={
        "online":online ,
        "rssi_dbm":rssi ,
        "free_heap":heap ,
        "free_heap_min":heap_min ,
        "weak_rssi":bool (rssi is not None and rssi <-75 ),
        "heap_low":bool (heap is not None and heap <22000 ),
        "heap_cliff":bool (
        (heap_min is not None and heap_min <8000 )
        or (heap is not None and heap <14000 )
        ),
        }
        return {
        "node_id":self .node_id ,
        "first_seen":self .first_seen ,
        "last_seen":self .last_seen ,
        "online":online ,
        "age_s":int (round (age ))if age is not None else None ,
        "transports":self .live_transports (win ),
        "counts":dict (self .counts ),
        "threat":threat ,
        "firmware_version":self .firmware_version ,
        "firmware_version_at":self .firmware_version_at ,
        "xvf_firmware_version":self .xvf_firmware_version ,
        "nn_model_version":self .nn_model_version ,
        "fw":self .firmware_version ,
        "last_detection":self .last_detection ,
        "last_heartbeat":self .last_heartbeat ,
        "joined_detection":joined ,
        "last_mel":self .last_mel ,
        "online_window_s":win ,
        "health":health ,
        "series":{
        "threat":self .threat_series [-90 :],
        "rssi":self .rssi_series [-90 :],
        "heap":self .heap_series [-90 :],
        "confidence":self .conf_series [-90 :],
        },
        "errors":self .errors [-10 :],
        **self .loitering_view (),
        }


class State :
    def __init__ (self ,token :str )->None :
        self .token =token 
        self .lock =threading .Lock ()
        self .started_at =_now_iso ()
        self .items ={
        "pb_detection":CheckItem ("pb_detection"),
        "pb_heartbeat":CheckItem ("pb_heartbeat"),
        "pb_mel":CheckItem ("pb_mel"),
        "mqtt_v1":CheckItem ("mqtt_v1"),
        "fog_track":CheckItem ("fog_track"),
        "fog_alert":CheckItem ("fog_alert"),
        }
        self .events :list [dict [str ,Any ]]=[]
        self .nodes :dict [str ,NodeRecord ]={}
        self .max_events =400 
        self .total_bytes =0 
        self .mqtt_recent :list [dict [str ,Any ]]=[]
        self .mqtt_status :str ="off"
        self .mesh_mqtt_status :str ="off"
        self .last_mel_toast :dict [str ,Any ]|None =None 
        self .ingest_log :list [dict [str ,Any ]]=[]
        self .max_ingest =8000 
        self .episodes =EpisodeRegistry ()
        self .tracks =TrackEngine ()

    def _now_ms (self )->int :
        return int (time .time ()*1000 )

    def _upsert_episode_row (self ,ep :hub_episodes .Episode ,now :str )->None :
        """Insert or mutate timeline row for episode (caller holds lock)."""
        row =enrich_event (episode_event_dict (ep ,ts_iso =now ))
        row ["summary"]=build_event_summary (
        "episode",row .get ("meta")if isinstance (row .get ("meta"),dict )else {}
        )
        idx =find_event_index (self .events ,ep .episode_id )
        if idx >=0 :
            if idx !=0 :
                self .events .pop (idx )
                self .events .insert (0 ,row )
            else :
                self .events [0 ]=row 
        else :
            self .events .insert (0 ,row )
        del self .events [self .max_events :]

    def _close_stale_episodes (self ,now :str |None =None )->None :
        ts =now or _now_iso ()
        for ep in self .episodes .close_stale (self ._now_ms ()):
            self ._upsert_episode_row (ep ,ts )

    def _node (self ,node_id :str )->NodeRecord :
        now =_now_iso ()
        nid =_canon_node_id (node_id )
        if not nid :
            nid =(node_id or "").strip ()[:NODE_ID_RE_MAX ]
            # MQTT wire = nevod-746E8C, topic/PB = 746E8C → один узел.
        alias =f"nevod-{nid }"
        if alias !=nid and alias in self .nodes :
            other =self .nodes .pop (alias )
            existing =self .nodes .get (nid )
            if existing is None :
                other .node_id =nid 
                self .nodes [nid ]=other 
            else :
                _merge_node_into (existing ,other )
        n =self .nodes .get (nid )
        if not n :
            n =NodeRecord (node_id =nid ,first_seen =now ,last_seen =now )
            self .nodes [nid ]=n 
        else :
            n .node_id =nid 
        n .last_seen =now 
        return n 

    def _record_ingest (self ,kind :str ,node_id :str )->None :
        """hb|det|mel (PB) или mqtt_hb|mqtt_det. Caller holds lock."""
        if kind not in _INGEST_KINDS :
            return 
        _t =time .time ()
        _nid =node_id if isinstance (node_id ,str )else ""
        self .ingest_log .append ({"t":_t ,"kind":kind ,"node_id":_nid })
        if _g("STORE") is not None :
            try :
                _g("STORE") .append_ingest (kind ,_nid ,_t )
            except Exception :
                pass 
        cutoff =time .time ()-INGEST_TIMELINE_WINDOW_S 
        self .ingest_log =[
        e for e in self .ingest_log if float (e .get ("t",0 ))>=cutoff 
        ]
        if len (self .ingest_log )>self .max_ingest :
            self .ingest_log =self .ingest_log [-self .max_ingest :]

    def _persist_detection (self ,rec :dict [str ,Any ],* ,via :str ,node_id :str )->None :
        """Класс и пеленг в sqlite. Ошибка записи не рвёт ingest."""
        store =_g("STORE")
        if store is None or not hasattr (store ,"append_detection"):
            return 
        try :
            ts =rec .get ("timestamp_ms")
            try :
                ts_ms =int (ts )if ts is not None else 0 
            except (TypeError ,ValueError ):
                ts_ms =0 
            if ts_ms <1_000_000_000_000 :
                ts_ms =self ._now_ms ()
            mel =None 
            node =self .nodes .get (node_id )
            last_mel ={}
            if node is not None :
                lm =getattr (node ,"last_mel",None )
                if isinstance (lm ,dict ):
                    last_mel =lm 
            saved =last_mel .get ("saved_file")
            if isinstance (saved ,str )and saved :
                mel_ts =last_mel .get ("timestamp_ms")
                try :
                    mel_ms =int (mel_ts )if mel_ts is not None else ts_ms 
                except (TypeError ,ValueError ):
                    mel_ms =ts_ms 
                if abs (mel_ms -ts_ms )<=90_000 :
                    mel =saved 
            store .append_detection (
            rec ,via =via ,node_id =node_id ,ts_ms =ts_ms ,mel_file =mel 
            )
        except Exception :
            pass 

    def _attach_mel (self ,node_id :str ,ts :Any ,saved_file :str )->None :
        store =_g("STORE")
        if store is None or not hasattr (store ,"attach_mel_file"):
            return 
        try :
            ts_ms =int (ts )if ts is not None else self ._now_ms ()
        except (TypeError ,ValueError ):
            ts_ms =self ._now_ms ()
        try :
            store .attach_mel_file (node_id ,ts_ms ,saved_file )
        except Exception :
            pass 

    def mark (
    self ,
    key :str ,
    *,
    ok :bool ,
    error :str ="",
    meta :dict [str ,Any ]|None =None ,
    transport :str ="",
    body_len :int =0 ,
    )->None :
        now =_now_iso ()
        meta =meta or {}
        webhook_det :dict [str ,Any ]|None =None 
        adsb_ctx :dict [str ,Any ]|None =None 
        if key =="pb_detection"and _g("ADSB") is not None :
            try :
                ad =_g("ADSB") .public_dict ()
                adsb_ctx ={
                "civilian_in_radius":bool (ad .get ("civilian_in_radius")),
                "adsb_radius_m":ad .get ("radius_m"),
                "adsb_count_in_radius":(ad .get ("meta")or {}).get (
                "adsb_count_in_radius"
                ),
                }
            except Exception :
                adsb_ctx =None 
        with self .lock :
            item =self .items [key ]
            item .count +=1 
            item .last_at =now 
            self .total_bytes +=max (0 ,body_len )
            if ok :
                item .ok =True 
                item .last_error =""
                item .last_meta =meta 
            else :
                item .last_error =error 
                if meta :
                    item .last_meta =meta 
            node_id =meta .get ("node_id")
            if isinstance (node_id ,str )and node_id :
                node_id =_canon_node_id (node_id )or node_id .strip ()[:NODE_ID_RE_MAX ]
                meta ["node_id"]=node_id 
                node =self ._node (node_id )
                node .counts [key ]=node .counts .get (key ,0 )+1 
                if transport :
                    node .touch_transport (transport ,now )
                if not ok :
                    node .errors .insert (
                    0 ,{"ts":now ,"key":key ,"error":error }
                    )
                    del node .errors [20 :]
                elif key =="pb_detection":
                    prev =node .last_detection or {}
                    merged =dict (prev )
                    for k ,v in meta .items ():
                        if v is not None or k not in merged :
                            merged [k ]=v 
                    if adsb_ctx is not None :
                        civ =bool (adsb_ctx .get ("civilian_in_radius"))
                        meta ["civilian_in_radius"]=civ 
                        meta ["adsb_radius_m"]=adsb_ctx .get ("adsb_radius_m")
                        meta ["adsb_count_in_radius"]=adsb_ctx .get (
                        "adsb_count_in_radius"
                        )
                        merged ["civilian_in_radius"]=civ 
                        merged ["adsb_radius_m"]=meta .get ("adsb_radius_m")
                        merged ["adsb_count_in_radius"]=meta .get (
                        "adsb_count_in_radius"
                        )
                    merged ["via"]=key 
                    merged ["recv_at"]=now 
                    joined =join_detection_with_hb (merged ,node .last_heartbeat )
                    merged ["joined"]=joined .get ("joined")
                    merged ["join_reason"]=joined .get ("join_reason")
                    for k in ("spl_fast","noise_dbfs","lat","lon"):
                        if joined .get (k )is not None :
                            merged [k ]=joined [k ]
                    node .last_detection =merged 
                    node .push_series ("threat_series",meta .get ("threat"))
                    node .push_series ("conf_series",meta .get ("confidence")or meta .get ("p"))
                    if merged .get ("joined"):
                        self ._feed_track (merged ,node )
                        if _g("WEBHOOKS") is not None :
                            webhook_det =dict (merged )
                    if _g("STORE") is not None :
                        try :
                            _g("STORE") .upsert_node (node_id ,node .to_dict ())
                        except Exception :
                            pass 
                elif key =="pb_heartbeat":
                    prev =node .last_heartbeat or {}
                    # PB wire has no rssi/heap/gps_fix — do not inherit MQTT health
                    # as live fields (via=pb would otherwise show stale cliff/fix).
                    _mqtt_only_hb =(
                    "rssi_dbm",
                    "free_heap",
                    "free_heap_min",
                    "temp_c",
                    "cpu_mhz",
                    "cpu_load",
                    "infer_ms",
                    "arena_used_bytes",
                    "gps_fix",
                    "gps_source",
                    "gps_valid",
                    "health",
                    "fw",
                    "schema",
                    "node_position",
                    )
                    merged ={k :v for k ,v in prev .items ()if k not in _mqtt_only_hb }
                    for k ,v in meta .items ():
                        if v is not None or k not in merged :
                            merged [k ]=v 
                    merged ["via"]=key 
                    merged ["health_source"]="pb"
                    merged ["recv_at"]=now 
                    node .last_heartbeat =merged 
                    _apply_firmware_version (node ,merged .get ("firmware_version"),now )
                    _apply_xvf_firmware_version (node ,merged .get ("xvf_firmware_version"),now )
                    _apply_nn_model_version (node ,merged .get ("nn_model_version"),now )
                    node .push_series ("rssi_series",merged .get ("rssi_dbm"))
                    node .push_series ("heap_series",merged .get ("free_heap"))
                    hm =merged .get ("free_heap_min")
                    if hm is not None :
                        try :
                            node .free_heap_min =float (hm )
                        except (TypeError ,ValueError ):
                            pass 
                    if _g("STORE") is not None :
                        try :
                            _g("STORE") .upsert_node (node_id ,node .to_dict ())
                        except Exception :
                            pass 
                            # Re-join last DET against fresh HB
                    if node .last_detection :
                        j =join_detection_with_hb (node .last_detection ,merged )
                        for k in ("joined","join_reason","spl_fast","noise_dbfs","lat","lon"):
                            if k in j :
                                node .last_detection [k ]=j [k ]
                elif key =="pb_mel":
                    clean ={k :v for k ,v in meta .items ()if k !="_data"}
                    via =clean .get ("via")or key
                    node .last_mel ={**clean ,"via":via ,"recv_at":now }
                    self .last_mel_toast ={
                    "ts":now ,
                    "node_id":node_id ,
                    "saved_file":clean .get ("saved_file"),
                    "payload_bytes":clean .get ("payload_bytes")or clean .get ("wire_bytes"),
                    "num_bands":clean .get ("num_bands"),
                    "num_frames":clean .get ("num_frames"),
                    "via":via ,
                    }
            skip_feed_insert =False 
            if ok and key =="pb_detection"and isinstance (node_id ,str )and node_id :
                ep ,_act =apply_detection (
                self .episodes ,
                node_id =node_id ,
                now_ms =self ._now_ms (),
                class_name =meta .get ("class_name")or meta .get ("class"),
                threat =meta .get ("threat"),
                confidence =meta .get ("confidence",meta .get ("p")),
                azimuth_deg =meta .get ("azimuth_deg"),
                via ="pb",
                )
                self ._upsert_episode_row (ep ,now )
                self ._persist_detection (meta ,via ="pb",node_id =node_id )
                skip_feed_insert =True 
            elif ok and key =="pb_mel"and isinstance (node_id ,str )and node_id :
                ep_m ,mel_act =apply_mel (
                self .episodes ,
                node_id =node_id ,
                now_ms =self ._now_ms (),
                saved_file =meta .get ("saved_file"),
                via ="pb",
                )
                if mel_act =="attach"and ep_m is not None :
                    self ._upsert_episode_row (ep_m ,now )
                    skip_feed_insert =True 
                saved =meta .get ("saved_file")
                if isinstance (saved ,str )and saved :
                    self ._attach_mel (node_id ,meta .get ("timestamp_ms"),saved )
            if not skip_feed_insert :
                self .events .insert (
                0 ,
                enrich_event (
                {
                "ts":now ,
                "key":key ,
                "ok":ok ,
                "error":error ,
                "node_id":node_id ,
                "meta":_event_meta (meta ),
                }
                ),
                )
                del self .events [self .max_events :]
            if ok and key in ("pb_detection","pb_heartbeat","pb_mel"):
                kind ={
                "pb_detection":"det",
                "pb_heartbeat":"hb",
                "pb_mel":"mel",
                }[key ]
                _nid =node_id if isinstance (node_id ,str )else ""
                self ._record_ingest (kind ,_nid )
        if webhook_det is not None and _g("WEBHOOKS") is not None :
            _g("WEBHOOKS") .schedule_joined_detection (webhook_det )

    def reset (self ,*,clear_mel :bool =True )->dict [str ,Any ]:
        with self .lock :
            for it in self .items .values ():
                it .ok =False 
                it .count =0 
                it .last_error =""
                it .last_meta ={}
                it .last_at =""
            self .events .clear ()
            self .nodes .clear ()
            self .tracks =TrackEngine ()
            self .episodes .clear ()
            self .total_bytes =0 
            self .mqtt_recent .clear ()
            self .last_mel_toast =None 
            self .ingest_log .clear ()
            self .started_at =_now_iso ()
        mel_info =clear_saved_mel ()if clear_mel else {"removed":0 }
        return {"mel_removed":mel_info .get ("removed",0 )}

    def note_mqtt (self ,topic :str ,payload :bytes )->None :
        """MQTT v1: structured DET/HB (≡ PB path) + fw.version → node card."""
        text =""
        try :
            text =payload .decode ("utf-8",errors ="replace")[:800 ]
        except Exception :
            text =f"<{len (payload )} bytes>"
        now =_now_iso ()
        parts =[p for p in str (topic ).split ("/")if p ]
        topic_node =parts [1 ]if len (parts )>=3 and parts [0 ]=="nevod"else ""
        mqtt_kind =parts [2 ]if len (parts )>=3 and parts [0 ]=="nevod"else ""
        j :dict [str ,Any ]|None =None 
        try :
            parsed =json .loads (payload .decode ("utf-8"))
            if isinstance (parsed ,dict ):
                j =parsed 
        except Exception :
            j =None 
        entry :dict [str ,Any ]={
        "ts":now ,
        "topic":topic ,
        "bytes":len (payload ),
        "preview":text [:240 ],
        "ok":True ,
        }
        webhook_det :dict [str ,Any ]|None =None 
        lora_fwd :tuple [str ,bytes ,dict [str ,Any ]]|None =None 
        with self .lock :
            item =self .items ["mqtt_v1"]
            item .count +=1 
            item .ok =True 
            item .last_at =now 
            item .last_meta ={"topic":topic ,"bytes":len (payload )}
            self .total_bytes +=max (0 ,len (payload ))

            if j is not None and mqtt_kind =="heartbeat":
                node_id =_mqtt_node_id (j ,topic_node )
                if node_id :
                    node =self ._node (node_id )
                    node .touch_transport ("mqtt",now )
                    if mqtt_is_lora (j ):
                        node .touch_transport ("lora",now )
                    node .counts ["mqtt_heartbeat"]=node .counts .get ("mqtt_heartbeat",0 )+1 
                    prev =_parse_mqtt_heartbeat (j ,node_id ,now )
                    prev ["health_source"]="mqtt"
                    node .last_heartbeat =prev 
                    node .note_loitering (prev .get ("loitering"),prev .get ("presence_s"),now )
                    fw =prev .get ("firmware_version")
                    if fw :
                        _apply_firmware_version (node ,fw ,now )
                    xvf =prev .get ("xvf_firmware_version")
                    if xvf :
                        _apply_xvf_firmware_version (node ,xvf ,now )
                    nn =prev .get ("nn_model_version")
                    if nn :
                        _apply_nn_model_version (node ,nn ,now )
                    rssi =prev .get ("rssi_dbm")
                    heap =prev .get ("free_heap")
                    node .push_series (
                    "rssi_series",float (rssi )if rssi is not None else None 
                    )
                    node .push_series (
                    "heap_series",float (heap )if heap is not None else None 
                    )
                    hm =prev .get ("free_heap_min")
                    if hm is not None :
                        try :
                            node .free_heap_min =float (hm )
                        except (TypeError ,ValueError ):
                            pass 
                    if node .last_detection :
                        joined =join_detection_with_hb (node .last_detection ,prev )
                        for k in (
                        "joined",
                        "join_reason",
                        "spl_fast",
                        "noise_dbfs",
                        "lat",
                        "lon",
                        "hb_age_ms",
                        ):
                            if k in joined :
                                node .last_detection [k ]=joined [k ]
                    if mqtt_is_lora (j )and not looks_pro (
                    j ,j .get ("extensions"),prev .get ("device_type")
                    ):
                        packed =lora_mqtt_pb ("heartbeat",j )
                        if packed :
                            lora_fwd =(
                            packed [0 ],
                            packed [1 ],
                            {
                            "node_id":node_id ,
                            "lat":prev .get ("lat"),
                            "lon":prev .get ("lon"),
                            "timestamp_ms":prev .get ("timestamp_ms"),
                            "path_lora":True ,
                            },
                            )
                    meta ={
                    "topic":topic ,
                    "bytes":len (payload ),
                    "node_id":node_id ,
                    "schema":prev .get ("schema"),
                    "status":prev .get ("status"),
                    "rssi_dbm":rssi ,
                    "free_heap":heap ,
                    "free_heap_min":prev .get ("free_heap_min"),
                    "temp_c":prev .get ("temp_c"),
                    "cpu_mhz":prev .get ("cpu_mhz"),
                    "cpu_load":prev .get ("cpu_load"),
                    "infer_ms":prev .get ("infer_ms"),
                    "arena_used_bytes":prev .get ("arena_used_bytes"),
                    "noise_dbfs":prev .get ("noise_dbfs"),
                    "spl_fast":prev .get ("spl_fast"),
                    "uptime_s":prev .get ("uptime_s"),
                    "firmware_version":fw or node .firmware_version ,
                    "device_type":prev .get ("device_type"),
                    "lat":prev .get ("lat"),
                    "lon":prev .get ("lon"),
                    "alt_m":prev .get ("alt_m"),
                    "install_height_m":prev .get ("install_height_m"),
                    "alt_ref":prev .get ("alt_ref"),
                    "health":prev .get ("health"),
                    "fw":prev .get ("fw"),
                    "node_position":prev .get ("node_position"),
                    }
                    item .last_meta =meta 
                    entry .update (
                    {
                    "key":"mqtt_heartbeat",
                    "node_id":node_id ,
                    "meta":_event_meta (meta ),
                    }
                    )
                    self .events .insert (
                    0 ,
                    enrich_event (
                    {
                    "ts":now ,
                    "key":"mqtt_heartbeat",
                    "ok":True ,
                    "error":"",
                    "node_id":node_id ,
                    "meta":_event_meta (meta ),
                    }
                    ),
                    )
                    del self .events [self .max_events :]
                    self ._record_ingest ("mqtt_hb",node_id )
                    if _g("STORE") is not None :
                        try :
                            _g("STORE") .upsert_node (node_id ,node .to_dict ())
                        except Exception :
                            pass 

            elif j is not None and mqtt_kind =="detection":
                node_id =_mqtt_node_id (j ,topic_node )
                if node_id :
                    merged =_parse_mqtt_detection (j ,node_id ,now )
                    fw =merged .get ("firmware_version")
                    node =self ._node (node_id )
                    node .touch_transport ("mqtt",now )
                    if mqtt_is_lora (j ):
                        node .touch_transport ("lora",now )
                    node .counts ["mqtt_detection"]=node .counts .get ("mqtt_detection",0 )+1 
                    if fw :
                        _apply_firmware_version (node ,fw ,now )
                    joined =join_detection_with_hb (merged ,node .last_heartbeat )
                    merged ["joined"]=joined .get ("joined")
                    merged ["join_reason"]=joined .get ("join_reason")
                    for k in ("spl_fast","noise_dbfs","lat","lon","hb_age_ms"):
                        if joined .get (k )is not None :
                            merged [k ]=joined [k ]
                    node .last_detection =merged 
                    node .note_loitering (merged .get ("loitering"),merged .get ("presence_s"),now )
                    if mqtt_is_lora (j )and not looks_pro (
                    j ,j .get ("extensions"),merged .get ("device_type")
                    ):
                        packed =lora_mqtt_pb ("detection",j )
                        if packed :
                            lora_fwd =(
                            packed [0 ],
                            packed [1 ],
                            {
                            "node_id":node_id ,
                            "lat":merged .get ("lat"),
                            "lon":merged .get ("lon"),
                            "timestamp_ms":merged .get ("timestamp_ms"),
                            "path_lora":True ,
                            },
                            )
                    node .push_series ("threat_series",merged .get ("threat"))
                    node .push_series ("conf_series",merged .get ("confidence"))
                    meta ={
                    "topic":topic ,
                    "bytes":len (payload ),
                    "node_id":node_id ,
                    "schema":merged .get ("schema"),
                    "msg_id":merged .get ("msg_id"),
                    "threat":merged .get ("threat"),
                    "class_name":merged .get ("class_name"),
                    "class_ru":merged .get ("class_ru"),
                    "class_id":merged .get ("class_id"),
                    "confidence":merged .get ("confidence"),
                    "early_warning":merged .get ("early_warning"),
                    "confirmed":merged .get ("confirmed"),
                    "alarm_tier":merged .get ("alarm_tier"),
                    "tone_agreed":merged .get ("tone_agreed"),
                    "azimuth_deg":merged .get ("azimuth_deg"),
                    "doa_confidence":merged .get ("doa_confidence"),
                    "bpf_hz":merged .get ("bpf_hz"),
                    "hps_score":merged .get ("hps_score"),
                    "rpm_valid":merged .get ("rpm_valid"),
                    "rpm":merged .get ("rpm"),
                    "blade_count":merged .get ("blade_count"),
                    "fusion_decision":merged .get ("fusion_decision"),
                    "detection_layers":merged .get ("detection_layers"),
                    "firmware_version":fw or node .firmware_version ,
                    "detection":merged .get ("detection"),
                    "target":merged .get ("target"),
                    "bearing":merged .get ("bearing"),
                    "extensions":merged .get ("extensions"),
                    "model":merged .get ("model"),
                    "window":merged .get ("window"),
                    }
                    item .last_meta =meta 
                    entry .update (
                    {
                    "key":"mqtt_detection",
                    "node_id":node_id ,
                    "meta":_event_meta (meta ),
                    }
                    )
                    ep ,_act =apply_detection (
                    self .episodes ,
                    node_id =node_id ,
                    now_ms =self ._now_ms (),
                    class_name =merged .get ("class_name"),
                    threat =merged .get ("threat"),
                    confidence =merged .get ("confidence"),
                    azimuth_deg =merged .get ("azimuth_deg"),
                    via ="mqtt",
                    )
                    self ._upsert_episode_row (ep ,now )
                    self ._record_ingest ("mqtt_det",node_id )
                    det_via ="lora"if mqtt_is_lora (j )else "mqtt"
                    self ._persist_detection (merged ,via =det_via ,node_id =node_id )
                    if merged .get ("joined"):
                        self ._feed_track (merged ,node )
                        if _g("WEBHOOKS") is not None :
                            webhook_det =dict (merged )
                    if _g("STORE") is not None :
                        try :
                            _g("STORE") .upsert_node (node_id ,node .to_dict ())
                        except Exception :
                            pass 
            else :
                nid =_canon_node_id (topic_node )or topic_node 
                if nid :
                    entry ["node_id"]=nid 
                    # Unparsed / non DET|HB → единая лента с preview (не отдельный UI feed).
                self .events .insert (
                0 ,
                enrich_event (
                {
                "ts":now ,
                "key":"",
                "ok":True ,
                "error":"",
                "node_id":nid or None ,
                "transport":"mqtt",
                "kind":"",
                "parse_ok":False ,
                "meta":{
                "topic":topic ,
                "bytes":len (payload ),
                "preview":text [:240 ],
                },
                "summary":f"{topic } · {text [:80 ]}"if text else topic ,
                "via_label":"MQTT",
                }
                ),
                )
                del self .events [self .max_events :]

            self .mqtt_recent .insert (0 ,entry )
            del self .mqtt_recent [80 :]
        if webhook_det is not None and _g("WEBHOOKS") is not None :
            _g("WEBHOOKS") .schedule_joined_detection (webhook_det )
        if lora_fwd is not None :
            _forward_enqueue (lora_fwd [0 ],lora_fwd [1 ],lora_fwd [2 ])


    def snapshot (self )->dict [str ,Any ]:
    # Не брать STATE.lock до ADSB.public_dict(): ADSB poll может ждать STATE.lock.
        hub_adsb =_g("ADSB") .public_dict ()if _g("ADSB") else {"enabled":False }
        hub_zones =_g("ZONES") .public_dict ()if _g("ZONES") else {}
        _fwd = _g("FORWARDER")
        if _fwd is not None:
            # Dashboard paints forward every tick — probe once so UI is not stuck on «сохранён».
            try:
                _fwd.ensure_probed()
            except Exception:  # noqa: BLE001
                pass
            hub_forward = _fwd.public_dict(include_origin=False)
        else:
            hub_forward = {"enabled": False}
        hub_webhooks =_g("WEBHOOKS") .public_config ()if _g("WEBHOOKS") else {"channels":[]}
        hub_redis_d =hub_redis .public_dict ()
        mqtt_d =(
        _g("MQTT") .public_dict ()
        if _g("MQTT") is not None 
        else {
        "enabled":False ,
        "host":"",
        "port":1883 ,
        "username":"",
        "has_password":False ,
        "topics":list (MQTT_TOPICS_DEFAULT ),
        "status":{},
        }
        )
        mel_saved =list_saved_mel (limit =40 )
        node_labels =_g("STORE") .all_node_labels ()if _g("STORE") is not None else {}
        webhook_del =_g("STORE") .recent_webhooks (30 )if _g("STORE") else []
        with self .lock :
            self ._close_stale_episodes ()
            checks ={
            k :{
            "ok":v .ok ,
            "count":v .count ,
            "last_error":v .last_error ,
            "last_meta":v .last_meta ,
            "last_at":v .last_at ,
            "label":CHECK_LABELS .get (k ,k ),
            "hint":CHECK_HINTS .get (k ,""),
            }
            for k ,v in self .items .items ()
            }
            all_core =all (
            checks [k ]["ok"]
            for k in ("pb_detection","pb_heartbeat","pb_mel")
            )
            ingest_tl ,ingest_by =build_ingest_histograms (self .ingest_log )
            nodes =[n .to_dict ()for n in self .nodes .values ()]
            attach_node_labels (nodes ,node_labels )
            try :
                import hub_mesh_adapter
                mel_rx =hub_mesh_adapter .mel_progress ()
            except Exception :
                mel_rx ={}
            for n in nodes :
                nid =str (n .get ("node_id")or "")
                ep =self .episodes .open_by_node .get (nid )
                n ["active_episode"]=ep .to_public ()if ep is not None else None 
                n ["lora_mel_rx"]=mel_rx .get (nid )
                # Online first, then newest last_seen.
            nodes .sort (
            key =lambda x :(not x .get ("online"),-(x .get ("age_s")is not None ),x .get ("age_s")or 1e9 )
            )
            online =sum (1 for n in nodes if n ["online"])
            online_ids =[n ["node_id"]for n in nodes if n ["online"]]
            self .tracks .set_civil (civil_aircraft_from_adsb (hub_adsb ))
            _track_now =self ._now_ms ()
            track_rows =self .tracks .snapshot (_track_now )
            track_drops =self .tracks .dropped_ids (_track_now )
            out ={
            "service":"nevod_hub",
            "version":HUB_VERSION ,
            "started_at":self .started_at ,
            "ready_core_pb":all_core ,
            "node_count":len (nodes ),
            "online_count":online ,
            "online_nodes":online_ids ,
            "online_window_s":120.0 ,
            "total_bytes":self .total_bytes ,
            "checks":checks ,
            "checklist":[
            {
            "id":k ,
            "label":CHECK_LABELS .get (k ,k ),
            "hint":CHECK_HINTS .get (k ,""),
            "ok":checks [k ]["ok"],
            "count":checks [k ]["count"],
            "last_error":checks [k ]["last_error"],
            }
            for k in (
            "pb_detection",
            "pb_heartbeat",
            "pb_mel",
            "mqtt_v1",
            )
            ],
            "nodes":nodes ,
            "tracks":track_rows ,
            "recent":[enrich_event (e )for e in self .events [:80 ]],
            "mel_saved":mel_saved ,
            "mel_save_enabled":MEL_SAVE ,
            "last_mel_toast":self .last_mel_toast ,
            "mqtt_status":self .mqtt_status ,
            "mesh_mqtt_status":getattr(self,"mesh_mqtt_status","off"),
            "mqtt_recent":list (self .mqtt_recent [:40 ]),
            "mqtt":{
            **mqtt_d ,
            "status":self .mqtt_status ,
            },
            "ingest_timeline":ingest_tl ,
            "ingest_timeline_by_node":ingest_by ,
            "hub":{
            "webhooks":hub_webhooks ,
            # PB origin не в dashboard — только через /api/hub/cloud + unlock.
            "forward":hub_forward ,
            "adsb":hub_adsb ,
            "zones":hub_zones ,
            "engineer":{"unlock_set":bool (_unlock_hash_stored ())},
            "redis":hub_redis_d ,
            "webhook_deliveries":webhook_del ,
            "device_tokens_configured":len (_g("DEVICE_TOKENS") ),
            "require_device_token":_g("REQUIRE_DEVICE_TOKEN") ,
            "mel_lan_only":(
            bool (_g("ZONES") .mel_lan_only_global )
            if _g("ZONES") is not None 
            else os .environ .get ("HUB_MEL_LAN_ONLY","0").lower ()
            in ("1","true","yes")
            ),
            "dashboard_open":_g("DASHBOARD_OPEN") ,
            },
            }
            # DEM вне lock (сеть/кэш OpenTopoData).
        try :
            attach_node_tips (out ["nodes"])
        except Exception :
            pass 
        self ._publish_tracks (track_rows ,track_drops )
        return out 

    def _feed_track (self ,det :dict [str ,Any ],node :"NodeRecord")->None :
        try :
            hb =node .last_heartbeat or {}
            obs =observation_from_detection (
            det ,hb .get ("lat"),hb .get ("lon"),self ._now_ms ()
            )
            if obs :
                self .tracks .ingest (obs ,now_ms =self ._now_ms ())
        except Exception as exc :
            print (f"[hub] track ingest: {exc}",flush =True )

    def _publish_tracks (self ,tracks :list ,drops :list )->None :
        mqtt =_g ("MQTT")
        if mqtt is None :
            return 
        try :
            mqtt .publish_tracks (tracks ,drops )
        except Exception as exc :
            print (f"[hub] track mqtt: {exc}",flush =True )


def _merge_node_into (dst :"NodeRecord",src :"NodeRecord")->None :
    """Слить alias-узел в канонический (in-place)."""
    if src .first_seen and (not dst .first_seen or src .first_seen <dst .first_seen ):
        dst .first_seen =src .first_seen 
    if src .last_seen and (not dst .last_seen or src .last_seen >dst .last_seen ):
        dst .last_seen =src .last_seen 
    dst .transports |=src .transports 
    for k ,v in (src .transport_at or {}).items ():
        prev =dst .transport_at .get (k )
        if not prev or v >prev :
            dst .transport_at [k ]=v 
    for k ,v in src .counts .items ():
        dst .counts [k ]=dst .counts .get (k ,0 )+int (v or 0 )
    if src .firmware_version and (
    not dst .firmware_version 
    or (src .firmware_version_at or "")>=(dst .firmware_version_at or "")
    ):
        dst .firmware_version =src .firmware_version 
        dst .firmware_version_at =src .firmware_version_at 
    if src .xvf_firmware_version and (
    not dst .xvf_firmware_version 
    or (src .xvf_firmware_version_at or "")>=(dst .xvf_firmware_version_at or "")
    ):
        dst .xvf_firmware_version =src .xvf_firmware_version 
        dst .xvf_firmware_version_at =src .xvf_firmware_version_at 
    if src .nn_model_version and (
    not dst .nn_model_version 
    or (src .nn_model_version_at or "")>=(dst .nn_model_version_at or "")
    ):
        dst .nn_model_version =src .nn_model_version 
        dst .nn_model_version_at =src .nn_model_version_at 
    for attr in ("last_detection","last_heartbeat","last_mel"):
        a ,b =getattr (dst ,attr ),getattr (src ,attr )
        if b and (not a or (b .get ("recv_at")or "")>=(a .get ("recv_at")or "")):
            setattr (dst ,attr ,b )
    for series in ("threat_series","rssi_series","heap_series","conf_series"):
        merged =list (getattr (dst ,series )or [])+list (getattr (src ,series )or [])
        setattr (dst ,series ,merged [-90 :])
        # Lifetime watermark: keep the lower non-None value across aliases.
    mins =[v for v in (dst .free_heap_min ,src .free_heap_min )if v is not None ]
    if mins :
        dst .free_heap_min =float (min (mins ))
    errs =list (dst .errors or [])+list (src .errors or [])
    dst .errors =errs [:20 ]



