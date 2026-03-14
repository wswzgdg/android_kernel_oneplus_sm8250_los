// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/jiffies.h>
#include <linux/hashtable.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/rmap.h>

/*
 * Lightweight MGLRU for bring-up:
 * - no page-table walk
 * - no memcg mm list
 * - generation queues + reclaim from oldest generation
 * - no KMI-visible struct layout changes
 */

#define MGLRU_MAX_NR_GENS	4
#define MGLRU_MIN_BATCH		64UL
#define MGLRU_MAX_TIERS		3
#define MGLRU_MAX_BOOST_LEVEL	3
#define MGLRU_ACCESS_CTRL_BATCH	128UL
#define MGLRU_PTWALK_BATCH	32UL

#ifndef thp_nr_pages
#define thp_nr_pages(page) hpage_nr_pages(page)
#endif

struct mglru_lruvec_state {
	struct hlist_node node;
	struct lruvec *lruvec;
	unsigned long max_seq;
	unsigned long min_seq[ANON_AND_FILE];
	unsigned long next_age;
	unsigned long last_file_refault;
	unsigned long refault_boost_until;
	unsigned int refault_boost_level;
	unsigned long look_around_total;
	unsigned long look_around_hot;
	unsigned long ptwalk_total_seen;
	unsigned long ptwalk_hot_seen;
	unsigned long tier_budget[MGLRU_MAX_TIERS];
	unsigned long sanitize_retry_at[ANON_AND_FILE];
	struct list_head lists[MGLRU_MAX_NR_GENS][ANON_AND_FILE];
	unsigned long nr_total[ANON_AND_FILE];
};

static DEFINE_HASHTABLE(mglru_table, 8);
static DEFINE_SPINLOCK(mglru_table_lock);
static atomic64_t mglru_add_cnt = ATOMIC64_INIT(0);
static atomic64_t mglru_del_cnt = ATOMIC64_INIT(0);
static atomic64_t mglru_scan_pick_cnt = ATOMIC64_INIT(0);
static atomic64_t mglru_scan_fallback_cnt = ATOMIC64_INIT(0);
static atomic64_t mglru_tier_protect_skip_cnt = ATOMIC64_INIT(0);
static atomic64_t mglru_age_cnt = ATOMIC64_INIT(0);
static atomic64_t mglru_tier_add_cnt[MGLRU_MAX_TIERS];
static atomic64_t mglru_look_around_total_cnt = ATOMIC64_INIT(0);
static atomic64_t mglru_look_around_hot_cnt = ATOMIC64_INIT(0);
static atomic64_t mglru_access_promote_cnt = ATOMIC64_INIT(0);
static atomic64_t mglru_ptwalk_total_cnt = ATOMIC64_INIT(0);
static atomic64_t mglru_ptwalk_hot_cnt = ATOMIC64_INIT(0);
static atomic64_t mglru_ptwalk_clamp_cnt = ATOMIC64_INIT(0);
static unsigned long mglru_log_next_jiffies;

static inline bool mglru_debug_log_enabled(void)
{
	return IS_ENABLED(CONFIG_LRU_GEN_DEBUG_LOG);
}

static inline bool lru_gen_managed(enum lru_list lru)
{
	return lru == LRU_INACTIVE_ANON || lru == LRU_INACTIVE_FILE;
}

static inline void mglru_assert_lru_lock(struct lruvec *lruvec)
{
	lockdep_assert_held(&lruvec_pgdat(lruvec)->lru_lock);
}

static inline int mglru_gen_from_seq(unsigned long seq)
{
	return seq % MGLRU_MAX_NR_GENS;
}

static bool mglru_try_inc_min_seq(struct mglru_lruvec_state *state, int type)
{
	int gen;

	if (state->min_seq[type] >= state->max_seq)
		return false;

	gen = mglru_gen_from_seq(state->min_seq[type]);
	if (!list_empty(&state->lists[gen][type]))
		return false;

	state->min_seq[type]++;
	return true;
}

