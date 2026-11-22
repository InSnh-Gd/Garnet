# host tests of the GarnetAccel aux kext: `make -C tests all` (run from src/garnet-accel or through `make test`).
#   all       the host test (fast) - BRING_ROOT=<Garnet tree root> compares the two GarnetMetalOps.h copies
#   plants    the planted breaks of the host test (about 1 minute)
#   gates     the planted breaks of the link gates A/B/C and of the host twin (about 5 minutes)
BRING_ROOT ?= ../..
ROOT := ..
BUILD ?= $(if $(TMPDIR),$(TMPDIR),/tmp)/garnetaccel-tests
all: paths
	mkdir -p $(BUILD)
	clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I $(ROOT)/src $(ROOT)/tests/host_test.cpp -o $(BUILD)/host-test
	cd $(ROOT) && $(abspath $(BUILD))/host-test . $(BRING_ROOT)
plants:
	BRING_ROOT=$(BRING_ROOT) zsh ./plant.sh
gates:
	zsh ./gate_plants.sh
paths:
	python3 layout_paths_test.py
.PHONY: all plants gates paths
