// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 * Changyuan Lyu <changyuanl@google.com>
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <ptyadav@amazon.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/file.h>
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/liveupdate.h>
#include <linux/kexec_handover.h>
#include <linux/kho_array.h>
#include <linux/shmem_fs.h>
#include <linux/bits.h>
#include "internal.h"

static const char memfd_luo_compatible[] = "memfd-v1";

/*
 * The preserved folio descriptor is made of 2 parts. The bottom 11 bits are
 * used for storing flags, the others for storing the PFN. 11 bits are used
 * instead of 12 since we are going to store it in struct kho_array as a value,
 * and that only accepts values up to LONG_MAX.
 */
#define PRESERVED_PFN_SHIFT		11
#define PRESERVED_PFN_MASK		GENMASK(63, PRESERVED_PFN_SHIFT)
#define PRESERVED_FLAG_DIRTY		BIT(0)
#define PRESERVED_FLAG_UPTODATE		BIT(1)

#define PRESERVED_FOLIO_PFN(desc)	(((desc) & PRESERVED_PFN_MASK) >> PRESERVED_PFN_SHIFT)
#define PRESERVED_FOLIO_FLAGS(desc)	((desc) & ~PRESERVED_PFN_MASK)
#define PRESERVED_FOLIO_MKDESC(pfn, flags) (((pfn) << PRESERVED_PFN_SHIFT) | (flags))

static int memfd_luo_preserve_folios(struct kho_array *ka,
				     struct folio **folios,
				     unsigned int nr_folios)
{
	struct ka_iter iter;
	int err;
	long i;

	ka_iter_init_write(&iter, ka);

	for (i = 0; i < nr_folios; i++) {
		struct folio *folio = folios[i];
		unsigned int flags = 0;
		unsigned long pfn;

		if (ka_iter_setpos(&iter, folio->index))
			goto err_unpreserve;

		err = kho_preserve_folio(folio);
		if (err)
			goto err_unpreserve;

		pfn = folio_pfn(folio);
		if (folio_test_dirty(folio))
			flags |= PRESERVED_FLAG_DIRTY;
		if (folio_test_uptodate(folio))
			flags |= PRESERVED_FLAG_UPTODATE;

		err = ka_iter_setentry(&iter, ka_mk_value(PRESERVED_FOLIO_MKDESC(pfn, flags)));
		if (err) {
			WARN_ON_ONCE(kho_unpreserve_folio(folio));
			goto err_unpreserve;
		}
	}

	err = kho_array_preserve(ka);
	if (err)
		goto err_unpreserve;

	return 0;

err_unpreserve:
	i--;
	for (; i >= 0; i--)
		WARN_ON_ONCE(kho_unpreserve_folio(folios[i]));

	kho_array_destroy(ka);
	return err;
}

static void memfd_luo_unpreserve_folios(struct kho_array *ka)
{
	struct ka_iter iter;
	void *entry;

	ka_iter_init_read(&iter, ka);

	ka_iter_for_each(&iter, entry) {
		unsigned long foliodesc = ka_to_value(entry);
		struct folio *folio;

		folio = pfn_folio(PRESERVED_FOLIO_PFN(foliodesc));
		WARN_ON_ONCE(kho_unpreserve_folio(folio));
		unpin_folio(folio);
	}

	WARN_ON_ONCE(kho_array_unpreserve(ka));
	kho_array_destroy(ka);
}

static void *memfd_luo_create_fdt(unsigned long size)
{
	struct folio *fdt_folio;
	int err = 0;
	void *fdt;

	fdt_folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	if (!fdt_folio)
		return NULL;

	fdt = folio_address(fdt_folio);

	err |= fdt_create(fdt, PAGE_SIZE);
	err |= fdt_finish_reservemap(fdt);
	err |= fdt_begin_node(fdt, "");
	if (err)
		goto free;

	return fdt;

free:
	folio_put(fdt_folio);
	return NULL;
}

static int memfd_luo_finish_fdt(void *fdt)
{
	int err;

	err = fdt_end_node(fdt);
	if (err)
		return err;

	return fdt_finish(fdt);
}