static int mglru_page_tier(struct page *page, enum lru_list lru)
{
	if (!is_file_lru(lru))
		return 0;

	if (PageWorkingset(page))
		return 2;

	if (PageReferenced(page))
		return 1;

	return 0;
}

static bool mglru_refault_boosted(struct mglru_lruvec_state *state)
{
	return time_before(jiffies, READ_ONCE(state->refault_boost_until));
}

static unsigned int mglru_get_boost_level(struct mglru_lruvec_state *state)
{
	if (!mglru_refault_boosted(state))
		return 0;

	return READ_ONCE(state->refault_boost_level);
}

static void mglru_refresh_tier_budget(struct mglru_lruvec_state *state)
{
	unsigned long file_total;
	unsigned long old_t1, old_t2;
	unsigned long target_t1, target_t2;
	unsigned long cap_t1, cap_t2;
	unsigned long floor_t1, floor_t2;
	unsigned long pt_total, pt_hot;
	unsigned long pt_ratio_x100 = 0;
	unsigned long t1;
	unsigned long t2;
	unsigned int boost_level;

	file_total = state->nr_total[1];
	if (!file_total) {
		state->tier_budget[0] = 0;
		state->tier_budget[1] = 0;
		state->tier_budget[2] = 0;
		return;
	}

	/*
	 * Keep tier protection conservative: only a small fraction of file pages
	 * can be skipped per aging interval to avoid long-term over-protection.
	 */
	target_t1 = file_total >> 6;	/* ~1.5% for tier-1 */
	target_t2 = file_total >> 5;	/* ~3% for tier-2 */
	if (!target_t1)
		target_t1 = 1;
	if (!target_t2)
		target_t2 = 1;

	boost_level = mglru_get_boost_level(state);

	/* Refault spikes get a temporary, bounded increase. */
	if (boost_level >= 1) {
		target_t1 += file_total >> 8;
		target_t2 += file_total >> 7;
	}
	if (boost_level >= 2) {
		target_t1 += file_total >> 9;
		target_t2 += file_total >> 8;
	}
	if (boost_level >= 3) {
		target_t1 += file_total >> 10;
		target_t2 += file_total >> 9;
	}

	/* Hard cap to avoid single-interval budget spikes. */
	cap_t1 = file_total / 8;	/* <=12.5% */
	cap_t2 = file_total / 6;	/* <=16.6% */
	if (!cap_t1)
		cap_t1 = 1;
	if (!cap_t2)
		cap_t2 = 1;
	if (target_t1 > cap_t1)
		target_t1 = cap_t1;
	if (target_t2 > cap_t2)
		target_t2 = cap_t2;

	/*
	 * Smooth with previous budget:
	 * new = 75% old + 25% target, reducing reclaim behavior jitter.
	 */
	old_t1 = state->tier_budget[1];
	old_t2 = state->tier_budget[2];
	t1 = (old_t1 * 3 + target_t1) >> 2;
	t2 = (old_t2 * 3 + target_t2) >> 2;
	if (!t1)
		t1 = 1;
	if (!t2)
		t2 = 1;

	/*
	 * Under refault pressure, keep a minimum floor so protection does not
	 * collapse too quickly between adjacent aging intervals.
	 */
	if (boost_level >= 1) {
		floor_t1 = file_total >> 8;	/* ~0.4% */
		floor_t2 = file_total >> 7;	/* ~0.8% */
		if (!floor_t1)
			floor_t1 = 1;
		if (!floor_t2)
			floor_t2 = 1;
		if (t1 < floor_t1)
			t1 = floor_t1;
		if (t2 < floor_t2)
			t2 = floor_t2;
	}
	if (boost_level >= 2) {
		floor_t1 = file_total >> 7;
		floor_t2 = file_total >> 6;
		if (!floor_t1)
			floor_t1 = 1;
		if (!floor_t2)
			floor_t2 = 1;
		if (t1 < floor_t1)
			t1 = floor_t1;
		if (t2 < floor_t2)
			t2 = floor_t2;
	}

	/*
	 * PTWALK-driven soft limiter:
	 * when sampled hot ratio is low, shrink tier budgets so file cache
	 * protection does not push anon too early into zram.
	 */
	pt_total = (unsigned long)atomic64_read(&mglru_ptwalk_total_cnt);
	pt_hot = (unsigned long)atomic64_read(&mglru_ptwalk_hot_cnt);
	if (pt_total >= 1024)
		pt_ratio_x100 = (pt_hot * 100) / pt_total;

	if (pt_total >= 1024) {
		if (pt_ratio_x100 < 25) {
			t1 = max(t1 >> 2, 1UL);
			t2 = max(t2 >> 2, 1UL);
		} else if (pt_ratio_x100 < 35) {
			t1 = max(t1 >> 1, 1UL);
			t2 = max(t2 >> 1, 1UL);
		} else if (pt_ratio_x100 < 45) {
			t1 = max((t1 * 3) >> 2, 1UL);
			t2 = max((t2 * 3) >> 2, 1UL);
		} else if (pt_ratio_x100 > 70 && boost_level >= 1) {
			t1 += t1 >> 2;
			t2 += t2 >> 2;
		}
	}

	if (t1 > cap_t1)
		t1 = cap_t1;
	if (t2 > cap_t2)
		t2 = cap_t2;

	state->tier_budget[0] = 0;
	state->tier_budget[1] = t1;
	state->tier_budget[2] = t2;
}

