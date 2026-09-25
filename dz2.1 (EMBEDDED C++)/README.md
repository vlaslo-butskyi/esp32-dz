# ДЗ 2.1 — C++ для MCU: обмеження ресурсів і особливості (EMBEDDED C++)

Класичний Arduino blink, переписаний за правилами embedded C++, і його розширена версія з кнопкою на перериванні та виміром часу ітерації `loop()`. Плата — ESP32-S3-DevKitC-1.

## Прошивки

- [`blink`](blink/) — базове завдання: `enum class` для стану, `constexpr` для піна й інтервалу, клас `Led` з `init()` і `set()`, неблокуючий superloop на `millis()`, конфігурація у структурі зі `static constexpr`.
- [`blink_modes`](blink_modes/) — те саме плюс обидва опційні пункти: кнопка через `attachInterrupt` з `volatile` прапорцем і трьома режимами по колу, антидребезг у `loop()`, профайлер ітерації superloop зі звітом кожні 1000 ітерацій.

## Де що в коді

| Пункт завдання | Де |
|---|---|
| `enum class` для стану LED | `LedState` в обох варіантах |
| `constexpr` для піна й інтервалу | `BlinkConfig` / `Config` — уся конфігурація одним блоком |
| Клас `Led` з `init()` і `set(LedState)` | обидва варіанти |
| Без `delay()`, неблокуючий superloop | `loop()` на порівнянні `millis()` |
| Без глобальних змінних | єдина глобальна — `volatile bool buttonEvent` для ISR; решта стану у `static`-локальних змінних `loop()` |
| Без динамічної памʼяті та важких контейнерів STL | ні `new`, ні `malloc`, ні `String`, ні `std::vector` — увесь стан статичний, 7 байт у `blink` і 54 байти у `blink_modes` |
| Вимір часу ітерації `loop()` (опційно) | `LoopProfiler` у `blink_modes` |
| Кнопка через переривання (опційно) | `onButtonEdge()` з `IRAM_ATTR` у `blink_modes` |
| `volatile` для прапорця з ISR | `buttonEvent` у `blink_modes` |

Кожен варіант має власний README з розбором: чим `static constexpr` кращий за `#define`, чому в `Led` немає `virtual`, навіщо `volatile` прапорцю з ISR, чому антидребезг мусить ловити й дзвін відпускання, і скільки все це коштує в байтах за таблицею символів.
