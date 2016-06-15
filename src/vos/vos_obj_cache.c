/**
 * (C) Copyright 2016 Intel Corporation.
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
/**
 * Object cache for VOS OI table.
 * Object index is in Persistent memory. This cache in DRAM
 * maintains an LRU which is accessible in the I/O path. The object
 * index API defined for PMEM are used here by the cache..
 *
 * LRU cache implementation:
 * Simple LRU based object cache for Object index table
 * Uses a hashtable and a doubly linked list to set and get
 * entries. The size of both hashtable and linked list are
 * fixed length.
 *
 * Author: Vishwanath Venkatesan <vishwanath.venkatesan@intel.com>
 */

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <vos_obj.h>
#include <vos_internal.h>
#include <vos_hhash.h>
#include <daos_errno.h>

static struct vos_obj_ref *
vos_oref_create(struct vc_hdl *vco, daos_unit_oid_t oid)
{
	struct vos_obj_ref *oref;

	D_ALLOC_PTR(oref);
	if (!oref)
		return NULL;

	oref->or_co  = vco;
	oref->or_oid = oid;
	oref->or_lrefcnt = 0;
	return oref;
}

static void
vos_oref_free(struct vos_obj_ref *oref)
{
	if (oref->or_co)
		vos_co_putref_handle(oref->or_co);

	D_FREE_PTR(oref);
}

/**
 * Compare daos_unit_oid_t keys
 */
static inline bool
vlru_compare_keys(daos_unit_oid_t *oid_1,
		  daos_unit_oid_t *oid_2)
{
	return !memcmp(oid_1, oid_2,
		       sizeof(daos_unit_oid_t));
}

static inline int32_t
oid2bucket(daos_unit_oid_t *oid, uint64_t nbuckets)
{
	uint64_t hash_value;

	hash_value = vos_generate_crc64((void *)oid,
					sizeof(daos_unit_oid_t));
	return vos_generate_jch(hash_value, nbuckets);
}

static inline struct vos_obj_ref*
vlru_tail_of(daos_list_t *head)
{
	return daos_list_entry(head->prev,
			       struct vos_obj_ref,
			       or_llink);
}

static inline void
vlru_add_idle_list(struct vos_obj_ref *oref, daos_list_t *head,
		   uint32_t *refs_held, uint32_t *cache_filled)
{
	daos_list_add(&oref->or_llink, head);
	/* Decrement busy counter  and increase idle counter */
	(*refs_held)--;
	(*cache_filled)++;
}

/**
 * unlink from idle list (refcount == 0)
 * and reduce cache occupancy
 */
static inline void
vlru_idle_ref_unlink(struct vos_obj_ref *ref,
		     struct vlru_list *lru_list)
{
	if (!ref->or_lrefcnt) {
		daos_list_del(&ref->or_llink);
		lru_list->vll_refs_held++;
		lru_list->vll_cache_filled--;
	}
}

static void
vlru_ref_evict(struct vlru_list *lru_list, struct vos_obj_ref *obj_ref)
{
	D_DEBUG(DF_VOS3, "Woh! Cache is full evicting!\n");
	D_ASSERT(!obj_ref->or_lrefcnt);

	daos_list_del(&obj_ref->or_hlink);
	daos_list_del(&obj_ref->or_llink);
	lru_list->vll_cache_filled--;

	vos_oref_free(obj_ref);
}

/**
 * LRU create creates the hashtable and the
 * list (together act as a cache) for maintaining
 * and locating object reference
 * entries
 */
static int
vlru_create(int num_buckets, int cache_size, struct vlru_htable *htable,
	    struct vlru_list *list)
{
	int	i;

	if (num_buckets > LRU_HASH_MAX_BUCKETS) {
		D_ERROR("Buckets count shall not be more than: %d\n",
			LRU_HASH_MAX_BUCKETS);
		return -DER_INVAL;
	}

	if (cache_size > LRU_CACHE_MAX_SIZE) {
		D_ERROR("Cache size shall not be more than: %d\n",
			LRU_CACHE_MAX_SIZE);
		return -DER_INVAL;
	}

	D_ALLOC(htable->vlh_buckets,
		num_buckets * sizeof(htable->vlh_buckets[0]));

