# Збірка кастомного чипа конденсатора для Wokwi.
#
#   make            - зібрати ceramic-cap.chip.wasm
#   make clean      - прибрати результат
#
# Потрібен clang з ціллю wasm32 і лінкер wasm-ld (пакет lld).
# На macOS: brew install llvm lld
# Шлях до llvm можна перевизначити:  make LLVM_BIN=/opt/homebrew/opt/llvm/bin
#
# Конденсатор спільний для всіх чотирьох RC-варіантів: wokwi.toml кожного з них
# посилається сюди через ../chips/ceramic-cap.chip.wasm.

LLVM_BIN ?= /opt/homebrew/opt/llvm/bin
CLANG    ?= $(LLVM_BIN)/clang

CHIPS   = ceramic-cap
TARGETS = $(CHIPS:%=%.chip.wasm)

all: $(TARGETS)

%.chip.wasm: %.chip.c wokwi-api.h chip-gfx.h
	$(CLANG) --target=wasm32-unknown-unknown -nostdlib -O2 \
	  -Wl,--no-entry -Wl,--import-memory -Wl,--export-table -Wl,--export=chip_init -Wl,-z,stack-size=8192 \
	  -Wall -Wno-unused-function -Werror -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean

# Wokwi не приймає ".." у шляху до чипа, тому зібраний чип разом із його
# описом розкладається по теках RC-варіантів. Джерела лишаються тут одні.
RC_DIRS = ../raw_interrupt_rc ../time_debounce_rc ../state_debounce_rc ../polling_debounce_rc

install: all
	@for d in $(RC_DIRS); do \
	  mkdir -p "$$d/chips"; \
	  cp $(CHIPS:%=%.chip.wasm) $(CHIPS:%=%.chip.json) "$$d/chips/"; \
	  echo "  -> $$d/chips/"; \
	done

.PHONY: install
