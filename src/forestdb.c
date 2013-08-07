/*
 * Copyright 2013 Jung-Sang Ahn <jungsang.ahn@gmail.com>.
 * All Rights Reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filemgr.h"
#include "hbtrie.h"
#include "btree.h"
#include "docio.h"
#include "btreeblock.h"
#include "forestdb.h"
#include "common.h"
#include "wal.h"
#include "filemgr_ops_linux.h"

#ifdef __DEBUG
#ifndef __DEBUG_FDB
	#undef DBG
	#undef DBGCMD
	#define DBG(args...)
	#define DBGCMD(command...)
#endif
#endif

INLINE size_t _fdb_readkey_wrap(void *handle, uint64_t offset, void *buf)
{
	keylen_t keylen;
	docio_read_doc_key((struct docio_handle *)handle, offset, &keylen, buf);
	return keylen;
}

fdb_status fdb_open(fdb_handle *handle, char *filename, fdb_config config)
{
    DBGCMD(
        struct timeval _a_,_b_,_rr_;
        gettimeofday(&_a_, NULL);
    );

    struct filemgr_config fconfig;
    bid_t trie_root_bid = BLK_NOT_FOUND;
	bid_t seq_root_bid;

#ifdef _MEMPOOL
    mempool_init();
#endif

	fconfig.blocksize = config.blocksize = FDB_BLOCKSIZE;
	fconfig.ncacheblock = config.buffercache_size / FDB_BLOCKSIZE;
	fconfig.flag = 0x0;
	handle->fileops = get_linux_filemgr_ops();
	handle->btreeblkops = btreeblk_get_ops();
	handle->file = filemgr_open(filename, handle->fileops, fconfig);
	handle->trie = (struct hbtrie *)malloc(sizeof(struct hbtrie));
	handle->bhandle = (struct btreeblk_handle *)malloc(sizeof(struct btreeblk_handle));
	handle->dhandle = (struct docio_handle *)malloc(sizeof(struct docio_handle));
	handle->config = config;
	handle->btree_fanout = fconfig.blocksize / (config.chunksize+config.offsetsize);

    //assert( CHK_POW2(config.wal_threshold) );
    //wal_init(handle->file, config.wal_threshold);
    wal_init(handle->file, FDB_WAL_NBUCKET);
    docio_init(handle->dhandle, handle->file);
    btreeblk_init(handle->bhandle, handle->file, handle->file->blocksize);

	if (handle->file->header.size > 0 && handle->file->header.data) {
		size_t offset = 0;
		seq_memcpy(&trie_root_bid, handle->file->header.data + offset, sizeof(trie_root_bid), offset);
		seq_memcpy(&seq_root_bid, handle->file->header.data + offset, sizeof(seq_root_bid), offset);
		seq_memcpy(&handle->ndocs, handle->file->header.data + offset, sizeof(handle->ndocs), offset);
		seq_memcpy(&handle->datasize, handle->file->header.data + offset, sizeof(handle->datasize), offset);
	}
	hbtrie_init(handle->trie, config.chunksize, config.offsetsize, handle->file->blocksize, trie_root_bid, 
		handle->bhandle, handle->btreeblkops, handle->dhandle, _fdb_readkey_wrap);

        handle->seqtree = (struct btree*)malloc(sizeof(struct btree));
        btree_init(handle->seqtree, handle->bhandle, handle->btreeblkops, kv_ops,
                   handle->trie->btree_nodesize, sizeof(fdb_seqnum_t),
                   handle->trie->valuelen, 0x0, NULL);
    }
#endif

    DBGCMD(
        gettimeofday(&_b_, NULL);
        _rr_ = _utime_gap(_a_,_b_);
    );
    DBG("fdb_open %s, %"_FSEC".%06"_FUSEC" sec elapsed.\n",
        filename, _rr_.tv_sec, _rr_.tv_usec);

    return FDB_RESULT_SUCCESS;
}

fdb_status fdb_doc_create(fdb_doc **doc, void *key, size_t keylen, void *meta, size_t metalen,
	void *body, size_t bodylen)
{
	*doc = (fdb_doc*)malloc(sizeof(fdb_doc));
	if (*doc == NULL) return FDB_RESULT_FAIL;

	if (key && keylen>0) {
		(*doc)->key = (void *)malloc(keylen);
		if ((*doc)->key == NULL) return FDB_RESULT_FAIL;
		memcpy((*doc)->key, key, keylen);
		(*doc)->keylen = keylen;
	}else{
		(*doc)->key = NULL;
		(*doc)->keylen = 0;
	}
	if (meta && metalen > 0) {
		(*doc)->meta = (void *)malloc(metalen);
		if ((*doc)->meta == NULL) return FDB_RESULT_FAIL;
		memcpy((*doc)->meta, meta, metalen);
		(*doc)->metalen = metalen;
	}else{
		(*doc)->meta = NULL;
		(*doc)->metalen = 0;
	}
	if (body && bodylen > 0) {
		(*doc)->body = (void *)malloc(bodylen);
		if ((*doc)->body == NULL) return FDB_RESULT_FAIL;
		memcpy((*doc)->body, body, bodylen);
		(*doc)->bodylen = bodylen;
	}else{
		(*doc)->body = NULL;
		(*doc)->bodylen = 0;
	}

	return FDB_RESULT_SUCCESS;
}

// doc MUST BE allocated by malloc
fdb_status fdb_doc_free(fdb_doc *doc)
{
	if (doc->key) free(doc->key);
	if (doc->meta) free(doc->meta);
	if (doc->body) free(doc->body);
	free(doc);
	return FDB_RESULT_SUCCESS;
}

uint8_t sandbox[65536];

INLINE void _fdb_wal_flush_func(void *voidhandle, struct wal_item *item)
{
	hbtrie_result hr;
	fdb_handle *handle = (fdb_handle *)voidhandle;
	uint64_t old_offset;

	if (item->action == WAL_ACT_INSERT) {
		hr = hbtrie_insert(handle->trie, item->key, item->keylen, &item->offset, &old_offset);
		if (hr == HBTRIE_RESULT_SUCCESS) {
			handle->ndocs++;
		}else{
			// update
			struct docio_object doc;
			doc.key = (void *)sandbox;
			doc.meta = (void *)sandbox;
			docio_read_doc_key_meta(handle->dhandle, old_offset, &doc);
			handle->datasize -= 
				(doc.length.keylen + doc.length.metalen + doc.length.bodylen + sizeof(struct docio_length));
		}
		handle->datasize += item->doc_size;
		btreeblk_end(handle->bhandle);
	}else{
		//hbtrie_remove(handle->trie, key, keylen);
		//btreeblk_end(handle->bhandle);
	}
}

fdb_status fdb_get(fdb_handle *handle, fdb_doc *doc)
{
    uint64_t offset;
    wal_result wr;
    hbtrie_result hr;

    if (doc->key == NULL || doc->keylen == 0) {
        return FDB_RESULT_INVALID_ARGS;
    }

    wr = wal_find(handle->file, doc, &offset);
    if (wr == WAL_RESULT_FAIL) {
        hr = hbtrie_find(handle->trie, doc->key, doc->keylen, &offset);
        btreeblk_end(handle->bhandle);
    }

	if (wr != WAL_RESULT_FAIL || hr != HBTRIE_RESULT_FAIL) {
		_doc.key = doc->key;
		_doc.length.keylen = doc->keylen;
		_doc.meta = doc->meta;
		_doc.body = doc->body;
		docio_read_doc(handle->dhandle, offset, &_doc);

        if (_doc.length.keylen != doc->keylen) {
            return FDB_RESULT_FAIL;
        }

        doc->metalen = _doc.length.metalen;
        doc->bodylen = _doc.length.bodylen;
        doc->meta = _doc.meta;
        doc->body = _doc.body;

        return FDB_RESULT_SUCCESS;
    }

    return FDB_RESULT_FAIL;
}

fdb_status fdb_get_metaonly(fdb_handle *handle, fdb_doc *doc, uint64_t *body_offset)
{
	uint64_t offset;
	struct docio_object _doc;
	wal_result wr;
	hbtrie_result hr;

	if (doc->key == NULL || doc->keylen == 0) return FDB_RESULT_INVALID_ARGS;
	
	wr = wal_find(handle->file, doc, &offset);

	if (wr == WAL_RESULT_FAIL) {
		hr = hbtrie_find(handle->trie, doc->key, doc->keylen, &offset);
		btreeblk_end(handle->bhandle);
	}

	if (wr != WAL_RESULT_FAIL || hr != HBTRIE_RESULT_FAIL) {
		_doc.key = doc->key;
		_doc.length.keylen = doc->keylen;
		_doc.meta = _doc.body = NULL;
		*body_offset = docio_read_doc_key_meta(handle->dhandle, offset, &_doc);

		if (_doc.length.keylen != doc->keylen) return FDB_RESULT_FAIL;
		
		doc->metalen = _doc.length.metalen;
		doc->bodylen = _doc.length.bodylen;
		doc->meta = _doc.meta;
		doc->body = _doc.body;

		return FDB_RESULT_SUCCESS;
	}

	return FDB_RESULT_FAIL;
}

fdb_status fdb_set(fdb_handle *handle, fdb_doc *doc)
{
	uint64_t offset;
	struct docio_object _doc;
	
	if ( (doc->key == NULL) || (doc->keylen == 0) ||
		(doc->metalen > 0 && doc->meta == NULL) || 
		(doc->bodylen > 0 && doc->body == NULL)) return FDB_RESULT_INVALID_ARGS;
	
	_doc.length.keylen = doc->keylen;
	_doc.length.metalen = doc->metalen;
	_doc.length.bodylen = doc->bodylen;
	_doc.key = doc->key;
	_doc.meta = doc->meta;
	_doc.body = doc->body;

	if (_doc.body) {
		offset = docio_append_doc(handle->dhandle, &_doc);
		wal_insert(handle->file, doc, offset);
	}else{
		//remove
		wal_remove(handle->file, doc);
	}

	#ifdef __WAL_FLUSH_BEFORE_COMMIT
		if (wal_get_size(handle->file) > handle->config.wal_threshold) {
			wal_flush(handle->file, (void *)handle, _fdb_wal_flush_func);
		}
	#endif
	return FDB_RESULT_SUCCESS;
}

fdb_status fdb_commit(fdb_handle *handle)
{
	btreeblk_end(handle->bhandle);
	if (wal_get_size(handle->file) > handle->config.wal_threshold) {
		wal_flush(handle->file, handle, _fdb_wal_flush_func);
	}
	filemgr_commit(handle->file);
	return FDB_RESULT_SUCCESS;
}

void _fdb_set_file_header(fdb_handle *handle)
{
	uint8_t buf[256];
	size_t offset = 0;

	/*
	memcpy(buf + offset, &handle->trie->root_bid, sizeof(handle->trie->root_bid));
	memcpy(buf + sizeof(handle->trie->root_bid), &handle->trie->root_bid, sizeof(handle->trie->root_bid));
	*/
	seq_memcpy(buf + offset, &handle->trie->root_bid, sizeof(handle->trie->root_bid), offset);
	// this should be changed to the root bid of seq b-tree
	seq_memcpy(buf + offset, &handle->trie->root_bid, sizeof(handle->trie->root_bid), offset);
	seq_memcpy(buf + offset, &handle->ndocs, sizeof(handle->ndocs), offset);
	seq_memcpy(buf + offset, &handle->datasize, sizeof(handle->datasize), offset);
	
	filemgr_update_header(handle->file, buf, 32);
}

