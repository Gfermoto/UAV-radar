# Contributing · Участие

## Русский

Спасибо, что смотрите на **UAV-radar / NEVOD**.

### Что открыто для PR

- Прошивка **RTSP Mic** (`src/`, `BUILD.md`, тесты)
- Документация, BOM, корпус STL, лендинг (`docs/`)
- Инструменты HIL / скрипты без секретов

### Что не принимаем сюда

- Исходники модели детекции БПЛА и закрытой логики NEVOD DIY — они в подписанном `.bin`, не в этом репозитории
- Секреты, токены, приватные ключи, персональные координаты

### Как помочь без кода

- Собрать узел и описать опыт в [Discussions](https://github.com/Gfermoto/UAV-radar/discussions) (когда включены) или Issue с меткой `build-report`
- Улучшить инструкцию [DIY_GUIDE.md](docs/DIY_GUIDE.md) — особенно фото и неоднозначные шаги
- Сообщить о битой ссылке / устаревшем релизе

### Процесс

1. Issue с проблемой или идеей (шаблоны в `.github/ISSUE_TEMPLATE/`)
2. Fork → ветка → PR в `main`
3. Для mic: по возможности `pio test` / native tests из README

Лицензия вкладов в открытую часть — [MIT](LICENSE), если не оговорено иное.

---

## English

### Welcome PRs

Open **RTSP Mic** firmware, docs, BOM, enclosure STL, landing page, non-secret tooling.

### Out of scope here

UAV detection model / closed NEVOD DIY logic (ships only as signed `.bin`). No secrets or personal coordinates in PRs.

### Process

Open an Issue → PR to `main`. Mic changes: run tests when practical. Contributions to the open tree are MIT unless stated otherwise.