static int mglru_pick_gen(struct mglru_lruvec_state *state, struct page *page,
			  enum lru_list lru, bool reclaiming, int tier)
{
	int type = is_file_lru(lru);
	unsigned long seq;

	if (PageActive(page)) {
		seq = state->max_seq;
	} else if (reclaiming) {
		seq = state->min_seq[type];
	} else if (type && tier >= 2) {
		/* hot file pages: protect with the youngest generation */
		seq = state->max_seq;
	} else if (type && tier == 1) {
		/* warm file pages: keep newer than cold ones */
		if (state->max_seq > state->min_seq[type] + 1)
			seq = state->max_seq - 1;
		else
			seq = state->max_seq;
	} else if ((type == 0 && !PageSwapCache(page)) ||
		   (PageReclaim(page) && (PageDirty(page) || PageWriteback(page)))) {
		/*
		 * Non-swapcache anon and pages pending writeback are less
		 * reclaimable right away, keep them slightly newer.
		 */
		seq = state->min_seq[type] + 1;
	} else {
		seq = state->max_seq;
	}

	return mglru_gen_from_seq(seq);
}

static void mglru_access_control_type(struct mglru_lruvec_state *state, int type)
{
	unsigned long nr_scanned = 0;
	int old_gen, young_gen;
	struct page *page;
	struct list_head *pos, *next;
	struct list_head *old_list, *young_list;

	if (!IS_ENABLED(CONFIG_LRU_GEN_LITE_ACCESS_CTRL))
		return;

	old_gen = mglru_gen_from_seq(state->min_seq[type]);
	young_gen = mglru_gen_from_seq(state->max_seq);
	old_list = &state->lists[old_gen][type];
	young_list = &state->lists[young_gen][type];

	list_for_each_safe(pos, next, old_list) {
		page = lru_to_page(pos);
		if (nr_scanned++ >= MGLRU_ACCESS_CTRL_BATCH)
			break;
		if (PageUnevictable(page))
			continue;
		if (!TestClearPageReferenced(page))
			continue;

		list_move(&page->lru, young_list);
		atomic64_add(thp_nr_pages(page), &mglru_access_promote_cnt);
	}
}

#ifdef CONFIG_LRU_GEN_LITE_PTWALK
static void mglru_ptwalk_collect_type(struct mglru_lruvec_state *state, int type,
				      struct page **pages, unsigned int *nr)
{
	int old_gen;
	struct page *page;

