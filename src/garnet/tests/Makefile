# Host regressions for RDNA4 discovery and firmware preparation; no GPU access.
CXX := clang++
BUILD ?= ../build-host-rdna4
SRC := ../src
CPPFLAGS := -I$(SRC) -I$(SRC)/amd -I../hosttest/shim
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -O1 -g \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
TESTS := $(BUILD)/ipdiscovery_test $(BUILD)/asic_rdna4_test $(BUILD)/fw_loader_test $(BUILD)/gfx_golden_test \
  $(BUILD)/native_display_identity_test $(BUILD)/native_display_peer_test $(BUILD)/native_edid_map_test \
  $(BUILD)/edid_modes_test $(BUILD)/dp_edid_aux_test $(BUILD)/dcn41_modeset_test $(BUILD)/primary_fb_test $(BUILD)/mm_index_lock_test \
  $(BUILD)/dcn_irq_contract_test $(BUILD)/dcn_txn_boundary_test
TLB83_TEST := $(BUILD)/gfx_tlb83_test
SCANOUT_FULL_TEST := $(BUILD)/scanout_full_test
TESTS += $(TLB83_TEST)
TESTS += $(SCANOUT_FULL_TEST)
TESTS += $(BUILD)/smu_setup_test
TESTS += $(BUILD)/init_gart_test
TESTS += $(BUILD)/mes_resources_test
TESTS += $(BUILD)/gmc_resources_test
TESTS += $(BUILD)/primary_vbl_test
TESTS += $(BUILD)/dcn41_cursor_test
TESTS += $(BUILD)/vmid_pool_test
TESTS += $(BUILD)/dcn41_ddc_sleep_test
TESTS += $(BUILD)/garnet_irq_wait_test
TESTS += $(BUILD)/dcn41_vbl_irq_test
TESTS += $(BUILD)/dcn41_dpms_test
TESTS += $(BUILD)/garnet_power_policy_test
TESTS += $(BUILD)/dcn41_fams2_test
TESTS += $(BUILD)/dcn41_fams2_mall_test
TESTS += $(BUILD)/dcn41_det_test
TESTS += $(BUILD)/garnet_gfxoff_guard_test
TESTS += $(BUILD)/dcn41_dmub_boot_test
PRT_SPARSE_TEST := $(BUILD)/native_prt_sparse_test
TLB83_ARGS := $(SRC)/amd/gmc_v12_0.cpp $(SRC)/amd/amdgpu_regs.h $(SRC)/GarnetDriver.cpp $(SRC)/amd
TLB83_PINNED_SOURCES := $(TLB83_ARGS) $(addprefix $(SRC)/amd/,psp_v14_0.cpp smu_v14_0.cpp \
  sdma_v7_0.cpp amdgpu_init.cpp amdgpu_gart.cpp mes_v12_1.cpp cp_v12_0.cpp rlc_v12_0.cpp compute_test.cpp)
SCANOUT_FULL_ARGS := $(SRC)/dcn/garnet_sdma_fastcopy.cpp $(SRC)/dcn/dcn.cpp $(SRC)/GarnetUserClient.cpp

all: $(TESTS) $(PRT_SPARSE_TEST)
	@set -e; for test in $(filter-out $(TLB83_TEST) $(SCANOUT_FULL_TEST) $(BUILD)/garnet_gfxoff_guard_test $(BUILD)/dcn41_dmub_boot_test,$(TESTS)); do "$$test"; done
	$(BUILD)/dcn41_dmub_boot_test $(wildcard ../firmware/dcn_4_0_1_dmcub.bin)
	$(BUILD)/garnet_gfxoff_guard_test "$(abspath ..)"
	$(TLB83_TEST) $(TLB83_ARGS)
	$(SCANOUT_FULL_TEST) $(SCANOUT_FULL_ARGS)
	$(PRT_SPARSE_TEST) "$(abspath ../../..)"

$(BUILD):
	mkdir -p $@

$(BUILD)/ipdiscovery_test: ipdiscovery_test.cpp $(SRC)/ipdiscovery.cpp $(SRC)/ipdiscovery.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) ipdiscovery_test.cpp $(SRC)/ipdiscovery.cpp -o $@

