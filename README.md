# Alarm System on Blynk IoT + ESP32-C3

Проект реализует сирену на `ESP32-C3` с управлением через `Blynk IoT`.

Что умеет прошивка:
- включает сирену на заданное количество секунд;
- выключает сирену по `0` или по таймеру;
- публикует оставшееся время в секундах;
- поддерживает presets для простого UI и прямое API-управление.

## Datastream'ы

Обязательные:
- `V0` (`Integer`): API управления. Передайте число секунд для старта, `0` для остановки.
- `V1` (`Integer`): отображение оставшегося времени в секундах.

UI для человека:
- `V2` (`Integer` или `Enumerable`): preset menu `0..5`
- `V3` (`Integer`): кнопка `Start`
- `V4` (`Integer`): кнопка `Stop`
- `V5` (`Integer`, опционально): состояние сирены, `1` или `0`

## Рекомендованный UI в Blynk

Сделайте на дашборде:
- `Value Display` или `Labeled Value` для `V1`
- `Menu` или `Dropdown` для `V2`
- кнопку `Start` на `V3` в режиме `Push`
- кнопку `Stop` на `V4` в режиме `Push`
- для обеих кнопок включите подтверждение (`confirmation`)
- при необходимости добавьте `LED` или `State` на `V5`

Preset items для `V2` должны идти строго в таком порядке:
- `0` -> `5s`
- `1` -> `10s`
- `2` -> `30s`
- `3` -> `60s`
- `4` -> `5m`
- `5` -> `30m`

Логика:
- пользователь выбирает preset в `V2`
- нажимает `Start`
- контроллер запускает сирену на соответствующее время
- `V1` показывает оставшееся число секунд
- `Stop` немедленно выключает сирену

Если нужно управление из внешней автоматизации, используйте только `V0`:
- `V0 = N` запускает сирену на `N` секунд
- `V0 = 0` выключает сирену

## Настройка прошивки

Перед сборкой задайте свой Blynk template:

```bash
idf.py menuconfig
```

Нужно заполнить:
- `Component config -> Blynk.Edgent -> Template ID`
- `Component config -> Blynk.Edgent -> Template Name`

При необходимости настройте:
- `Alarm system -> Siren GPIO`
- `Alarm system -> Siren active GPIO level`

Плейсхолдеры в `sdkconfig`:
- `TMPLXXXXXX`
- `Alarm System`

их нужно заменить на реальные значения вашего Blynk Template.

## Сборка

```bash
./idf.sh build
./idf.sh -p /dev/ttyACM0 flash monitor
```

Если увидите ошибку вида:

```text
Tool doesn't match supported version from list ['esp-14.2.0_20260121']
... esp-14.2.0_20251107 ...
```

то у вас локальный `ESP-IDF` уже ожидает новый toolchain, а в окружении остался старый.
В этом проекте `./idf.sh` заранее экспортирует `IDF_MAINTAINER=1`, чтобы такая проверка стала предупреждением, а не фатальной ошибкой.

Если хотите исправить окружение правильно, а не обходить проверку, переустановите инструменты для текущего `ESP-IDF`:

```bash
/home/gena/esp/esp-idf/install.sh esp32c3
```

После этого очистите каталог сборки и пересоберите проект:

```bash
rm -rf build
./idf.sh build
```

## Замечание по памяти flash

В проекте включена разметка под `4MB` flash и OTA-разделы Blynk.
Если у вашей платы другой объём flash, скорректируйте [sdkconfig](/home/gena/work/alarm_system/sdkconfig:1) и [partitions.csv](/home/gena/work/alarm_system/partitions.csv:1).