/* TODO: Review the code after conversion to kho_array. */
static int memfd_luo_prepare(struct liveupdate_file_handler *handler,
			     struct file *file, u64 *data)
{
	struct inode *inode = file_inode(file);
	unsigned int max_folios, nr_folios = 0;
	struct folio **folios;
	struct kho_array *ka;
	long size, nr_pinned;
	pgoff_t offset;
	int err = 0;
	void *fdt;
	u64 pos;

	inode_lock(inode);
	shmem_i_mapping_freeze(inode, true);

	size = i_size_read(inode);
	if ((PAGE_ALIGN(size) / PAGE_SIZE) > UINT_MAX) {
		err = -E2BIG;
		goto err_unlock;
	}

	/*
	 * Guess the number of folios based on inode size. Real number might end
	 * up being smaller if there are higher order folios.
	 */
	max_folios = PAGE_ALIGN(size) / PAGE_SIZE;
	folios = kvmalloc_array(max_folios, sizeof(*folios), GFP_KERNEL);
	if (!folios) {
		err = -ENOMEM;
		goto err_unfreeze;
	}

	/*
	 * Pin the folios so they don't move around behind our back. This also
	 * ensures none of the folios are in CMA -- which ensures they don't
	 * fall in KHO scratch memory. It also moves swapped out folios back to
	 * memory.
	 *
	 * A side effect of doing this is that it allocates a folio for all
	 * indices in the file. This might waste memory on sparse memfds. If
	 * that is really a problem in the future, we can have a
	 * memfd_pin_folios() variant that does not allocate a page on empty
	 * slots.
	 */
	nr_pinned = memfd_pin_folios(file, 0, size - 1, folios, max_folios,
				     &offset);
	if (nr_pinned < 0) {
		err = nr_pinned;
		pr_err("failed to pin folios: %d\n", err);
		goto err_free_folios;
	}
	/* nr_pinned won't be more than max_folios which is also unsigned int. */
	nr_folios = (unsigned int)nr_pinned;

	fdt = memfd_luo_create_fdt(PAGE_SIZE);
	if (!fdt) {
		err = -ENOMEM;
		goto err_unpin;
	}

	pos = file->f_pos;
	err = fdt_property(fdt, "pos", &pos, sizeof(pos));
	if (err)
		goto err_free_fdt;

	err = fdt_property(fdt, "size", &size, sizeof(size));
	if (err)
		goto err_free_fdt;

	err = fdt_property_placeholder(fdt, "folios", sizeof(*ka), (void **)&ka);
	if (err) {
		pr_err("Failed to reserve folios property in FDT: %s\n",
		       fdt_strerror(err));
		err = -ENOMEM;
		goto err_free_fdt;
	}

	err = memfd_luo_preserve_folios(ka, folios, nr_folios);
	if (err)
		goto err_free_fdt;

	err = memfd_luo_finish_fdt(fdt);
	if (err)
		goto err_unpreserve;

	err = kho_preserve_folio(virt_to_folio(fdt));
	if (err)
		goto err_unpreserve;

	kvfree(folios);
	inode_unlock(inode);

	*data = virt_to_phys(fdt);
	return 0;

err_unpreserve:
	memfd_luo_unpreserve_folios(ka);
err_free_fdt:
	folio_put(virt_to_folio(fdt));
err_unpin:
	unpin_folios(folios, nr_pinned);
err_free_folios:
	kvfree(folios);
err_unfreeze:
	shmem_i_mapping_freeze(inode, false);
err_unlock:
	inode_unlock(inode);
	return err;
}

static int memfd_luo_freeze(struct liveupdate_file_handler *handler,
			    struct file *file, u64 *data)
{
	u64 pos = file->f_pos;
	void *fdt;
	int err;

	if (WARN_ON_ONCE(!*data))
		return -EINVAL;

	fdt = phys_to_virt(*data);

