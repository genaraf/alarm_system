# Alarm System on Blynk IoT + ESP32-C3

Проект реализует сирену на `ESP32-C3` с управлением через `Blynk IoT`.

## Hardware

Референсная аппаратная конфигурация для этого проекта:

- Плата: [ESP32-C3 Super Mini](https://www.espboards.dev/esp32/esp32-c3-super-mini/)
- Ключ питания нагрузки: [Pololu Mini MOSFET Slide Switch with Reverse Voltage Protection, SV (item 2811)](https://www.pololu.com/product/2811)
- Понижающий преобразователь питания: [Pololu 5V, 2.5A Step-Down Voltage Regulator D24V25F5 (item 2850)](https://www.pololu.com/product/2850)

### Схема подключения

Ниже референсная схема для варианта, где входное питание `VIN = 12V`, оно подается на `Pololu 2850`, а сирена включается через `Pololu 2811`.

```text
      Внешнее питание 12 V DC
      +-------------------------------+
      |                               |
      |   +-----------------------+   |
VIN+--+-->| Pololu 2850 VIN       |   |
GND ---+->| Pololu 2850 GND       |   |
         |                       5V|---+-----> ESP32-C3 Super Mini 5V
         |                    GND  |---+-----> ESP32-C3 Super Mini GND
         |                    EN   |---x  (не подключать)
         +-----------------------+-+   |
                                     5V|
                                       |
                                       v
                         +-------------------------------+
                         | Pololu 2811                   |
                         |                               |
                         | VIN  <------------------- 5V  |
                         | GND  <------------------ GND  |
ESP32 GPIO4 ------------>| ON                            |
                         | VOUT ---------------------> + Сирена
                         +-------------------------------+
                                                      |
GND --------------------------------------------------+-----> - Сирена

ESP32-C3 Super Mini:
- GPIO9 -> штатная кнопка BOOT на плате
- GPIO8 -> штатный LED на плате
```

Для управления от микроконтроллера:

- переведите slide switch на `Pololu 2811` в положение `OFF`
- `GPIO4` подключите к входу `ON` модуля `Pololu 2811`
- `VOUT` модуля `Pololu 2811` подайте на плюс сирены
- минус сирены подключите к общему `GND`

Если плата `ESP32-C3 Super Mini` питается не от `Pololu 2850`, а от `USB-C`, то общую землю с `Pololu 2811` и сиреной все равно нужно объединить.

Текущие GPIO по проекту:

- `GPIO4` -> управление сиреной, силовым ключом или внешним драйвером нагрузки
- `GPIO8` -> статусный LED
- `GPIO9` -> кнопка `BOOT` для `reconfigure`

Важно для `ESP32-C3 Super Mini`:

- `GPIO8` и `GPIO9` относятся к strapping pins
- `GPIO8` должен оставаться в высоком уровне во время reset, иначе загрузка/прошивка может работать нестабильно
- `GPIO9` управляет boot mode: `LOW` при reset переводит чип в download mode
- если используете внешний LED на `GPIO8`, не делайте схему, которая тянет линию вниз при старте

По данным источников:

- `ESP32-C3 Super Mini` — компактная плата на `ESP32-C3` с `4 MB` flash, native USB и выведенными `GPIO8`/`GPIO9`
- модуль `Pololu 2811` — компактный high-side MOSFET power switch с защитой от переполюсовки; его можно использовать как электронный выключатель питания нагрузки
- модуль `Pololu 2850` — понижающий DC-DC регулятор `5 V / 2.5 A` с входным диапазоном `6 V – 38 V`; его можно использовать как источник `5 V` для сирены, периферии или всей низковольтной части устройства
- в этой референсной схеме предполагается `VIN = 12V`

Что умеет прошивка:
- включает сирену на заданное количество секунд;
- выключает сирену по `0` или по таймеру;
- публикует оставшееся время в секундах;
- публикует текущий `Wi-Fi RSSI` в `dBm`;
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
- `V6` (`Integer`, опционально): текущий `Wi-Fi RSSI` в `dBm`

Итоговая конфигурация template:

| Name | Pin | Type | Widget | Назначение |
|---|---|---|---|---|
| `V0` | `V0` | `Integer` | нет | внешний API: `N` секунд / `0` стоп |
| `V1` | `V1` | `Integer` | `Formatted Text`, `Value Display` или `Labeled Value` | оставшееся время в секундах |
| `V2` | `V2` | `Integer` или `Enumerable` | `Menu` / `Dropdown` | preset `0..5` |
| `V3` | `V3` | `Integer` | `Button` `Push` | `Start` |
| `V4` | `V4` | `Integer` | `Button` `Push` | `Stop` |
| `V5` | `V5` | `Integer` | `LED` или `State` | состояние сирены `1/0` |
| `V6` | `V6` | `Integer` | `Value Display` / `Labeled Value` | `Wi-Fi RSSI` в `dBm`, например `-67` |

Важно:
- в `Blynk` поле `Name` должно совпадать с `V`-именем, то есть `V0`, `V1`, `V2`, `V3`, `V4`, `V5`, `V6`
- для этой прошивки одного совпадения `Pin` недостаточно, потому что `Blynk MQTT API` публикует значения по имени datastream

## Рекомендованный UI в Blynk

Сделайте на дашборде:
- `Value Display` или `Labeled Value` для `V1`
- `Menu` или `Dropdown` для `V2`
- кнопку `Start` на `V3` в режиме `Push`
- кнопку `Stop` на `V4` в режиме `Push`
- для обеих кнопок включите подтверждение (`confirmation`)
- при необходимости добавьте `LED` или `State` на `V5`
- для `V6` добавьте `Value Display` и подпись вроде `Wi-Fi RSSI`

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
- `Alarm system -> Reconfigure button GPIO`
- `Alarm system -> Status LED GPIO`
- `Alarm system -> Blynk Wi-Fi RSSI datastream`

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
