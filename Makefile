#
# OsitoK — root Makefile (delegator)
#
# Each architecture lives under arch/<arch>/ with its own Makefile.
# This top-level Makefile just dispatches to the right one.
#
#   make            → builds the default arch (Xtensa / ESP8266)
#   make xtensa     → ESP8266 (Wemos D1, Xtensa LX106 @ 80MHz)
#   make x86        → x86_64 bare-metal AI OS (UEFI, requires gnu-efi)
#   make arm        → AArch64 (SM8350, ROG Phone 5)
#   make wasm       → wasm32 hosted build
#   make clean-all  → wipe build artifacts in every arch
#
# The legacy `make flash` target stays at this level for muscle memory:
# it forwards to arch/xtensa/.

.DEFAULT_GOAL := xtensa

.PHONY: xtensa x86 arm wasm flash clean clean-all

xtensa:
	$(MAKE) -C arch/xtensa

x86:
	$(MAKE) -C arch/x86

arm:
	$(MAKE) -C arch/arm

wasm:
	$(MAKE) -C arch/wasm

flash:
	$(MAKE) -C arch/xtensa flash

clean:
	$(MAKE) -C arch/xtensa clean

clean-all:
	-$(MAKE) -C arch/xtensa clean
	-$(MAKE) -C arch/x86 clean
	-$(MAKE) -C arch/arm clean
	-$(MAKE) -C arch/wasm clean
