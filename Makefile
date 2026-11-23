# SPDX-License-Identifier: GPL-3.0-only
# Melodee Modules for Schwung (Ableton Move): build/<os>-<arch>/melodee-modules/ holds the module (module.json,
# dsp.so); its Modules folder holds the synth module files.
OS := $(shell uname -s)
OUT ?= build/$(OS)-$(shell uname -m)
MM = melodee-modules
CFLAGS ?= -O2
MV_CFLAGS = -std=gnu11 -fPIC -fvisibility=hidden -Wall -Wextra -Werror -I$(MM)/sdk -I$(MM)/src -Isrc
SRC = src/dsp.c src/mv_synth.c $(MM)/src/mm_engine.c $(MM)/src/mm_file.c $(MM)/src/mm_image.c $(MM)/src/mm_schema.c $(MM)/src/mm_sign.c
OBJ = $(SRC:%.c=$(OUT)/obj/%.o)
MODULE = $(OUT)/melodee-modules
ifeq ($(OS),Linux)
# (the part's thread-local: in the static TLS block, so the audio callback's first call does not allocate one)
MV_CFLAGS += -ftls-model=initial-exec
LIBS = -lm
TEST_LIBS = -ldl -lm
else
LIBS =
TEST_LIBS =
endif

.PHONY: all test test-each move dist clean

all: $(MODULE)/dsp.so $(MODULE)/module.json $(OUT)/test_dsp

$(OUT)/obj/%.o: %.c $(wildcard src/*.h) $(wildcard $(MM)/src/*.h) $(wildcard $(MM)/sdk/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MV_CFLAGS) -c $< -o $@

$(MODULE)/dsp.so: $(OBJ)
	@mkdir -p $(MODULE)/Modules
	$(CC) $(CFLAGS) $(LDFLAGS) -shared $(OBJ) -o $@ $(LIBS)

$(MODULE)/module.json: module/module.json
	@mkdir -p $(MODULE)
	cp $< $@

$(OUT)/test_dsp: tests/test_dsp.c src/schwung_api.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -std=gnu11 -g -Wall -Wextra -Werror -Isrc $< -o $@ $(TEST_LIBS)

# a PPG Wave 2.2 factory bank (PPG's, never in this repository): the test compares the PPG module's reading of it with
# its factory programs when it is there
PPG_BANK ?= ../saadet/private/ppg/ppg22-factory.syx

# the module with the test modules beside it (and a broken file, a second TRIO), as Schwung loads it; the contracts
# checked against Schwung's rules
test: all
	rm -rf $(OUT)/test && mkdir -p $(OUT)/test/melodee-modules/Modules $(OUT)/test/melodee-modules/SysEx
	cp $(MODULE)/dsp.so $(MODULE)/module.json $(OUT)/test/melodee-modules/
	cp $(MM)/tests/fixtures/*.mmod $(OUT)/test/melodee-modules/Modules/
	cp $(MM)/tests/fixtures/trio.mmod $(OUT)/test/melodee-modules/Modules/trio-copy.mmod
	head -c 3000 $(MM)/tests/fixtures/hydra.mmod > $(OUT)/test/melodee-modules/Modules/broken.mmod
	if [ -f "$(PPG_BANK)" ]; then cp "$(PPG_BANK)" "$(OUT)/test/melodee-modules/SysEx/PPG factory.syx"; fi
	$(OUT)/test_dsp $(OUT)/test/melodee-modules $(OUT)/test/contracts
	python3 tests/check_contract.py $(OUT)/test/contracts

# each module file alone in the Modules folder
test-each: all
	for f in $(MM)/tests/fixtures/*.mmod; do \
		rm -rf $(OUT)/one && mkdir -p $(OUT)/one/Modules && cp $(MODULE)/dsp.so $(OUT)/one/ && cp $$f $(OUT)/one/Modules/ \
		&& $(OUT)/test_dsp --one $(OUT)/one || exit 1; done

# Linux aarch64 for the Move (Cortex-A72, glibc 2.35: GCC 12 and glibc 2.36's older symbols), tested there
move:
	docker run --rm --platform linux/arm64 -v "$(CURDIR)":/w -w /w arm64v8/gcc:12-bookworm \
		make OUT=build/move-aarch64 CFLAGS="-O2 -mcpu=cortex-a72" all test test-each

# dist/melodee-modules-module.tar.gz: the Move build, one melodee-modules/ folder, Modules and SysEx empty
# (Schwung's catalog asset shape)
dist:
	test -f build/move-aarch64/melodee-modules/dsp.so
	rm -rf dist && mkdir -p dist/melodee-modules/Modules dist/melodee-modules/SysEx
	cp build/move-aarch64/melodee-modules/dsp.so module/module.json dist/melodee-modules/
	touch dist/melodee-modules/Modules/.keep dist/melodee-modules/SysEx/.keep
	COPYFILE_DISABLE=1 tar --no-xattrs -C dist -czf dist/melodee-modules-module.tar.gz melodee-modules

clean:
	rm -rf build dist
