#ifndef TARANTOOL_BOX_VINYL_INDEX_H_INCLUDED
#define TARANTOOL_BOX_VINYL_INDEX_H_INCLUDED
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

#include "index.h"

/**
 * Base class that provides API to vinyl primary and secondary indexes.
 *
 * Vinyl primary index differs from secondary in methods of data
 * storing and searching.
 *
 * - Primary index stores full tuples - this is tuples that user inserts
 *   in space by space:insert({...}). Such indexes are calling covering.
 *
 * - Secondary index stores not full tuples but only parts that represent its
 *   key merged with primary key (see key_defs_merge function). This approach
 *   allows to reduce disk space needed for index storing. Such indexes are
 *   calling not covering.
 *
 * Primary index already stores full tuples that can be returned to user.
 * But secondary index doesn't storing full tuple and for getting them 
 * need to fetch primary key from partial tuple and by this key find full tuple
 * in primary index.
 */
class VinylIndex: public Index {
public:
	VinylIndex(struct key_def *key_def);

	virtual struct tuple*
	replace(struct tuple*,
	        struct tuple*, enum dup_replace_mode) override;

	virtual struct tuple*
	findByKey(const char *key, uint32_t) const override;

	virtual struct iterator*
	allocIterator() const override;

	virtual void
	initIterator(struct iterator *iterator,
	             enum iterator_type type,
	             const char *key, uint32_t part_count) const override;

	virtual size_t
	bsize() const override;

	virtual struct tuple *
	min(const char *key, uint32_t part_count) const override;

	virtual struct tuple *
	max(const char *key, uint32_t part_count) const override;

	virtual size_t
	count(enum iterator_type type, const char *key, uint32_t part_count)
		const override;

	virtual struct tuple *
	iterator_next(struct iterator *iter) const;

	virtual struct tuple *
	iterator_eq(struct iterator *iter) const;

	virtual const struct key_def *
	get_key_extractor() const;

	struct vy_env *env;
	struct vy_index *db;
	struct space *space;
};

class VinylPrimaryIndex: public VinylIndex {
public:
	VinylPrimaryIndex(struct key_def *key_def);

	virtual void
	open() override;
};

/**
 * While primary index has only one key_def that is used for validating
 * tuples, secondary index has three key_defs:
 *
 * - key_def (from class Index) - this is 'public' key_def that
 *   represents index format for external using.
 *
 * - secondary_key_def - this key_def is used for extracting merged secondary
 *   key and primary key from full tuple.
 *
 * - secondary_to_primary_key_def - this key_def is used for extracting
 *   primary key from partial tuple, that consists of merged secondary and
 *   primary keys.
 */
class VinylSecondaryIndex: public VinylIndex {
public:
	VinylSecondaryIndex(struct key_def *key_def);

	virtual struct tuple*
	findByKey(const char *key, uint32_t) const override;

	virtual void
	open() override;

	virtual struct tuple *
	iterator_next(struct iterator *iter) const override;

	virtual struct tuple *
	iterator_eq(struct iterator *iter) const override;

	virtual const struct key_def *
	get_key_extractor() const override;

	virtual ~VinylSecondaryIndex() override;

	struct key_def *secondary_key_def;
	struct key_def *secondary_to_primary_key_def;
};

#endif /* TARANTOOL_BOX_VINYL_INDEX_H_INCLUDED */