	if (*nr >= MGLRU_PTWALK_BATCH)
		return;

	old_gen = mglru_gen_from_seq(state->min_seq[type]);
	list_for_each_entry(page, &state->lists[old_gen][type], lru) {
		if (*nr >= MGLRU_PTWALK_BATCH)
			break;
		if (PageUnevictable(page))
			continue;
		if (!get_page_unless_zero(page))
			continue;
		pages[(*nr)++] = page;
	}
}

static unsigned int mglru_ptwalk_prepare(struct mglru_lruvec_state *state,
					 struct page **pages)
{
	unsigned int nr = 0;

	mglru_ptwalk_collect_type(state, 0, pages, &nr);
	mglru_ptwalk_collect_type(state, 1, pages, &nr);

	return nr;
}

static void mglru_ptwalk_scan_pages(struct page **pages, unsigned int nr)
{
	unsigned int i;
	unsigned long vm_flags;
	int refs;

	for (i = 0; i < nr; i++) {
		atomic64_inc(&mglru_ptwalk_total_cnt);
		refs = page_referenced(pages[i], 1, NULL, &vm_flags);
		if (refs > 0)
			atomic64_inc(&mglru_ptwalk_hot_cnt);
		put_page(pages[i]);
	}
}
#else
static unsigned int mglru_ptwalk_prepare(struct mglru_lruvec_state *state,
					 struct page **pages)
{
	(void)state;
	(void)pages;
	return 0;
}

static void mglru_ptwalk_scan_pages(struct page **pages, unsigned int nr)
{
	(void)pages;
	(void)nr;
}
#endif

static struct mglru_lruvec_state *mglru_find_locked(struct lruvec *lruvec)
{
	struct mglru_lruvec_state *state;

	hash_for_each_possible(mglru_table, state, node, (unsigned long)lruvec) {
		if (state->lruvec == lruvec)
			return state;
	}

	return NULL;
}

static bool mglru_state_empty(struct mglru_lruvec_state *state)
{
	int gen, type;

	for (gen = 0; gen < MGLRU_MAX_NR_GENS; gen++)
		for (type = 0; type < ANON_AND_FILE; type++)
			if (!list_empty(&state->lists[gen][type]))
				return false;

	return true;
}

static bool mglru_type_has_list_pages(struct mglru_lruvec_state *state, int type)
{
	int gen;

	for (gen = 0; gen < MGLRU_MAX_NR_GENS; gen++) {
		if (!list_empty(&state->lists[gen][type]))
			return true;
	}

	return false;
}

static unsigned long mglru_recount_type_total(struct mglru_lruvec_state *state, int type)
{
	int gen;
	unsigned long total = 0;
	struct page *page;

	for (gen = 0; gen < MGLRU_MAX_NR_GENS; gen++) {
		list_for_each_entry(page, &state->lists[gen][type], lru)
			total += thp_nr_pages(page);
	}

	return total;
}

static void mglru_sub_type_total(struct mglru_lruvec_state *state, int type,
				 unsigned long pages)
{
	unsigned long old_total;
	unsigned long new_total;

	if (likely(state->nr_total[type] >= pages)) {
		state->nr_total[type] -= pages;
		return;
	}

	old_total = state->nr_total[type];
	new_total = mglru_recount_type_total(state, type);
	state->nr_total[type] = new_total;
	state->sanitize_retry_at[type] = jiffies + HZ;

	if (mglru_debug_log_enabled())
		pr_info_ratelimited("mglru-lite: total underflow resync lruvec=%px type=%d old=%lu new=%lu sub=%lu\n",
				    state->lruvec, type, old_total, new_total, pages);
}

