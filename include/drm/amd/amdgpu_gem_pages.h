/* SPDX-License-Identifier: MIT */
#ifndef __AMDGPU_GEM_PAGES_H__
#define __AMDGPU_GEM_PAGES_H__

struct drm_gem_object;
struct page;

int amdgpu_gem_pin_gtt_pages(struct drm_gem_object *gobj, struct page ***pages,
			     unsigned long *npages);
void amdgpu_gem_unpin_gtt_pages(struct drm_gem_object *gobj);

#endif
