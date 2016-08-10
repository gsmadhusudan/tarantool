/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "vinyl_index.h"

#include <stdio.h>
#include <inttypes.h>
#include <bit/bit.h> /* load/store */

#include "trivia/util.h"
#include "cfg.h"
#include "say.h"
#include "scoped_guard.h"

#include "vinyl_engine.h"
#include "vinyl_space.h"
#include "tuple.h"
#include "tuple_update.h"
#include "schema.h"
#include "space.h"
#include "txn.h"
#include "vinyl.h"

/**
 * Allocate a new key_def with a set union of key parts from
 * first and second key defs. Parts of the new key_def consist
 * of first key_def's parts and those parts of the second key_def
 * that were not among the first parts.
 *
 * @throws OutOfMemory
 */
static struct key_def *
key_defs_merge(struct key_def *first, struct key_def *second)
{
	int new_part_count = first->part_count + second->part_count;
	struct key_part *sec_parts = second->parts;
	/*
	 * Find and remove part duplicates, i.e. parts counted
	 * twice since they are present in both key defs.
	 */
	for (struct key_part *iter = sec_parts,
	     *end = sec_parts + second->part_count; iter != end; ++iter)
		if (key_def_contains_fieldno(first, iter->fieldno)) {
			--new_part_count;
		}

	struct key_def *new_def;
	new_def =  key_def_new(first->space_id, first->iid, first->name,
			      first->type, &first->opts, new_part_count);

	/* Write position in the new key def. */
	uint32_t pos = 0;
	/* Append first key def's parts to the new key_def. */
	for (struct key_part *iter = first->parts,
	     *end = first->parts + first->part_count; iter != end; ++iter) {

		key_def_set_part(new_def, pos++, iter->fieldno, iter->type);
	}
	/* Set-append second key def's part to the new key def. */
	for (struct key_part *iter = sec_parts,
	     *end = sec_parts + second->part_count; iter != end; ++iter) {

		if (key_def_contains_fieldno(first, iter->fieldno)) {
			continue;
		}
		key_def_set_part(new_def, pos++, iter->fieldno, iter->type);
	}
	return new_def;
}

struct vinyl_iterator {
	struct iterator base;
	/* key and part_count used only for EQ */
	const char *key;
	int part_count;
	const VinylIndex *index;
	struct key_def *key_def;
	struct vy_cursor *cursor;
};

VinylIndex::VinylIndex(struct key_def *key_def_arg)
	: Index(key_def_arg), db(NULL),
	  space(space_cache_find(key_def->space_id))
{
	VinylEngine *engine = (VinylEngine *)space->handler->engine;
	env = engine->env;
}

struct tuple*
VinylIndex::findByKey(const char *key, uint32_t part_count) const
{
	assert(key_def->opts.is_unique && part_count == key_def->part_count);
	/*
	 * engine_tx might be empty, even if we are in txn context.
	 * This can happen on a first-read statement.
	 */
	struct vy_tx *transaction = in_txn() ?
		(struct vy_tx *) in_txn()->engine_tx : NULL;
	struct tuple *tuple = NULL;
	if (vy_get(transaction, db, key, part_count, &tuple) != 0)
		diag_raise();
	return tuple;
}


struct tuple *
VinylIndex::replace(struct tuple*, struct tuple*, enum dup_replace_mode)
{
	/* This method is unused by vinyl index.
	 *
	 * see: vinyl_space.cc
	 */
	unreachable();
	return NULL;
}

size_t
VinylIndex::bsize() const
{
	return vy_index_bsize(db);
}

struct tuple *
VinylIndex::min(const char *key, uint32_t part_count) const
{
	struct iterator *it = allocIterator();
	auto guard = make_scoped_guard([=]{it->free(it);});
	initIterator(it, ITER_GE, key, part_count);
	return it->next(it);
}

struct tuple *
VinylIndex::max(const char *key, uint32_t part_count) const
{
	struct iterator *it = allocIterator();
	auto guard = make_scoped_guard([=]{it->free(it);});
	initIterator(it, ITER_LE, key, part_count);
	return it->next(it);
}

size_t
VinylIndex::count(enum iterator_type type, const char *key,
		  uint32_t part_count) const
{
	struct iterator *it = allocIterator();
	auto guard = make_scoped_guard([=]{it->free(it);});
	initIterator(it, type, key, part_count);
	size_t count = 0;
	struct tuple *tuple = NULL;
	while ((tuple = it->next(it)) != NULL)
		++count;
	return count;
}

VinylPrimaryIndex::VinylPrimaryIndex(struct key_def *key_def)
	: VinylIndex(key_def) { }

