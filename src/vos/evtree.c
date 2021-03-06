/**
 * (C) Copyright 2017-2018 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
#define D_LOGFAC	DD_FAC(vos)

#include "evt_priv.h"
#include "vos_internal.h"

enum {
	/** no overlap */
	RT_OVERLAP_NO		= 0,
	/* set if rt1 range or epoch matches rt2 */
	RT_OVERLAP_SAME		= (1 << 1),
	/* set if rt1 is before rt2 */
	RT_OVERLAP_OVER		= (1 << 2),
	/* set if rt1 is after rt2 */
	RT_OVERLAP_UNDER	= (1 << 3),
	/* set if rt1 range includes rt2 */
	RT_OVERLAP_INCLUDED	= (1 << 4),
	/* set if rt2 range includes rt1 */
	RT_OVERLAP_INCLUDES	= (1 << 5),
	/* set if rt2 range overlaps rt1 */
	RT_OVERLAP_PARTIAL	= (1 << 6),
};

static struct evt_policy_ops evt_ssof_pol_ops;
/**
 * Tree policy table.
 * - Sorted by Start Offset(SSOF): it is the only policy for now.
 */
static struct evt_policy_ops *evt_policies[] = {
	&evt_ssof_pol_ops,
	NULL,
};

static struct evt_rect *evt_node_mbr_get(struct evt_context *tcx,
					 uint64_t nd_off);

/** Helper function for starting a PMDK transaction, if applicable */
static inline int
evt_tx_begin(struct evt_context *tcx)
{
	if (!evt_has_tx(tcx))
		return 0;

	return umem_tx_begin(evt_umm(tcx), NULL);
}

/** Helper function for ending a PMDK transaction, if applicable */
static inline int
evt_tx_end(struct evt_context *tcx, int rc)
{
	if (!evt_has_tx(tcx))
		return rc;

	if (rc != 0)
		return umem_tx_abort(evt_umm(tcx), rc);

	return umem_tx_commit(evt_umm(tcx));
}

/**
 * Returns true if the first rectangle \a rt1 is at least as wide as the second
 * rectangle \a rt2.
 */
static bool
evt_rect_is_wider(const struct evt_rect *rt1, const struct evt_rect *rt2)
{
	return (rt1->rc_ex.ex_lo <= rt2->rc_ex.ex_lo &&
		rt1->rc_ex.ex_hi >= rt2->rc_ex.ex_hi);
}

static bool
evt_rect_same_extent(const struct evt_rect *rt1, const struct evt_rect *rt2)
{
	return (rt1->rc_ex.ex_lo == rt2->rc_ex.ex_lo &&
		rt1->rc_ex.ex_hi == rt2->rc_ex.ex_hi);
}
/**
 * Check if two rectangles overlap with each other.
 *
 * NB: This function is not symmetric(caller cannot arbitrarily change order
 * of input rectangles), the first rectangle \a rt1 should be in-tree, the
 * second rectangle \a rt2 should be the one being searched/inserted.
 */
static void
evt_rect_overlap(const struct evt_rect *rt1, const struct evt_rect *rt2,
		 int *range, int *time)
{
	*time = *range = RT_OVERLAP_NO;

	if (rt1->rc_ex.ex_lo > rt2->rc_ex.ex_hi || /* no offset overlap */
	    rt1->rc_ex.ex_hi < rt2->rc_ex.ex_lo)
		return;

	/* NB: By definition, there is always epoch overlap since all
	 * updates are from epc to INF.  Determine here what kind
	 * of overlap exists.
	 */
	if (rt1->rc_epc == rt2->rc_epc)
		*time = RT_OVERLAP_SAME;
	else if (rt1->rc_epc < rt2->rc_epc)
		*time = RT_OVERLAP_OVER;
	else
		*time = RT_OVERLAP_UNDER;

	if (evt_rect_same_extent(rt1, rt2))
		*range = RT_OVERLAP_SAME;
	else if (evt_rect_is_wider(rt1, rt2))
		*range = RT_OVERLAP_INCLUDED;
	else if (evt_rect_is_wider(rt2, rt1))
		*range = RT_OVERLAP_INCLUDES;
	else
		*range = RT_OVERLAP_PARTIAL;
}

/**
 * Calculate the Minimum Bounding Rectangle (MBR) of two rectangles and store
 * the MBR into the first rectangle \a rt1.
 *
 * This function returns false if \a rt1 is unchanged (it fully includes rt2),
 * otherwise it returns true.
 */
static bool
evt_rect_merge(struct evt_rect *rt1, const struct evt_rect *rt2)
{
	bool	changed = false;

	if (rt1->rc_ex.ex_lo > rt2->rc_ex.ex_lo) {
		rt1->rc_ex.ex_lo = rt2->rc_ex.ex_lo;
		changed = true;
	}

	if (rt1->rc_ex.ex_hi < rt2->rc_ex.ex_hi) {
		rt1->rc_ex.ex_hi = rt2->rc_ex.ex_hi;
		changed = true;
	}

	if (rt1->rc_epc > rt2->rc_epc) {
		rt1->rc_epc = rt2->rc_epc;
		changed = true;
	}

	return changed;
}

/**
 * Compare two weights.
 *
 * \return
 *	-1: \a wt1 is smaller than \a wt2
 *	+1: \a wt1 is larger than \a wt2
 *	 0: equal
 */
static int
evt_weight_cmp(struct evt_weight *wt1, struct evt_weight *wt2)
{
	if (wt1->wt_major < wt2->wt_major)
		return -1;

	if (wt1->wt_major > wt2->wt_major)
		return 1;

	if (wt1->wt_minor < wt2->wt_minor)
		return -1;

	if (wt1->wt_minor > wt2->wt_minor)
		return 1;

	return 0;
}

/**
 * Calculate difference between two weights and return it to \a wt_diff.
 */
static void
evt_weight_diff(struct evt_weight *wt1, struct evt_weight *wt2,
		struct evt_weight *wt_diff)
{
	/* NB: they can be negative */
	wt_diff->wt_major = wt1->wt_major - wt2->wt_major;
	wt_diff->wt_minor = wt1->wt_minor - wt2->wt_minor;
}

/** Initialize an entry list */
void
evt_ent_array_init(struct evt_entry_array *ent_array)
{
	memset(ent_array, 0, sizeof(*ent_array));
	ent_array->ea_ents = ent_array->ea_embedded_ents;
	ent_array->ea_size = EVT_EMBEDDED_NR;
}

/** Finalize an entry list */
void
evt_ent_array_fini(struct evt_entry_array *ent_array)
{
	if (ent_array->ea_size > EVT_EMBEDDED_NR)
		D_FREE(ent_array->ea_ents);

	ent_array->ea_size = ent_array->ea_ent_nr = 0;
}

/** When we go over the embedded limit, set a minimum allocation */
#define EVT_MIN_ALLOC 4096

static void
ent_array_reset(struct evt_context *tcx, struct evt_entry_array *ent_array)
{
	size_t size;

	size = sizeof(ent_array->ea_ents[0]) * ent_array->ea_ent_nr;
	memset(ent_array->ea_ents, 0, size);
	ent_array->ea_ent_nr = 0;
	ent_array->ea_inob = tcx->tc_inob;
}

static bool
ent_array_resize(struct evt_context *tcx, struct evt_entry_array *ent_array,
		 uint32_t new_size)
{
	struct evt_list_entry	*ents;

	D_ALLOC_ARRAY(ents, new_size);
	if (ents == NULL)
		return -DER_NOMEM;

	memcpy(ents, ent_array->ea_ents,
	       sizeof(ents[0]) * ent_array->ea_ent_nr);
	if (ent_array->ea_ents != ent_array->ea_embedded_ents)
		D_FREE(ent_array->ea_ents);
	ent_array->ea_ents = ents;
	ent_array->ea_size = new_size;
	return 0;
}


/**
 * Take an embedded entry, or allocate a new entry if all embedded entries
 * have been taken. If notify_realloc is set, it will return -DER_AGAIN
 * if reallocation was done so callers can revalidate cached references
 * to entries, if necessary.
 */
static int
ent_array_alloc(struct evt_context *tcx, struct evt_entry_array *ent_array,
		struct evt_entry **entry, bool notify_realloc)
{
	uint32_t		 size;
	int			 i;
	int			 rc;

	if (ent_array->ea_max == 0) {
		/* Calculate the upper size bound for the array.  Based on the
		 * size of the evtree, this is an upper limit on the number of
		 * entries needed to fit all covered and visible extents
		 * including split extents.
		 */
		size = 1;
		for (i = 0; i < tcx->tc_depth; i++)
			size *= tcx->tc_order;
		/* With splitting, we need 3x the space in worst case.  Each new
		 * extent inserted can add at most 1 extent to the output. The
		 * cases are:
		 * 1. New extent covers existing one:   1 visible, 1 covered
		 * 2. New extent splits existing one:   3 visible, 1 covered
		 * 3. New extent overlaps existing one: 2 visible, 1 covered
		 * 4. New extent covers nothing:        1 visible, 0 covered
		 *
		 * So, each new extent can add at most 2 new rectangle (as in
		 * case #2.   So if we allocate 3x the max entries in the tree,
		 * we will always have sufficient space to store entries.
		 */
		size *= 3;
		ent_array->ea_max = size;
		ent_array->ea_inob = tcx->tc_inob;
	}
	if (ent_array->ea_ent_nr == ent_array->ea_size) {
		size = ent_array->ea_size * 4;
		if (size < EVT_MIN_ALLOC)
			size = EVT_MIN_ALLOC;
		if (size > ent_array->ea_max)
			size = ent_array->ea_max;

		rc = ent_array_resize(tcx, ent_array, size);
		if (rc != 0)
			return rc;

		if (notify_realloc)
			return -DER_AGAIN; /* Invalidate any cached state */
	}
	D_ASSERT(ent_array->ea_ent_nr < ent_array->ea_size);

	*entry = evt_ent_array_get(ent_array, ent_array->ea_ent_nr++);
	return 0;
}

int
evt_rect_cmp(const struct evt_rect *rt1, const struct evt_rect *rt2)
{
	if (rt1->rc_ex.ex_lo < rt2->rc_ex.ex_lo)
		return -1;

	if (rt1->rc_ex.ex_lo > rt2->rc_ex.ex_lo)
		return 1;

	if (rt1->rc_epc > rt2->rc_epc)
		return -1;

	if (rt1->rc_epc < rt2->rc_epc)
		return 1;

	if (rt1->rc_ex.ex_hi < rt2->rc_ex.ex_hi)
		return -1;

	if (rt1->rc_ex.ex_hi > rt2->rc_ex.ex_hi)
		return 1;

	return 0;
}

#define PRINT_ENT(ent)							\
	D_PRINT("%s:%d " #ent ": " DF_ENT " visibility = %"PRIx64"\n",	\
		__func__, __LINE__, DP_ENT(ent),			\
		(ent)->en_visibility)

#define evt_flags_equal(flags, set) \
	(((flags) & (EVT_COVERED | EVT_VISIBLE)) == (set))

#define evt_flags_get(flags) \
	((flags) & (EVT_COVERED | EVT_VISIBLE))

#define evt_flags_valid(flags)			\
	(evt_flags_get(flags) == EVT_VISIBLE ||	\
	 evt_flags_get(flags) == EVT_COVERED)

