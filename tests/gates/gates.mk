# Release gates (validation/V1_C_GATES_2026-10-18.md, tests/gates/README_KO.md).
# Not part of `make test`: they need private inputs (stock firmware tree,
# vehicle export, private boot list), proot, qemu-arm and the pinned ARM
# toolchain. Each target prints its own SKIP (exit 77) when an input is missing.
#
#   make gate-zip GATE_ZIP=dist/mazda-aa-dr-TAG.zip [GATE_COMMIT=sha40]
#   make gate-g7 gate-g8 gate-g9 gate-g16 GATE_CANDIDATE=zip-or-unpacked-dir
#   make gate-soak-fast | gate-soak
#   make gate-replay MX5DR_REPLAY_CHAIN=/private/chain.txt
GATE_PYTHON ?= python3
GATE_PREVIOUS_TAG ?= v1.0.0-beta.17
GATE_PREVIOUS_SHA256 ?= b2f0c03f2c549b964d6e2401c257664c2f31642487693dc79a6915cb06316502
GATE_WORK ?= $(BUILD)/gates
GATE_PREVIOUS ?= $(GATE_WORK)/previous/mazda-aa-dr-$(GATE_PREVIOUS_TAG).zip
GATE_G8_FLAGS ?=
GATE_G9_STRIDE ?= 1

.PHONY: gate-zip gate-g7 gate-g8 gate-g9 gate-g16 gate-soak-fast gate-soak gate-replay gate-previous gates-stock

gate-zip:
	@test -n "$(GATE_ZIP)" || { echo 'set GATE_ZIP'; exit 2; }
	$(GATE_PYTHON) tests/gates/g5g6_zipcheck.py --zip "$(GATE_ZIP)" $(if $(GATE_COMMIT),--commit $(GATE_COMMIT))

gate-previous:
	@mkdir -p $(GATE_WORK)/previous
	@test -s "$(GATE_PREVIOUS)" || gh release download $(GATE_PREVIOUS_TAG) --repo junoh-moon/mazda-aa-dr \
	  --pattern mazda-aa-dr-$(GATE_PREVIOUS_TAG).zip --dir $(GATE_WORK)/previous
	@echo "$(GATE_PREVIOUS_SHA256)  $(GATE_PREVIOUS)" | sha256sum -c -

gate-g7:
	@test -n "$(GATE_CANDIDATE)" || { echo 'set GATE_CANDIDATE'; exit 2; }
	MX5DR_GATE_WORK=$(GATE_WORK)/g7cmu $(GATE_PYTHON) tests/gates/g7_cmu_emulation.py --bundle "$(GATE_CANDIDATE)"
	MX5DR_GATE_WORK=$(GATE_WORK)/g7mb $(GATE_PYTHON) tests/gates/g7_multiboot.py --bundle "$(GATE_CANDIDATE)"
	MX5DR_GATE_WORK=$(GATE_WORK)/g7menus $(GATE_PYTHON) tests/gates/g7_menus.py --bundle "$(GATE_CANDIDATE)"
	MX5DR_GATE_WORK=$(GATE_WORK)/g7touch $(GATE_PYTHON) tests/gates/g7_touch.py --bundle "$(GATE_CANDIDATE)" --scenario A
	MX5DR_GATE_WORK=$(GATE_WORK)/g7touch $(GATE_PYTHON) tests/gates/g7_touch.py --bundle "$(GATE_CANDIDATE)" --scenario B

gate-g8: gate-previous
	@test -n "$(GATE_CANDIDATE)" || { echo 'set GATE_CANDIDATE'; exit 2; }
	MX5DR_GATE_WORK=$(GATE_WORK)/g8 $(GATE_PYTHON) tests/gates/g8_upgrade.py --previous "$(GATE_PREVIOUS)" \
	  --previous-sha256 $(GATE_PREVIOUS_SHA256) --candidate "$(GATE_CANDIDATE)" $(GATE_G8_FLAGS)

gate-g16: gate-previous
	@test -n "$(GATE_CANDIDATE)" || { echo 'set GATE_CANDIDATE'; exit 2; }
	MX5DR_GATE_WORK=$(GATE_WORK)/g16 $(GATE_PYTHON) tests/gates/g16_rollback.py --candidate "$(GATE_CANDIDATE)" \
	  --previous "$(GATE_PREVIOUS)" --previous-sha256 $(GATE_PREVIOUS_SHA256)

gate-g9:
	@test -n "$(GATE_CANDIDATE)" || { echo 'set GATE_CANDIDATE'; exit 2; }
	MX5DR_GATE_WORK=$(GATE_WORK)/g9 $(GATE_PYTHON) tests/gates/g9_powerloss.py --candidate "$(GATE_CANDIDATE)" \
	  --stride $(GATE_G9_STRIDE)

gates-stock: gate-g7 gate-g8 gate-g16 gate-g9

gate-soak-fast:
	sh tests/gates/g10_soak.sh --fast $(GATE_WORK)/soak-fast

gate-soak:
	sh tests/gates/g10_soak.sh $(GATE_WORK)/soak

gate-replay:
	sh tests/gates/g11_replay.sh $(GATE_WORK)/replay