static void mglru_sanitize_type_state(struct mglru_lruvec_state *state, int type)
{
	bool has_pages;
	unsigned long old_total;
	unsigned long new_total;

	has_pages = mglru_type_has_list_pages(state, type);
	if ((has_pages && state->nr_total[type]) || (!has_pages && !state->nr_total[type]))
		return;

	if (time_before(jiffies, READ_ONCE(state->sanitize_retry_at[type])))
		return;

	old_total = state->nr_total[type];
	new_total = mglru_recount_type_total(state, type);
	state->nr_total[type] = new_total;
	state->sanitize_retry_at[type] = jiffies + HZ;

	if (mglru_debug_log_enabled() && old_total != new_total)
		pr_info_ratelimited("mglru-lite: sanitize resync lruvec=%px type=%d old=%lu new=%lu\n",
				    state->lruvec, type, old_total, new_total);
}

static struct mglru_lruvec_state *mglru_get_state(struct lruvec *lruvec, bool create)
{
	int gen, type;
	struct mglru_lruvec_state *state, *new_state;

	mglru_assert_lru_lock(lruvec);

	spin_lock(&mglru_table_lock);
	state = mglru_find_locked(lruvec);
	spin_unlock(&mglru_table_lock);
	if (state || !create)
		return state;

	new_state = kzalloc(sizeof(*new_state), GFP_ATOMIC);
	if (!new_state)
		return NULL;

	new_state->lruvec = lruvec;
	new_state->max_seq = 0;
	new_state->min_seq[0] = 0;
	new_state->min_seq[1] = 0;
	new_state->next_age = jiffies + HZ;
	new_state->last_file_refault = 0;
	new_state->refault_boost_until = 0;
	new_state->refault_boost_level = 0;
	new_state->look_around_total = 0;
	new_state->look_around_hot = 0;
	new_state->ptwalk_total_seen = 0;
	new_state->ptwalk_hot_seen = 0;
	new_state->tier_budget[0] = 0;
	new_state->tier_budget[1] = 0;
	new_state->tier_budget[2] = 0;
	new_state->sanitize_retry_at[0] = 0;
	new_state->sanitize_retry_at[1] = 0;
	for (gen = 0; gen < MGLRU_MAX_NR_GENS; gen++)
		for (type = 0; type < ANON_AND_FILE; type++)
			INIT_LIST_HEAD(&new_state->lists[gen][type]);

	spin_lock(&mglru_table_lock);
	state = mglru_find_locked(lruvec);
	if (!state) {
		hash_add(mglru_table, &new_state->node, (unsigned long)lruvec);
		state = new_state;
		new_state = NULL;
		if (mglru_debug_log_enabled())
			pr_info_ratelimited("mglru-lite: state created for lruvec=%px\n", lruvec);
	}
	spin_unlock(&mglru_table_lock);

	kfree(new_state);
	return state;
}

static void mglru_try_drop_state(struct lruvec *lruvec, struct mglru_lruvec_state *state)
{
	bool drop = false;

	if (!state || !mglru_state_empty(state))
		return;

	mglru_assert_lru_lock(lruvec);

	spin_lock(&mglru_table_lock);
	if (mglru_find_locked(lruvec) == state && mglru_state_empty(state)) {
		hash_del(&state->node);
		drop = true;
	}
	spin_unlock(&mglru_table_lock);

	if (drop) {
		if (mglru_debug_log_enabled())
			pr_info_ratelimited("mglru-lite: state dropped for lruvec=%px\n", lruvec);
		kfree(state);
	}
}

