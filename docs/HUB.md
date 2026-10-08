# NEVOD Hub — установка и настройка

Панель участка. Узлы шлют события на Hub, Hub пересылает их в ваш кабинет [nevod.endorphine.agency](https://nevod.endorphine.agency). Аудио с узлов наружу не уходит. Один двор без нескольких узлов обходится и без Hub: хватает WebUI, MQTT и Home Assistant. Hub нужен, когда узлов несколько или события приходят по LoRa и сами до кабинета не доходят.

Образ публичный: `ghcr.io/gfermoto/uav-radar/hub:latest`. Логин в реестр не нужен. В каталоге `hub/` этого репозитория только файлы запуска (compose, `.env`, брокер). Исходников панели здесь нет.

**[English](#english)**

## Русский

### Что поставить

На компьютере в той же сети, что и узлы: Docker и Docker Compose. Открытый порт **9443**. Если есть шлюз LoRa — ещё **1883**.

### Ключ

Один ключ на учётную запись, из кабинета. Им пользуются все ваши узлы.

1. Откройте кабинет и скопируйте токен. Без слова `Bearer` и без пробела в начале.
2. На каждом узле: **Настройки → Интеграции → Cloud (HTTPS) → Токен Cloud** — вставьте его и сохраните. Часы узла должны быть зелёными (NTP), координаты — на вкладке «Система».
3. Тот же токен — в файл `.env` Hub, поле `INGEST_TOKEN`.

Узел по Wi-Fi или Ethernet прикладывает этот ключ к событию сам. Hub его не подменяет. Кадр LoRa ключа не несёт: для таких узлов впишите тот же токен в `HUB_FORWARD_TOKEN`. Пустое `HUB_FORWARD_TOKEN` ключом приёма не заполняется.

Ключ из свежей прошивки общий. Облако может принять такой узел и всё равно не показать его в кабинете: прибор остаётся ничей. Чужой личный токен узел закрепляет, и ваш ключ потом получает отказ, пока узел не отвяжут в кабинете. На узле и на Hub ставьте токен своей учётной записи.

### Установка

```bash
git clone https://github.com/Gfermoto/UAV-radar.git
cd UAV-radar/hub
cp .env.example .env
```

В `.env`:

| Поле | Что писать |
|------|------------|
| `HUB_CN` | LAN-адрес этой машины, как его видят узлы. Например `192.168.1.10` |
| `INGEST_TOKEN` | токен кабинета |
| `HUB_FORWARD_TOKEN` | тот же токен, если есть LoRa. Для одних узлов по Wi-Fi можно оставить пустым |
| `HUB_FORWARD_URL` | не меняйте: `https://nevod.endorphine.agency` |

```bash
docker compose pull
docker compose up -d
```

Шлюз LoRa на этой же машине:

```bash
docker compose --profile mqtt up -d
```

Шлюз публикует в брокер `HUB_CN:1883`, корень топика `msh/NEVOD/`. Карточка Hub читает `msh/NEVOD/2/e/`. Брокер в этой конфигурации без пароля: не открывайте 1883 в интернет.

Обновление: `docker compose pull && docker compose up -d`. Данные панели, сертификат и спектры лежат в томах и при обновлении не стираются.

### Первый вход

Откройте `https://HUB_CN:9443` (подставьте свой адрес). Браузер предупредит о сертификате: он выписан на этот адрес самой панелью, это ожидаемо. На первом экране задайте свой логин и пароль (пароль от 8 символов). Заводского пароля нет. С улицы пароль не сбрасывается.

Если забыли его на своей машине:

```bash
docker exec nevod-hub python3 -c "
from pathlib import Path
import hub_store, hub_auth
s = hub_store.HubStore(Path('/data/hub/hub.sqlite'))
hub_auth.bind_store(s)
hub_auth._ui_set('admin', 'новый-пароль-8')
print('ok')
"
```

Сменили `HUB_CN` — старый сертификат останется на прежний адрес. Остановите контейнер, удалите том `hub_certs` и запустите снова. Том с базой (`hub_data`) не удаляйте.

### Узлы

На каждом узле, **Настройки → Интеграции → Hub**:

1. Адрес `https://192.168.1.10:9443` — ваш `HUB_CN` и порт.
2. Галочка **«Сначала Hub»**.
3. Сохранить.

Пока Hub отвечает, события идут ему, и он пересылает их в кабинет. Если Hub недоступен, узел с заданным токеном пишет в кабинет напрямую. MQTT домашнего брокера эта галочка не выключает: локальные алерты и Home Assistant остаются как были.

В кабинете узел виден по короткому id из шести hex. Тревога появляется, когда узел её подтвердил.

### Что Hub не делает

- Не отправляет аудио.
- Не публикует домашние MQTT-сообщения в облако. В кабинет уходят события protobuf и спектр, если узел его прислал.
- Не лечит отказ «узел принадлежит другому владельцу» другим ключом. Это решается в кабинете, не сменой ключа на Hub.

---

<a id="english"></a>
## English

A site panel. Nodes send events to the Hub, and the Hub forwards them to your account at [nevod.endorphine.agency](https://nevod.endorphine.agency). Audio never leaves the nodes. One yard with a single node does not need a Hub: the web UI, MQTT, and Home Assistant are enough. Use the Hub when you have several nodes, or when events arrive over LoRa and cannot reach the account on their own.

The image is public: `ghcr.io/gfermoto/uav-radar/hub:latest`. No registry login. The `hub/` directory in this repo is only the run files (compose, `.env`, broker). The panel source is not here.

### What you need

A computer on the same LAN as the nodes, with Docker and Docker Compose. Open port **9443**. A LoRa gateway also needs **1883**.

### The key

One key per account, from the cabinet. Every node of yours uses it.

1. Open the account and copy the token. No `Bearer` word, no leading space.
2. On each node: **Settings → Integrations → Cloud (HTTPS) → Cloud token**. Paste it and save. The node clock must be green (NTP), and coordinates belong on the System tab.
3. Put the same token in the Hub `.env` as `INGEST_TOKEN`.

A Wi-Fi or Ethernet node attaches this key to the event itself. The Hub does not replace it. A LoRa frame carries no key: set the same token as `HUB_FORWARD_TOKEN`. An empty `HUB_FORWARD_TOKEN` is not filled from `INGEST_TOKEN`.

The key baked into a fresh flash is shared. The cloud may accept that node and still leave it out of the account. Someone else's personal token does bind the node, and yours is then refused until the node is unbound in the account. Put your own account token on the node and on the Hub.

### Install

```bash
git clone https://github.com/Gfermoto/UAV-radar.git
cd UAV-radar/hub
cp .env.example .env
```

| Field | Value |
|-------|--------|
| `HUB_CN` | This machine’s LAN address, as the nodes see it. Example: `192.168.1.10` |
| `INGEST_TOKEN` | the account token |
| `HUB_FORWARD_TOKEN` | the same token if you have LoRa. Wi-Fi-only sites can leave it empty |
| `HUB_FORWARD_URL` | leave `https://nevod.endorphine.agency` |

```bash
docker compose pull
docker compose up -d
```

LoRa gateway on the same machine:

```bash
docker compose --profile mqtt up -d
```

Point the gateway at `HUB_CN:1883`, topic root `msh/NEVOD/`. The Hub card reads `msh/NEVOD/2/e/`. This broker has no password: do not expose 1883 to the internet.

Update with `docker compose pull && docker compose up -d`. The panel database, certificate, and spectra stay in volumes.

### First login

Open `https://HUB_CN:9443`. The browser will warn about the certificate: the panel issued it for this address. On the first screen choose your own username and a password of at least 8 characters. There is no factory password. It cannot be reset from the internet.

If you forgot it on your own machine:

```bash
docker exec nevod-hub python3 -c "
from pathlib import Path
import hub_store, hub_auth
s = hub_store.HubStore(Path('/data/hub/hub.sqlite'))
hub_auth.bind_store(s)
hub_auth._ui_set('admin', 'new-password-8')
print('ok')
"
```

If you change `HUB_CN`, the old certificate still names the previous address. Stop the container, remove the `hub_certs` volume, and start again. Do not remove `hub_data`.

### Nodes

On each node, **Settings → Integrations → Hub**:

1. Address `https://192.168.1.10:9443` — your `HUB_CN` and port.
2. Enable **Hub first**.
3. Save.

While the Hub answers, events go there and it forwards them to the account. If the Hub is down, a node that has a token writes to the account directly. The checkbox does not turn off your home MQTT broker: local alerts and Home Assistant stay as they were.

The account shows the node by its six-hex id. An alert appears when the node confirms it.

### What the Hub does not do

- It does not upload audio.
- It does not publish home MQTT into the cloud. The account receives protobuf events and a spectrum when a node sent one.
- It does not fix “this node belongs to another owner” by trying a different key. That is settled in the account, not by swapping the Hub key.