static int
evt_ent_cmp(const struct evt_entry *ent1, const struct evt_entry *ent2,
	    int flags)
{
	struct evt_rect		 rt1;
	struct evt_rect		 rt2;

	if (!flags)
		goto cmp_ext;

	/* Ensure we've selected one or the other */
	D_ASSERT(flags == EVT_VISIBLE || flags == EVT_COVERED);
	D_ASSERT(evt_flags_valid(ent1->en_visibility));
	D_ASSERT(evt_flags_valid(ent2->en_visibility));

	if (evt_flags_get(ent1->en_visibility) ==
	    evt_flags_get(ent2->en_visibility))
		goto cmp_ext;

	if (evt_flags_equal(ent1->en_visibility, EVT_VISIBLE))
		return (flags & EVT_VISIBLE) ? -1 : 1;

	return (flags & EVT_VISIBLE) ? 1 : -1;
cmp_ext:
	evt_ent2rect(&rt1, ent1);
	evt_ent2rect(&rt2, ent2);

	return evt_rect_cmp(&rt1, &rt2);
}

int evt_ent_list_cmp(const void *p1, const void *p2)
{
	const struct evt_list_entry	*le1	= p1;
	const struct evt_list_entry	*le2	= p2;

	return evt_ent_cmp(&le1->le_ent, &le2->le_ent, 0);
}

int evt_ent_list_cmp_visible(const void *p1, const void *p2)
{
	const struct evt_list_entry	*le1	= p1;
	const struct evt_list_entry	*le2	= p2;

	return evt_ent_cmp(&le1->le_ent, &le2->le_ent, EVT_VISIBLE);
}

int evt_ent_list_cmp_covered(const void *p1, const void *p2)
{
	const struct evt_list_entry	*le1	= p1;
	const struct evt_list_entry	*le2	= p2;

	return evt_ent_cmp(&le1->le_ent, &le2->le_ent, EVT_COVERED);
}

static inline struct evt_entry *
evt_array_get_list_entry(d_list_t *link)
{
	struct evt_list_entry *le = d_list_entry(link, struct evt_list_entry,
						 le_link);

	return &le->le_ent;
}

static inline d_list_t *
evt_array_get_entry_link(struct evt_entry *ent)
{
	struct evt_list_entry *le;

	le = container_of(ent, struct evt_list_entry, le_ent);

	return &le->le_link;
}

static struct evt_entry *
evt_find_next_visible(struct evt_entry *this_ent, d_list_t *head,
		      d_list_t **next)
{
	d_list_t		*temp;
	struct evt_extent	*this_ext;
	struct evt_extent	*next_ext;
	struct evt_entry	*next_ent;

	while (*next != head) {
		next_ent = evt_array_get_list_entry(*next);

		if (next_ent->en_epoch > this_ent->en_epoch)
			return next_ent; /* next_ent is a later update */
		this_ext = &this_ent->en_sel_ext;
		next_ext = &next_ent->en_sel_ext;
		if (next_ext->ex_hi > this_ext->ex_hi)
			return next_ent; /* next_ent extends past end */

		/* next_ent is covered */
		next_ent->en_visibility |= EVT_COVERED;
		temp = *next;
		*next = temp->next;
	}

	return NULL;
}

static void
evt_ent_addr_update(struct evt_context *tcx, struct evt_entry *ent,
		    daos_size_t diff)
{
	if (bio_addr_is_hole(&ent->en_addr))
		return; /* Nothing to do for holes */

	D_ASSERT(tcx->tc_inob != 0);
	ent->en_addr.ba_off += diff * tcx->tc_inob;
}

static void
evt_split_entry(struct evt_context *tcx, struct evt_entry *current,
		struct evt_entry *next, struct evt_entry *split,
		struct evt_entry *covered)
{
	daos_off_t	 diff;

	*covered = *split = *current;
	diff = next->en_sel_ext.ex_hi + 1 - split->en_sel_ext.ex_lo;
	split->en_sel_ext.ex_lo = next->en_sel_ext.ex_hi + 1;
	/* mark the entries as partial */
	split->en_visibility = EVT_PARTIAL;
	current->en_visibility |= EVT_PARTIAL;
	covered->en_visibility = EVT_PARTIAL | EVT_COVERED;
	current->en_sel_ext.ex_hi = next->en_sel_ext.ex_lo - 1;
	covered->en_sel_ext = next->en_sel_ext;
	evt_ent_addr_update(tcx, split, diff);
	evt_ent_addr_update(tcx, covered,
			    evt_extent_width(&current->en_sel_ext));
}

static d_list_t *
evt_insert_sorted(struct evt_entry *this_ent, d_list_t *head, d_list_t *current)
{
	d_list_t		*start = current;
	struct evt_entry	*next_ent;
	d_list_t		*this_link;
	int			 cmp;

	this_link = evt_array_get_entry_link(this_ent);

	while (current != head) {
		next_ent = evt_array_get_list_entry(current);
		cmp = evt_ent_cmp(this_ent, next_ent, 0);
		if (cmp < 0) {
			d_list_add(this_link, current->prev);
			goto out;
		}
		current = current->next;
	}
	d_list_add_tail(this_link, head);
out:
	if (start == current)
		return this_link;
	return start;
}

static int
evt_find_visible(struct evt_context *tcx, struct evt_entry_array *ent_array,
		 int *num_visible)
{
	struct evt_extent	*this_ext;
	struct evt_extent	*next_ext;
	struct evt_entry	*this_ent;
	struct evt_entry	*next_ent;
	struct evt_entry	*temp_ent;
	struct evt_entry	*split;
	d_list_t		 covered;
	d_list_t		*current;
	d_list_t		*next;
	daos_size_t		 diff;
	bool			 insert;
	int			 rc = 0;

	/* reset the linked list.  We'll reconstruct it */
	D_INIT_LIST_HEAD(&covered);
	*num_visible = 0;

	/* Now place all entries sorted in covered list */
	evt_ent_array_for_each(this_ent, ent_array) {
		next = evt_array_get_entry_link(this_ent);
		d_list_add_tail(next, &covered);
	}

	/* Now uncover entries */
	current = covered.next;
	/* Some compilers can't tell that this_ent will be initialized */
	this_ent = evt_array_get_list_entry(current);
	insert = true;
	next = current->next;

	while (next != &covered) {
		if (insert) {
			this_ent = evt_array_get_list_entry(current);
			this_ent->en_visibility |= EVT_VISIBLE;
			(*num_visible)++;
		}

		insert = true;

		/* Find next visible rectangle */
		next_ent = evt_find_next_visible(this_ent, &covered, &next);
		if (next_ent == NULL)
			return 0;

		this_ext = &this_ent->en_sel_ext;
		next_ext = &next_ent->en_sel_ext;
		current = next;
		next = current->next;
		/* NB: Three possibilities
		 * 1. No intersection.  Current entry is inserted in entirety
		 * 2. Partial intersection, next is earlier. Next is truncated
		 * 3. Partial intersection, next is later. Current is truncated
		 * 4. Current entry contains next_entry.  Current is split
		 * in two and both are truncated.
		 */
		if (next_ext->ex_lo >= this_ext->ex_hi + 1) {
			/* Case #1, entry already inserted, nothing to do */
			continue;
		}

		/* allocate a record for truncated entry */
		rc = ent_array_alloc(tcx, ent_array, &temp_ent, true);
		if (rc != 0)
			return rc;

		if (next_ent->en_epoch < this_ent->en_epoch) {
			/* Case #2, next rect is partially under this rect,
			 * Truncate left end of next_rec, reinsert.
			 */
			*temp_ent = *next_ent;
			temp_ent->en_visibility = EVT_COVERED | EVT_PARTIAL;
			next_ent->en_visibility |= EVT_PARTIAL;
			diff = this_ext->ex_hi + 1 - next_ext->ex_lo;
			next_ext->ex_lo = this_ext->ex_hi + 1;
			temp_ent->en_sel_ext.ex_hi = next_ext->ex_lo - 1;
			evt_ent_addr_update(tcx, next_ent, diff);
			/* current now points at next_ent.  Remove it and
			 * reinsert it in the list in case truncation moved
			 * it to a new position
			 */
			d_list_del(current);
			next = evt_insert_sorted(next_ent, &covered, next);

			/* Now we need to rerun this iteration without
			 * inserting this_ent again
			 */
			insert = false;
		} else if (next_ext->ex_hi >= this_ext->ex_hi) {
			/* Case #3, truncate entry */
			*temp_ent = *this_ent;
			temp_ent->en_visibility = EVT_COVERED | EVT_PARTIAL;
			this_ent->en_visibility |= EVT_PARTIAL;
			this_ext->ex_hi = next_ext->ex_lo - 1;
			temp_ent->en_sel_ext.ex_lo = next_ext->ex_lo;
			evt_ent_addr_update(tcx, temp_ent,
					    evt_extent_width(this_ext));
		} else {
			rc = ent_array_alloc(tcx, ent_array, &split, true);
			if (rc != 0) {
				/* Decrement ea_ent_nr by 1 to "free" the space
				 * allocated for temp_ent
				 */
				ent_array->ea_ent_nr--;
				return rc;
			}
			/* Case #4, split, insert tail into sorted list */
			evt_split_entry(tcx, this_ent, next_ent, split,
					temp_ent);
			/* Current points at next_ent */
			next = evt_insert_sorted(split, &covered, next);
		}
	}

	this_ent = evt_array_get_list_entry(current);
	D_ASSERT(!evt_flags_equal(this_ent->en_visibility, EVT_COVERED));
	this_ent->en_visibility |= EVT_VISIBLE;
	(*num_visible)++;

	return 0;
}

/** Place all entries into covered list in sorted order based on selected
 * range.   Then walk through the range to find only extents that are visible
 * and place them in the main list.   Update the selection bounds for visible
 * rectangles.
 */
int
evt_ent_array_sort(struct evt_context *tcx, struct evt_entry_array *ent_array,
		   int flags)
{
	struct evt_list_entry	*ents;
	struct evt_entry	*ent;
	int			(*compar)(const void *, const void *);
	int			 total;
	int			 num_visible;
	int			 rc;

	if (ent_array->ea_ent_nr == 0)
		return 0;

	if (ent_array->ea_ent_nr == 1) {
		ent = evt_ent_array_get(ent_array, 0);
		ent->en_visibility = EVT_VISIBLE;
		num_visible = 1;
		goto re_sort;
	}

	for (;;) {
		ents = ent_array->ea_ents;

		/* Sort the array first */
		qsort(ents, ent_array->ea_ent_nr, sizeof(ents[0]),
		      evt_ent_list_cmp);

		/* Now separate entries into covered and visible */
		rc = evt_find_visible(tcx, ent_array, &num_visible);

		if (rc != 0) {
			if (rc == -DER_AGAIN)
				continue; /* List reallocated, start over */
			return rc;
		}
		break;
	}

re_sort:
	ents = ent_array->ea_ents;
	total = ent_array->ea_ent_nr;
	compar = evt_ent_list_cmp;
	/* Now re-sort the entries */
	if (evt_flags_equal(flags, EVT_VISIBLE)) {
		compar = evt_ent_list_cmp_visible;
		total = num_visible;
	} else if (evt_flags_equal(flags, EVT_COVERED)) {
		compar = evt_ent_list_cmp_covered;
		total = ent_array->ea_ent_nr - num_visible;
	}

	if (ent_array->ea_ent_nr != 1)
		qsort(ents, ent_array->ea_ent_nr, sizeof(ents[0]), compar);

	ent_array->ea_ent_nr = total;

	return 0;
}

