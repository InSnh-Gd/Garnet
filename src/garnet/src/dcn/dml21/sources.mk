DML21_C_SRCS := $(addprefix $(DML21_DIR)/src/, \
    dml2_top/dml2_top_interfaces.c dml2_top/dml2_top_soc15.c \
    dml2_core/dml2_core_dcn4.c dml2_core/dml2_core_utils.c \
    dml2_core/dml2_core_factory.c dml2_core/dml2_core_dcn4_calcs.c \
    dml2_dpmm/dml2_dpmm_dcn4.c dml2_dpmm/dml2_dpmm_factory.c \
    dml2_mcg/dml2_mcg_dcn4.c dml2_mcg/dml2_mcg_factory.c \
    dml2_pmo/dml2_pmo_dcn3.c dml2_pmo/dml2_pmo_factory.c \
    dml2_pmo/dml2_pmo_dcn4_fams2.c \
    dml2_standalone_libraries/lib_float_math.c dml2_standalone_libraries/lib_frl_cap_check.c)
DML21_CPPFLAGS := $(addprefix -I,$(DML21_DIR)/port $(DML21_DIR)/inc \
    $(DML21_DIR)/inc/bounding_boxes $(DML21_DIR)/src/inc $(sort $(dir $(DML21_C_SRCS))))
