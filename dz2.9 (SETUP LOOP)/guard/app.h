// Навмисно крихка пара заголовків: app.h включає board.h, board.h включає
// app.h. Потрібна вона рівно для того, щоб показати, від чого і від чого НЕ
// рятує сторожа включення з blink/app.h. До прошивки не входить — збирається
// звичайним компілятором хоста через ./run.sh.
//
// Сторожа тут записана через `#if !defined(...) || defined(NO_GUARD)`, щоб її
// можна було вимкнути ключем -DNO_GUARD, не правлячи файл.

#if !defined(DEMO_APP_H) || defined(NO_GUARD)
#define DEMO_APP_H

#include "board.h"

typedef struct AppConfig {
    BoardPins pins;  // потрібен тип із board.h
    unsigned  interval_ms;
} AppConfig;

void app_run(const AppConfig *cfg);

#endif  // DEMO_APP_H