daos_handle_t
evt_tcx2hdl(struct evt_context *tcx)
{
	daos_handle_t hdl;

	evt_tcx_addref(tcx); /* +1 for opener */
	hdl.cookie = (uint64_t)tcx;
	return hdl;
}

struct evt_context *
evt_hdl2tcx(daos_handle_t toh)
{
	struct evt_context *tcx;

	tcx = (struct evt_context *)toh.cookie;
	if (tcx->tc_magic != EVT_HDL_ALIVE) {
		D_WARN("Invalid tree handle %x\n", tcx->tc_magic);
		return NULL;
	}
	return tcx;
}

static void
evt_tcx_set_dep(struct evt_context *tcx, unsigned int depth)
{
	tcx->tc_depth = depth;
	tcx->tc_trace = &tcx->tc_traces[EVT_TRACE_MAX - depth];
}

static struct evt_trace *
evt_tcx_trace(struct evt_context *tcx, int level)
{
	D_ASSERT(tcx->tc_depth > 0);
	D_ASSERT(level >= 0 && level < tcx->tc_depth);
	D_ASSERT(&tcx->tc_trace[level] < &tcx->tc_traces[EVT_TRACE_MAX]);

	return &tcx->tc_trace[level];
}

static void
evt_tcx_set_trace(struct evt_context *tcx, int level, uint64_t nd_off, int at)
{
	struct evt_trace *trace;

	D_ASSERT(at >= 0 && at < tcx->tc_order);

	D_DEBUG(DB_TRACE, "set trace[%d] "DF_X64"/%d\n", level, nd_off, at);

	trace = evt_tcx_trace(tcx, level);
	trace->tr_node = nd_off;
	trace->tr_at = at;
}

/** Reset all traces within context and set root as the 0-level trace */
static void
evt_tcx_reset_trace(struct evt_context *tcx)
{
	memset(&tcx->tc_traces[0], 0,
	       sizeof(tcx->tc_traces[0]) * EVT_TRACE_MAX);
	evt_tcx_set_dep(tcx, tcx->tc_root->tr_depth);
	evt_tcx_set_trace(tcx, 0, tcx->tc_root->tr_node, 0);
}

/**
 * Create a evtree context for create or open
 *
 * \param root_mmid	[IN]	Optional, root memory ID for open
 * \param root		[IN]	Optional, root address for inplace open
 * \param feats		[IN]	Optional, feature bits for create
 * \param order		[IN]	Optional, tree order for create
 * \param uma		[IN]	Memory attribute for the tree
 * \param info		[IN]	NVMe free space info
 * \param tcx_pp	[OUT]	The returned tree context
 */
static int
evt_tcx_create(TMMID(struct evt_root) root_mmid, struct evt_root *root,
	       uint64_t feats, unsigned int order, struct umem_attr *uma,
	       void *info, struct evt_context **tcx_pp)
{
	struct evt_context	*tcx;
	int			 depth;
	int			 rc;

	D_ALLOC_PTR(tcx);
	if (tcx == NULL)
		return -DER_NOMEM;

	tcx->tc_ref = 1; /* for the caller */
	tcx->tc_magic = EVT_HDL_ALIVE;

	/* XXX choose ops based on feature bits */
	tcx->tc_ops = evt_policies[0];

	rc = umem_class_init(uma, &tcx->tc_umm);
	if (rc != 0) {
		D_ERROR("Failed to setup mem class %d: %d\n", uma->uma_id, rc);
		D_GOTO(failed, rc);
	}
	tcx->tc_pmempool_uuid = umem_get_uuid(&tcx->tc_umm);
	tcx->tc_blks_info = info;

	if (!TMMID_IS_NULL(root_mmid)) { /* non-inplace tree open */
		tcx->tc_root_mmid = root_mmid;
		if (root == NULL)
			root = umem_id2ptr_typed(&tcx->tc_umm, root_mmid);
	}
	tcx->tc_root = root;

	if (root == NULL || root->tr_feats == 0) { /* tree creation */
		tcx->tc_feats	= feats;
		tcx->tc_order	= order;
		depth		= 0;
		D_DEBUG(DB_TRACE, "Create context for a new tree\n");

	} else {
		if (tcx->tc_pmempool_uuid != root->tr_pool_uuid) {
			D_ERROR("Mixing pools in same evtree not allowed\n");
			rc = -DER_INVAL;
			goto failed;
		}

		tcx->tc_feats	= root->tr_feats;
		tcx->tc_order	= root->tr_order;
		tcx->tc_inob	= root->tr_inob;
		depth		= root->tr_depth;
		D_DEBUG(DB_TRACE, "Load tree context from "TMMID_PF"\n",
			TMMID_P(root_mmid));
	}

	/* Initialize the embedded iterator entry array.  This is a minor
	 * optimization if the iterator is used more than once
	 */
	evt_ent_array_init(&tcx->tc_iter.it_entries);
	evt_tcx_set_dep(tcx, depth);
	*tcx_pp = tcx;
	return 0;

 failed:
	D_DEBUG(DB_TRACE, "Failed to create tree context: %d\n", rc);
	evt_tcx_decref(tcx);
	return rc;
}

int
evt_tcx_clone(struct evt_context *tcx, struct evt_context **tcx_pp)
{
	struct umem_attr uma;
	int		 rc;

	umem_attr_get(&tcx->tc_umm, &uma);
	if (!tcx->tc_root || tcx->tc_root->tr_feats == 0)
		return -DER_INVAL;

	rc = evt_tcx_create(tcx->tc_root_mmid, tcx->tc_root, -1, -1, &uma,
			    tcx->tc_blks_info, tcx_pp);
	return rc;
}

static int
evt_desc_free(struct evt_context *tcx, struct evt_desc *desc, daos_size_t size)
{
	bio_addr_t	*addr = &desc->dc_ex_addr;
	int		 rc = 0;

	if (bio_addr_is_hole(addr))
		return 0;

	if (addr->ba_type == DAOS_MEDIA_SCM) {
		umem_id_t mmid;

		mmid.pool_uuid_lo = tcx->tc_pmempool_uuid;
		mmid.off = addr->ba_off;
		rc = umem_free(evt_umm(tcx), mmid);
	} else {
		struct vea_space_info *vsi = tcx->tc_blks_info;
		uint64_t blk_off;
		uint32_t blk_cnt;

		D_ASSERT(addr->ba_type == DAOS_MEDIA_NVME);
		D_ASSERT(vsi != NULL);

		blk_off = vos_byte2blkoff(addr->ba_off);
		blk_cnt = vos_byte2blkcnt(size);

		rc = vea_free(vsi, blk_off, blk_cnt);
		if (rc)
			D_ERROR("Error on block free. %d\n", rc);
	}

	return rc;
}

/** check if a node is full */
static bool
evt_node_is_full(struct evt_context *tcx, uint64_t nd_off)
{
	struct evt_node *nd = evt_off2node(tcx, nd_off);

	D_ASSERT(nd->tn_nr <= tcx->tc_order);
	return nd->tn_nr == tcx->tc_order;
}

static inline void
evt_node_unset(struct evt_context *tcx, uint64_t nd_off, unsigned int bits)
{
	struct evt_node *nd = evt_off2node(tcx, nd_off);

	nd->tn_flags &= ~bits;
}

static inline bool
evt_node_is_set(struct evt_context *tcx, uint64_t nd_off, unsigned int bits)
{
	struct evt_node *nd = evt_off2node(tcx, nd_off);

	return nd->tn_flags & bits;
}

static inline bool
evt_node_is_leaf(struct evt_context *tcx, uint64_t nd_off)
{
	return evt_node_is_set(tcx, nd_off, EVT_NODE_LEAF);
}

static inline bool
evt_node_is_root(struct evt_context *tcx, uint64_t nd_off)
{
	return evt_node_is_set(tcx, nd_off, EVT_NODE_ROOT);
}

/** Return the rectangle at the offset of @at */
struct evt_node_entry *
evt_node_entry_at(struct evt_context *tcx, uint64_t nd_off, unsigned int at)
{
	struct evt_node		*nd = evt_off2node(tcx, nd_off);

	return &nd->tn_rec[at];
}

/** Return the address of child umem offset at the offset of @at */
static uint64_t
evt_node_child_at(struct evt_context *tcx, uint64_t nd_off, unsigned int at)
{
	struct evt_node_entry	*ne = evt_node_entry_at(tcx, nd_off, at);

	D_ASSERT(!evt_node_is_leaf(tcx, nd_off));
	return ne->ne_child;
}

/** Return the data pointer at the offset of @at */
static struct evt_desc *
evt_node_desc_at(struct evt_context *tcx, uint64_t nd_off, unsigned int at)
{
	struct evt_node_entry	*ne = evt_node_entry_at(tcx, nd_off, at);

	D_ASSERT(evt_node_is_leaf(tcx, nd_off));

	return evt_off2desc(tcx, ne->ne_child);
}

/** Return the rectangle at the offset of @at */
struct evt_rect *
evt_node_rect_at(struct evt_context *tcx, uint64_t nd_off, unsigned int at)
{
	struct evt_node_entry	*ne = evt_node_entry_at(tcx, nd_off, at);

	return &ne->ne_rect;
}

/**
 * Update the rectangle stored at the offset \a at of the specified node.
 * This function should update the MBR of the tree node the new rectangle
 * can enlarge the MBR.
 *
 * XXX, It will be ignored if the change shrinks the MBR of the node, this
 * should be fixed in the future.
 *
 * \param	tn_off [IN]	Tree node offset
 * \param	at	[IN]	Rectangle offset within the tree node.
 * \return	true		Node MBR changed
 *		false		No changed.
 */
static bool
evt_node_rect_update(struct evt_context *tcx, uint64_t tn_off,
		     unsigned int at, struct evt_rect *rect)
{
	struct evt_node_entry	*etmp;
	struct evt_rect		*rtmp;
	bool			 changed;

	/* update the rectangle at the specified position */
	etmp = evt_node_entry_at(tcx, tn_off, at);
	etmp->ne_rect = *rect;

	/* make adjustments to the position of the rectangle */
	if (tcx->tc_ops->po_adjust)
		tcx->tc_ops->po_adjust(tcx, tn_off, etmp, at);

	/* merge the rectangle with the current node */
	rtmp = evt_node_mbr_get(tcx, tn_off);
	changed = evt_rect_merge(rtmp, rect);

	return changed;
}

/**
 * Return the size of evtree node, leaf node has different size with internal
 * node.
 */
static int
evt_node_size(struct evt_context *tcx)
{
	return sizeof(struct evt_node) +
	       sizeof(struct evt_node_entry) * tcx->tc_order;
}

