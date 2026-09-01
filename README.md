# UAV-radar · NEVOD

[![MIT](https://img.shields.io/badge/license-MIT-c8e84a?labelColor=0a1210)](LICENSE)
[![ESP32-S3](https://img.shields.io/badge/MCU-ESP32--S3-8fa094?labelColor=0a1210)](platformio.ini)
[![XVF3800](https://img.shields.io/badge/DSP-XMOS%20XVF3800-8fa094?labelColor=0a1210)](https://www.seeedstudio.com/)
[![Pages](https://img.shields.io/badge/site-landing-c8e84a?labelColor=0a1210)](https://gfermoto.github.io/UAV-radar/)
[![Release](https://img.shields.io/github/v/release/Gfermoto/UAV-radar?include_prereleases&label=latest&labelColor=0a1210&color=c8e84a)](https://github.com/Gfermoto/UAV-radar/releases)

**Свой акустический узел. Своя сеть. Без камеры.**  
DIY DePIN: железо у вас дома → слух локально или в народном радаре.

**[Landing](https://gfermoto.github.io/UAV-radar/)** ·
**[DIY guide](docs/DIY_GUIDE.md)** ·
**[BOM ≈ ₽9 800](docs/BOM.md)** ·
**[Releases](https://github.com/Gfermoto/UAV-radar/releases)** ·
**[Discussions](https://github.com/Gfermoto/UAV-radar/discussions)**

**[Русский](#русский)** · **[English](#english)**

---

<a id="русский"></a>
## Русский

### Зачем это DePIN

**NEVOD** — физическая сеть слуха: каждый узел принадлежит человеку, который его собрал. Нет подписки «чтобы слышать свой двор». Облако — **opt-in** для общего раннего оповещения; координаты нод **не публикуются**; владельцам — преференции сервиса.

| Ступень | Что делаете | Что получаете |
|---------|-------------|----------------|
| **1. Смотрите** | Landing + этот README | Понимание за 2 минуты |
| **2. Пробуете железо** | Открытый **RTSP Mic** ([MIT](LICENSE)) | Opus / RTSP / WebUI — сад, птицы, [BirdNET](https://github.com/kahst/BirdNET-Analyzer) |
| **3. Собираете узел** | [Смета](docs/BOM.md) + [корпус](docs/ENCLOSURE.md) + `nevod-diy-*.bin` | Детекция 3 классов БПЛА на плате, DoA, MQTT / Home Assistant |
| **4. Усиливаете сеть** | Cloud‑токен + координаты | Народный радар — больше узлов = меньше слепых зон |

Каждый новый двор — новый сенсор в общей ткани. Органический рост = DIY, не дата‑центр.

### Две прошивки — не смешивайте

| Часть | Что даёт |
|-------|----------|
| **Открытый mic** ([MIT](LICENSE), исходники здесь) | Звук → Opus → RTSP и WebUI. [v0.3.0](https://github.com/Gfermoto/UAV-radar/releases/tag/v0.3.0) · `rtsp-mic-0.3.0.bin` |
| **NEVOD DIY (датчик)** | Подписанный `.bin` с распознанием **трёх классов БПЛА** на ESP32-S3. [Releases `nevod-diy-*`](https://github.com/Gfermoto/UAV-radar/releases) · полная сборка [DIY_GUIDE.md](docs/DIY_GUIDE.md) |

Железо одно (XIAO ESP32-S3 + XVF3800). Сценарий выбираете прошивкой.

### Старт за вечер

1. **Смета ядра ≈ ₽9 793** (снимок) — [docs/BOM.md](docs/BOM.md)
2. **STL корпуса** — [enclosure release](https://github.com/Gfermoto/UAV-radar/releases/tag/nevod-diy-enclosure-v0.1.0)
3. **Прошивка + настройка + ветер / IP** — [docs/DIY_GUIDE.md](docs/DIY_GUIDE.md)
4. Собрали? Опишите опыт — [Discussions](https://github.com/Gfermoto/UAV-radar/discussions) или Issue [`build-report`](https://github.com/Gfermoto/UAV-radar/issues/new/choose)

Локально достаточно WebUI / MQTT / Home Assistant. Народный радар — когда готовы: [кабинет](https://nevod.endorphine.agency).

### Визуализация

**Три класса БПЛА (датчик)**

![Три класса БПЛА, распознаваемые бинарником NEVOD DIY](img/tri_klassa.png)

**MEL открытого mic — птица**

![Пример MEL-спектрограммы: пение птицы](img/bird.jpg)

**WebUI**

![Интерфейс WebUI: уровни, пеленг, MEL, DSP](img/gui.jpg)

### Материалы

| Материал | Где |
|----------|-----|
| Лендинг | [gfermoto.github.io/UAV-radar](https://gfermoto.github.io/UAV-radar/) |
| Смета | [docs/BOM.md](docs/BOM.md) |
| Корпус (STL) | [docs/ENCLOSURE.md](docs/ENCLOSURE.md) |
| Полная инструкция DIY | [docs/DIY_GUIDE.md](docs/DIY_GUIDE.md) |
| Бинарник датчика | [Releases `nevod-diy-*`](https://github.com/Gfermoto/UAV-radar/releases) |
| Открытый mic | [v0.3.0](https://github.com/Gfermoto/UAV-radar/releases/tag/v0.3.0) |
| Фото сборки | [`img/`](img/) |
| Участие | [CONTRIBUTING.md](CONTRIBUTING.md) |

### История

Сначала — RTSP‑слух неба для птиц. Затем та же плата — акустический мониторинг БПЛА. Имя **UAV-radar** — про этот вектор; **NEVOD** — про сеть узлов.

### Смежные проекты

| Проект | Ссылка | Зачем |
|--------|--------|-------|
| **BirdLense Hub** | [Gfermoto/BirdLense-Hub](https://github.com/Gfermoto/BirdLense-Hub) | Птицы / кормушка — другой канал |
| **BirdNET** | [kahst/BirdNET-Analyzer](https://github.com/kahst/BirdNET-Analyzer) | Виды по аудио с mic |

### Открытый mic — кратко

- 16 kHz mono, XVF3800: DoA, луч, MEL  
- Opus + RTSP `:554`, WebUI `:80`, опционально MQTT / HA  
- Сборка: `pio run -e rtsp_mic` · `pio run -e rtsp_mic -t upload` — детали в [BUILD.md](BUILD.md)

### Документация

| Файл | О чём |
|------|--------|
| [BUILD.md](BUILD.md) | Сборка, прошивка, пины, тесты |
| [docs/BOM.md](docs/BOM.md) | Смета узла |
| [docs/ENCLOSURE.md](docs/ENCLOSURE.md) | Корпус |
| [docs/DIY_GUIDE.md](docs/DIY_GUIDE.md) | Полный DIY |
| [docs/LED.md](docs/LED.md) | Кольцо DoA / статус |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Ядра, аудио, задачи |
| [docs/API_REFERENCE.md](docs/API_REFERENCE.md) | MQTT, NVS, команды |
| [openapi-webui.yaml](openapi-webui.yaml) | HTTP API |
| [SECURITY.md](SECURITY.md) | Уязвимости |
| [tools/hil/README.md](tools/hil/README.md) | Проверки на устройстве |

---

<a id="english"></a>
## English

### Why this is DePIN

**NEVOD** is a physical hearing network: every node is owned by the person who built it. No subscription just to listen to your own yard. The cloud is **opt-in** for shared early warning; node coordinates are **not published**; owners get service preferences.

| Step | You do | You get |
|------|--------|---------|
| **1. Browse** | Landing + this README | Clarity in two minutes |
| **2. Try the board** | Open **RTSP Mic** ([MIT](LICENSE)) | Opus / RTSP / WebUI — garden, birds, [BirdNET](https://github.com/kahst/BirdNET-Analyzer) |
| **3. Build a node** | [BOM](docs/BOM.md) + [enclosure](docs/ENCLOSURE.md) + `nevod-diy-*.bin` | On-device 3-class UAV detect, DoA, MQTT / Home Assistant |
| **4. Grow the mesh** | Cloud token + coordinates | People’s radar — more nodes, fewer blind spots |

Growth is DIY density, not a data center.

### Two firmwares — do not mix

| Piece | What you get |
|-------|----------------|
| **Open mic** ([MIT](LICENSE), sources here) | Audio → Opus → RTSP + WebUI. [v0.3.0](https://github.com/Gfermoto/UAV-radar/releases/tag/v0.3.0) · `rtsp-mic-0.3.0.bin` |
| **NEVOD DIY (sensor)** | Signed `.bin` with **three UAV classes** on ESP32-S3. [Releases `nevod-diy-*`](https://github.com/Gfermoto/UAV-radar/releases) · full build [DIY_GUIDE.md](docs/DIY_GUIDE.md) |

Same hardware (XIAO ESP32-S3 + XVF3800). Firmware picks the job.

### Start tonight

1. **Core BOM ≈ ₽9,793** (snapshot) — [docs/BOM.md](docs/BOM.md)  
2. **Enclosure STL** — [enclosure release](https://github.com/Gfermoto/UAV-radar/releases/tag/nevod-diy-enclosure-v0.1.0)  
3. **Flash + setup + wind / IP** — [docs/DIY_GUIDE.md](docs/DIY_GUIDE.md)  
4. Built one? Tell the story — [Discussions](https://github.com/Gfermoto/UAV-radar/discussions) or a [`build-report`](https://github.com/Gfermoto/UAV-radar/issues/new/choose) issue  

Local-only: WebUI / MQTT / Home Assistant. People’s radar when ready: [cabinet](https://nevod.endorphine.agency).

### Gallery

**Three UAV classes (sensor)**

![Three UAV classes recognized by the NEVOD DIY binary](img/tri_klassa.png)

**Open-mic MEL — bird**

![MEL spectrogram example: bird song](img/bird.jpg)

**WebUI**

![RTSP mic WebUI: levels, DoA, MEL, DSP](img/gui.jpg)

### Materials

| Material | Where |
|----------|-------|
| Landing | [gfermoto.github.io/UAV-radar](https://gfermoto.github.io/UAV-radar/) |
| BOM | [docs/BOM.md](docs/BOM.md) |
| Enclosure (STL) | [docs/ENCLOSURE.md](docs/ENCLOSURE.md) |
| Full DIY guide | [docs/DIY_GUIDE.md](docs/DIY_GUIDE.md) |
| Sensor binary | [Releases `nevod-diy-*`](https://github.com/Gfermoto/UAV-radar/releases) |
| Open mic | [v0.3.0](https://github.com/Gfermoto/UAV-radar/releases/tag/v0.3.0) |
| Build photos | [`img/`](img/) |
| Contributing | [CONTRIBUTING.md](CONTRIBUTING.md) |

### History

Started as RTSP sky-listening for birds. Same board later for UAV acoustic monitoring. **UAV-radar** names the direction; **NEVOD** names the node network.

### Related projects

| Project | Link | Purpose |
|---------|------|---------|
| **BirdLense Hub** | [Gfermoto/BirdLense-Hub](https://github.com/Gfermoto/BirdLense-Hub) | Birds / feeder — another channel |
| **BirdNET** | [kahst/BirdNET-Analyzer](https://github.com/kahst/BirdNET-Analyzer) | Species ID from mic audio |

### Open mic — short

- 16 kHz mono, XVF3800: DoA, beam, MEL  
- Opus + RTSP `:554`, WebUI `:80`, optional MQTT / HA  
- Build: `pio run -e rtsp_mic` · upload — see [BUILD.md](BUILD.md)

### Docs

| File | About |
|------|--------|
| [BUILD.md](BUILD.md) | Build, flash, pins, tests |
| [docs/BOM.md](docs/BOM.md) | Parts list |
| [docs/ENCLOSURE.md](docs/ENCLOSURE.md) | Enclosure |
| [docs/DIY_GUIDE.md](docs/DIY_GUIDE.md) | Full DIY |
| [docs/LED.md](docs/LED.md) | DoA ring / status LED |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Cores, audio, tasks |
| [docs/API_REFERENCE.md](docs/API_REFERENCE.md) | MQTT, NVS, commands |
| [openapi-webui.yaml](openapi-webui.yaml) | HTTP API |
| [SECURITY.md](SECURITY.md) | Vulnerability reporting |
| [tools/hil/README.md](tools/hil/README.md) | On-device checks |
