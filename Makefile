ESPHOME ?= uv run esphome
CONFIG ?= ptouch_bridge.yaml
PORT ?= /dev/ttyUSB0

.PHONY: help sync validate compile build flash upload ota logs run dashboard clean

help:
	@echo "Targets:"
	@echo "  make sync      Install/update ESPHome with uv"
	@echo "  make validate  Validate $(CONFIG)"
	@echo "  make build     Compile $(CONFIG)"
	@echo "  make flash     Upload $(CONFIG) to $(PORT)"
	@echo "  make ota       Upload $(CONFIG) over the network"
	@echo "  make logs      Stream serial logs from $(PORT)"
	@echo "  make run       Compile, upload, and stream logs"
	@echo "  make dashboard Start the ESPHome dashboard"
	@echo "  make clean     Remove ESPHome build output"

sync:
	uv sync

validate:
	$(ESPHOME) config $(CONFIG)

compile build:
	$(ESPHOME) compile $(CONFIG)

flash upload:
	$(ESPHOME) upload $(CONFIG) --device $(PORT)

ota:
	$(ESPHOME) upload $(CONFIG)

logs:
	$(ESPHOME) logs $(CONFIG) --device $(PORT)

run:
	$(ESPHOME) run $(CONFIG) --device $(PORT)

dashboard:
	$(ESPHOME) dashboard .

clean:
	rm -rf .esphome/build