/** Allocate a evtree node */
static int
evt_node_alloc(struct evt_context *tcx, unsigned int flags, uint64_t *nd_off)
{
	struct evt_node		*nd;
	TMMID(struct evt_node)	 nd_mmid;

	nd_mmid = umem_zalloc_typed(evt_umm(tcx), struct evt_node,
				    evt_node_size(tcx));
	if (TMMID_IS_NULL(nd_mmid))
		return -DER_NOMEM;

	D_DEBUG(DB_TRACE, "Allocate new node "TMMID_PF" %d bytes\n",
		TMMID_P(nd_mmid), evt_node_size(tcx));
	nd = evt_tmmid2ptr(tcx, nd_mmid);
	nd->tn_flags = flags;
	nd->tn_magic = EVT_NODE_MAGIC;

	*nd_off = nd_mmid.oid.off;
	return 0;
}

static inline int
evt_node_tx_add(struct evt_context *tcx, uint64_t nd_off)
{
	struct evt_node	*nd;

	if (!evt_has_tx(tcx))
		return 0;

	nd = evt_off2node(tcx, nd_off);

	return umem_tx_add_ptr(evt_umm(tcx), nd, evt_node_size(tcx));
}

static int
evt_node_free(struct evt_context *tcx, uint64_t nd_off)
{
	return umem_free(evt_umm(tcx), evt_off2mmid(tcx, nd_off));
}

/**
 * Destroy a tree node and all its desendants nodes, or leaf records and
 * data extents.
 */
static int
evt_node_destroy(struct evt_context *tcx, uint64_t nd_off, int level)
{
	struct evt_node_entry	*ne;
	struct evt_node		*nd;
	struct evt_rect		*rect;
	bool			 leaf;
	int			 i;
	int			 rc = 0;

	nd = evt_off2node(tcx, nd_off);
	leaf = evt_node_is_leaf(tcx, nd_off);

	D_DEBUG(DB_TRACE, "Destroy %s node at level %d (nr = %d)\n",
		leaf ? "leaf" : "", level, nd->tn_nr);

	for (i = 0; i < nd->tn_nr; i++) {
		ne = evt_node_entry_at(tcx, nd_off, i);
		rect = evt_node_rect_at(tcx, nd_off, i);
		if (leaf) {
			/* NB: This will be replaced with a callback */
			rc = evt_desc_free(tcx, evt_off2desc(tcx, ne->ne_child),
					   tcx->tc_inob * evt_rect_width(rect));
			if (rc != 0)
				return rc;
			rc = umem_free(evt_umm(tcx),
				       evt_off2mmid(tcx, ne->ne_child));
			if (rc != 0)
				return rc;
		} else {
			rc = evt_node_destroy(tcx, ne->ne_child, level + 1);
			if (rc != 0)
				return rc;
		}
	}
	return evt_node_free(tcx, nd_off);
}

/** Return the MBR of a node */
static struct evt_rect *
evt_node_mbr_get(struct evt_context *tcx, uint64_t nd_off)
{
	struct evt_node	*node;

	node = evt_off2node(tcx, nd_off);
	return &node->tn_mbr;
}

/** (Re)compute MBR for a tree node */
static void
evt_node_mbr_cal(struct evt_context *tcx, uint64_t nd_off)
{
	struct evt_node	*node;
	struct evt_rect *mbr;
	int		 i;

	node = evt_off2node(tcx, nd_off);
	D_ASSERT(node->tn_nr != 0);

	mbr = &node->tn_mbr;
	*mbr = *evt_node_rect_at(tcx, nd_off, 0);
	for (i = 1; i < node->tn_nr; i++) {
		struct evt_rect *rect;

		rect = evt_node_rect_at(tcx, nd_off, i);
		evt_rect_merge(mbr, rect);
	}
	D_DEBUG(DB_TRACE, "Compute out MBR "DF_RECT"("DF_X64"), nr=%d\n",
		DP_RECT(mbr), nd_off, node->tn_nr);
}

/**
 * Split tree node \a src_off by moving some entries from it to the new
 * node \a dst_off. This function also updates MBRs for both nodes.
 *
 * Node split is a customized method of tree policy.
 */
static int
evt_node_split(struct evt_context *tcx, bool leaf,
	       uint64_t src_off, uint64_t dst_off)
{
	int	rc;

	rc = tcx->tc_ops->po_split(tcx, leaf, src_off, dst_off);
	if (rc == 0) { /* calculate MBR for both nodes */
		evt_node_mbr_cal(tcx, src_off);
		evt_node_mbr_cal(tcx, dst_off);
	}
	return rc;
}

/**
 * Insert a new entry into a node \a nd_off, update MBR of the node if it's
 * enlarged after inserting the new entry. This function should be called
 * only if the node has empty slot (not full).
 *
 * Entry insertion is a customized method of tree policy.
 */
static int
evt_node_insert(struct evt_context *tcx, uint64_t nd_off, uint64_t in_off,
		const struct evt_entry_in *ent, bool *mbr_changed)
{
	struct evt_rect *mbr;
	struct evt_node *nd;
	int		 rc;
	bool		 changed = 0;

	nd  = evt_off2node(tcx, nd_off);
	mbr = evt_node_mbr_get(tcx, nd_off);

	D_DEBUG(DB_TRACE, "Insert "DF_RECT" into "DF_RECT"("DF_X64")\n",
		DP_RECT(&ent->ei_rect), DP_RECT(mbr), nd_off);

	rc = tcx->tc_ops->po_insert(tcx, nd_off, in_off, ent);
	if (rc == 0) {
		if (nd->tn_nr == 1) {
			nd->tn_mbr = ent->ei_rect;
			changed = true;
		} else {
			changed = evt_rect_merge(&nd->tn_mbr, &ent->ei_rect);
		}
		D_DEBUG(DB_TRACE, "New MBR is "DF_RECT", nr=%d\n",
			DP_RECT(mbr), nd->tn_nr);
	}

	if (mbr_changed)
		*mbr_changed = changed;

	return rc;
}

/**
 * Calculate weight difference of the node between before and after adding
 * a new rectangle \a rect. This function is supposed to help caller to choose
 * destination node for insertion.
 *
 * Weight calculation is a customized method of tree policy.
 */
static void
evt_node_weight_diff(struct evt_context *tcx, uint64_t nd_off,
		     const struct evt_rect *rect,
		     struct evt_weight *weight_diff)
{
	struct evt_node	  *nd = evt_off2node(tcx, nd_off);
	struct evt_rect	   rtmp;
	struct evt_weight  wt_org;
	struct evt_weight  wt_new;
	int		   range;
	int		   time;

	evt_rect_overlap(&nd->tn_mbr, rect, &range, &time);
	if ((time & (RT_OVERLAP_SAME | RT_OVERLAP_OVER)) &&
	    (range & RT_OVERLAP_INCLUDED)) {
		/* no difference, because the rectangle is included by the
		 * MBR of the node.
		 */
		memset(weight_diff, 0, sizeof(*weight_diff));
		return;
	}

	memset(&wt_org, 0, sizeof(wt_org));
	memset(&wt_new, 0, sizeof(wt_new));

	rtmp = nd->tn_mbr;
	tcx->tc_ops->po_rect_weight(tcx, &rtmp, &wt_org);

	evt_rect_merge(&rtmp, rect);
	tcx->tc_ops->po_rect_weight(tcx, &rtmp, &wt_new);

	evt_weight_diff(&wt_new, &wt_org, weight_diff);
}

/** The tree root is empty */
static inline bool
evt_root_empty(struct evt_context *tcx)
{
	struct evt_root *root = tcx->tc_root;

	return root == NULL || root->tr_node == 0;
}

/** Add the tree root to the transaction */
static int
evt_root_tx_add(struct evt_context *tcx)
{
	struct umem_instance *umm = evt_umm(tcx);
	int		      rc;

	if (!evt_has_tx(tcx))
		return 0;

	if (!TMMID_IS_NULL(tcx->tc_root_mmid)) {
		rc = umem_tx_add_mmid_typed(umm, tcx->tc_root_mmid);
	} else {
		D_ASSERT(tcx->tc_root != NULL);
		rc = umem_tx_add_ptr(umm, tcx->tc_root, sizeof(*tcx->tc_root));
	}
	return rc;
}

/** Initialize the tree root */
static int
evt_root_init(struct evt_context *tcx)
{
	struct evt_root	*root;
	int		 rc;

	rc = evt_root_tx_add(tcx);
	if (rc != 0)
		return rc;

	root = tcx->tc_root;

	root->tr_feats = tcx->tc_feats;
	root->tr_order = tcx->tc_order;
	root->tr_node  = 0;
	root->tr_pool_uuid = tcx->tc_pmempool_uuid;

	return 0;
}

/** Allocate a root node for a new tree. */
static int
evt_root_alloc(struct evt_context *tcx)
{
	tcx->tc_root_mmid = umem_znew_typed(evt_umm(tcx), struct evt_root);
	if (TMMID_IS_NULL(tcx->tc_root_mmid))
		return -DER_NOMEM;

	tcx->tc_root = evt_tmmid2ptr(tcx, tcx->tc_root_mmid);
	return evt_root_init(tcx);
}

static int
evt_root_free(struct evt_context *tcx)
{
	int	rc;
	if (!TMMID_IS_NULL(tcx->tc_root_mmid)) {
		rc = umem_free_typed(evt_umm(tcx), tcx->tc_root_mmid);
		tcx->tc_root_mmid = EVT_ROOT_NULL;
	} else {
		rc = evt_root_tx_add(tcx);
		if (rc != 0)
			goto out;
		memset(tcx->tc_root, 0, sizeof(*tcx->tc_root));
	}
out:
	tcx->tc_root = NULL;
	return rc;
}

/**
 * Activate an empty tree by allocating a node for the root and set the
 * tree depth to one.
 */
static int
evt_root_activate(struct evt_context *tcx, uint32_t inob)
{
	struct evt_root		*root;
	uint64_t		 nd_off;
	int			 rc;

	root = tcx->tc_root;

	D_ASSERT(root->tr_depth == 0);
	D_ASSERT(root->tr_node == 0);

	/* root node is also a leaf node */
	rc = evt_node_alloc(tcx, EVT_NODE_ROOT | EVT_NODE_LEAF, &nd_off);
	if (rc != 0)
		return rc;

	rc = evt_root_tx_add(tcx);
	if (rc != 0)
		return rc;

	root->tr_node = nd_off;
	root->tr_depth = 1;
	if (inob != 0)
		tcx->tc_inob = root->tr_inob = inob;

	evt_tcx_set_dep(tcx, root->tr_depth);
	evt_tcx_set_trace(tcx, 0, nd_off, 0);

	return 0;
}

static int
evt_root_deactivate(struct evt_context *tcx)
{
	struct evt_root	*root = tcx->tc_root;
	int		 rc;

	D_ASSERT(root->tr_depth != 0);
	D_ASSERT(root->tr_node != 0);

	rc = evt_root_tx_add(tcx);
	if (rc != 0)
		return rc;

	root->tr_depth = 0;
	rc = umem_free(evt_umm(tcx), evt_off2mmid(tcx, root->tr_node));
	if (rc != 0)
		return rc;

	root->tr_node = 0;
	evt_tcx_set_dep(tcx, 0);
	return 0;
}

/** Destroy the root node and all its descendants. */
static int
evt_root_destroy(struct evt_context *tcx)
{
	uint64_t	node;
	int		rc;

	node = tcx->tc_root->tr_node;
	if (node != 0) {
		/* destroy the root node and all descendants */
		rc = evt_node_destroy(tcx, node, 0);
		if (rc != 0)
			return rc;
	}

	return evt_root_free(tcx);
}

