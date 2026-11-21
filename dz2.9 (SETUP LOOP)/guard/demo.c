// -DBOARD_FIRST міняє те, який заголовок потрапляє в трансляцію першим.
// З циклом це міняє тільки текст помилки, а не її наявність.
#ifdef BOARD_FIRST
#include "board.h"
#else
#include "app.h"
#endif

void app_run(const AppConfig *cfg) { (void)cfg; }
void board_apply(const AppConfig *cfg) { (void)cfg; }

int main(void) { return 0; }
