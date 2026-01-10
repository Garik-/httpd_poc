NPM ?= npm
NPM_CI ?= $(NPM) ci
NPM_RUN ?= $(NPM) run
NPM_BUILD ?= $(NPM_RUN) build

WEB_DIR := web
FIRMWARE_DIR := firmware

IDF_VERSION ?= v5.5.2
ESP_IDF ?= $(HOME)/esp/$(IDF_VERSION)/esp-idf

.PHONY: build-web
build-web:
	$(NPM_CI) --prefix $(WEB_DIR)
	$(NPM_BUILD) --prefix $(WEB_DIR)

.PHONY: build-firmware
build-firmware:
	. $(ESP_IDF)/export.sh && idf.py -C $(FIRMWARE_DIR) build


.PHONY: build
build: build-web build-firmware