	if (!htable->vlh_buckets) {
		D_ERROR("Error in Allocating buckets\n");
		return -DER_NOMEM;
	}

	htable->vlh_nbuckets = num_buckets;
	for (i = 0; i < num_buckets; i++)
		DAOS_INIT_LIST_HEAD(&htable->vlh_buckets[i]);

	DAOS_INIT_LIST_HEAD(&list->vll_list_head);
	list->vll_cache_size = cache_size;
	list->vll_cache_filled = 0;
	list->vll_refs_held = 0;

	return 0;
}

static struct vos_obj_ref*
vlru_find_item(struct vlru_htable *lru_table, daos_unit_oid_t oid)
{

	struct vos_obj_ref	*litem;
	int32_t			bucket_id;

	bucket_id = oid2bucket(&oid, lru_table->vlh_nbuckets);
	daos_list_for_each_entry(litem,
				 &lru_table->vlh_buckets[bucket_id],
				 or_hlink) {
		if (vlru_compare_keys(&oid, &litem->or_oid))
			return litem;
	}

	return NULL;
}

int
vlru_create_ref_hdl(daos_handle_t coh, daos_unit_oid_t oid,
		    struct vos_obj_ref **ref)
{
	struct vos_obj_ref	*oref = NULL;
	struct vc_hdl		*co_hdl = NULL;
	int			 rc;

	co_hdl = vos_co_lookup_handle(coh);
	if (!co_hdl) {
		D_ERROR("Invalid handle for container\n");
		return -DER_INVAL;
	}

	oref = vos_oref_create(co_hdl, oid);
	if (!oref)
		D_GOTO(failed, rc = -DER_NOMEM);

	/**
	 * TODO: Add Btree iterator handle
	 */
	*ref = oref;
	return 0;
 failed:
	vos_co_putref_handle(co_hdl);
	return rc;
}

/**
 * Implementation function for vos_obj_ref_hold
 */
static int
vlru_ref_hold(struct vlru_htable *lru_table, struct vlru_list *lru_list,
	      daos_handle_t coh, daos_unit_oid_t oid, struct vos_obj_ref **oref)
{

	int			ret  = 0;
	int32_t			bucket_id;
	struct vos_obj_ref	*l_oref = NULL;
	daos_list_t		*head = NULL;

	/**
	 *
	 * General algorithm:
	 * If oid is not in cache, bring it from PMEM.
	 * -- If oi not in PMEM (throw an error)
	 * -- PMEM oi lookup
	 * ---- Bring reference to cache (in hash-alone)
	 * -- Add reference in hash
	*/

	/**
	 * Lets check the head of the cache (idle list).
	 * if this oid has been accessed in the past.
	 */
	if (lru_list->vll_cache_filled) {
		head = &(lru_list->vll_list_head);
		l_oref = daos_list_entry(head->next,
					 struct vos_obj_ref,
					 or_llink);
		if (vlru_compare_keys(&oid, &l_oref->or_oid))
			goto ulink;
	}

	/**
	 * Not the head?
	 * its faster to search with hash table than the list
	 * Finding if reference in hash-table
	 */
	l_oref = vlru_find_item(lru_table, oid);
	if (l_oref) {
ulink:
		/**
		 * This reference is about to get busy again.
		 * Lets unlink from idle list and make it busy
		 * again by incrementing held count.
		 * (if it exists, i.e refcount == 0)
		 * Its still accessible from hash-table
		 * (hash-table acts as a busy-list also).
		 */
		vlru_idle_ref_unlink(l_oref, lru_list);
	} else {
		/* Not found */
		ret  = vlru_create_ref_hdl(coh, oid, &l_oref);
		if (ret) {
			D_ERROR("Error allocing ref handle\n");
			return ret;
		}

		ret = vos_oi_lookup(coh, oid, &l_oref->or_obj);
		if (ret) {
			D_ERROR("Error looking up container handle\n");
			goto exit;
		}

		bucket_id = oid2bucket(&l_oref->or_oid,
				       lru_table->vlh_nbuckets);
		daos_list_add(&l_oref->or_hlink,
			      &lru_table->vlh_buckets[bucket_id]);
		/* Busy oref count */
		lru_list->vll_refs_held++;
	}
	l_oref->or_lrefcnt++;
	*oref = l_oref;

exit:
	if (ret && l_oref)
		vos_oref_free(l_oref);

	return ret;
}