/** Select a node from two for the rectangle \a rect being inserted */
static uint64_t
evt_select_node(struct evt_context *tcx, const struct evt_rect *rect,
		uint64_t nd_off1, uint64_t nd_off2)
{
	struct evt_weight	wt1;
	struct evt_weight	wt2;
	int			rc;

	evt_node_weight_diff(tcx, nd_off1, rect, &wt1);
	evt_node_weight_diff(tcx, nd_off2, rect, &wt2);

	rc = evt_weight_cmp(&wt1, &wt2);
	return rc < 0 ? nd_off1 : nd_off2;
}

/**
 * Insert an entry \a entry to the leaf node located by the trace of \a tcx.
 * If the leaf node is full it will be split. The split will bubble up if its
 * parent is also full.
 */
static int
evt_insert_or_split(struct evt_context *tcx, const struct evt_entry_in *ent_new)
{
	struct evt_rect		*mbr	  = NULL;
	uint64_t		 nm_save = 0;
	struct evt_entry_in	 entry	  = *ent_new;
	int			 rc	  = 0;
	int			 level	  = tcx->tc_depth - 1;
	bool			 mbr_changed = false;

	while (1) {
		struct evt_trace	*trace;
		uint64_t		 nm_cur;
		uint64_t		 nm_new;
		uint64_t		 nm_ins;
		bool			 leaf;

		trace	= &tcx->tc_trace[level];
		nm_cur	= trace->tr_node;
		if (!trace->tr_tx_added) {
			rc = evt_node_tx_add(tcx, nm_cur);
			if (rc != 0)
				return rc;
			trace->tr_tx_added = true;
		}

		if (mbr) { /* This is set only if no more insert or split */
			D_ASSERT(mbr_changed);
			/* Update the child MBR stored in the current node
			 * because MBR of child has been enlarged.
			 */
			mbr_changed = evt_node_rect_update(tcx, nm_cur,
							   trace->tr_at, mbr);
			if (!mbr_changed || level == 0)
				D_GOTO(out, 0);

			/* continue to merge MBR with upper level node */
			mbr = evt_node_mbr_get(tcx, nm_cur);
			level--;
			continue;
		}

		if (!evt_node_is_full(tcx, nm_cur)) {
			bool	changed;

			rc = evt_node_insert(tcx, nm_cur, nm_save,
					     &entry, &changed);
			if (rc != 0)
				D_GOTO(failed, rc);

			/* NB: mbr_changed could have been set while splitting
			 * the child node.
			 */
			mbr_changed |= changed;
			if (!mbr_changed || level == 0)
				D_GOTO(out, 0);

			/* continue to merge MBR with upper level node */
			mbr = evt_node_mbr_get(tcx, nm_cur);
			level--;
			continue;
		}
		/* Try to split */

		D_DEBUG(DB_TRACE, "Split node at level %d\n", level);

		leaf = evt_node_is_leaf(tcx, nm_cur);
		rc = evt_node_alloc(tcx, leaf ? EVT_NODE_LEAF : 0, &nm_new);
		if (rc != 0)
			D_GOTO(failed, rc);

		rc = evt_node_split(tcx, leaf, nm_cur, nm_new);
		if (rc != 0) {
			D_DEBUG(DB_TRACE, "Failed to split node: %d\n", rc);
			D_GOTO(failed, rc);
		}

		/* choose a node for insert between the current node and the
		 * new created node.
		 */
		nm_ins = evt_select_node(tcx, &entry.ei_rect, nm_cur, nm_new);
		rc = evt_node_insert(tcx, nm_ins, nm_save, &entry, NULL);
		if (rc != 0)
			D_GOTO(failed, rc);

		/* Insert the new node to upper level node:
		 * - If the current node is not root, insert it to its parent
		 * - If the current node is root, create a new root
		 */
		nm_save = nm_new;
		entry.ei_rect = *evt_node_mbr_get(tcx, nm_new);
		if (level != 0) { /* not root */
			level--;
			/* After splitting, MBR of the current node has been
			 * changed (half of its entries are moved out, and
			 * probably added a new entry), so we need to update
			 * its MBR stored in its parent.
			 */
			trace = &tcx->tc_trace[level];
			mbr_changed = evt_node_rect_update(tcx,
						trace->tr_node, trace->tr_at,
						evt_node_mbr_get(tcx, nm_cur));
			/* continue to insert the new node to its parent */
			continue;
		}

		D_DEBUG(DB_TRACE, "Create a new root, depth=%d.\n",
			tcx->tc_root->tr_depth + 1);

		D_ASSERT(evt_node_is_root(tcx, nm_cur));
		evt_node_unset(tcx, nm_cur, EVT_NODE_ROOT);

		rc = evt_node_alloc(tcx, EVT_NODE_ROOT, &nm_new);
		if (rc != 0)
			D_GOTO(failed, rc);

		rc = evt_node_insert(tcx, nm_new, nm_save, &entry, NULL);
		if (rc != 0)
			D_GOTO(failed, rc);

		evt_tcx_set_dep(tcx, tcx->tc_depth + 1);
		tcx->tc_trace->tr_node = nm_new;
		tcx->tc_trace->tr_at = 0;

		rc = evt_root_tx_add(tcx);
		if (rc != 0)
			D_GOTO(failed, rc);
		tcx->tc_root->tr_node = nm_new;
		tcx->tc_root->tr_depth++;

		/* continue the loop and insert the original root node into
		 * the new root node.
		 */
		entry.ei_rect = *evt_node_mbr_get(tcx, nm_cur);
		nm_save = nm_cur;
	}
 out:
	return 0;
 failed:
	D_ERROR("Failed to insert entry to level %d: %d\n", level, rc);
	return rc;
}

/** Insert a single entry to evtree */
static int
evt_insert_entry(struct evt_context *tcx, const struct evt_entry_in *ent)
{
	uint64_t		nd_off;
	int			level;
	int			i;

	D_DEBUG(DB_TRACE, "Inserting rectangle "DF_RECT"\n",
		DP_RECT(&ent->ei_rect));

	evt_tcx_reset_trace(tcx);
	nd_off = tcx->tc_trace->tr_node; /* NB: trace points at root node */
	level = 0;

	while (1) {
		struct evt_node		*nd;
		uint64_t		 nm_cur;
		uint64_t		 nm_dst;
		int			 tr_at;

		if (evt_node_is_leaf(tcx, nd_off)) {
			evt_tcx_set_trace(tcx, level, nd_off, 0);
			break;
		}

		tr_at = -1;
		nm_dst = 0;
		nd = evt_off2node(tcx, nd_off);

		for (i = 0; i < nd->tn_nr; i++) {
			nm_cur = evt_node_child_at(tcx, nd_off, i);
			if (nm_dst == 0) {
				nm_dst = nm_cur;
			} else {
				nm_dst = evt_select_node(tcx, &ent->ei_rect,
							 nm_dst, nm_cur);
			}

			/* check if the current child is the new destination */
			if (nm_dst == nm_cur)
				tr_at = i;
		}

		/* store the trace in case we need to bubble split */
		evt_tcx_set_trace(tcx, level, nd_off, tr_at);
		nd_off = nm_dst;
		level++;
	}
	D_ASSERT(level == tcx->tc_depth - 1);

	return evt_insert_or_split(tcx, ent);
}

static void
evt_desc_copy(struct evt_context *tcx, const struct evt_entry_in *ent)
{
	struct evt_desc		*dst_desc;
	struct evt_trace	*trace;
	uint64_t		 nd_off;
	daos_size_t		 size;

	trace = &tcx->tc_trace[tcx->tc_depth - 1];
	nd_off = trace->tr_node;
	dst_desc = evt_node_desc_at(tcx, nd_off, trace->tr_at);

	D_ASSERT(ent->ei_inob != 0);
	size = ent->ei_inob * evt_rect_width(&ent->ei_rect);

	/* Free the pmem that dst_desc references */
	evt_desc_free(tcx, dst_desc, size);

	uuid_copy(dst_desc->dc_cookie, ent->ei_cookie);
	dst_desc->dc_ex_addr = ent->ei_addr;
	dst_desc->dc_ver = ent->ei_ver;
	dst_desc->dc_csum = ent->ei_csum;
}

/**
 * Insert a versioned extent (rectangle) and its data offset into the tree.
 *
 * Please check API comment in evtree.h for the details.
 */
int
evt_insert(daos_handle_t toh, const struct evt_entry_in *entry)
{
	struct evt_context	*tcx;
	struct evt_entry_array	 ent_array;
	int			 rc;

	tcx = evt_hdl2tcx(toh);
	if (tcx == NULL)
		return -DER_NO_HDL;

	if (tcx->tc_inob && entry->ei_inob && tcx->tc_inob != entry->ei_inob) {
		D_ERROR("Variable record size not supported in evtree:"
			" %d != %d\n", entry->ei_inob, tcx->tc_inob);
		return -DER_INVAL;
	}

	evt_ent_array_init(&ent_array);

	/* Phase-1: Check for overwrite */
	rc = evt_ent_array_fill(tcx, EVT_FIND_OVERWRITE, NULL, &entry->ei_rect,
				&ent_array);
	if (rc != 0)
		return rc;

	rc = evt_tx_begin(tcx);
	if (rc != 0)
		return rc;

	if (tcx->tc_depth == 0) { /* empty tree */
		rc = evt_root_activate(tcx, entry->ei_inob);
		if (rc != 0)
			goto out;
	} else if (tcx->tc_inob == 0 && entry->ei_inob != 0) {
		rc = evt_root_tx_add(tcx);
		if (rc != 0)
			goto out;
		tcx->tc_inob = tcx->tc_root->tr_inob = entry->ei_inob;
	}

	D_ASSERT(ent_array.ea_ent_nr <= 1);
	if (ent_array.ea_ent_nr == 1) {
		/*
		 * NB: This is part of the current hack to keep "supporting"
		 * overwrite for same epoch, full overwrite.
		 * No copy for duplicate punch.
		 */
		if (entry->ei_inob > 0)
			evt_desc_copy(tcx, entry);
		goto out;
	}

	/* Phase-2: Inserting */
	rc = evt_insert_entry(tcx, entry);

	/* No need for evt_ent_array_fill as there will be no allocations
	 * with 1 entry in the list
	 */
out:
	return evt_tx_end(tcx, rc);
}

/** Fill the entry with the extent at the specified position of \a nd_off */
void
evt_entry_fill(struct evt_context *tcx, uint64_t nd_off,
	       unsigned int at, const struct evt_rect *rect_srch,
	       struct evt_entry *entry)
{
	struct evt_desc	   *desc;
	struct evt_rect	   *rect;
	daos_off_t	    offset;
	daos_size_t	    width;
	daos_size_t	    nr;

	rect = evt_node_rect_at(tcx, nd_off, at);
	desc = evt_node_desc_at(tcx, nd_off, at);

	offset = 0;
	width = evt_rect_width(rect);

	entry->en_visibility = 0; /* Unknown */