void
VinylPrimaryIndex::open()
{
	assert(db == NULL);
	/* Create vinyl database. */
	db = vy_index_new(env, key_def, tuple_format_default);
	if ((db == NULL) || vy_index_open(db))
		diag_raise();
}

VinylSecondaryIndex::VinylSecondaryIndex(struct key_def *key_def_arg)
	: VinylIndex(key_def_arg), secondary_key_def(NULL),
	  secondary_to_primary_key_def(NULL)
{ }

/**
 * Get tuple from the primary index by the partial tuple from secondary index.
 */
static struct tuple *
index_lookup_full_tuple(const VinylSecondaryIndex *index, struct tuple *tuple)
{
	assert(index->key_def->iid != 0);
	const char *primary_key;
	/**
	 * Use the primary key extractor for fetching
	 * primary key from merged primary and secondary keys.
	 */
	struct key_def *extractor = index->secondary_to_primary_key_def;
	primary_key = tuple_extract_key(tuple, extractor, NULL);
	/**
	 * Use the primary index for getting full tuple.
	 */
	const Index *primary = index_find(index->space, 0);
	mp_decode_array(&primary_key); /* Skip array header. */
	tuple = primary->findByKey(primary_key,
				   extractor->part_count);
	return tuple;
}

struct tuple*
VinylSecondaryIndex::findByKey(const char *key, uint32_t part_count) const
{
	struct tuple *tuple = VinylIndex::findByKey(key, part_count);
	if (tuple) {
		/**
		 * Secondary index stores partial tuples so findByKey
		 * returns not full tuple. For getting full tuple
		 * need to find it in primary index by primary key from
		 * this partial tuple.
		 */
		return index_lookup_full_tuple(this, tuple);
	}
	return NULL;
}

void
VinylSecondaryIndex::open()
{
	assert(db == NULL);
	/**
	 * vinyl_key_def is key_def for using inside vinyl.
	 *
	 * It is merged key_def of this index and key_def of primary index,
	 * in which parts field numbers are condensed.
	 * For instance:
	 * - merged primary and secondary: 3 (str), 6 (uint), 4 (scalar)
	 * - vinyl_key_def: 0 (str), 1 (uint), 2 (scalar)
	 *
	 * Condensing is neccessary because partial tuple consists only
	 * from primary key and this secondary key fields in a row.
	 */
	struct key_def *vinyl_key_def = NULL;
	auto guard = make_scoped_guard([&]{
		if (vinyl_key_def)
			key_def_delete(vinyl_key_def);
        });

	Index *primary = index_find(space, 0);
	/* Allocate a new (temporary) key_def for vinyl. */
	vinyl_key_def = key_defs_merge(key_def, primary->key_def);
	/* Remember this merged key_def. */
	secondary_key_def = key_def_dup(vinyl_key_def);

	/* Create primary extractor key_def. */
	struct key_part *iter = vinyl_key_def->parts;
	for (uint32_t i = 0; i < vinyl_key_def->part_count; ++i, ++iter) {
		key_def_set_part(vinyl_key_def, i, i, iter->type);
	}

	secondary_to_primary_key_def =
		key_def_build_extractor(primary->key_def, secondary_key_def);

	/* Create vinyl database. */
	db = vy_index_new(env, vinyl_key_def, tuple_format_default);
	if ((db == NULL) || vy_index_open(db)) {
		key_def_delete(secondary_key_def);
		key_def_delete(secondary_to_primary_key_def);
		diag_raise();
	}
}

VinylSecondaryIndex::~VinylSecondaryIndex() {
	key_def_delete(secondary_key_def);
	key_def_delete(secondary_to_primary_key_def);
}

struct tuple *
VinylIndex::iterator_next(struct iterator *iter) const
{
	struct vinyl_iterator *it = (struct vinyl_iterator *) iter;
	assert(it->cursor != NULL);

	uint32_t it_sc_version = ::sc_version;

	struct tuple *tuple;
	if (vy_cursor_next(it->cursor, &tuple) != 0)
		diag_raise();
	if (tuple == NULL) { /* not found */
		/* immediately close the cursor */
		vy_cursor_delete(it->cursor);
		it->cursor = NULL;
		iter->next = NULL;
		return NULL;
	}
	/* found */
	if (it_sc_version != ::sc_version)
		return NULL;
	return tuple;
}

