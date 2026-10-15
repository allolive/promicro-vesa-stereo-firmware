# make test     host simulation of the timing code (needs g++ and pytest)
# make build    the firmware for a Pro Micro 5V / Leonardo (needs arduino-cli)
# make upload PORT=/dev/ttyACM0
# make build-lightrig / upload-lightrig PORT=...   the light timer in tools/lightrig
# make version  the firmware version (FIRMWARE_VERSION in the sketch)
SKETCH := firmware/promicro-vesa-stereo
FQBN ?= arduino:avr:leonardo
ARDUINO_CLI ?= arduino-cli
BUILD := build

.PHONY: test build upload build-lightrig upload-lightrig version clean

version:
	@sed -n 's/^#define FIRMWARE_VERSION "\(.*\)"/\1/p' $(SKETCH)/promicro-vesa-stereo.ino

test:
	python3 -m pytest -q tests

build:
	$(ARDUINO_CLI) compile --fqbn $(FQBN) --warnings all --output-dir $(BUILD) $(SKETCH)

upload: build
	$(ARDUINO_CLI) upload --fqbn $(FQBN) -p $(PORT) --input-dir $(BUILD) $(SKETCH)

build-lightrig:
	$(ARDUINO_CLI) compile --fqbn $(FQBN) --warnings all --output-dir $(BUILD)/lightrig tools/lightrig

upload-lightrig: build-lightrig
	$(ARDUINO_CLI) upload --fqbn $(FQBN) -p $(PORT) --input-dir $(BUILD)/lightrig tools/lightrig

clean:
	rm -rf $(BUILD)