bool mglru_tier_should_protect(struct lruvec *lruvec, struct page *page, enum lru_list lru,
			       int priority, bool may_swap)
{
	unsigned long pages;
	int tier, swappiness;
	int min_prio_t1;
	int min_prio_t2;
	unsigned int boost_level;
	struct mglru_lruvec_state *state;
	bool protect = false;

	if (!lru_gen_enabled() || !is_file_lru(lru))
		return false;

	mglru_assert_lru_lock(lruvec);
	state = mglru_get_state(lruvec, false);
	if (!state)
		return false;

	tier = mglru_page_tier(page, lru);
	if (!tier)
		return false;

	swappiness = may_swap ? READ_ONCE(vm_swappiness) : 0;
	boost_level = mglru_get_boost_level(state);
	min_prio_t2 = 4;
	min_prio_t1 = 7;

	/*
	 * On swap-capable reclaim, protect file tiers less aggressively so
	 * anon does not get swapped too early.
	 */
	if (may_swap && swappiness >= 80) {
		min_prio_t2 = 3;
		min_prio_t1 = 6;
	}
	if (boost_level >= 1 && min_prio_t2 > 2)
		min_prio_t2--;
	if (boost_level >= 2 && min_prio_t1 > 5)
		min_prio_t1--;

	/* Tier-2 is stickier than tier-1; both relax under heavy pressure. */
	if (tier >= 2)
		protect = swappiness <= 140 && priority >= min_prio_t2;
	else if (tier == 1)
		protect = swappiness <= 100 && priority >= min_prio_t1;

	/* Refault spike temporarily boosts only top tier, and still bounded. */
	if (boost_level >= 2 && tier >= 2 &&
	    swappiness <= 100 && priority >= min_prio_t2)
		protect = true;

	if (!protect)
		return false;

	pages = thp_nr_pages(page);
	if (tier >= 1 && tier < MGLRU_MAX_TIERS) {
		if (state->tier_budget[tier] < pages)
			return false;
		state->tier_budget[tier] -= pages;
	}

	if (protect)
		atomic64_inc(&mglru_tier_protect_skip_cnt);

	return protect;
}

struct list_head *lru_gen_get_scan_list(struct lruvec *lruvec, enum lru_list lru)
{
	int gen, type;
	unsigned long span;
	struct mglru_lruvec_state *state;

	if (!lru_gen_enabled() || !lru_gen_managed(lru))
		return &lruvec->lists[lru];

	mglru_assert_lru_lock(lruvec);
	state = mglru_get_state(lruvec, false);
	if (!state)
		return &lruvec->lists[lru];

	type = is_file_lru(lru);
	mglru_sanitize_type_state(state, type);
	span = state->max_seq - state->min_seq[type];

	/*
	 * Be conservative on phones: only use generation queues when we have
	 * enough depth and enough queued pages. Otherwise fallback to classic
	 * inactive lists to avoid overly aggressive reclaim/swap.
	 */
	if (span < 1 || state->nr_total[type] < MGLRU_MIN_BATCH) {
		atomic64_inc(&mglru_scan_fallback_cnt);
		mglru_try_drop_state(lruvec, state);
		return &lruvec->lists[lru];
	}

	gen = mglru_gen_from_seq(state->min_seq[type]);
	if (!list_empty(&state->lists[gen][type])) {
		atomic64_inc(&mglru_scan_pick_cnt);
		return &state->lists[gen][type];
	}

	/*
	 * Advance at most one generation per call to avoid overly aggressive
	 * min_seq jumps under bursty reclaim.
	 */
	if (mglru_try_inc_min_seq(state, type)) {
		gen = mglru_gen_from_seq(state->min_seq[type]);
		if (!list_empty(&state->lists[gen][type])) {
			atomic64_inc(&mglru_scan_pick_cnt);
			return &state->lists[gen][type];
		}
	}

	mglru_try_drop_state(lruvec, state);
	return &lruvec->lists[lru];
}

