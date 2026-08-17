# UAV-radar · NEVOD

**[English](#english)** · **[Русский](#русский)**

Acoustic listening for **UAVs** on DIY hardware — and an open RTSP microphone you can also use for birds and nature.

Hardware: Seeed XIAO ESP32-S3 + XMOS XVF3800.

---

<a id="english"></a>
## English

### What this is

**NEVOD** is a DIY acoustic network: simple outdoor nodes that listen for **mechanical sound in the air** (UAVs / drones). This repository is the public face of that work.

Inside you will find:

| Piece | What you get |
|-------|----------------|
| **Open mic firmware** (this tree, [MIT](LICENSE)) | Capture → Opus → RTSP + WebUI. Flash it and stream audio on your LAN. |
| **NEVOD sensor firmware** | Detection stays closed-source. A signed `.bin` will appear in GitHub Releases for people building a node. |

If you only need a networked mic — for birds, a garden, Home Assistant, an NVR — the open firmware is enough. Detection of UAVs is the NEVOD path.

![MIC DEV GUI](./gui.jpg)

### How it started

I built this first as a **bird and nature mic**: listen to the sky over RTSP on my own board. The same passive listening turned out useful when I started worrying about what else flies overhead. So the project grew from birds toward **UAV acoustics** — same hardware, clearer purpose for the network.

The repo name is that turn: birds opened it, UAV listening is why it is called UAV-radar.

### Related projects

| Project | Link | Notes |
|---------|------|--------|
| **BirdLense Hub** | [Gfermoto/BirdLense-Hub](https://github.com/Gfermoto/BirdLense-Hub) | Feeder camera / species — same interest in birds, different question |
| **BirdNET** | [kahst/BirdNET-Analyzer](https://github.com/kahst/BirdNET-Analyzer) · [Cornell BirdNET](https://birdnet.cornell.edu/) | Open bird ID from sound; a good sink for a nature RTSP stream from this mic |

### Mic features (open firmware)

- 16 kHz mono, XVF3800: direction of arrival (DoA), beamforming, MEL
- Opus and local RTSP (`:554`)
- A-weighted level, WebUI (`:80`), optional MQTT / Home Assistant
- Wi‑Fi; optional Ethernet (W5500)

### Quick start

```bash
pio run -e rtsp_mic
pio run -e rtsp_mic -t upload
pio device monitor -e rtsp_mic
```

- WebUI: `http://<ip>/` — Basic auth; default password `rtsp-mic-change-me` (**change it**)
- Stream: `rtsp://<ip>:554/`

Pins, build, tests: [BUILD.md](BUILD.md). Internals: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

### LED ring

RGB DoA ring on the mic (via XVF) and a separate Wi‑Fi status LED on the XIAO. Toggle the ring in WebUI or `POST /api/system/led` — [docs/LED.md](docs/LED.md).

### Docs

| File | About |
|------|--------|
| [BUILD.md](BUILD.md) | Build, flash, pins, tests |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Cores, audio path, tasks |
| [docs/API_REFERENCE.md](docs/API_REFERENCE.md) | MQTT, NVS, commands |
| [openapi-webui.yaml](openapi-webui.yaml) | HTTP API |
| [tools/hil/README.md](tools/hil/README.md) | On-device checks |

---

<a id="русский"></a>
## Русский

### Что это

**NEVOD** — самодельная акустическая сеть: уличные узлы, которые слушают **механический шум в воздухе** (БПЛА / дроны). Этот репозиторий — открытая часть той работы.

| Часть | Что даёт |
|-------|----------|
| **Открытая прошивка микрофона** (это дерево, [MIT](LICENSE)) | Захват → Opus → RTSP и WebUI. Прошили — слушаете поток в LAN. |
| **Прошивка датчика NEVOD** | Детекция закрыта. Подписанный `.bin` появится в GitHub Releases для сборки узла. |

Нужен только сетевой микрофон — птицы, сад, Home Assistant, регистратор — достаточно открытой прошивки. Детекция БПЛА — путь NEVOD.

![MIC DEV GUI](./gui.jpg)

### Откуда взялось

Сначала это был **микрофон для птиц и природы**: слушать небо по RTSP на своём железе. Тот же пассивный слух пригодился, когда появилась обеспокоенность тем, что ещё летает над головой. Так проект вырос от записи природы к **акустике БПЛА** — железо то же, смысл сети яснее.

Имя репозитория — про этот поворот: птицы были началом, слушать БПЛА — причина названия UAV-radar.

### Соседние проекты

| Проект | Ссылка | Зачем |
|--------|--------|--------|
| **BirdLense Hub** | [Gfermoto/BirdLense-Hub](https://github.com/Gfermoto/BirdLense-Hub) | Кормушка и виды — тот же интерес к птицам, другой вопрос |
| **BirdNET** | [kahst/BirdNET-Analyzer](https://github.com/kahst/BirdNET-Analyzer) · [Cornell BirdNET](https://birdnet.cornell.edu/) | Открытое определение птиц по звуку; удобный приёмник для nature-потока с этого mic |

### Возможности mic (открытая прошивка)

- 16 kHz mono, XVF3800: пеленг (DoA), луч, MEL
- Opus и локальный RTSP (`:554`)
- уровень (A-weighted), WebUI (`:80`), по желанию MQTT / Home Assistant
- Wi‑Fi; опционально Ethernet (W5500)

### Быстрый старт

```bash
pio run -e rtsp_mic
pio run -e rtsp_mic -t upload
pio device monitor -e rtsp_mic
```

- WebUI: `http://<ip>/` — Basic; пароль по умолчанию `rtsp-mic-change-me` (**смените**)
- Поток: `rtsp://<ip>:554/`

Пины и сборка: [BUILD.md](BUILD.md). Устройство прошивки: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

### Кольцо светодиодов

RGB-кольцо DoA на микрофоне (через XVF) и отдельный светодиод Wi‑Fi на XIAO. Вкл/выкл кольца в WebUI или `POST /api/system/led` — [docs/LED.md](docs/LED.md).

### Документация

| Файл | О чём |
|------|--------|
| [BUILD.md](BUILD.md) | Сборка, прошивка, пины, тесты |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Ядра, звук, задачи |
| [docs/API_REFERENCE.md](docs/API_REFERENCE.md) | MQTT, NVS, команды |
| [openapi-webui.yaml](openapi-webui.yaml) | HTTP API |
| [tools/hil/README.md](tools/hil/README.md) | Проверки на плате |
