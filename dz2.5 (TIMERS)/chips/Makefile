# Збірка кастомних чипів для Wokwi: силова частина витяжки.
#
#   make            - зібрати всі чипи
#   make install    - зібрати і розкласти по теках п'яти прошивок
#   make clean      - прибрати результат
#
# Потрібен clang з ціллю wasm32 і лінкер wasm-ld (пакет lld).
# На macOS: brew install llvm lld
# Шлях до llvm можна перевизначити:  make LLVM_BIN=/opt/homebrew/opt/llvm/bin
#
# Джерела перенесені з ДЗ 2.2 без змін: транзисторний ключ із relay_timing,
# двигун, діод і конденсатор із motor_pwm. У Wokwi таких деталей немає, тому
# кожна з них — окремий чип рівно з тими виводами, що в деталі на платі.

LLVM_BIN ?= /opt/homebrew/opt/llvm/bin
CLANG    ?= $(LLVM_BIN)/clang

CHIPS   = bc547-switch dc-motor flyback-diode electrolytic-cap
TARGETS = $(CHIPS:%=%.chip.wasm)

all: $(TARGETS)

%.chip.wasm: %.chip.c wokwi-api.h chip-gfx.h
	$(CLANG) --target=wasm32-unknown-unknown -nostdlib -O2 \
	  -Wl,--no-entry -Wl,--import-memory -Wl,--export-table -Wl,--export=chip_init -Wl,-z,stack-size=8192 \
	  -Wall -Wno-unused-function -Werror -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean

# Wokwi не приймає ".." у шляху до чипа, тому зібрані чипи разом з описами
# розкладаються по теках прошивок. Джерела лишаються тут одні.
VARIANTS = ../timer_tick ../timer_phase ../two_timers ../millis_vs_timer ../fan_full

install: all
	@for d in $(VARIANTS); do \
	  mkdir -p "$$d/chips"; \
	  cp $(CHIPS:%=%.chip.wasm) $(CHIPS:%=%.chip.json) "$$d/chips/"; \
	  echo "  -> $$d/chips/"; \
	done

.PHONY: install
