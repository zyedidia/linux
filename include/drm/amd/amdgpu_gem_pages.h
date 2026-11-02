/* SPDX-License-Identifier: MIT */
#ifndef __AMDGPU_GEM_PAGES_H__
#define __AMDGPU_GEM_PAGES_H__

#include <linux/types.h>

struct drm_gem_object;
struct page;

int amdgpu_gem_gtt_pages(struct drm_gem_object *gobj, struct page ***pages,
			 unsigned long *npages);
void amdgpu_gem_set_move_notify(struct drm_gem_object *gobj,
				void (*fn)(void *priv), void *priv);
int amdgpu_gem_vram_ranges(struct drm_gem_object *gobj,
			   int (*fn)(void *arg, u64 offset, u64 size),
			   void *arg);
int amdgpu_gem_pin_placed(struct drm_gem_object *gobj);
int amdgpu_gem_pin_vram(struct drm_gem_object *gobj);
void amdgpu_gem_unpin(struct drm_gem_object *gobj);

#endif