void lru_gen_age_lruvec(struct lruvec *lruvec)
{
	unsigned long flags;
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	struct page *ptwalk_pages[MGLRU_PTWALK_BATCH];
	unsigned int nr_ptwalk = 0;
	struct mglru_lruvec_state *state;

	if (!lru_gen_enabled())
		return;

	spin_lock_irqsave(&pgdat->lru_lock, flags);
	state = mglru_get_state(lruvec, false);
	if (!state || time_before(jiffies, READ_ONCE(state->next_age)))
		goto unlock;

	if (!time_before(jiffies, state->next_age)) {
		unsigned long refault;
		unsigned long delta;
		unsigned long pt_total_now;
		unsigned long pt_hot_now;
		unsigned long pt_delta_total;
		unsigned long pt_delta_hot;
		unsigned int boost_level = 0;
		unsigned long boost_dur;

		state->max_seq++;
		if (state->max_seq - state->min_seq[0] >= MGLRU_MAX_NR_GENS)
			state->min_seq[0] = state->max_seq - (MGLRU_MAX_NR_GENS - 1);
		if (state->max_seq - state->min_seq[1] >= MGLRU_MAX_NR_GENS)
			state->min_seq[1] = state->max_seq - (MGLRU_MAX_NR_GENS - 1);
		state->next_age = jiffies + HZ;
		atomic64_inc(&mglru_age_cnt);
		refault = lruvec_page_state(lruvec, WORKINGSET_ACTIVATE);
		delta = refault - state->last_file_refault;
		state->last_file_refault = refault;
		/*
		 * Keep high boost for bursty refault spikes only.
		 * Raise thresholds to avoid frequent steady-state boost=3.
		 */
		if (delta >= MGLRU_MIN_BATCH * 12)
			boost_level = 3;
		else if (delta >= MGLRU_MIN_BATCH * 6)
			boost_level = 2;
		else if (delta >= MGLRU_MIN_BATCH)
			boost_level = 1;

		if (boost_level) {
			pt_total_now = (unsigned long)atomic64_read(&mglru_ptwalk_total_cnt);
			pt_hot_now = (unsigned long)atomic64_read(&mglru_ptwalk_hot_cnt);
			pt_delta_total = pt_total_now - state->ptwalk_total_seen;
			pt_delta_hot = pt_hot_now - state->ptwalk_hot_seen;
			state->ptwalk_total_seen = pt_total_now;
			state->ptwalk_hot_seen = pt_hot_now;

			/*
			 * PTWALK hot ratio is a suppressor only:
			 * if page-table evidence indicates low heat,
			 * cap boost level to avoid over-protecting file cache.
			 */
			if (pt_delta_total >= MGLRU_PTWALK_BATCH) {
				if (pt_delta_hot * 100 < pt_delta_total * 25 &&
				    boost_level > 1) {
					boost_level = 1;
					atomic64_inc(&mglru_ptwalk_clamp_cnt);
				} else if (pt_delta_hot * 100 < pt_delta_total * 35 &&
					   boost_level > 2) {
					boost_level = 2;
					atomic64_inc(&mglru_ptwalk_clamp_cnt);
				}
			}

			boost_dur = HZ;
			if (boost_level >= 3)
				boost_dur = 2 * HZ;
			state->refault_boost_level = boost_level;
			state->refault_boost_until = jiffies + boost_dur;
		} else if (!mglru_refault_boosted(state)) {
			state->refault_boost_level = 0;
		}
		/* Keep look-around stats recent and bounded. */
		state->look_around_total >>= 1;
		state->look_around_hot >>= 1;
		mglru_access_control_type(state, 0);
		mglru_access_control_type(state, 1);
		nr_ptwalk = mglru_ptwalk_prepare(state, ptwalk_pages);
		mglru_refresh_tier_budget(state);

		if (mglru_debug_log_enabled() &&
		    (!mglru_log_next_jiffies || time_after_eq(jiffies, mglru_log_next_jiffies))) {
			mglru_log_next_jiffies = jiffies + 5 * HZ;
			pr_info("mglru-lite: age=%lld add=%lld del=%lld scan_pick=%lld lruvec=%px max_seq=%lu min=[%lu,%lu]\n",
				(long long)atomic64_read(&mglru_age_cnt),
				(long long)atomic64_read(&mglru_add_cnt),
				(long long)atomic64_read(&mglru_del_cnt),
				(long long)atomic64_read(&mglru_scan_pick_cnt),
				lruvec, state->max_seq, state->min_seq[0], state->min_seq[1]);
			pr_info("mglru-lite: scan_fallback=%lld total=[%lu,%lu]\n",
				(long long)atomic64_read(&mglru_scan_fallback_cnt),
				state->nr_total[0], state->nr_total[1]);
			pr_info("mglru-lite: tier_add=[%lld,%lld,%lld]\n",
				(long long)atomic64_read(&mglru_tier_add_cnt[0]),
				(long long)atomic64_read(&mglru_tier_add_cnt[1]),
				(long long)atomic64_read(&mglru_tier_add_cnt[2]));
			pr_info("mglru-lite: tier_protect_skip=%lld refault_boost=%d\n",
				(long long)atomic64_read(&mglru_tier_protect_skip_cnt),
				mglru_refault_boosted(state));
			pr_info("mglru-lite: boost_level=%u look_around=[%lu/%lu] global=[%lld/%lld]\n",
				state->refault_boost_level,
				state->look_around_hot, state->look_around_total,
				(long long)atomic64_read(&mglru_look_around_hot_cnt),
				(long long)atomic64_read(&mglru_look_around_total_cnt));
			pr_info("mglru-lite: access_promote=%lld\n",
				(long long)atomic64_read(&mglru_access_promote_cnt));
			pr_info("mglru-lite: ptwalk=[%lld/%lld]\n",
				(long long)atomic64_read(&mglru_ptwalk_hot_cnt),
				(long long)atomic64_read(&mglru_ptwalk_total_cnt));
			pr_info("mglru-lite: ptwalk_clamp=%lld\n",
				(long long)atomic64_read(&mglru_ptwalk_clamp_cnt));
			pr_info("mglru-lite: tier_budget=[%lu,%lu,%lu]\n",
				state->tier_budget[0], state->tier_budget[1],
				state->tier_budget[2]);
		}
	}
	mglru_try_drop_state(lruvec, state);

unlock:
	spin_unlock_irqrestore(&pgdat->lru_lock, flags);
	if (nr_ptwalk)
		mglru_ptwalk_scan_pages(ptwalk_pages, nr_ptwalk);
}