	/*
	 * The pos might have changed since prepare. Everything else stays the
	 * same.
	 */
	err = fdt_setprop(fdt, 0, "pos", &pos, sizeof(pos));
	if (err)
		return err;

	return 0;
}

static void memfd_luo_cancel(struct liveupdate_file_handler *handler,
			     struct file *file, u64 data)
{
	struct inode *inode = file_inode(file);
	struct folio *fdt_folio;
	struct kho_array *ka;
	void *fdt;
	int len;

	if (WARN_ON_ONCE(!data))
		return;

	inode_lock(inode);
	shmem_i_mapping_freeze(inode, false);

	fdt = phys_to_virt(data);
	fdt_folio = virt_to_folio(fdt);
	ka = (struct kho_array *)fdt_getprop(folio_address(fdt_folio), 0, "folios", &len);
	memfd_luo_unpreserve_folios(ka);

	kho_unpreserve_folio(fdt_folio);
	folio_put(fdt_folio);
	inode_unlock(inode);
}

static struct folio *memfd_luo_get_fdt(u64 data)
{
	return kho_restore_folio((phys_addr_t)data);
}

static void memfd_luo_discard_folios(struct kho_array *ka)
{
	struct ka_iter iter;
	void *entry;
	int err;

	err = kho_array_restore(ka);
	if (err) {
		pr_err("failed to restore 'folios' array: %d\n", err);
		return;
	}
	ka_iter_init_read(&iter, ka);

	ka_iter_for_each(&iter, entry) {
		unsigned long foliodesc = ka_to_value(entry);
		struct folio *folio;
		phys_addr_t phys;

		phys = PFN_PHYS(PRESERVED_FOLIO_PFN(foliodesc));
		folio = kho_restore_folio(phys);
		if (!folio) {
			pr_warn_ratelimited("Unable to restore folio at physical address: %llx\n",
					    phys);
			continue;
		}

		folio_put(folio);
	}

	kho_array_destroy(ka);
}

static void memfd_luo_finish(struct liveupdate_file_handler *handler,
			     struct file *file, u64 data, bool reclaimed)
{
	struct folio *fdt_folio;
	struct kho_array *ka;
	int len;

	if (reclaimed)
		return;

	fdt_folio = memfd_luo_get_fdt(data);

	ka = (struct kho_array *)fdt_getprop(folio_address(fdt_folio), 0, "folios", &len);
	if (!ka || len != sizeof(*ka))
		pr_warn("invalid 'folios' property\n");
	else
		memfd_luo_discard_folios(ka);

	folio_put(fdt_folio);
}

/*
 * TODO: Review the code after conversion to kho_array.
 * Also, perhaps split this function into helpers to make it simpler?
 */
static int memfd_luo_retrieve(struct liveupdate_file_handler *handler, u64 data,
			      struct file **file_p)
{
	struct address_space *mapping;
	struct folio *folio, *fdt_folio;
	const u64 *pos, *size;
	struct kho_array *ka;
	struct ka_iter iter;
	struct inode *inode;
	struct file *file;
	int len, ret = 0;
	const void *fdt;
	void *entry;

	fdt_folio = memfd_luo_get_fdt(data);
	if (!fdt_folio)
		return -ENOENT;

	fdt = page_to_virt(folio_page(fdt_folio, 0));

	ka = (struct kho_array *)fdt_getprop(fdt, 0, "folios", &len);
	if (!ka || len != sizeof(*ka)) {
		pr_err("invalid 'folios' property\n");
		ret = -EINVAL;
		goto put_fdt;
	}

	ret = kho_array_restore(ka);
	if (ret) {
		pr_err("Failed to restore 'folios' array\n");
		goto put_fdt;
	}
	ka_iter_init_read(&iter, ka);

	size = fdt_getprop(fdt, 0, "size", &len);
	if (!size || len != sizeof(u64)) {
		pr_err("invalid 'size' property\n");
		ret = -EINVAL;
		goto put_folios;
	}

	pos = fdt_getprop(fdt, 0, "pos", &len);
	if (!pos || len != sizeof(u64)) {
		pr_err("invalid 'pos' property\n");
		ret = -EINVAL;
		goto put_folios;
	}

