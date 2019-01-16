/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "pp_debug.h"
#include <linux/firmware.h>
#include <drm/drmP.h>
#include "amdgpu.h"
#include "amdgpu_smu.h"
#include "soc15_common.h"
#include "smu_v11_0.h"
#include "atom.h"

enum amd_pm_state_type smu_get_current_power_state(struct smu_context *smu)
{
	/* not support power state */
	return POWER_STATE_TYPE_DEFAULT;
}

int smu_get_power_num_states(struct smu_context *smu,
			     struct pp_states_info *state_info)
{
	if (!state_info)
		return -EINVAL;

	/* not support power state */
	memset(state_info, 0, sizeof(struct pp_states_info));
	state_info->nums = 0;

	return 0;
}

int smu_common_read_sensor(struct smu_context *smu, enum amd_pp_sensors sensor,
			   void *data, uint32_t *size)
{
	int ret = 0;

	switch (sensor) {
	case AMDGPU_PP_SENSOR_ENABLED_SMC_FEATURES_MASK:
		ret = smu_feature_get_enabled_mask(smu, (uint32_t *)data, 2);
		*size = 8;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		*size = 0;

	return ret;
}

int smu_update_table(struct smu_context *smu, uint32_t table_id,
		     void *table_data, bool drv2smu)
{
	struct smu_table_context *smu_table = &smu->smu_table;
	struct smu_table *table = NULL;
	int ret = 0;

	if (!table_data || table_id >= smu_table->table_count)
		return -EINVAL;

	table = &smu_table->tables[table_id];

	if (drv2smu)
		memcpy(table->cpu_addr, table_data, table->size);

	ret = smu_send_smc_msg_with_param(smu, SMU_MSG_SetDriverDramAddrHigh,
					  upper_32_bits(table->mc_address));
	if (ret)
		return ret;
	ret = smu_send_smc_msg_with_param(smu, SMU_MSG_SetDriverDramAddrLow,
					  lower_32_bits(table->mc_address));
	if (ret)
		return ret;
	ret = smu_send_smc_msg_with_param(smu, drv2smu ?
					  SMU_MSG_TransferTableDram2Smu :
					  SMU_MSG_TransferTableSmu2Dram,
					  table_id);
	if (ret)
		return ret;

	if (!drv2smu)
		memcpy(table_data, table->cpu_addr, table->size);

	return ret;
}

bool is_support_sw_smu(struct amdgpu_device *adev)
{
	if (amdgpu_dpm != 1)
		return false;

	if (adev->asic_type >= CHIP_VEGA20)
		return true;

	return false;
}

int smu_sys_get_pp_table(struct smu_context *smu, void **table)
{
	struct smu_table_context *smu_table = &smu->smu_table;

	if (!smu_table->power_play_table && !smu_table->hardcode_pptable)
		return -EINVAL;

	if (smu_table->hardcode_pptable)
		*table = smu_table->hardcode_pptable;
	else
		*table = smu_table->power_play_table;

	return smu_table->power_play_table_size;
}

int smu_sys_set_pp_table(struct smu_context *smu,  void *buf, size_t size)
{
	struct smu_table_context *smu_table = &smu->smu_table;
	ATOM_COMMON_TABLE_HEADER *header = (ATOM_COMMON_TABLE_HEADER *)buf;
	int ret = 0;

	if (header->usStructureSize != size) {
		pr_err("pp table size not matched !\n");
		return -EIO;
	}

	mutex_lock(&smu->mutex);
	if (!smu_table->hardcode_pptable)
		smu_table->hardcode_pptable = kzalloc(size, GFP_KERNEL);
	if (!smu_table->hardcode_pptable) {
		ret = -ENOMEM;
		goto failed;
	}

	memcpy(smu_table->hardcode_pptable, buf, size);
	smu_table->power_play_table = smu_table->hardcode_pptable;
	smu_table->power_play_table_size = size;
	mutex_unlock(&smu->mutex);

	ret = smu_reset(smu);
	if (ret)
		pr_info("smu reset failed, ret = %d\n", ret);

failed:
	mutex_unlock(&smu->mutex);
	return ret;
}

int smu_feature_init_dpm(struct smu_context *smu)
{
	struct smu_feature *feature = &smu->smu_feature;
	int ret = 0;
	uint32_t unallowed_feature_mask[SMU_FEATURE_MAX/32];

	bitmap_fill(feature->allowed, SMU_FEATURE_MAX);

	ret = smu_get_unallowed_feature_mask(smu, unallowed_feature_mask,
					     SMU_FEATURE_MAX/32);
	if (ret)
		return ret;

	bitmap_andnot(feature->allowed, feature->allowed,
		      (unsigned long *)unallowed_feature_mask,
		      feature->feature_num);

	return ret;
}

int smu_feature_is_enabled(struct smu_context *smu, int feature_id)
{
	struct smu_feature *feature = &smu->smu_feature;
	WARN_ON(feature_id > feature->feature_num);
	return test_bit(feature_id, feature->enabled);
}

int smu_feature_set_enabled(struct smu_context *smu, int feature_id, bool enable)
{
	struct smu_feature *feature = &smu->smu_feature;
	WARN_ON(feature_id > feature->feature_num);
	if (enable)
		test_and_set_bit(feature_id, feature->enabled);
	else
		test_and_clear_bit(feature_id, feature->enabled);
	return 0;
}

int smu_feature_is_supported(struct smu_context *smu, int feature_id)
{
	struct smu_feature *feature = &smu->smu_feature;
	WARN_ON(feature_id > feature->feature_num);
	return test_bit(feature_id, feature->supported);
}

int smu_feature_set_supported(struct smu_context *smu, int feature_id,
			      bool enable)
{
	struct smu_feature *feature = &smu->smu_feature;
	WARN_ON(feature_id > feature->feature_num);
	if (enable)
		test_and_set_bit(feature_id, feature->supported);
	else
		test_and_clear_bit(feature_id, feature->supported);
	return 0;
}

static int smu_set_funcs(struct amdgpu_device *adev)
{
	struct smu_context *smu = &adev->smu;

	switch (adev->asic_type) {
	case CHIP_VEGA20:
		smu_v11_0_set_smu_funcs(smu);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int smu_early_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct smu_context *smu = &adev->smu;

	smu->adev = adev;
	mutex_init(&smu->mutex);

	return smu_set_funcs(adev);
}

int smu_get_atom_data_table(struct smu_context *smu, uint32_t table,
			    uint16_t *size, uint8_t *frev, uint8_t *crev,
			    uint8_t **addr)
{
	struct amdgpu_device *adev = smu->adev;
	uint16_t data_start;

	if (!amdgpu_atom_parse_data_header(adev->mode_info.atom_context, table,
					   size, frev, crev, &data_start))
		return -EINVAL;

	*addr = (uint8_t *)adev->mode_info.atom_context->bios + data_start;

	return 0;
}

static int smu_initialize_pptable(struct smu_context *smu)
{
	/* TODO */
	return 0;
}

static int smu_smc_table_sw_init(struct smu_context *smu)
{
	int ret;

	ret = smu_initialize_pptable(smu);
	if (ret) {
		pr_err("Failed to init smu_initialize_pptable!\n");
		return ret;
	}

	/**
	 * Create smu_table structure, and init smc tables such as
	 * TABLE_PPTABLE, TABLE_WATERMARKS, TABLE_SMU_METRICS, and etc.
	 */
	ret = smu_init_smc_tables(smu);
	if (ret) {
		pr_err("Failed to init smc tables!\n");
		return ret;
	}

	/**
	 * Create smu_power_context structure, and allocate smu_dpm_context and
	 * context size to fill the smu_power_context data.
	 */
	ret = smu_init_power(smu);
	if (ret) {
		pr_err("Failed to init smu_init_power!\n");
		return ret;
	}

	return 0;
}

static int smu_smc_table_sw_fini(struct smu_context *smu)
{
	int ret;

	ret = smu_fini_smc_tables(smu);
	if (ret) {
		pr_err("Failed to smu_fini_smc_tables!\n");
		return ret;
	}

	return 0;
}

static int smu_sw_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct smu_context *smu = &adev->smu;
	int ret;

	if (!is_support_sw_smu(adev))
		return -EINVAL;

	smu->pool_size = adev->pm.smu_prv_buffer_size;

	ret = smu_init_microcode(smu);
	if (ret) {
		pr_err("Failed to load smu firmware!\n");
		return ret;
	}

	return 0;
}

static int smu_sw_fini(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct smu_context *smu = &adev->smu;
	int ret;

	if (!is_support_sw_smu(adev))
		return -EINVAL;

	ret = smu_smc_table_sw_fini(smu);
	if (ret) {
		pr_err("Failed to sw fini smc table!\n");
		return ret;
	}

	ret = smu_fini_power(smu);
	if (ret) {
		pr_err("Failed to init smu_fini_power!\n");
		return ret;
	}

	return 0;
}

static int smu_init_fb_allocations(struct smu_context *smu)
{
	struct amdgpu_device *adev = smu->adev;
	struct smu_table_context *smu_table = &smu->smu_table;
	struct smu_table *tables = smu_table->tables;
	uint32_t table_count = smu_table->table_count;
	uint32_t i = 0;
	int32_t ret = 0;

	if (table_count <= 0)
		return -EINVAL;

	for (i = 0 ; i < table_count; i++) {
		if (tables[i].size == 0)
			continue;
		ret = amdgpu_bo_create_kernel(adev,
					      tables[i].size,
					      tables[i].align,
					      tables[i].domain,
					      &tables[i].bo,
					      &tables[i].mc_address,
					      &tables[i].cpu_addr);
		if (ret)
			goto failed;
	}

	return 0;
failed:
	for (; i > 0; i--) {
		if (tables[i].size == 0)
			continue;
		amdgpu_bo_free_kernel(&tables[i].bo,
				      &tables[i].mc_address,
				      &tables[i].cpu_addr);

	}
	return ret;
}

static int smu_fini_fb_allocations(struct smu_context *smu)
{
	struct smu_table_context *smu_table = &smu->smu_table;
	struct smu_table *tables = smu_table->tables;
	uint32_t table_count = smu_table->table_count;
	uint32_t i = 0;

	if (table_count == 0 || tables == NULL)
		return 0;

	for (i = 0 ; i < table_count; i++) {
		if (tables[i].size == 0)
			continue;
		amdgpu_bo_free_kernel(&tables[i].bo,
				      &tables[i].mc_address,
				      &tables[i].cpu_addr);
	}

	return 0;
}

static int smu_smc_table_hw_init(struct smu_context *smu)
{
	int ret;

	ret = smu_read_pptable_from_vbios(smu);
	if (ret)
		return ret;

	/* get boot_values from vbios to set revision, gfxclk, and etc. */
	ret = smu_get_vbios_bootup_values(smu);
	if (ret)
		return ret;

	ret = smu_get_clk_info_from_vbios(smu);
	if (ret)
		return ret;

	/*
	 * check if the format_revision in vbios is up to pptable header
	 * version, and the structure size is not 0.
	 */
	ret = smu_get_clk_info_from_vbios(smu);
	if (ret)
		return ret;

	ret = smu_check_pptable(smu);
	if (ret)
		return ret;

	/*
	 * allocate vram bos to store smc table contents.
	 */
	ret = smu_init_fb_allocations(smu);
	if (ret)
		return ret;

	/*
	 * Parse pptable format and fill PPTable_t smc_pptable to
	 * smu_table_context structure. And read the smc_dpm_table from vbios,
	 * then fill it into smc_pptable.
	 */
	ret = smu_parse_pptable(smu);
	if (ret)
		return ret;

	/*
	 * Send msg GetDriverIfVersion to check if the return value is equal
	 * with DRIVER_IF_VERSION of smc header.
	 */
	ret = smu_check_fw_version(smu);
	if (ret)
		return ret;

	/*
	 * Copy pptable bo in the vram to smc with SMU MSGs such as
	 * SetDriverDramAddr and TransferTableDram2Smu.
	 */
	ret = smu_write_pptable(smu);
	if (ret)
		return ret;

	/* issue RunAfllBtc msg */
	ret = smu_run_afll_btc(smu);
	if (ret)
		return ret;

	ret = smu_feature_enable_all(smu);
	if (ret)
		return ret;

	ret = smu_notify_display_change(smu);
	if (ret)
		return ret;

	/*
	 * Set min deep sleep dce fclk with bootup value from vbios via
	 * SetMinDeepSleepDcefclk MSG.
	 */
	ret = smu_set_min_dcef_deep_sleep(smu);
	if (ret)
		return ret;

	/*
	 * Set initialized values (get from vbios) to dpm tables context such as
	 * gfxclk, memclk, dcefclk, and etc. And enable the DPM feature for each
	 * type of clks.
	 */
	ret = smu_populate_smc_pptable(smu);
	if (ret)
		return ret;

	ret = smu_init_max_sustainable_clocks(smu);
	if (ret)
		return ret;

	ret = smu_populate_umd_state_clk(smu);
	if (ret)
		return ret;

	ret = smu_get_power_limit(smu);
	if (ret)
		return ret;

	/*
	 * Set PMSTATUSLOG table bo address with SetToolsDramAddr MSG for tools.
	 */
	ret = smu_set_tool_table_location(smu);

	return ret;
}

/**
 * smu_alloc_memory_pool - allocate memory pool in the system memory
 *
 * @smu: amdgpu_device pointer
 *
 * This memory pool will be used for SMC use and msg SetSystemVirtualDramAddr
 * and DramLogSetDramAddr can notify it changed.
 *
 * Returns 0 on success, error on failure.
 */
static int smu_alloc_memory_pool(struct smu_context *smu)
{
	struct amdgpu_device *adev = smu->adev;
	struct smu_table_context *smu_table = &smu->smu_table;
	struct smu_table *memory_pool = &smu_table->memory_pool;
	uint64_t pool_size = smu->pool_size;
	int ret = 0;

	if (pool_size == SMU_MEMORY_POOL_SIZE_ZERO)
		return ret;

	memory_pool->size = pool_size;
	memory_pool->align = PAGE_SIZE;
	memory_pool->domain = AMDGPU_GEM_DOMAIN_GTT;

	switch (pool_size) {
	case SMU_MEMORY_POOL_SIZE_256_MB:
	case SMU_MEMORY_POOL_SIZE_512_MB:
	case SMU_MEMORY_POOL_SIZE_1_GB:
	case SMU_MEMORY_POOL_SIZE_2_GB:
		ret = amdgpu_bo_create_kernel(adev,
					      memory_pool->size,
					      memory_pool->align,
					      memory_pool->domain,
					      &memory_pool->bo,
					      &memory_pool->mc_address,
					      &memory_pool->cpu_addr);
		break;
	default:
		break;
	}

	return ret;
}

static int smu_free_memory_pool(struct smu_context *smu)
{
	struct smu_table_context *smu_table = &smu->smu_table;
	struct smu_table *memory_pool = &smu_table->memory_pool;
	int ret = 0;

	if (memory_pool->size == SMU_MEMORY_POOL_SIZE_ZERO)
		return ret;

	amdgpu_bo_free_kernel(&memory_pool->bo,
			      &memory_pool->mc_address,
			      &memory_pool->cpu_addr);

	memset(memory_pool, 0, sizeof(struct smu_table));

	return ret;
}
static int smu_hw_init(void *handle)
{
	int ret;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct smu_context *smu = &adev->smu;

	if (!is_support_sw_smu(adev))
		return -EINVAL;

	if (adev->firmware.load_type != AMDGPU_FW_LOAD_PSP) {
		ret = smu_load_microcode(smu);
		if (ret)
			return ret;
	}

	ret = smu_check_fw_status(smu);
	if (ret) {
		pr_err("SMC firmware status is not correct\n");
		return ret;
	}

	mutex_lock(&smu->mutex);

	ret = smu_smc_table_hw_init(smu);
	if (ret)
		goto failed;

	ret = smu_start_thermal_control(smu);
	if (ret)
		goto failed;

	mutex_unlock(&smu->mutex);

	pr_info("SMU is initialized successfully!\n");

	return 0;

failed:
	mutex_unlock(&smu->mutex);
	return ret;
}

static int smu_hw_fini(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct smu_context *smu = &adev->smu;
	struct smu_table_context *table_context = &smu->smu_table;
	int ret = 0;

	if (!is_support_sw_smu(adev))
		return -EINVAL;

	if (table_context->driver_pptable) {
		kfree(table_context->driver_pptable);
		table_context->driver_pptable = NULL;
	}

	if (table_context->max_sustainable_clocks) {
		kfree(table_context->max_sustainable_clocks);
		table_context->max_sustainable_clocks = NULL;
	}

	ret = smu_fini_fb_allocations(smu);
	if (ret)
		return ret;

	ret = smu_free_memory_pool(smu);
	if (ret)
		return ret;

	return 0;
}

int smu_reset(struct smu_context *smu)
{
	struct amdgpu_device *adev = smu->adev;
	int ret = 0;

	ret = smu_hw_fini(adev);
	if (ret)
		return ret;

	ret = smu_hw_init(adev);
	if (ret)
		return ret;

	return ret;
}

static int smu_suspend(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	if (!is_support_sw_smu(adev))
		return -EINVAL;

	return 0;
}

static int smu_resume(void *handle)
{
	int ret;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct smu_context *smu = &adev->smu;

	if (!is_support_sw_smu(adev))
		return -EINVAL;

	mutex_lock(&smu->mutex);

	/* TODO */

	mutex_unlock(&smu->mutex);

	return 0;
}

int smu_display_configuration_change(struct smu_context *smu,
				     const struct amd_pp_display_configuration *display_config)
{
	int index = 0;
	int num_of_active_display = 0;

	if (!is_support_sw_smu(smu->adev))
		return -EINVAL;

	if (!display_config)
		return -EINVAL;

	mutex_lock(&smu->mutex);

	smu_set_deep_sleep_dcefclk(smu,
				   display_config->min_dcef_deep_sleep_set_clk / 100);

	for (index = 0; index < display_config->num_path_including_non_display; index++) {
		if (display_config->displays[index].controller_id != 0)
			num_of_active_display++;
	}

	smu_set_active_display_count(smu, num_of_active_display);

	smu_store_cc6_data(smu, display_config->cpu_pstate_separation_time,
			   display_config->cpu_cc6_disable,
			   display_config->cpu_pstate_disable,
			   display_config->nb_pstate_switch_disable);

	mutex_unlock(&smu->mutex);

	return 0;
}

static int smu_get_clock_info(struct smu_context *smu,
			      struct smu_clock_info *clk_info,
			      enum smu_perf_level_designation designation)
{
	int ret;
	struct smu_performance_level level = {0};

	if (!clk_info)
		return -EINVAL;

	ret = smu_get_perf_level(smu, PERF_LEVEL_ACTIVITY, &level);
	if (ret)
		return -EINVAL;

	clk_info->min_mem_clk = level.memory_clock;
	clk_info->min_eng_clk = level.core_clock;
	clk_info->min_bus_bandwidth = level.non_local_mem_freq * level.non_local_mem_width;

	ret = smu_get_perf_level(smu, designation, &level);
	if (ret)
		return -EINVAL;

	clk_info->min_mem_clk = level.memory_clock;
	clk_info->min_eng_clk = level.core_clock;
	clk_info->min_bus_bandwidth = level.non_local_mem_freq * level.non_local_mem_width;

	return 0;
}

int smu_get_current_clocks(struct smu_context *smu,
			   struct amd_pp_clock_info *clocks)
{
	struct amd_pp_simple_clock_info simple_clocks = {0};
	struct smu_clock_info hw_clocks;
	int ret = 0;

	if (!is_support_sw_smu(smu->adev))
		return -EINVAL;

	mutex_lock(&smu->mutex);

	smu_get_dal_power_level(smu, &simple_clocks);

	if (smu->support_power_containment)
		ret = smu_get_clock_info(smu, &hw_clocks,
					 PERF_LEVEL_POWER_CONTAINMENT);
	else
		ret = smu_get_clock_info(smu, &hw_clocks, PERF_LEVEL_ACTIVITY);

	if (ret) {
		pr_err("Error in smu_get_clock_info\n");
		goto failed;
	}

	clocks->min_engine_clock = hw_clocks.min_eng_clk;
	clocks->max_engine_clock = hw_clocks.max_eng_clk;
	clocks->min_memory_clock = hw_clocks.min_mem_clk;
	clocks->max_memory_clock = hw_clocks.max_mem_clk;
	clocks->min_bus_bandwidth = hw_clocks.min_bus_bandwidth;
	clocks->max_bus_bandwidth = hw_clocks.max_bus_bandwidth;
	clocks->max_engine_clock_in_sr = hw_clocks.max_eng_clk;
	clocks->min_engine_clock_in_sr = hw_clocks.min_eng_clk;

        if (simple_clocks.level == 0)
                clocks->max_clocks_state = PP_DAL_POWERLEVEL_7;
        else
                clocks->max_clocks_state = simple_clocks.level;

        if (!smu_get_current_shallow_sleep_clocks(smu, &hw_clocks)) {
                clocks->max_engine_clock_in_sr = hw_clocks.max_eng_clk;
                clocks->min_engine_clock_in_sr = hw_clocks.min_eng_clk;
        }

failed:
	mutex_unlock(&smu->mutex);
	return ret;
}

static int smu_set_clockgating_state(void *handle,
				     enum amd_clockgating_state state)
{
	return 0;
}

static int smu_set_powergating_state(void *handle,
				     enum amd_powergating_state state)
{
	return 0;
}

const struct amd_ip_funcs smu_ip_funcs = {
	.name = "smu",
	.early_init = smu_early_init,
	.late_init = NULL,
	.sw_init = smu_sw_init,
	.sw_fini = smu_sw_fini,
	.hw_init = smu_hw_init,
	.hw_fini = smu_hw_fini,
	.suspend = smu_suspend,
	.resume = smu_resume,
	.is_idle = NULL,
	.check_soft_reset = NULL,
	.wait_for_idle = NULL,
	.soft_reset = NULL,
	.set_clockgating_state = smu_set_clockgating_state,
	.set_powergating_state = smu_set_powergating_state,
};

const struct amdgpu_ip_block_version smu_v11_0_ip_block =
{
	.type = AMD_IP_BLOCK_TYPE_SMC,
	.major = 11,
	.minor = 0,
	.rev = 0,
	.funcs = &smu_ip_funcs,
};