$(BUILD)/asic_rdna4_test: asic_rdna4_test.cpp $(SRC)/amd/amdgpu_asic.cpp $(SRC)/amd/fw_table.cpp $(SRC)/amd/amdgpu_asic.h $(SRC)/fw/fw_table.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) asic_rdna4_test.cpp $(SRC)/amd/amdgpu_asic.cpp $(SRC)/amd/fw_table.cpp -o $@

$(BUILD)/fw_loader_test: fw_loader_test.cpp $(SRC)/amd/fw_loader.cpp $(SRC)/amd/amdgpu_ucode_extract.cpp $(wildcard $(SRC)/amd/*.h ../hosttest/shim/IOKit/*.h) $(SRC)/fw/fw_table.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) fw_loader_test.cpp $(SRC)/amd/fw_loader.cpp $(SRC)/amd/amdgpu_ucode_extract.cpp -o $@

$(BUILD)/dcn41_fams2_test: dcn41_fams2_test.cpp $(SRC)/dcn/dcn41_fams2_pure.h ../../dcn41/dcn41_dmub.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) dcn41_fams2_test.cpp -o $@

$(BUILD)/dcn41_fams2_mall_test: dcn41_fams2_mall_test.cpp $(SRC)/dcn/dcn41_mall_pure.h $(SRC)/amd/amdgpu_asic.cpp $(SRC)/amd/amdgpu_asic.h ../../dcn41/dcn41_dmub.h | $(BUILD)
	$(CXX) $(CPPFLAGS) -I../../dcn41 $(CXXFLAGS) dcn41_fams2_mall_test.cpp $(SRC)/amd/amdgpu_asic.cpp -o $@

$(BUILD)/gfx_golden_test: gfx_golden_test.cpp $(SRC)/amd/gfx_v12_0.cpp $(SRC)/amd/amdgpu_asic.cpp $(wildcard $(SRC)/amd/*.h ../hosttest/shim/IOKit/*.h) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) gfx_golden_test.cpp $(SRC)/amd/amdgpu_asic.cpp -o $@

$(BUILD)/native_display_peer_test: native_display_peer_test.cpp $(SRC)/dcn/display_state_pure.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) native_display_peer_test.cpp -o $@

$(BUILD)/mm_index_lock_test: mm_index_lock_test.cpp $(SRC)/amd/amdgpu_regs.h ../hosttest/shim/IOKit/IOLocks.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread mm_index_lock_test.cpp -o $@

$(BUILD)/native_display_identity_test: native_display_identity_test.cpp $(SRC)/amd/native_display_identity_pure.h $(SRC)/dcn/display_state_pure.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) native_display_identity_test.cpp -o $@

$(BUILD)/native_edid_map_test: native_edid_map_test.cpp $(SRC)/amd/native_edid_map_pure.h $(SRC)/GarnetDisplayOps.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) native_edid_map_test.cpp -o $@

$(BUILD)/edid_modes_test: edid_modes_test.cpp $(SRC)/dcn/edid_modes_pure.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) edid_modes_test.cpp -o $@

$(BUILD)/dp_edid_aux_test: dp_edid_aux_test.cpp $(SRC)/dcn/dp_edid_aux.h $(SRC)/dcn/edid_modes_pure.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) dp_edid_aux_test.cpp -o $@

$(BUILD)/dcn41_modeset_test: dcn41_modeset_test.cpp $(SRC)/dcn/dcn41_modeset.h $(SRC)/dcn/dp_edid_aux.h $(SRC)/dcn/edid_modes_pure.h ../../dcn41/dcn41_allow_ranges.h | $(BUILD)
	$(CXX) $(CPPFLAGS) -I../../dcn41 $(CXXFLAGS) -Wframe-larger-than=2048 dcn41_modeset_test.cpp -o $@

$(BUILD)/dcn41_cursor_test: dcn41_cursor_test.cpp $(SRC)/dcn/dcn41_cursor_pure.h ../../dcn41/dcn41_allow.h ../../dcn41/dcn41_allow.c ../../dcn41/dcn41_regs.h | $(BUILD)
	$(CXX) $(CPPFLAGS) -I../../dcn41 $(CXXFLAGS) dcn41_cursor_test.cpp -x c++ ../../dcn41/dcn41_allow.c -o $@

$(BUILD)/vmid_pool_test: vmid_pool_test.cpp $(SRC)/amd/rdna4_vmid_pool.h $(SRC)/amd/rdna4_sparse_pt_pure.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) vmid_pool_test.cpp -o $@

$(BUILD)/dcn41_ddc_sleep_test: dcn41_ddc_sleep_test.cpp $(SRC)/dcn/dcn41_ddc_pad_pure.h $(SRC)/amd/rdna4_sleep_pure.h ../../dcn41/dcn41_allow.h ../../dcn41/dcn41_allow.c | $(BUILD)
	$(CXX) $(CPPFLAGS) -I../../dcn41 $(CXXFLAGS) dcn41_ddc_sleep_test.cpp -x c++ ../../dcn41/dcn41_allow.c -o $@

$(BUILD)/garnet_irq_wait_test: garnet_irq_wait_test.cpp $(SRC)/amd/garnet_irq_wait_pure.h $(SRC)/amd/garnet_submit_pure.h $(SRC)/amd/amdgpu_pm4.h $(SRC)/amd/cp_pm4_gfx12.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) garnet_irq_wait_test.cpp -o $@

$(BUILD)/dcn41_vbl_irq_test: dcn41_vbl_irq_test.cpp $(SRC)/dcn/dcn41_vbl_irq_pure.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) dcn41_vbl_irq_test.cpp -o $@

$(BUILD)/dcn41_dpms_test: dcn41_dpms_test.cpp $(SRC)/dcn/dcn41_dpms_pure.h $(SRC)/dcn/dcn41_modeset.h $(SRC)/dcn/dcn41_ddc_pad_pure.h ../../dcn41/dcn41_allow_ranges.h | $(BUILD)
	$(CXX) $(CPPFLAGS) -I../../dcn41 $(CXXFLAGS) dcn41_dpms_test.cpp -o $@

$(BUILD)/garnet_power_policy_test: garnet_power_policy_test.cpp $(SRC)/amd/garnet_power_policy_pure.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) garnet_power_policy_test.cpp -o $@

$(BUILD)/dcn41_det_test: dcn41_det_test.cpp $(SRC)/dcn/dcn41_det_pure.h ../../dcn41/dcn41_allow_ranges.h | $(BUILD)
	$(CXX) $(CPPFLAGS) -I../../dcn41 $(CXXFLAGS) dcn41_det_test.cpp -o $@

$(BUILD)/garnet_gfxoff_guard_test: garnet_gfxoff_guard_test.cpp $(SRC)/amd/garnet_gfxoff_pure.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) garnet_gfxoff_guard_test.cpp -o $@

$(BUILD)/dcn41_dmub_boot_test: dcn41_dmub_boot_test.cpp $(SRC)/dcn/dcn41_dmub_boot_pure.h $(SRC)/amd/amdgpu_ucode_extract.cpp $(SRC)/amd/amdgpu_ucode_extract.h $(SRC)/amd/amdgpu_ucode_psp.h ../../dcn41/dcn41_dmub.h ../../dcn41/dcn41_dmub.c ../../dcn41/dcn41_core.c ../../dcn41/dcn41_allow_ranges.h ../../dcn41/dcn41_regs.h | $(BUILD)
	$(CXX) $(CPPFLAGS) -I../../dcn41 $(CXXFLAGS) dcn41_dmub_boot_test.cpp $(SRC)/amd/amdgpu_ucode_extract.cpp -x c++ ../../dcn41/dcn41_dmub.c ../../dcn41/dcn41_core.c -o $@

$(BUILD)/primary_fb_test: primary_fb_test.cpp $(SRC)/dcn/primary_fb_pure.h $(SRC)/dcn/edid_modes_pure.h $(SRC)/dcn/display_state_pure.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) primary_fb_test.cpp -o $@

$(BUILD)/primary_vbl_actual.inc: test-primary-vbl-production.py $(SRC)/GarnetPrimaryFramebuffer.cpp | $(BUILD)
	python3 test-primary-vbl-production.py $(SRC)/GarnetPrimaryFramebuffer.cpp $@

$(BUILD)/primary_vbl_test: primary_vbl_test.cpp $(BUILD)/primary_vbl_actual.inc | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I$(BUILD) primary_vbl_test.cpp -o $@

DCN_IRQ_INC := $(BUILD)/dcn_irq_actual.inc
$(DCN_IRQ_INC): $(SRC)/dcn/dcn.cpp | $(BUILD)
	python3 -c 'import sys; from pathlib import Path; s=Path(sys.argv[1]).read_text(); assert "gDcnIrqWriteScope" not in s and "DcnIrqWriteScope" not in s; a=s.index("bool ihEntry("); b=s.index("\n}\n\n// ---- the verbs", a)+3; Path(sys.argv[2]).write_text(s[a:b])' "$<" "$@"

$(BUILD)/dcn_irq_contract_test: dcn_irq_contract_test.cpp $(DCN_IRQ_INC) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I$(BUILD) dcn_irq_contract_test.cpp -o $@

DCN_TXN_INC := $(BUILD)/dcn_txn_boundaries_actual.inc
$(DCN_TXN_INC): $(SRC)/dcn/dcn_scanout_mt.cpp $(SRC)/dcn/dcn_disp2_scanx.cpp Makefile | $(BUILD)
	python3 -c 'import sys; from pathlib import Path; mt=Path(sys.argv[1]).read_text(); dx=Path(sys.argv[2]).read_text(); \
	a=mt.index("static bool scan_poll_locked("); b=mt.index("\n// The interrupt path", a); assert "dcn41_irq_ack" not in mt[a:b], "scan_poll_locked must stay read-only (called by unleased scanStatus)"; \
	a=mt.index("uint32_t scanBoGone("); b=mt.index("\n// ---- build 0.0.605", a); p1=mt[a:b]; \
	a=mt.index("static void scan_watchdog_loop("); b=mt.index("uint32_t scanGeneration(", a); p1a=mt[a:b]; \
	a=mt.index("static void mt_watchdog("); b=mt.index("static bool mt_start_watchdog(", a); p1b=mt[a:b]; \
	a=dx.index("void wait_for_dcn_threads_quiescent() {"); b=dx.index("// =====================================================================================================================================================================================================", a); p2a=dx[a:b]; \
	assert "for (;;)" in p2a and "2000u" not in p2a; \
	a=mt.index("static void scan_shutdown_under_txn()"); b=mt.index("\n}  // namespace rdna4dcn", a); p2=mt[a:b]; \
	a=dx.index("static uint32_t scan_x_release_under_txn("); b=dx.index("uint32_t scanXBoGone(", a); p3=dx[a:b]; \
	a=dx.index("void sx_watchdog(void *arg, wait_result_t) {"); b=dx.index("// disp2", a); p4=dx[a:b]; \
	a=dx.index("static void scan_x_shutdown_under_txn()"); b=dx.index("uint32_t scanXReport(", a); p5=dx[a:b]; \
	a=dx.index("uint32_t scanXBoGone("); b=dx.index("// instance 0", a); p6=dx[a:b]; \
	Path(sys.argv[3]).write_text("\n\n".join((p1,p1a,p1b,p2a,p2,p3,p4,p5,p6)))' "$(SRC)/dcn/dcn_scanout_mt.cpp" "$(SRC)/dcn/dcn_disp2_scanx.cpp" "$@"

$(BUILD)/dcn_txn_boundary_test: dcn_txn_boundary_test.cpp $(DCN_TXN_INC) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I$(BUILD) dcn_txn_boundary_test.cpp -o $@

$(TLB83_TEST): gfx_tlb83_test.cpp $(TLB83_PINNED_SOURCES) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -MF $@.d $< -o $@
-include $(TLB83_TEST).d

$(SCANOUT_FULL_TEST): scanout_full_test.cpp $(SRC)/dcn/scanout_full.h $(SRC)/dcn/liveraster.h \
  $(SCANOUT_FULL_ARGS) ../../dcn41/dcn41_core.c ../../dcn41/dcn41_otg.c | $(BUILD)
	$(CXX) $(CPPFLAGS) -I$(SRC)/dcn -I../../dcn41 $(CXXFLAGS) -x c++ $< ../../dcn41/dcn41_core.c ../../dcn41/dcn41_otg.c -o $@

gfx-golden: $(BUILD)/gfx_golden_test
	$<

$(BUILD)/smu_setup_actual.inc: test-smu-setup-production.py $(SRC)/amd/smu_v14_0.cpp | $(BUILD)
	python3 $< "$@" "$(SRC)/amd/smu_v14_0.cpp"

$(BUILD)/smu_setup_test: smu_setup_test.cpp $(BUILD)/smu_setup_actual.inc $(SRC)/amd/amdgpu_smu.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I$(BUILD) $< -o $@

smu-setup: $(BUILD)/smu_setup_test
	$<

$(BUILD)/init_gart_actual.inc: test-init-production.py $(SRC)/amd/amdgpu_init.cpp | $(BUILD)
	python3 $< "$@" "$(SRC)/amd/amdgpu_init.cpp"

$(BUILD)/init_gart_test: init_gart_test.cpp $(BUILD)/init_gart_actual.inc $(SRC)/amd/amdgpu_ip.h $(SRC)/amd/amdgpu_regs.h $(SRC)/amd/amdgpu_psp.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I$(BUILD) $< -o $@

init-gart: $(BUILD)/init_gart_test
	$<

$(BUILD)/mes_resources_actual.inc: test-mes-production.py $(SRC)/amd/mes_v12_1.cpp | $(BUILD)
	python3 $< "$@" "$(SRC)/amd/mes_v12_1.cpp"

$(BUILD)/mes_resources_test: mes_resources_test.cpp $(BUILD)/mes_resources_actual.inc $(SRC)/amd/amdgpu_mes.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I$(BUILD) $< -o $@

mes-resources: $(BUILD)/mes_resources_test
	$<

$(BUILD)/gmc_resources_actual.inc: test-gmc-production.py $(SRC)/amd/gmc_v12_0.cpp | $(BUILD)
	python3 $< "$@" "$(SRC)/amd/gmc_v12_0.cpp"

$(BUILD)/gmc_resources_test: gmc_resources_test.cpp $(BUILD)/gmc_resources_actual.inc $(SRC)/amd/amdgpu_gmc.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I$(BUILD) $< -o $@

gmc-resources: $(BUILD)/gmc_resources_test
	$<

PTMEM_INC := $(BUILD)/native_ptmem_actual.inc
$(PTMEM_INC): test-native-ptmem-production.py $(SRC)/amd/garnet_submit_internal.hpp | $(BUILD)
	python3 $< "$@" "$(SRC)/amd/garnet_submit_internal.hpp"

$(BUILD)/native_ptmem_production_test: native_ptmem_production_test.cpp $(PTMEM_INC) $(SRC)/amd/garnet_submit_pure.h $(SRC)/amd/garnet_multisession_pure.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I$(BUILD) $< -o $@

$(BUILD)/native_large_pt_test: native_large_pt_test.cpp $(SRC)/amd/garnet_submit_pure.h $(SRC)/amd/native_hostimport_pure.h $(SRC)/GarnetNativeABI.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

large-pt: $(BUILD)/native_large_pt_test $(BUILD)/native_ptmem_production_test
	$(BUILD)/native_large_pt_test "$(abspath ../../..)"
	$(BUILD)/native_ptmem_production_test "$(abspath ../../..)" "$(abspath $(PTMEM_INC))"

$(BUILD)/native_systemmemory_test: native_systemmemory_test.cpp $(SRC)/amd/garnet_multisession_pure.h \
  $(SRC)/amd/native_hostimport_pure.h $(SRC)/GarnetNativeABI.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread $< -o $@

system-memory: $(BUILD)/native_systemmemory_test
	$(BUILD)/native_systemmemory_test "$(abspath ../../..)"

$(PRT_SPARSE_TEST): native_prt_sparse_test.cpp $(PTMEM_INC) $(SRC)/amd/garnet_native_boot_pure.h \
  $(SRC)/amd/garnet_submit_pure.h $(SRC)/amd/native_hostimport_pure.h $(SRC)/amd/garnet_multisession_pure.h \
  $(SRC)/GarnetNativeABI.h | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I$(BUILD) $< -o $@

prt-sparse: $(PRT_SPARSE_TEST)
	$(PRT_SPARSE_TEST) "$(abspath ../../..)"

.PHONY: all gfx-golden smu-setup init-gart mes-resources gmc-resources large-pt system-memory prt-sparse
