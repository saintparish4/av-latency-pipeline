# Shortcuts for the commands in README.md. Every target calls a CMake preset
# or a script in scripts/, so the scripts remain the source of truth.
#   make help     list targets
#   make          build and test the rel preset

SHELL := bash
.SHELLFLAGS := -euo pipefail -c
.DEFAULT_GOAL := test

CPU ?= 3
RUNS ?= 10
SECTOR ?= 60
FUZZ_SECONDS ?= 60
IMAGE ?= avl
DATA := data/OS-2-128-U1_v2.3.0_1024x10.pcap
PY := .venv/bin/python
VENV := .venv/.installed

.PHONY: help data venv build test debug asan tsan sanitize fuzz diff check \
  docker docker-check format format-check lint hooks validate ladder \
  reproduce clean

help: ## list targets
	@grep -E '^[a-z-]+:.*## ' $(MAKEFILE_LIST) | \
	  awk -F':.*## ' '{ printf "  %-14s %s\n", $$1, $$2 }'

$(DATA):
	scripts/fetch_data.sh

data: $(DATA) ## fetch and verify the pinned capture

# A stamp file, because .venv/bin/python is a symlink to the system Python.
$(VENV): requirements.txt
	[ -x $(PY) ] || python3 -m venv .venv
	$(PY) -m pip install -q -r requirements.txt
	touch $(VENV)

venv: $(VENV) ## create .venv from requirements.txt

build: ## configure and build the rel preset
	cmake --preset rel > /dev/null
	cmake --build --preset rel

test: build $(DATA) ## build, then run all tests (rel)
	ctest --preset rel

debug: $(DATA) ## build and test the debug preset
	cmake --preset debug > /dev/null
	cmake --build --preset debug
	ctest --preset debug

asan: $(DATA) ## build and test under ASan + UBSan
	cmake --preset asan > /dev/null
	cmake --build --preset asan
	ctest --preset asan

tsan: $(DATA) ## build and test under TSan
	cmake --preset tsan > /dev/null
	cmake --build --preset tsan
	ctest --preset tsan

sanitize: asan tsan ## both sanitizer presets

fuzz: $(DATA) ## build fuzzers, run each for FUZZ_SECONDS (default 60)
	cmake --preset fuzz > /dev/null
	cmake --build --preset fuzz
	scripts/fuzz.sh $(FUZZ_SECONDS)

diff: build $(DATA) $(VENV) ## differential tests against the Ouster SDK and Open3D
	$(PY) -I analysis/diff_reference.py --build build/rel --data data

check: ## full correctness check: scripts/reproduce.sh --check
	scripts/reproduce.sh --check

docker: ## build the pinned toolchain image
	docker build -t $(IMAGE) .

docker-check: docker ## run the correctness check inside the image
	docker run --rm --security-opt seccomp=unconfined -v "$$PWD":/src -w /src $(IMAGE) \
	  scripts/reproduce.sh --check

format: ## clang-format every C++/CUDA file in place
	scripts/format.sh

format-check: ## fail if any file needs formatting (as CI does)
	scripts/format.sh --check

lint: ## clang-tidy, shellcheck and shfmt
	cmake --preset rel > /dev/null
	scripts/lint.sh

hooks: ## install the pre-commit hook (once per clone)
	scripts/install_hooks.sh

validate: build $(DATA) $(VENV) ## harness A/A and injected-stall checks
	PATH="$$PWD/.venv/bin:$$PATH" RUNS=$(RUNS) scripts/validate_harness.sh

ladder: build $(DATA) $(VENV) ## optimization ladder on CPU (default 3), SECTOR degrees (default 60)
	scripts/build_link_orders.sh 4
	PATH="$$PWD/.venv/bin:$$PATH" CPU=$(CPU) RUNS=$(RUNS) scripts/ladder.sh $(SECTOR)

reproduce: ## full measurement run (needs a native host and sudo)
	CPU=$(CPU) RUNS=$(RUNS) scripts/reproduce.sh

clean: ## remove build trees (keeps data/, results/ and .venv/)
	rm -rf build out