	file = shmem_file_setup("", 0, VM_NORESERVE);

	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		pr_err("failed to setup file: %d\n", ret);
		goto put_folios;
	}

	inode = file->f_inode;
	mapping = inode->i_mapping;
	vfs_setpos(file, *pos, MAX_LFS_FILESIZE);

	ka_iter_for_each(&iter, entry) {
		unsigned long foliodesc = ka_to_value(entry);
		phys_addr_t phys;
		u64 index;
		int flags;

		phys = PFN_PHYS(PRESERVED_FOLIO_PFN(foliodesc));

		folio = kho_restore_folio(phys);
		if (!folio) {
			pr_err("Unable to restore folio at physical address: %llx\n",
			       phys);
			goto put_file;
		}
		index = ka_iter_getpos(&iter);
		flags = PRESERVED_FOLIO_FLAGS(foliodesc);

		/* Set up the folio for insertion. */
		__folio_set_locked(folio);
		__folio_set_swapbacked(folio);

		ret = mem_cgroup_charge(folio, NULL, mapping_gfp_mask(mapping));
		if (ret) {
			pr_err("shmem: failed to charge folio index %ld: %d\n",
			       ka_iter_getpos(&iter), ret);
			goto unlock_folio;
		}

		ret = shmem_add_to_page_cache(folio, mapping, index, NULL,
					      mapping_gfp_mask(mapping));
		if (ret) {
			pr_err("shmem: failed to add to page cache folio index %ld: %d\n",
			       ka_iter_getpos(&iter), ret);
			goto unlock_folio;
		}

		if (flags & PRESERVED_FLAG_UPTODATE)
			folio_mark_uptodate(folio);
		if (flags & PRESERVED_FLAG_DIRTY)
			folio_mark_dirty(folio);

		ret = shmem_inode_acct_blocks(inode, 1);
		if (ret) {
			pr_err("shmem: failed to account folio index %ld: %d\n",
			       ka_iter_getpos(&iter), ret);
			goto unlock_folio;
		}

		shmem_recalc_inode(inode, 1, 0);
		folio_add_lru(folio);

		/*
		 * shmem_add_to_page_cache() takes a ref on the folio and locks
		 * it because usually the caller intends to use the folio. We
		 * don't, so drop the ref and lock.
		 */
		folio_unlock(folio);
		folio_put(folio);
	}

	inode->i_size = *size;
	*file_p = file;
	kho_array_destroy(ka);
	folio_put(fdt_folio);
	return 0;

unlock_folio:
	folio_unlock(folio);
	folio_put(folio);
put_file:
	fput(file);
	ka_iter_nextentry(&iter);
put_folios:
	ka_iter_for_each(&iter, entry) {
		unsigned long foliodesc = ka_to_value(entry);

		folio = kho_restore_folio(PRESERVED_FOLIO_PFN(foliodesc));
		if (folio)
			folio_put(folio);
	}

	kho_array_destroy(ka);
put_fdt:
	folio_put(fdt_folio);
	return ret;
}

static bool memfd_luo_can_preserve(struct liveupdate_file_handler *handler,
				   struct file *file)
{
	struct inode *inode = file_inode(file);

	return shmem_file(file) && !inode->i_nlink;
}

static const struct liveupdate_file_ops memfd_luo_file_ops = {
	.prepare = memfd_luo_prepare,
	.freeze = memfd_luo_freeze,
	.cancel = memfd_luo_cancel,
	.finish = memfd_luo_finish,
	.retrieve = memfd_luo_retrieve,
	.can_preserve = memfd_luo_can_preserve,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler memfd_luo_handler = {
	.ops = &memfd_luo_file_ops,
	.compatible = memfd_luo_compatible,
};

static int __init memfd_luo_init(void)
{
	int err;

	err = liveupdate_register_file_handler(&memfd_luo_handler);
	if (err)
		pr_err("Could not register luo filesystem handler: %d\n", err);

	return err;
}
late_initcall(memfd_luo_init);