	if (rect_srch && rect_srch->rc_ex.ex_lo > rect->rc_ex.ex_lo) {
		offset = rect_srch->rc_ex.ex_lo - rect->rc_ex.ex_lo;
		D_ASSERTF(width > offset, DF_U64"/"DF_U64"\n", width, offset);
		width -= offset;
		entry->en_visibility = EVT_PARTIAL;
	}

	if (rect_srch && rect_srch->rc_ex.ex_hi < rect->rc_ex.ex_hi) {
		nr = rect->rc_ex.ex_hi - rect_srch->rc_ex.ex_hi;
		D_ASSERTF(width > nr, DF_U64"/"DF_U64"\n", width, nr);
		width -= nr;
		entry->en_visibility = EVT_PARTIAL;
	}

	entry->en_epoch = rect->rc_epc;
	entry->en_ext = entry->en_sel_ext = rect->rc_ex;
	entry->en_sel_ext.ex_lo += offset;
	entry->en_sel_ext.ex_hi = entry->en_sel_ext.ex_lo + width - 1;

	entry->en_addr = desc->dc_ex_addr;
	uuid_copy(entry->en_cookie, desc->dc_cookie);
	entry->en_ver = desc->dc_ver;
	entry->en_csum = desc->dc_csum;

	if (offset != 0) {
		/* Adjust cached pointer since we're only referencing a
		 * part of the extent
		 */
		evt_ent_addr_update(tcx, entry, offset);
	}
}

/**
 * Find all versioned extents which intercept with the input one \a rect.
 * It attaches all found extents and their data pointers on \a ent_array if
 * \a no_overlap is false, otherwise returns error if there is any overlapped
 * extent.
 *
 * \param rect		[IN]	Rectangle to check.
 * \param no_overlap	[IN]	Returns error if \a rect overlap with any
 *				existent extent.
 * \param ent_array	[OUT]	The returned entries for overlapped extents.
 */
int
evt_ent_array_fill(struct evt_context *tcx, enum evt_find_opc find_opc,
		   const struct evt_filter *filter, const struct evt_rect *rect,
		   struct evt_entry_array *ent_array)
{
	uint64_t	nd_off;
	int		level;
	int		at;
	int		i;
	int		rc = 0;

	D_DEBUG(DB_TRACE, "Searching rectangle "DF_RECT" opc=%d\n",
		DP_RECT(rect), find_opc);
	if (tcx->tc_root->tr_depth == 0)
		return 0; /* empty tree */

	ent_array_reset(tcx, ent_array);
	evt_tcx_reset_trace(tcx);

	level = at = 0;
	nd_off = tcx->tc_root->tr_node;
	while (1) {
		struct evt_rect *mbr;
		struct evt_node	*node;
		bool		 leaf;

		node = evt_off2node(tcx, nd_off);
		leaf = evt_node_is_leaf(tcx, nd_off);
		mbr  = evt_node_mbr_get(tcx, nd_off);

		D_ASSERT(!leaf || at == 0);
		D_DEBUG(DB_TRACE,
			"Checking "DF_RECT"("DF_X64"), l=%d, a=%d, f=%d\n",
			DP_RECT(mbr), nd_off, level, at, leaf);

		for (i = at; i < node->tn_nr; i++) {
			struct evt_entry	*ent;
			struct evt_rect		*rtmp;
			int			 time_overlap;
			int			 range_overlap;

			rtmp = evt_node_rect_at(tcx, nd_off, i);
			D_DEBUG(DB_TRACE, " rect[%d]="DF_RECT"\n",
				i, DP_RECT(rtmp));

			if (evt_filter_rect(filter, rtmp))
				continue; /* Doesn't match the filter */

			evt_rect_overlap(rtmp, rect, &range_overlap,
					 &time_overlap);
			switch (range_overlap) {
			default:
				D_ASSERT(0);
			case RT_OVERLAP_NO:
				continue; /* skip, no overlap */

			case RT_OVERLAP_SAME:
			case RT_OVERLAP_INCLUDED:
			case RT_OVERLAP_INCLUDES:
			case RT_OVERLAP_PARTIAL:
				break; /* overlapped */
			}

			switch (time_overlap) {
			default:
				D_ASSERT(0);
			case RT_OVERLAP_NO:
			case RT_OVERLAP_UNDER:
				continue; /* skip, no overlap */
			case RT_OVERLAP_OVER:
			case RT_OVERLAP_SAME:
				break; /* overlapped */
			}

			if (!leaf) {
				/* break the internal loop and enter the
				 * child node.
				 */
				D_DEBUG(DB_TRACE, "Enter the next level\n");
				break;
			}
			D_DEBUG(DB_TRACE, "Found overlapped leaf rect\n");

			/* early check */
			switch (find_opc) {
			default:
				D_ASSERTF(0, "%d\n", find_opc);
			case EVT_FIND_OVERWRITE:
				if (time_overlap != RT_OVERLAP_SAME)
					continue; /* not same epoch, skip */
				/* NB: This is temporary to allow full overwrite
				 * in same epoch to avoid breaking rebuild.
				 * Without some sequence number and client
				 * identifier, we can't do this robustly.
				 * There can be a race between rebuild and
				 * client doing different updates.  But this
				 * isn't any worse than what we already have in
				 * place so I did it this way to minimize
				 * change while we decide how to handle this
				 * properly.
				 */
				if (range_overlap != RT_OVERLAP_SAME) {
					D_DEBUG(DB_IO, "Same epoch partial "
						"overwrite not supported:"
						DF_RECT" overlaps with "DF_RECT
						"\n", DP_RECT(rect),
						DP_RECT(rtmp));
					rc = -DER_NO_PERM;
					goto out;
				}
				break; /* we can update the record in place */
			case EVT_FIND_SAME:
				if (range_overlap != RT_OVERLAP_SAME)
					continue;
				if (time_overlap != RT_OVERLAP_SAME)
					continue;
				break;
			case EVT_FIND_FIRST:
			case EVT_FIND_ALL:
				break;
			}

			rc = ent_array_alloc(tcx, ent_array, &ent, false);
			if (rc != 0) {
				D_ASSERT(rc != -DER_AGAIN);
				goto out;
			}

			evt_entry_fill(tcx, nd_off, i, rect, ent);
			switch (find_opc) {
			default:
				D_ASSERTF(0, "%d\n", find_opc);
			case EVT_FIND_OVERWRITE:
			case EVT_FIND_FIRST:
			case EVT_FIND_SAME:
				/* store the trace and return for clip or
				 * iteration.
				 * NB: clip is not implemented yet.
				 */
				evt_tcx_set_trace(tcx, level, nd_off, i);
				D_GOTO(out, rc = 0);

			case EVT_FIND_ALL:
				break;
			}
		}

		if (i < node->tn_nr) {
			/* overlapped with a non-leaf node, dive into it. */
			evt_tcx_set_trace(tcx, level, nd_off, i);
			nd_off = evt_node_child_at(tcx, nd_off, i);
			at = 0;
			level++;

		} else {
			struct evt_trace *trace;

			if (level == 0) { /* done with the root */
				D_DEBUG(DB_TRACE, "Found total %d rects\n",
					ent_array ? ent_array->ea_ent_nr : 0);
				return 0; /* succeed and return */
			}

			level--;
			trace = evt_tcx_trace(tcx, level);
			nd_off = trace->tr_node;
			at = trace->tr_at + 1;
			D_ASSERT(at <= tcx->tc_order);
		}
	}
out:
	if (rc != 0)
		evt_ent_array_fini(ent_array);

	return rc;
}

struct evt_max_rect {
	struct evt_rect		mr_rect;
	bool			mr_valid;
	bool			mr_punched;
};

static bool
saved_rect_is_greater(struct evt_max_rect *saved, struct evt_entry *ent)
{
	struct evt_rect	*r1;
	bool		 is_greater = false;

	if (!saved->mr_valid) /* No rectangle saved yet */
		return false;

	r1 = &saved->mr_rect;

	D_DEBUG(DB_TRACE, "Comparing saved "DF_RECT" to "DF_ENT"\n",
		DP_RECT(r1), DP_ENT(ent));
	if (r1->rc_ex.ex_hi > ent->en_sel_ext.ex_hi) {
		is_greater = true;
		goto out;
	}

	if (r1->rc_ex.ex_hi < ent->en_sel_ext.ex_hi)
		goto out;

	if (r1->rc_epc > ent->en_epoch)
		is_greater = true;

out:
	/* Now we need to update the lower bound of whichever rectangle is
	 * selected if the chosen rectangle is partially covered
	 */
	if (is_greater) {
		if (ent->en_epoch > r1->rc_epc) {
			if (ent->en_sel_ext.ex_hi >= r1->rc_ex.ex_lo)
				r1->rc_ex.ex_lo = ent->en_sel_ext.ex_hi + 1;
		}
	} else {
		if (r1->rc_epc > ent->en_epoch) {
			if (r1->rc_ex.ex_hi >= ent->en_sel_ext.ex_lo)
				ent->en_sel_ext.ex_lo = r1->rc_ex.ex_hi + 1;
		}
	}
	return is_greater;
}