fdb_status fdb_compact(fdb_handle *handle, char *new_filename)
{
	struct filemgr *new_file;
	struct filemgr_config fconfig;
	struct btreeblk_handle *new_bhandle;
	struct docio_handle *new_dhandle;
	struct hbtrie *new_trie;
	struct hbtrie_iterator it;
	struct docio_object doc;
	uint8_t k[HBTRIE_MAX_KEYLEN];
	size_t keylen;
	uint64_t offset, new_offset;
	hbtrie_result hr;
	DBGCMD( uint64_t count = 0 );
	//uint8_t metabuf[1024], bodybuf[1024];

	btreeblk_end(handle->bhandle);
	wal_flush(handle->file, handle, _fdb_wal_flush_func);
	_fdb_set_file_header(handle);

	fconfig.blocksize = FDB_BLOCKSIZE;
	fconfig.ncacheblock = handle->config.buffercache_size / FDB_BLOCKSIZE;
	fconfig.flag = 0x0;

	// open new file
	new_file = filemgr_open(new_filename, handle->fileops, fconfig);

	// create new hb-trie and related handles
	new_bhandle = (struct btreeblk_handle *)malloc(sizeof(struct btreeblk_handle));
	new_dhandle = (struct docio_handle *)malloc(sizeof(struct docio_handle));
	new_trie = (struct hbtrie *)malloc(sizeof(struct hbtrie));

	wal_init(new_file, handle->config.wal_threshold);
	docio_init(new_dhandle, new_file);
	btreeblk_init(new_bhandle, new_file, new_file->blocksize);
	hbtrie_init(new_trie, handle->trie->chunksize, handle->trie->valuelen, new_file->blocksize,
		BLK_NOT_FOUND, new_bhandle, handle->btreeblkops, new_dhandle, _fdb_readkey_wrap);

	// scan all live documents in the trie
	hr = hbtrie_iterator_init(handle->trie, &it, NULL, 0);

	while(hr != HBTRIE_RESULT_FAIL) {
		hr = hbtrie_next(&it, k, &keylen, &offset);
		btreeblk_end(handle->bhandle);
		if (hr == HBTRIE_RESULT_FAIL) break;

		doc.key = k;
		doc.length.keylen = keylen;
		doc.meta = sandbox;
		doc.body = NULL;
		docio_read_doc(handle->dhandle, offset, &doc);

		// re-write to new file
		new_offset = docio_append_doc(new_dhandle, &doc);
		//free(doc.meta);
		free(doc.body);
		hbtrie_insert(new_trie, k, keylen, &new_offset, NULL);
		btreeblk_end(new_bhandle);

		DBGCMD( count++ );
	}

	hr = hbtrie_iterator_free(&it);

	//filemgr_commit(new_file);
	filemgr_remove_from_cache(handle->file);
	
	filemgr_close(handle->file);
	handle->file = new_file;
	
	free(handle->bhandle);
	handle->bhandle = new_bhandle;

	free(handle->dhandle);
	handle->dhandle = new_dhandle;

	free(handle->trie);
	handle->trie = new_trie;
	
	return FDB_RESULT_SUCCESS;
}

fdb_status fdb_close(fdb_handle *handle)
{
	btreeblk_end(handle->bhandle);
	wal_flush(handle->file, handle, _fdb_wal_flush_func);
	filemgr_commit(handle->file);
	_fdb_set_file_header(handle);
	filemgr_close(handle->file);
	docio_free(handle->dhandle);
	free(handle->trie);
	#ifdef __FDB_SEQTREE
	if (handle->config.seqtree == FDB_SEQTREE_USE) {
		free(handle->seqtree->kv_ops);
		free(handle->seqtree);
	}
	#endif
	free(handle->bhandle);
	free(handle->dhandle);
	return FDB_RESULT_SUCCESS;
}

