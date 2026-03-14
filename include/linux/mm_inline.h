/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_MM_INLINE_H
#define LINUX_MM_INLINE_H

#include <linux/huge_mm.h>
#include <linux/swap.h>

#ifdef CONFIG_MAPPED_PROTECT
extern void mapped_page_try_sorthead(struct page *page);
extern unsigned long page_should_be_protect(struct page *page);
extern bool update_mapped_mul(struct page *page, bool inc_size);
extern void add_mapped_mul_op_lrulist(struct page *page, enum lru_list lru);
extern void dec_mapped_mul_op_lrulist(struct page *page, enum lru_list lru);
extern long multi_mapped(int file);
extern long multi_mapped_max(int file);
#endif

static inline int page_is_file_cache(struct page *page)
{
	return !PageSwapBacked(page);
}

static __always_inline void __update_lru_size(struct lruvec *lruvec,
				enum lru_list lru, enum zone_type zid,
				int nr_pages)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	lockdep_assert_held(&pgdat->lru_lock);

	__mod_node_page_state(pgdat, NR_LRU_BASE + lru, nr_pages);
	__mod_zone_page_state(&pgdat->node_zones[zid],
				NR_ZONE_LRU_BASE + lru, nr_pages);
}

static __always_inline void update_lru_size(struct lruvec *lruvec,
				enum lru_list lru, enum zone_type zid,
				int nr_pages)
{
	__update_lru_size(lruvec, lru, zid, nr_pages);
#ifdef CONFIG_MEMCG
	mem_cgroup_update_lru_size(lruvec, lru, zid, nr_pages);
#endif
}

static __always_inline void __clear_page_lru_flags(struct page *page)
{
	VM_BUG_ON_PAGE(!PageLRU(page), page);

	__ClearPageLRU(page);

	if (PageActive(page) && PageUnevictable(page))
		return;

	__ClearPageActive(page);
	__ClearPageUnevictable(page);
}

static __always_inline enum lru_list page_lru(struct page *page)
{
	enum lru_list lru;

	VM_BUG_ON_PAGE(PageActive(page) && PageUnevictable(page), page);

	if (PageUnevictable(page))
		return LRU_UNEVICTABLE;

	lru = page_is_file_cache(page) ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON;
	if (PageActive(page))
		lru += LRU_ACTIVE;

	return lru;
}

#define lru_to_page(head) (list_entry((head)->prev, struct page, lru))

#ifdef CONFIG_LRU_GEN

static inline bool lru_gen_enabled(void)
{
    return true;
}
static inline bool lru_gen_in_fault(void)
{
	return current->in_lru_fault;
}

static inline int lru_gen_from_seq(unsigned long seq)
{
	return seq % MAX_NR_GENS;
}

static inline int lru_hist_from_seq(unsigned long seq)
{
	return seq % NR_HIST_GENS;
}

static inline int lru_tier_from_refs(int refs)
{
	VM_BUG_ON(refs > BIT(LRU_REFS_WIDTH));
	return order_base_2(refs + 1);
}

static inline bool lru_gen_is_active(struct lruvec *lruvec, int gen)
{
	unsigned long max_seq = lruvec->lrugen.max_seq;
	VM_BUG_ON(gen >= MAX_NR_GENS);
	return gen == lru_gen_from_seq(max_seq) || gen == lru_gen_from_seq(max_seq - 1);
}

static inline void lru_gen_update_size(struct lruvec *lruvec, struct page *page,
				       int old_gen, int new_gen)
{
	int type = page_is_file_cache(page);
	int zone = page_zonenum(page);
	int delta = hpage_nr_pages(page);
	enum lru_list lru = type * LRU_INACTIVE_FILE;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	if (old_gen >= 0)
		WRITE_ONCE(lrugen->nr_pages[old_gen][type][zone],
			   lrugen->nr_pages[old_gen][type][zone] - delta);
	if (new_gen >= 0)
		WRITE_ONCE(lrugen->nr_pages[new_gen][type][zone],
			   lrugen->nr_pages[new_gen][type][zone] + delta);

	if (old_gen < 0) {
		if (lru_gen_is_active(lruvec, new_gen))
			lru += LRU_ACTIVE;
		update_lru_size(lruvec, lru, zone, delta);
		return;
	}

	if (new_gen < 0) {
		if (lru_gen_is_active(lruvec, old_gen))
			lru += LRU_ACTIVE;
		update_lru_size(lruvec, lru, zone, -delta);
		return;
	}

	if (!lru_gen_is_active(lruvec, old_gen) && lru_gen_is_active(lruvec, new_gen)) {
		update_lru_size(lruvec, lru, zone, -delta);
		update_lru_size(lruvec, lru + LRU_ACTIVE, zone, delta);
	}
}

