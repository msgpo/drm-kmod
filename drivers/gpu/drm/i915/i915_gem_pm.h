/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#ifndef __I915_GEM_PM_H__
#define __I915_GEM_PM_H__

#include <linux/types.h>

struct drm_i915_private;
struct work_struct;

void i915_gem_init__pm(struct drm_i915_private *i915);

bool i915_gem_load_power_context(struct drm_i915_private *i915);
void i915_gem_resume(struct drm_i915_private *i915);

void i915_gem_unpark(struct drm_i915_private *i915);
void i915_gem_park(struct drm_i915_private *i915);

void i915_gem_idle_work_handler(struct work_struct *work);

void i915_gem_suspend(struct drm_i915_private *i915);
void i915_gem_suspend_late(struct drm_i915_private *i915);

#endif /* __I915_GEM_PM_H__ */