int
vos_obj_cache_create(int32_t cache_size,
		     struct vos_obj_cache **occ)
{

	int			ret = 0;
	struct vos_obj_cache	*l_occ = NULL;

	D_ALLOC_PTR(l_occ);
	if (l_occ == NULL) {
		D_ERROR("Allocating object cache pointer\n");
		return -DER_NOMEM;
	}
	ret = vlru_create(LRU_HASH_MAX_BUCKETS, cache_size,
			  &l_occ->voc_lhtable_ref,
			  &l_occ->voc_llist_ref);
	if (ret) {
		D_ERROR("Creating an Object cache failed\n");
		if (l_occ != NULL)
			D_FREE_PTR(l_occ);
		goto exit;
	}
	*occ = l_occ;
exit:
	return ret;
}

void
vos_obj_cache_destroy(struct vos_obj_cache *occ)
{

	struct vos_obj_ref *ref = NULL;

	if (occ == NULL) {
		D_ERROR("Empty cache. Nothing to destroy\n");
		return;
	}

	/* Cannot destroy if there are busy references */
	D_ASSERT(occ->voc_llist_ref.vll_refs_held == 0);

	/**
	 * Deleting all the link and object references in cache
	 */
	while (!daos_list_empty(&(occ->voc_llist_ref.vll_list_head))) {
		ref = daos_list_entry(occ->voc_llist_ref.vll_list_head.next,
				      struct vos_obj_ref,
				      or_llink);
		daos_list_del(&ref->or_llink);
		vos_oref_free(ref);
	}

	if (occ->voc_lhtable_ref.vlh_buckets)
		D_FREE(occ->voc_lhtable_ref.vlh_buckets,
		       occ->voc_lhtable_ref.vlh_nbuckets *
		       sizeof(occ->voc_lhtable_ref.vlh_buckets[0]));

	D_FREE_PTR(occ);
}

/**
 * Return object cache for the current thread.
 */
struct vos_obj_cache *vos_obj_cache_current(void)
{
	return vos_get_obj_cache();
}

void
vos_obj_ref_release(struct vos_obj_cache *occ, struct vos_obj_ref *oref)
{

	daos_list_t		*head = NULL;
	uint32_t		avail_size = 0;
	struct vlru_list	*llist = NULL;


	D_ASSERT((occ != NULL) && (oref != NULL));

	/* Convenience Definition */
	llist = &occ->voc_llist_ref;

	/* Number of available positions in the list */
	avail_size = llist->vll_cache_size - llist->vll_cache_filled;

	D_ASSERT(oref->or_lrefcnt > 0);
	oref->or_lrefcnt--;
	/**
	 * Move it to LRU cache list
	 * If refcount==0
	 */
	if (!oref->or_lrefcnt) {
		head = &llist->vll_list_head;
		vlru_add_idle_list(oref, head,
				   &llist->vll_refs_held,
				   &llist->vll_cache_filled);
		avail_size--;
	}
	/**
	 * If the cache is full let us remove
	 * Remove from the last item in cache
	 * Check also if number of busy references is
	 * greater than the available space in
	 * list. If so, free up space in the cache.
	 * to fit all busy references.
	 */
	if (llist->vll_cache_filled &&
	   (!avail_size || llist->vll_refs_held > avail_size)) {
		while (llist->vll_refs_held + llist->vll_cache_filled >=
		       llist->vll_cache_size) {
			head = &llist->vll_list_head;
			vlru_ref_evict(llist, vlru_tail_of(head));
		}
	}
}

int
vos_obj_ref_hold(struct vos_obj_cache *occ, daos_handle_t coh,
		 daos_unit_oid_t oid, struct vos_obj_ref **oref_p)
{

	int	ret = 0;

	D_ASSERT(occ != NULL);
	ret = vlru_ref_hold(&occ->voc_lhtable_ref,
			    &occ->voc_llist_ref,
			    coh, oid, oref_p);
	return ret;
}