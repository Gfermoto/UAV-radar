#!/usr/bin/env python3
"""TLS / self-signed cert helpers for Nevod Hub."""
from __future__ import annotations

import socket
import subprocess
from pathlib import Path

def _parse_cn_list(raw: str) -> list[str]:
    """HUB_CN/MOCK_CN: один хост/IP или список через запятую → SAN."""
    parts = [p.strip() for p in (raw or "").split(",")]
    return [p for p in parts if p]


def _san_entry(name: str) -> str:
    try:
        socket.inet_aton(name)
        return f"IP:{name}"
    except OSError:
        return f"DNS:{name}"


def _cert_covers_names(crt: Path, names: list[str]) -> bool:
    try:
        out = subprocess.check_output(
            ["openssl", "x509", "-in", str(crt), "-noout", "-subject", "-ext", "subjectAltName"],
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return False
    return all(n in out for n in names)


def ensure_self_signed(
    cert_dir: Path, common_name: str, *, extra_names: list[str] | None = None
) -> tuple[Path, Path]:
    cert_dir.mkdir(parents=True, exist_ok=True)
    key = cert_dir / "hub.key"
    crt = cert_dir / "hub.crt"
    # compat with older Mock volumes
    if not crt.is_file() and (cert_dir / "mock_cloud.crt").is_file():
        key = cert_dir / "mock_cloud.key"
        crt = cert_dir / "mock_cloud.crt"
    names = _parse_cn_list(common_name)
    if extra_names:
        for n in extra_names:
            if n and n not in names:
                names.append(n)
    if not names:
        names = [guess_lan_ip()]
    primary = names[0]
    if key.exists() and crt.exists() and _cert_covers_names(crt, names):
        return crt, key
    for p in (key, crt):
        if p.exists():
            p.unlink()
    san = ",".join(_san_entry(n) for n in names)
    conf = cert_dir / "openssl.cnf"
    conf.write_text(
        "[req]\ndistinguished_name=req_distinguished_name\nx509_extensions=v3_req\n"
        "prompt=no\n[req_distinguished_name]\nCN={cn}\n"
        "[v3_req]\nsubjectAltName={san}\nbasicConstraints=CA:TRUE\n".format(
            cn=primary, san=san
        ),
        encoding="utf-8",
    )
    cmd = [
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", str(key), "-out", str(crt), "-days", "825",
        "-config", str(conf),
    ]
    subprocess.check_call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f"[hub] issued cert CN={primary} SAN={san}", flush=True)
    return crt, key


def guess_lan_ip() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


def print_diy_config(
    *,
    host: str,
    port: int,
    https: bool,
    token: str,
    ca_path: Path | None,
    alt_hosts: list[str] | None = None,
) -> None:
    scheme = "https" if https else "http"
    hosts = [host] + [h for h in (alt_hosts or []) if h and h != host]
    base = f"{scheme}://{host}:{port}"
    print("\n=== DIY WebUI → Cloud (лаб) ===")
    if https:
        print(f"ingest_url:     {base}")
        for h in hosts[1:]:
            print(f"  (альтернатива) {scheme}://{h}:{port}")
        print(f"ingest_token:   {'задан' if (token or '').strip() else 'не задан'}")
        print("api_enabled:    ON   (Cloud PB via ingest_url + token)")
        print("mel_upload:     1 (on detect) или 2 (lab ~0.1 Hz / 10s)")
        print("grpc_host:      можно оставить пустым — PB берёт origin из ingest_url")
        if ca_path:
            print(f"tls_ca:         кнопка WebUI «Подтянуть CA с Cloud» → GET {base}/ca.crt")
            print(f"                (или вручную PEM из {ca_path})")
            print("                System → TLS CA / Cloud → Подтянуть CA")
        print("\nFail-closed: без tls_ca HTTPS PB с платы не уйдут.")
    else:
        print("HTTP-режим: WebUI отклонит ingest_url (только https://).")
        print("Для PB без TLS на DIY:")
        print(f"  grpc_host={host}")
        print("  grpc_port=80   → прошивка шлёт http://{host}/api/v1/pb/*")
        print("  (порт слушателя mock должен быть 80, нужен root/cap)")
        print(f"  либо проксируйте {port}→80 и слушайте :80")
        print(f"ingest_token: {'задан' if (token or '').strip() else 'не задан'}")
        print("ingest_url:  ОСТАВИТЬ ПУСТЫМ (иначе WebUI потребует https)")
        print("api_enabled: OFF (Cloud PB off без https ingest_url)")
    print(f"\nЧеклист: {base}/")
    print(f"Health:   {base}/api/ingest/health")
    print("================================\n")
