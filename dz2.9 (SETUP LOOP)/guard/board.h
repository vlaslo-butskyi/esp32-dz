#if !defined(DEMO_BOARD_H) || defined(NO_GUARD)
#define DEMO_BOARD_H

#ifdef FIXED
// Полагоджений варіант: board.h не включає app.h, а оголошує тип наперед.
// Для параметра-вказівника цього досить — розмір структури компілятору тут
// не потрібен. Саме так цикл розривають насправді.
typedef struct AppConfig AppConfig;
#else
#include "app.h"  // потрібен тип AppConfig — і ось він, цикл
#endif

typedef struct BoardPins {
    unsigned led_port;
    unsigned led_pin;
} BoardPins;

void board_apply(const AppConfig *cfg);

#endif  // DEMO_BOARD_H
