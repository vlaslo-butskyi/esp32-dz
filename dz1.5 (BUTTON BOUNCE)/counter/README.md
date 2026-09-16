# ДЗ 1.5 — Лічильник брязкоту (BUTTON BOUNCE)

Код з умови ДЗ, трохи доповнений. Рахує FALLING-переривання від кнопки на GPIO 16 і після кожної пачки фронтів друкує, скільки їх було при натисканні і скільки при відпусканні. На цій прошивці робились усі виміри.

## Схема

<img src="schema.png" width="400" alt="Схема підключення">

| Компонент | Пін | Примітка |
|---|---|---|
| Кнопка | GPIO 16 | другий контакт на GND, `INPUT_PULLUP` |
| Аналізатор, CH1 | GPIO 16 | у Logic 2 це канал 0 |
| Аналізатор, GND | GND | |

## Як працює код

Файл: [`main.cpp`](main.cpp)

- `button_isr()` на кожен FALLING збільшує `button_counter`, як у коді з умови, і запам'ятовує час фронту через `micros()`.
- `loop()` чекає, поки після останнього фронту мине 30 мс (`BURST_GAP_MS`), і дивиться рівень піна: LOW означає натискання (`PRESS`), HIGH означає відпускання (`RELEASE`). Якщо кнопку відпустили без жодного FALLING, друкує `(clean)`.
- Ще є `HOLD` (фронти, поки кнопка натиснута) і `TAP` (натиснули й відпустили швидше за 30 мс).
- ISR і `loop()` працюють з кількома спільними змінними, тому `loop()` читає їх під `portENTER_CRITICAL`.

Шматок логу з основного запису, натискання 5 і 6:

```
PRESS   #5  FALLING: 1   span:    0.000 ms  total: 13
RELEASE #5  FALLING: 6   span:    0.655 ms  total: 19
         edges, us: 0 334 349 548 555 655
PRESS   #6  FALLING: 1   span:    0.000 ms  total: 20
RELEASE #6  FALLING: 19  span:    2.451 ms  total: 39
         edges, us: 0 106 595 804 816 949 1023 1456 1782 1876 1917 1967 2058 2121 2161 2188 2232 2363 2451
```

`total` рахується так само, як у коді з умови.

## Запуск

1. У [`platformio.ini`](../../platformio.ini):
   ```ini
   build_src_filter = +<../dz1.5 (BUTTON BOUNCE)/counter/main.cpp>
   ```
2. `pio run -t upload && pio device monitor`

У Wokwi: `pio run`, потім `Wokwi: Select Config File` → [`wokwi.toml`](wokwi.toml) і запуск [`diagram.json`](diagram.json).

## Файли

| Файл | Опис |
|---|---|
| [`main.cpp`](main.cpp) | прошивка |
| [`diagram.json`](diagram.json) | схема для Wokwi |
| [`wokwi.toml`](wokwi.toml) | конфіг Wokwi |
| [`schema.png`](schema.png) | скриншот схеми |

Інший варіант: [**debounce**](../debounce/), та сама кнопка з фільтром брязкоту.
