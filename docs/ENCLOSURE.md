# Корпус DIY v0.1.0-wip · DIY enclosure

**[Русский](#русский)** · **[English](#english)**

STL для печати корпуса узла NEVOD DIY (XIAO ESP32-S3 + XVF3800).

Файлы: [`enclosure/diy/v0.1.0-wip/stl/`](../enclosure/diy/v0.1.0-wip/stl/).  
Релиз: **[nevod-diy-enclosure-v0.1.0](https://github.com/Gfermoto/UAV-radar/releases/tag/nevod-diy-enclosure-v0.1.0)**.  
Смета: [BOM.md](BOM.md). Полная инструкция: [DIY_GUIDE.md](DIY_GUIDE.md).

Первый печатный drop — не финальный профиль: без Bambu `.3mf`. Метизы: [BOM.md](BOM.md) (O-ring 5/6 мм, M2 / M3×19).  
**Летний** комплект (корпус + летняя ветрозащита) годится для раздачи; **зимняя** ветрозащита — follow-up, не в этом STL. Лицензия: [MIT](../LICENSE).

---

<a id="русский"></a>
## Русский

| Файл | Деталь | Материал |
|------|--------|----------|
| `nevod-diy-body-v0.1.0.stl` | корпус (низ) | ASA (смета) или PETG |
| `nevod-diy-lid-v0.1.0.stl` | крышка | ASA или PETG |
| `nevod-diy-gasket-v0.1.0.stl` | прокладка | **TPU** |
| `nevod-diy-led-ring-v0.1.0.stl` | кольцо LED | прозрачный / матовый просвечивающийся ASA или PETG (**тот же состав**, что body/lid) |

Слой 0.2 мм — разумный старт. Infill / supports — по слайсеру.

### Постобработка (ASA / PETG)

1. Впаять закладные втулки **до** химии.  
2. Изнутри прокапать отверстия под LED, затем сгладить **снаружи** паром: **ASA** — ацетон; **PETG** — пар ТГФ, прокапывание LED‑отверстий изнутри **хлористым метиленом**.  
3. Отлёжка **≥ 24 ч**, потом сборка. TPU не обрабатывать растворителями.  
Вытяжка / перчатки; пары токсичны.

Полная сборка (с фото): [DIY_GUIDE.md](DIY_GUIDE.md) §7. Метизы: [BOM](BOM.md).

### SHA-256

```
671f0a112852af0fb3eb8720b0403683e8d2e520df3119085891fbfe2a8e7ea1  nevod-diy-body-v0.1.0.stl
11b0b0e0d9d5843eb146686fcda5b3b8654ab0def9992a3ef6737d5385d3d30b  nevod-diy-lid-v0.1.0.stl
efb9811aa72d92917db2e4675d3f1c93c0f4480cf7b29eb6c0801c0f765672bf  nevod-diy-gasket-v0.1.0.stl
4f9c97608a14d9871e6fac4c1d3b043041f57cd2ad26a419bc2e2810024499e4  nevod-diy-led-ring-v0.1.0.stl
```

---

<a id="english"></a>
## English

| File | Part | Material hint |
|------|------|----------------|
| `nevod-diy-body-v0.1.0.stl` | body (bottom) | ASA (BOM) or PETG |
| `nevod-diy-lid-v0.1.0.stl` | lid (top) | ASA or PETG |
| `nevod-diy-gasket-v0.1.0.stl` | gasket | **TPU** |
| `nevod-diy-led-ring-v0.1.0.stl` | LED ring | clear / matte translucent ASA or PETG (**same polymer** as body/lid) |

Layer 0.2 mm is a reasonable start. Infill / supports: your slicer.

**Summer** kit (enclosure + summer wind screen) is what ships for handout; **winter** wind protection is a follow-up, not in this STL.

### Post-processing (ASA / PETG)

1. Heat-set inserts **before** chemistry.  
2. Pre-drip LED holes from the inside, then vapor-smooth **outside**: **ASA** — acetone; **PETG** — THF vapor, LED holes dripped with **methylene chloride**.  
3. Rest **≥ 24 h**, then assemble. Do not solvent-treat TPU. Ventilate; fumes are hazardous.

Full assembly (with photos): [DIY_GUIDE.md](DIY_GUIDE.md) §7. Fasteners: [BOM](BOM.md).

### SHA-256

```
671f0a112852af0fb3eb8720b0403683e8d2e520df3119085891fbfe2a8e7ea1  nevod-diy-body-v0.1.0.stl
11b0b0e0d9d5843eb146686fcda5b3b8654ab0def9992a3ef6737d5385d3d30b  nevod-diy-lid-v0.1.0.stl
efb9811aa72d92917db2e4675d3f1c93c0f4480cf7b29eb6c0801c0f765672bf  nevod-diy-gasket-v0.1.0.stl
4f9c97608a14d9871e6fac4c1d3b043041f57cd2ad26a419bc2e2810024499e4  nevod-diy-led-ring-v0.1.0.stl
```