static inline bool lru_gen_add_page(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	int gen;
	unsigned long old_flags, new_flags;
	int type = page_is_file_cache(page);
	int zone = page_zonenum(page);
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	if (PageUnevictable(page) || !lrugen->enabled)
		return false;

	if (PageActive(page))
		gen = lru_gen_from_seq(lrugen->max_seq);
	else if ((type == LRU_GEN_ANON && !PageSwapCache(page)) ||
		 (PageReclaim(page) && (PageDirty(page) || PageWriteback(page))))
		gen = lru_gen_from_seq(lrugen->max_seq - 1);
	else
		gen = lru_gen_from_seq(lrugen->min_seq[type]);

	do {
		new_flags = old_flags = READ_ONCE(page->flags);
		VM_BUG_ON_PAGE(new_flags & LRU_GEN_MASK, page);
		new_flags &= ~(LRU_GEN_MASK | BIT(PG_active));
		new_flags |= (gen + 1UL) << LRU_GEN_PGOFF;
	} while (cmpxchg(&page->flags, old_flags, new_flags) != old_flags);

	lru_gen_update_size(lruvec, page, -1, gen);

	if (reclaiming)
		list_add_tail(&page->lru, &lrugen->lists[gen][type][zone]);
	else
		list_add(&page->lru, &lrugen->lists[gen][type][zone]);

	return true;
}

static inline bool lru_gen_del_page(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	unsigned long x;
	unsigned long old_flags, new_flags;

	do {
		new_flags = old_flags = READ_ONCE(page->flags);
		if (!(new_flags & LRU_GEN_MASK))
			return false;
		new_flags &= ~LRU_GEN_MASK;
	} while (cmpxchg(&page->flags, old_flags, new_flags) != old_flags);

	x = ((old_flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
	lru_gen_update_size(lruvec, page, x, -1);
	list_del(&page->lru);

	return true;
}

#else /* !CONFIG_LRU_GEN */

static inline bool lru_gen_add_page(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	return false;
}

static inline bool lru_gen_del_page(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	return false;
}

#endif /* CONFIG_LRU_GEN */

/* --- 底部标准接口 --- */
static __always_inline void add_page_to_lru_list(struct page *page,
				struct lruvec *lruvec)
{
	enum lru_list lru = page_lru(page);

#ifdef CONFIG_LRU_GEN
	if (lru_gen_add_page(lruvec, page, false))
		return;
#endif

#ifdef CONFIG_MAPPED_PROTECT
	add_mapped_mul_op_lrulist(page, lru);
#endif
	update_lru_size(lruvec, lru, page_zonenum(page), hpage_nr_pages(page));
	list_add(&page->lru, &lruvec->lists[lru]);
}

static __always_inline void add_page_to_lru_list_tail(struct page *page,
				struct lruvec *lruvec)
{
	enum lru_list lru = page_lru(page);

#ifdef CONFIG_LRU_GEN
	if (lru_gen_add_page(lruvec, page, true))
		return;
#endif

#ifdef CONFIG_MAPPED_PROTECT
	add_mapped_mul_op_lrulist(page, lru);
#endif
	update_lru_size(lruvec, lru, page_zonenum(page), hpage_nr_pages(page));
	list_add_tail(&page->lru, &lruvec->lists[lru]);
}

static __always_inline void del_page_from_lru_list(struct page *page,
				struct lruvec *lruvec)
{
	enum lru_list lru = page_lru(page);

#ifdef CONFIG_MAPPED_PROTECT
	dec_mapped_mul_op_lrulist(page, lru);
#endif

#ifdef CONFIG_LRU_GEN
	if (lru_gen_del_page(lruvec, page, false))
		return;
#endif
	list_del(&page->lru);
	update_lru_size(lruvec, lru, page_zonenum(page), -hpage_nr_pages(page));
}

#endif /* LINUX_MM_INLINE_H */