void lru_gen_isolate_page(struct lruvec *lruvec, struct page *page, enum lru_list lru)
{
	unsigned long pages;
	int type;
	struct mglru_lruvec_state *state;

	if (!lru_gen_enabled() || !lru_gen_managed(lru))
		return;

	mglru_assert_lru_lock(lruvec);
	state = mglru_get_state(lruvec, false);
	if (!state)
		return;

	type = is_file_lru(lru);
	pages = thp_nr_pages(page);
	mglru_sub_type_total(state, type, pages);
}

void mglru_look_around(struct lruvec *lruvec, struct page *page, enum lru_list lru)
{
	struct mglru_lruvec_state *state;
	bool hot;

	if (!lru_gen_enabled() || !is_file_lru(lru))
		return;

	mglru_assert_lru_lock(lruvec);
	state = mglru_get_state(lruvec, false);
	if (!state)
		return;

	hot = PageReferenced(page) || PageWorkingset(page) || PageActive(page);
	state->look_around_total++;
	atomic64_inc(&mglru_look_around_total_cnt);

	if (hot) {
		state->look_around_hot++;
		atomic64_inc(&mglru_look_around_hot_cnt);
		/*
		 * Lightweight thrash hint: a burst of hot pages in scan stream
		 * keeps a mild boost window to avoid immediate refault churn.
		 */
		if (state->look_around_total >= MGLRU_MIN_BATCH * 2 &&
		    state->look_around_hot * 2 >
		    state->look_around_total + MGLRU_MIN_BATCH / 2) {
			if (state->refault_boost_level == 0)
				state->refault_boost_level = 1;
			if (!mglru_refault_boosted(state))
				state->refault_boost_until = jiffies + HZ;
		}
	}
}
