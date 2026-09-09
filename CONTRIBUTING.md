# Contributing · Участие

## Русский

Спасибо, что смотрите на **UAV-radar / NEVOD** — распределённую DePIN-сеть акустических узлов для раннего оповещения.

### Как помочь сильнее всего (без кода)

1. **Собрать узел** по [DIY_GUIDE.md](docs/DIY_GUIDE.md) и рассказать в [Discussions](https://github.com/Gfermoto/UAV-radar/discussions) или Issue [`build-report`](../../issues/new/choose)
2. Следить за новостями проекта в Telegram: [t.me/UAV_radar](https://t.me/UAV_radar)
3. Привести соседа / второй угол — плотность = сила сети
4. Улучшить инструкцию: фото, неоднозначные шаги, битые ссылки, устаревшие релизы

### Что открыто для PR

- Прошивка **RTSP Mic** (`src/`, `BUILD.md`, тесты)
- Документация, BOM, корпус STL, лендинг (`docs/`)
- Инструменты HIL / скрипты без секретов

### Что не принимаем сюда

- Исходники модели детекции БПЛА и закрытой логики NEVOD DIY — только подписанный `.bin`
- Секреты, токены, приватные ключи, персональные координаты

### Процесс

1. Issue с проблемой или идеей (шаблоны в `.github/ISSUE_TEMPLATE/`)
2. Fork → ветка → PR в `main`
3. Для mic: по возможности `pio test` / native tests из [BUILD.md](BUILD.md)

Лицензия вкладов в открытую часть — [MIT](LICENSE), если не оговорено иное.

---

## English

Thanks for looking at **UAV-radar / NEVOD** — a distributed DePIN network of acoustic nodes for early warning.

### Highest-leverage help (no code)

1. **Build a node** with [DIY_GUIDE.md](docs/DIY_GUIDE.md) and share in [Discussions](https://github.com/Gfermoto/UAV-radar/discussions) or a [`build-report`](../../issues/new/choose) issue
2. Follow project news on Telegram: [t.me/UAV_radar](https://t.me/UAV_radar)
3. Recruit a neighbor / second angle — density is the product
4. Patch the guide: photos, ambiguous steps, dead links, stale releases

### Welcome PRs

Open **RTSP Mic** firmware, docs, BOM, enclosure STL, landing page, non-secret tooling.

### Out of scope here

UAV detection model / closed NEVOD DIY logic (signed `.bin` only). No secrets or personal coordinates in PRs.

### Process

Open an Issue → PR to `main`. Mic changes: run tests when practical. Contributions to the open tree are MIT unless stated otherwise.