struct tuple *
VinylIndex::iterator_eq(struct iterator *iter) const
{
	struct vinyl_iterator *it = (struct vinyl_iterator *) iter;
	assert(it->cursor != NULL);

	uint32_t it_sc_version = ::sc_version;
	struct tuple *tuple;
	if (vy_cursor_next(it->cursor, &tuple) != 0)
		diag_raise();
	if (tuple == NULL || it_sc_version != ::sc_version) {
		goto not_found;
	}

	/* check equality */
	if (tuple_compare_with_key(tuple, it->key, it->part_count,
				it->key_def) != 0) {
		goto not_found;
	}
	return tuple;
not_found:
	/* immediately close the cursor */
	vy_cursor_delete(it->cursor);
	it->cursor = NULL;
	iter->next = NULL;
	return NULL;
}

const struct key_def *
VinylIndex::get_key_extractor() const
{
	return key_def;
}

struct tuple *
VinylSecondaryIndex::iterator_next(struct iterator *iter) const
{
	struct tuple *tuple = VinylIndex::iterator_next(iter);
	if (tuple)
		return index_lookup_full_tuple(this, tuple);
	return NULL;
}

struct tuple *
VinylSecondaryIndex::iterator_eq(struct iterator *iter) const
{
	struct tuple *tuple = VinylIndex::iterator_eq(iter);
	if (tuple)
		return index_lookup_full_tuple(this, tuple);
	return NULL;
}

const struct key_def *
VinylSecondaryIndex::get_key_extractor() const
{
	return secondary_key_def;
}

void
vinyl_iterator_free(struct iterator *ptr)
{
	assert(ptr->free == vinyl_iterator_free);
	struct vinyl_iterator *it = (struct vinyl_iterator *) ptr;
	if (it->cursor) {
		vy_cursor_delete(it->cursor);
		it->cursor = NULL;
	}
	free(ptr);
}

struct tuple *
vinyl_iterator_last(struct iterator *ptr __attribute__((unused)))
{
	return NULL;
}

struct tuple *
vinyl_iterator_next(struct iterator *ptr)
{
	struct vinyl_iterator *it = (struct vinyl_iterator *) ptr;
	return it->index->iterator_next(ptr);
}

static struct tuple *
vinyl_iterator_eq(struct iterator *ptr)
{
	struct vinyl_iterator *it = (struct vinyl_iterator *) ptr;
	return it->index->iterator_eq(ptr);
}

static struct tuple *
vinyl_iterator_exact(struct iterator *ptr)
{
	struct vinyl_iterator *it = (struct vinyl_iterator *) ptr;
	ptr->next = vinyl_iterator_last;
	return it->index->findByKey(it->key, it->part_count);
}

struct iterator*
VinylIndex::allocIterator() const
{
	struct vinyl_iterator *it =
	        (struct vinyl_iterator *) calloc(1, sizeof(*it));
	if (it == NULL) {
	        tnt_raise(OutOfMemory, sizeof(struct vinyl_iterator),
	                  "calloc", "vinyl_iterator");
	}
	it->base.next = vinyl_iterator_last;
	it->base.free = vinyl_iterator_free;
	return (struct iterator *) it;

}

void
VinylIndex::initIterator(struct iterator *ptr,
                         enum iterator_type type,
                         const char *key, uint32_t part_count) const
{
	assert(part_count == 0 || key != NULL);
	struct vinyl_iterator *it = (struct vinyl_iterator *) ptr;
	assert(it->cursor == NULL);
	it->index = this;
	it->key_def = vy_index_key_def(db);
	it->key = key;
	it->part_count = part_count;

	enum vy_order order;
	switch (type) {
	case ITER_ALL:
	case ITER_GE:
		order = VINYL_GE;
		ptr->next = vinyl_iterator_next;
		break;
	case ITER_GT:
		order = part_count > 0 ? VINYL_GT : VINYL_GE;
		ptr->next = vinyl_iterator_next;
		break;
	case ITER_LE:
		order = VINYL_LE;
		ptr->next = vinyl_iterator_next;
		break;
	case ITER_LT:
		order = part_count > 0 ? VINYL_LT : VINYL_LE;
		ptr->next = vinyl_iterator_next;
		break;
	case ITER_EQ:
		/* point-lookup iterator (optimization) */
		if ((key_def->opts.is_unique) &&
		    (part_count == key_def->part_count)) {
			ptr->next = vinyl_iterator_exact;
			return;
		}
		order = VINYL_GE;
		ptr->next = vinyl_iterator_eq;
		break;
	case ITER_REQ:
		/* point-lookup iterator (optimization) */
		if ((key_def->opts.is_unique) &&
		    (part_count == key_def->part_count)) {
			ptr->next = vinyl_iterator_exact;
			return;
		}
		order = VINYL_LE;
		ptr->next = vinyl_iterator_eq;
		break;
	default:
		return Index::initIterator(ptr, type, key, part_count);
	}
	it->cursor = vy_cursor_new(db, key, part_count, order);
	if (it->cursor == NULL)
		diag_raise();
}