int
evt_get_size(daos_handle_t toh, daos_epoch_t epoch, daos_size_t *size)
{
	struct evt_context	 *tcx;
	struct evt_rect		  rect; /* specifies range we are searching */
	struct evt_max_rect	  saved_rect = {0};
	struct evt_entry	  ent;
	uint64_t		  nd_off;
	int			  level;
	int			  at;
	int			  i;

	if (size == NULL)
		return -DER_INVAL;

	tcx = evt_hdl2tcx(toh);
	if (tcx == NULL)
		return -DER_NO_HDL;

	*size = 0;

	D_DEBUG(DB_TRACE, "Finding evt range at epoch "DF_U64"\n", epoch);
	/* Start with the whole range.  We'll repeat the algorithm until we
	 * either we find nothing or we find a non-punched rectangle.
	 */
	rect.rc_ex.ex_lo = 0;
	rect.rc_ex.ex_hi = (daos_off_t)-1;
	rect.rc_epc = epoch;

	if (tcx->tc_root->tr_depth == 0)
		return 0; /* empty tree */

try_again:
	D_DEBUG(DB_TRACE, "Scanning for maximum in "DF_RECT"\n",
		DP_RECT(&rect));

	evt_tcx_reset_trace(tcx);

	saved_rect.mr_valid = false; /* Reset the saved entry */

	level = at = 0;
	nd_off = tcx->tc_root->tr_node;
	while (1) {
		struct evt_rect *mbr;
		struct evt_node	*node;
		bool		 leaf;

		node = evt_off2node(tcx, nd_off);
		leaf = evt_node_is_leaf(tcx, nd_off);
		mbr  = evt_node_mbr_get(tcx, nd_off);

		D_DEBUG(DB_TRACE, "Checking mbr="DF_RECT", l=%d, a=%d\n",
			DP_RECT(mbr), level, at);

		D_ASSERT(!leaf || at == 0);

		for (i = at; i < node->tn_nr; i++) {
			struct evt_rect		*rtmp;
			int			 time_overlap;
			int			 range_overlap;

			rtmp = evt_node_rect_at(tcx, nd_off, i);
			D_DEBUG(DB_TRACE, "Checking rect[%d]="DF_RECT"\n",
				i, DP_RECT(rtmp));

			evt_rect_overlap(rtmp, &rect, &range_overlap,
					 &time_overlap);
			if (range_overlap == RT_OVERLAP_NO)
				continue;
			D_ASSERT(time_overlap != RT_OVERLAP_NO);

			if (time_overlap == RT_OVERLAP_UNDER)
				continue;

			if (!leaf) {
				/* break the internal loop and enter the
				 * child node.
				 */
				break;
			}

			memset(&ent, 0, sizeof(ent));
			evt_entry_fill(tcx, nd_off, i, &rect, &ent);

			/* Ok, now that we've potentially trimmed the rectangle
			 * in ent, let's do the check again
			 */
			if (saved_rect_is_greater(&saved_rect, &ent))
				continue;

			saved_rect.mr_valid = true;
			if (bio_addr_is_hole(&ent.en_addr))
				saved_rect.mr_punched = true;
			else
				saved_rect.mr_punched = false;
			saved_rect.mr_rect.rc_epc = ent.en_epoch;
			saved_rect.mr_rect.rc_ex = ent.en_sel_ext;

			D_DEBUG(DB_TRACE, "New saved rectangle "DF_RECT
				" punched? : %s\n",
				DP_RECT(&saved_rect.mr_rect),
				saved_rect.mr_punched ? "yes" : "no");

			evt_tcx_set_trace(tcx, level, nd_off, i);
		}

		if (i < node->tn_nr) {
			/* overlapped with a non-leaf node, dive into it. */
			evt_tcx_set_trace(tcx, level, nd_off, i);
			nd_off = evt_node_child_at(tcx, nd_off, i);
			at = 0;
			level++;
		} else {
			struct evt_trace *trace;

			if (level == 0) { /* done with the root */
				daos_off_t	old;

				if (!saved_rect.mr_valid)
					return 0;

				old = saved_rect.mr_rect.rc_ex.ex_lo;

				if (saved_rect.mr_punched) {
					D_DEBUG(DB_TRACE,
						"Final extent in range is"
						" punched ("DF_RECT")\n",
						DP_RECT(&saved_rect.mr_rect));
					if (old == 0)
						return 0;
					rect.rc_ex.ex_hi = old - 1;

					goto try_again;
				}
				*size = saved_rect.mr_rect.rc_ex.ex_hi + 1;
				break;
			}

			level--;
			trace = evt_tcx_trace(tcx, level);
			nd_off = trace->tr_node;
			at = trace->tr_at + 1;
			D_ASSERT(at <= tcx->tc_order);
		}
	}
	/* Only way to break the outer loop is if we found a valid record */
	D_ASSERT(saved_rect.mr_valid);
	return 0;
}

/**
 * Find all versioned extents intercepting with the input rectangle \a rect
 * and return their data pointers.
 *
 * Please check API comment in evtree.h for the details.
 */
int
evt_find(daos_handle_t toh, const struct evt_rect *rect,
	 struct evt_entry_array *ent_array)
{
	struct evt_context	*tcx;
	int			 rc;

	tcx = evt_hdl2tcx(toh);
	if (tcx == NULL)
		return -DER_NO_HDL;

	evt_ent_array_init(ent_array);
	rc = evt_ent_array_fill(tcx, EVT_FIND_ALL, NULL, rect, ent_array);
	if (rc == 0)
		rc = evt_ent_array_sort(tcx, ent_array, EVT_VISIBLE);
	if (rc != 0)
		evt_ent_array_fini(ent_array);
	return rc;
}

/** move the probing trace forward or backward */
bool
evt_move_trace(struct evt_context *tcx, bool forward)
{
	struct evt_trace	*trace;
	struct evt_node		*nd;
	uint64_t		 nd_off;

	if (evt_root_empty(tcx))
		return false;

	trace = &tcx->tc_trace[tcx->tc_depth - 1];
	while (1) {
		nd_off = trace->tr_node;
		nd = evt_off2node(tcx, nd_off);

		/* already reach at the begin or end of this node */
		if ((trace->tr_at == (nd->tn_nr - 1) && forward) ||
		    (trace->tr_at == 0 && !forward)) {
			if (evt_node_is_root(tcx, nd_off)) {
				D_ASSERT(trace == tcx->tc_trace);
				D_DEBUG(DB_TRACE, "End\n");
				return false;
			}
			/* check its parent */
			trace--;
			continue;
		} /* else: not yet */

		if (forward)
			trace->tr_at++;
		else
			trace->tr_at--;
		break;
	}

	/* move to the first/last entry in the subtree */
	while (trace < &tcx->tc_trace[tcx->tc_depth - 1]) {
		uint64_t	tmp;

		tmp = evt_node_child_at(tcx, trace->tr_node, trace->tr_at);
		nd = evt_off2node(tcx, tmp);
		D_ASSERTF(nd->tn_nr != 0, "%d\n", nd->tn_nr);

		trace++;
		trace->tr_at = forward ? 0 : nd->tn_nr - 1;
		trace->tr_node = tmp;
	}

	return true;
}

/**
 * Open a tree by memory ID @root_mmid.
 * Please check API comment in evtree.h for the details.
 */
int
evt_open(TMMID(struct evt_root) root_mmid, struct umem_attr *uma,
	 daos_handle_t *toh)
{
	struct evt_context *tcx;
	int		    rc;

	rc = evt_tcx_create(root_mmid, NULL, -1, -1, uma, NULL, &tcx);
	if (rc != 0)
		return rc;

	*toh = evt_tcx2hdl(tcx); /* take refcount for open */
	evt_tcx_decref(tcx); /* -1 for create */
	return 0;
}

/**
 * Open a inplace tree by root address @root.
 * Please check API comment in evtree.h for the details.
 */
int
evt_open_inplace(struct evt_root *root, struct umem_attr *uma,
		 void *info, daos_handle_t *toh)
{
	struct evt_context *tcx;
	int		    rc;

	if (root->tr_order == 0) {
		D_DEBUG(DB_TRACE, "Tree order is zero\n");
		return -DER_INVAL;
	}

	rc = evt_tcx_create(EVT_ROOT_NULL, root, -1, -1, uma, info, &tcx);
	if (rc != 0)
		return rc;

	*toh = evt_tcx2hdl(tcx);
	evt_tcx_decref(tcx); /* -1 for tcx_create */
	return 0;
}

/**
 * Close a tree open handle.
 * Please check API comment in evtree.h for the details.
 */
int
evt_close(daos_handle_t toh)
{
	struct evt_context *tcx;

	tcx = evt_hdl2tcx(toh);
	if (tcx == NULL)
		return -DER_NO_HDL;

	evt_tcx_decref(tcx); /* -1 for open/create */
	return 0;
}

/**
 * Create a new tree and open it.
 * Please check API comment in evtree.h for the details.
 */
int
evt_create(uint64_t feats, unsigned int order, struct umem_attr *uma,
	   TMMID(struct evt_root) *root_mmid_p, daos_handle_t *toh)
{
	struct evt_context *tcx;
	int		    rc;

	if (!(feats & EVT_FEAT_SORT_SOFF)) {
		D_DEBUG(DB_TRACE, "Unknown feature bits "DF_X64"\n", feats);
		return -DER_INVAL;
	}

	if (order < EVT_ORDER_MIN || order > EVT_ORDER_MAX) {
		D_DEBUG(DB_TRACE, "Invalid tree order %d\n", order);
		return -DER_INVAL;
	}

	rc = evt_tcx_create(EVT_ROOT_NULL, NULL, feats, order, uma, NULL, &tcx);
	if (rc != 0)
		return rc;

	rc = evt_tx_begin(tcx);
	if (rc != 0)
		goto err;

	rc = evt_root_alloc(tcx);
	if (rc != 0)
		D_GOTO(out, rc);

	*root_mmid_p = tcx->tc_root_mmid;
	*toh = evt_tcx2hdl(tcx); /* take refcount for open */
out:
	rc = evt_tx_end(tcx, rc);

err:
	evt_tcx_decref(tcx); /* -1 for tcx_create */
	return rc;
}

/**
 * Create a new tree inplace of \a root, return the open handle.
 * Please check API comment in evtree.h for the details.
 */
int
evt_create_inplace(uint64_t feats, unsigned int order, struct umem_attr *uma,
		   struct evt_root *root, daos_handle_t *toh)
{
	struct evt_context *tcx;
	int		    rc;

	if (!(feats & EVT_FEAT_SORT_SOFF)) {
		D_DEBUG(DB_TRACE, "Unknown feature bits "DF_X64"\n", feats);
		return -DER_INVAL;
	}

	if (order < EVT_ORDER_MIN || order > EVT_ORDER_MAX) {
		D_DEBUG(DB_TRACE, "Invalid tree order %d\n", order);
		return -DER_INVAL;
	}

	rc = evt_tcx_create(EVT_ROOT_NULL, root, feats, order, uma, NULL, &tcx);
	if (rc != 0)
		return rc;

	rc = evt_tx_begin(tcx);
	if (rc != 0)
		goto err;

	rc = evt_root_init(tcx);
	if (rc != 0)
		D_GOTO(out, rc);

	*toh = evt_tcx2hdl(tcx); /* take refcount for open */
out:
	rc = evt_tx_end(tcx, rc);
err:
	evt_tcx_decref(tcx); /* -1 for tcx_create */
	return rc;
}

/**
 * Destroy the tree associated with the open handle.
 * Please check API comment in evtree.h for the details.
 */
int
evt_destroy(daos_handle_t toh)
{
	struct evt_context *tcx;
	int		    rc;

	tcx = evt_hdl2tcx(toh);
	if (tcx == NULL)
		return -DER_NO_HDL;

	rc = evt_tx_begin(tcx);
	if (rc != 0)
		return rc;

	rc = evt_root_destroy(tcx);

	rc = evt_tx_end(tcx, rc);

	/* Close the tcx even if the destroy failed */
	evt_tcx_decref(tcx);

	return rc;
}

/* Special value to not only print MBRs but also bounds for leaf records */
#define EVT_DEBUG_LEAF (-2)
/* Number of spaces to add at each level in debug output */
#define EVT_DEBUG_INDENT (4)

/** Output tree node status */
static void
evt_node_debug(struct evt_context *tcx, uint64_t nd_off,
	       int cur_level, int debug_level)
{
	struct evt_node *nd;
	int		 i;
	bool		 leaf;

	nd = evt_off2node(tcx, nd_off);
	leaf = evt_node_is_leaf(tcx, nd_off);

	/* NB: debug_level < 0 means output debug info for all levels,
	 * otherwise only output debug info for the specified tree level.
	 */
	if (leaf || cur_level == debug_level || debug_level < 0) {
		struct evt_rect *rect;

		rect = evt_node_mbr_get(tcx, nd_off);
		D_PRINT("%*snode="DF_X64", lvl=%d, mbr="DF_RECT
			", rect_nr=%d\n", cur_level * EVT_DEBUG_INDENT, "",
			nd_off, cur_level, DP_RECT(rect), nd->tn_nr);

		if (leaf && debug_level == EVT_DEBUG_LEAF) {
			for (i = 0; i < nd->tn_nr; i++) {
				rect = evt_node_rect_at(tcx, nd_off, i);

				D_PRINT("%*s    rect[%d] = "DF_RECT"\n",
					cur_level * EVT_DEBUG_INDENT, "", i,
					DP_RECT(rect));
			}
		}

		if (leaf || cur_level == debug_level)
			return;
	}

	for (i = 0; i < nd->tn_nr; i++) {
		uint64_t	child_off;

		child_off = evt_node_child_at(tcx, nd_off, i);
		evt_node_debug(tcx, child_off, cur_level + 1, debug_level);
	}
}

