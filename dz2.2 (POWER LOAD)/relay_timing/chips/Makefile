# Збірка кастомного чипа для Wokwi.
#
#   make            - зібрати bc547-switch.chip.wasm
#   make clean      - прибрати результат
#
# Потрібен clang з ціллю wasm32 і лінкер wasm-ld (пакет lld).
# На macOS: brew install llvm lld
# Шлях до llvm можна перевизначити:  make LLVM_BIN=/opt/homebrew/opt/llvm/bin

LLVM_BIN ?= /opt/homebrew/opt/llvm/bin
CLANG    ?= $(LLVM_BIN)/clang

CHIPS   = bc547-switch
TARGETS = $(CHIPS:%=%.chip.wasm)

all: $(TARGETS)

%.chip.wasm: %.chip.c wokwi-api.h
	$(CLANG) --target=wasm32-unknown-unknown -nostdlib -O2 \
	  -Wl,--no-entry -Wl,--import-memory -Wl,--export-table -Wl,--export=chip_init -Wl,-z,stack-size=8192 \
	  -Wall -Wno-unused-function -Werror -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
