/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The transport keeps the state that the host reads, or that decides what the
 * host reads, in protected functions and memory. Under UML with protection
 * keys, the protected functions run in gates and the memory is in the window,
 * where the rest of the guest cannot reach it. Elsewhere they are ordinary
 * functions and memory.
 *
 * A gate runs with signals off and never sleeps, so a protected function only
 * takes the locks here. A mutex is a spinlock in a gate, and a mutex
 * elsewhere, for hosts whose operations sleep.
 */
#ifndef _VDUSE_GUEST_PROT_H
#define _VDUSE_GUEST_PROT_H

#include <linux/overflow.h>
#include <linux/slab.h>

#ifdef CONFIG_UML_MPK

#include <asm/mpk.h>

#define __vduse_prot			__uml_mpk_entry
#define vduse_prot_call(fn, ...)	uml_mpk_gate(fn, __VA_ARGS__)

struct vduse_prot_lock {
	struct uml_mpk_lock l;
};

#define vduse_prot_lock_init(lock)	((lock)->l.locked = 0)
#define vduse_prot_lock(lock, flags)	\
	do { (void)(flags); uml_mpk_lock(&(lock)->l); } while (0)
#define vduse_prot_unlock(lock, flags)	uml_mpk_unlock(&(lock)->l)

#define vduse_prot_mutex		vduse_prot_lock
#define vduse_prot_mutex_init(lock)	vduse_prot_lock_init(lock)
#define vduse_prot_mutex_lock(lock)	uml_mpk_lock(&(lock)->l)
#define vduse_prot_mutex_unlock(lock)	uml_mpk_unlock(&(lock)->l)

/* Whether the protected memory is out of the rest of the guest's reach. */
static inline bool vduse_prot_active(void)
{
	return uml_mpk_enabled;
}

static inline void *vduse_prot_alloc(size_t size)
{
	return uml_mpk_enabled ? uml_mpk_alloc(size) :
				 kzalloc(size, GFP_ATOMIC);
}

static inline void vduse_prot_free(void *p)
{
	if (uml_mpk_enabled)
		uml_mpk_free(p);
	else
		kfree(p);
}

/* Whether memory that a protected function writes is outside the window. */
static inline bool vduse_prot_outside(const void *p, size_t len)
{
	return !uml_mpk_overlaps((unsigned long)p, len);
}

/*
 * Move a mapping into protected memory. Afterwards, *addr is its new address,
 * which vduse_prot_unmap() unmaps.
 */
static inline int vduse_prot_adopt(void **addr, size_t size)
{
	if (!uml_mpk_enabled)
		return 0;
	return uml_mpk_adopt(*addr, size, 1, 1, addr);
}

static inline void vduse_prot_unmap(void *addr, size_t size)
{
	uml_mpk_unmap(addr, size);
}

#else

#include <linux/mutex.h>
#include <linux/spinlock.h>

#define __vduse_prot
#define vduse_prot_call(fn, ...)	fn(__VA_ARGS__)

struct vduse_prot_lock {
	spinlock_t l;
};

#define vduse_prot_lock_init(lock)	spin_lock_init(&(lock)->l)
#define vduse_prot_lock(lock, flags)	spin_lock_irqsave(&(lock)->l, flags)
#define vduse_prot_unlock(lock, flags)	\
	spin_unlock_irqrestore(&(lock)->l, flags)

struct vduse_prot_mutex {
	struct mutex l;
};

#define vduse_prot_mutex_init(lock)	mutex_init(&(lock)->l)
#define vduse_prot_mutex_lock(lock)	mutex_lock(&(lock)->l)
#define vduse_prot_mutex_unlock(lock)	mutex_unlock(&(lock)->l)

static inline bool vduse_prot_active(void)
{
	return false;
}

static inline void *vduse_prot_alloc(size_t size)
{
	return kzalloc(size, GFP_KERNEL);
}

static inline void vduse_prot_free(void *p)
{
	kfree(p);
}

static inline bool vduse_prot_outside(const void *p, size_t len)
{
	return true;
}

static inline int vduse_prot_adopt(void **addr, size_t size)
{
	return 0;
}

static inline void vduse_prot_unmap(void *addr, size_t size)
{
}

#endif

static inline void *vduse_prot_alloc_array(size_t n, size_t size)
{
	size_t bytes;

	if (check_mul_overflow(n, size, &bytes))
		return NULL;
	return vduse_prot_alloc(bytes);
}

#endif /* _VDUSE_GUEST_PROT_H */