/**
 * Output status of tree nodes at level \a debug_level.
 * All nodes will be printed out if \a debug_level is negative.
 */
int
evt_debug(daos_handle_t toh, int debug_level)
{
	struct evt_context *tcx;

	tcx = evt_hdl2tcx(toh);
	if (tcx == NULL)
		return -DER_NO_HDL;

	D_PRINT("Tree depth=%d, order=%d, feats="DF_X64"\n",
		tcx->tc_depth, tcx->tc_order, tcx->tc_feats);

	if (tcx->tc_root->tr_node != 0)
		evt_node_debug(tcx, tcx->tc_root->tr_node, 0, debug_level);

	return 0;
}


/**
 * Tree policies
 *
 * Only support SSOF for now (see below).
 */

/**
 * Sorted by Start Offset (SSOF)
 *
 * Extents are sorted by start offset first, then high to low epoch, then end
 * offset
 */

/** Rectangle comparison for sorting */
static int
evt_ssof_cmp_rect(struct evt_context *tcx, const struct evt_rect *rt1,
		  const struct evt_rect *rt2)
{
	return evt_rect_cmp(rt1, rt2);
}

static int
evt_ssof_insert(struct evt_context *tcx, uint64_t nd_off,
		uint64_t in_off, const struct evt_entry_in *ent)
{
	struct evt_node		*nd   = evt_off2node(tcx, nd_off);
	struct evt_node_entry	*ne = NULL;
	int			 i;
	int			 rc;
	bool			 leaf;

	D_ASSERT(!evt_node_is_full(tcx, nd_off));

	leaf = evt_node_is_leaf(tcx, nd_off);

	/* NB: can use binary search to optimize */
	for (i = 0; i < nd->tn_nr; i++) {
		int	nr;

		ne = evt_node_entry_at(tcx, nd_off, i);
		rc = evt_ssof_cmp_rect(tcx, &ne->ne_rect, &ent->ei_rect);
		if (rc < 0)
			continue;

		nr = nd->tn_nr - i;
		memmove(ne + 1, ne, nr * sizeof(*ne));
		break;
	}

	if (i == nd->tn_nr) { /* attach at the end */
		ne = evt_node_entry_at(tcx, nd_off, nd->tn_nr);
	}

	ne->ne_rect = ent->ei_rect;
	if (leaf) {
		TMMID(struct evt_desc)	 desc_mmid;
		struct evt_desc		*desc;

		desc_mmid = umem_zalloc_typed(evt_umm(tcx), struct evt_desc,
					     sizeof(struct evt_desc));
		if (TMMID_IS_NULL(desc_mmid))
			return -DER_NOMEM;
		ne->ne_child = desc_mmid.oid.off;
		desc = evt_tmmid2ptr(tcx, desc_mmid);
		desc->dc_magic = EVT_DESC_MAGIC;
		desc->dc_ex_addr = ent->ei_addr;
		desc->dc_csum = ent->ei_csum;
		desc->dc_ver = ent->ei_ver;
		uuid_copy(desc->dc_cookie, ent->ei_cookie);
	} else {
		ne->ne_child = in_off;
	}

	nd->tn_nr++;
	return 0;
}

static int
evt_ssof_split(struct evt_context *tcx, bool leaf,
	       uint64_t src_off, uint64_t dst_off)
{
	struct evt_node	   *nd_src = evt_off2node(tcx, src_off);
	struct evt_node	   *nd_dst = evt_off2node(tcx, dst_off);
	struct evt_node_entry	*entry_src;
	struct evt_node_entry	*entry_dst;
	int		    nr;

	D_ASSERT(nd_src->tn_nr == tcx->tc_order);
	nr = nd_src->tn_nr / 2;
	/* give one more entry to the left (original) node if tree order is
	 * odd, because "append" could be the most common use-case at here,
	 * which means new entres will never be inserted into the original
	 * node. So we want to utilize the original as much as possible.
	 */
	nr += (nd_src->tn_nr % 2 != 0);

	entry_src = evt_node_entry_at(tcx, src_off, nr);
	entry_dst = evt_node_entry_at(tcx, dst_off, 0);
	memcpy(entry_dst, entry_src, sizeof(*entry_dst) * (nd_src->tn_nr - nr));

	nd_dst->tn_nr = nd_src->tn_nr - nr;
	nd_src->tn_nr = nr;
	return 0;
}

static int
evt_ssof_rect_weight(struct evt_context *tcx, const struct evt_rect *rect,
		     struct evt_weight *weight)
{
	memset(weight, 0, sizeof(*weight));
	weight->wt_major = rect->rc_ex.ex_hi - rect->rc_ex.ex_lo;
	/* NB: we don't consider about high epoch for SSOF because it's based
	 * on assumption there is no overwrite.
	 */
	weight->wt_minor = -rect->rc_epc;
	return 0;
}

static void
evt_ssof_adjust(struct evt_context *tcx, uint64_t nd_off,
		struct evt_node_entry *ne, int at)
{
	struct evt_node_entry	*etmp;
	struct evt_node		*nd = evt_off2node(tcx, nd_off);
	struct evt_node_entry	*dst_entry;
	struct evt_node_entry	*src_entry;
	struct evt_node_entry	 cached_entry;
	int			 count;
	int			 i;

	D_ASSERT(!evt_node_is_leaf(tcx, nd_off));

	/* Check if we need to move the entry left */
	for (i = at - 1, etmp = ne - 1; i >= 0; i--, etmp--) {
		if (evt_ssof_cmp_rect(tcx, &etmp->ne_rect, &ne->ne_rect) <= 0)
			break;
	}

	i++;
	if (i != at) {
		/* The entry needs to move left */
		etmp++;
		dst_entry = etmp + 1;
		src_entry = etmp;
		cached_entry = *ne;

		count = at - i;
		goto move;
	}

	/* Ok, now check if we need to move the entry right */
	for (i = at + 1, etmp = ne + 1; i < nd->tn_nr; i++, etmp++) {
		if (evt_ssof_cmp_rect(tcx, &etmp->ne_rect, &ne->ne_rect) >= 0)
			break;
	}

	i--;
	if (i != at) {
		/* the entry needs to move right */
		etmp--;
		count = i - at;
		dst_entry = ne;
		src_entry = dst_entry + 1;
		cached_entry = *ne;
		goto move;
	}

	return;
move:
	/* Execute the move */
	memmove(dst_entry, src_entry, sizeof(*dst_entry) * count);
	*etmp = cached_entry;
}

static struct evt_policy_ops evt_ssof_pol_ops = {
	.po_insert		= evt_ssof_insert,
	.po_adjust		= evt_ssof_adjust,
	.po_split		= evt_ssof_split,
	.po_rect_weight		= evt_ssof_rect_weight,
};

/* Delete the node pointed to by current trace */
static int
evt_node_delete(struct evt_context *tcx)
{
	struct evt_trace	*trace;
	struct evt_node		*node;
	struct evt_node_entry	*ne;
	uint64_t		 nm_cur;
	bool			 leaf;
	int			 level	= tcx->tc_depth - 1;
	int			 rc;

	/* We take a simple approach here which may be refined later.
	 * We simply remove the record, and if it's the last record, we
	 * bubble up removing any nodes that only have one record.
	 * Then we check the mbr at each level and make appropriate
	 * adjustments.
	 */
	while (1) {
		int			 count;

		trace = &tcx->tc_trace[level];
		nm_cur = trace->tr_node;
		leaf = evt_node_is_leaf(tcx, nm_cur);
		node = evt_off2node(tcx, nm_cur);

		ne = evt_node_entry_at(tcx, nm_cur, trace->tr_at);
		if (leaf) {
			/* Free the evt_desc */
			rc = umem_free(evt_umm(tcx),
				       evt_off2mmid(tcx, ne->ne_child));
			if (rc != 0)
				return rc;
			ne->ne_child = 0;
		}

		if (node->tn_nr == 1) {
			/* this node can be removed so bubble up */
			if (level == 0) {
				evt_root_deactivate(tcx);
				return 0;
			}

			rc = umem_free(evt_umm(tcx), evt_off2mmid(tcx, nm_cur));
			if (rc != 0)
				return rc;
			level--;
			continue;
		}

		if (!trace->tr_tx_added) {
			rc = evt_node_tx_add(tcx, nm_cur);
			if (rc != 0)
				return rc;
			trace->tr_tx_added = true;
		}

		/* Ok, remove the rect at the current trace */
		count = node->tn_nr - trace->tr_at - 1;
		node->tn_nr--;

		if (count == 0)
			break;

		memmove(ne, ne + 1, sizeof(*ne) * count);

		break;
	};

	/* Update MBR and bubble up */
	while (1) {
		struct evt_rect	mbr;
		int		i;

		ne -= trace->tr_at;
		mbr = ne->ne_rect;
		ne++;
		for (i = 1; i < node->tn_nr; i++, ne++)
			evt_rect_merge(&mbr, &ne->ne_rect);

		if (evt_rect_same_extent(&node->tn_mbr, &mbr) &&
		    node->tn_mbr.rc_epc == mbr.rc_epc)
			return 0; /* mbr hasn't changed */

		node->tn_mbr = mbr;

		if (level == 0)
			return 0;

		level--;

		trace = &tcx->tc_trace[level];
		nm_cur = trace->tr_node;
		node = evt_off2node(tcx, nm_cur);

		ne = evt_node_entry_at(tcx, nm_cur, trace->tr_at);
		ne->ne_rect = mbr;

		/* make adjustments to the position of the rectangle */
		if (!tcx->tc_ops->po_adjust)
			continue;

		if (!trace->tr_tx_added) {
			rc = evt_node_tx_add(tcx, nm_cur);
			if (rc != 0)
				return rc;
			trace->tr_tx_added = true;
		}

		tcx->tc_ops->po_adjust(tcx, nm_cur, ne, trace->tr_at);
	}

	return 0;
}

int evt_delete(daos_handle_t toh, const struct evt_rect *rect,
	       struct evt_entry *ent)
{
	struct evt_context	*tcx;
	struct evt_entry_array	 ent_array;
	int			 rc;

	tcx = evt_hdl2tcx(toh);
	if (tcx == NULL)
		return -DER_NO_HDL;

	/* NB: This function presently only supports exact match on extent. */
	evt_ent_array_init(&ent_array);

	rc = evt_ent_array_fill(tcx, EVT_FIND_SAME, NULL, rect, &ent_array);
	if (rc != 0)
		return rc;

	if (ent_array.ea_ent_nr == 0)
		return -DER_ENOENT;

	D_ASSERT(ent_array.ea_ent_nr == 1);
	if (ent != NULL)
		*ent = *evt_ent_array_get(&ent_array, 0);

	rc = evt_tx_begin(tcx);
	if (rc != 0)
		return rc;

	rc = evt_node_delete(tcx);

	/* No need for evt_ent_array_fill as there will be no allocations
	 * with 1 entry in the list
	 */
	return evt_tx_end(tcx, rc);
}
