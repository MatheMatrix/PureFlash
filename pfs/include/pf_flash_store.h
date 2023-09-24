/*
 * Copyright (C) 2016 Liu Lele(liu_lele@126.com)
 *
 * This code is licensed under the GPL.
 */
#ifndef flash_store_h__
#define flash_store_h__
#include <uuid/uuid.h>
#include <unordered_map>
#include <stdint.h>
#include <libaio.h>
#include <thread>

#include "pf_fixed_size_queue.h"
#include "basetype.h"
#include "pf_message.h"
#include "pf_conf.h"
#include "pf_mempool.h"
#include "pf_event_queue.h"
#include "pf_event_thread.h"
#include "pf_tray.h"
#include "pf_threadpool.h"
#include "pf_volume.h"
#include "pf_bitmap.h"
#include "pf_ioengine.h"
#include "pf_lmt.h"


class PfRedoLog;


struct scc_cow_context
{
	lmt_key *key;
	lmt_entry *srcEntry;
	lmt_entry *dstEntry;
	PfFlashStore *pfs;
	int rc;
};




class PfFlashStore : public PfEventThread
{
public:
	struct HeadPage {
		uint32_t magic;
		uint32_t version;
		unsigned char uuid[16];
		uint32_t key_size;
		uint32_t entry_size;
		uint64_t objsize;
		uint64_t tray_capacity;
		uint64_t meta_size;
		uint32_t objsize_order; //objsize = 2 ^ objsize_order
		uint32_t rsv1; //to make alignment at 8 byte
		uint64_t free_list_position;
		uint64_t free_list_size;
		uint64_t trim_list_position;
		uint64_t trim_list_size;
		uint64_t lmt_position;
		uint64_t lmt_size;
		uint64_t metadata_md5_position;
		uint64_t head_backup_position;
		uint64_t redolog_position;
		uint64_t redolog_size;
		char create_time[32];
	};

	//following are hot variables used by every IO. Put compact for cache hit convenience
	union {
		int fd;
		struct ns_entry *ns;
	};
	uint64_t in_obj_offset_mask; // := obj_size -1,

	std::unordered_map<struct lmt_key, struct lmt_entry*, struct lmt_hash> obj_lmt; //act as lmt table in S5
	PfFixedSizeQueue<int32_t> free_obj_queue;
	PfFixedSizeQueue<int32_t> trim_obj_queue;
	ObjectMemoryPool<lmt_entry> lmt_entry_pool;
	PfRedoLog* redolog;
	PfIoEngine* ioengine;
	HeadPage head;
	char tray_name[128];
	char uuid_str[64];

	int is_shared_disk;

	~PfFlashStore();
	/**
	 * init flash store from device. this function will create meta data
	 * and initialize the device if a device is not initialized.
	 *
	 * @return 0 on success, negative for error
	 * @retval -ENOENT  device not exist or failed to open
	 */
	int init(const char* dev_name);
	int shared_disk_init(const char* tray_name);
	int owner_init();
	int spdk_nvme_init(const char* trid_str);
	int register_controller(const char *trid_str);

	int process_event(int event_type, int arg_i, void* arg_p, void* arg_q);



	void trim_proc();
	std::thread trimming_thread;

	/**
	 * read data to buffer.
	 * a LBA is a block of data 4096 bytes.
	 * @return actual data length that readed to buf, in lba.
	 *         negative value for error
	 * actual data length may less than nlba in request to read. in this case, caller
	 * should treat the remaining part of buffer as 0.
	 */
	inline int do_read(IoSubTask* io);

	/**
	 * write data to flash store.
	 * a LBA is a block of data 4096 bytes.
	 * @return number of lbas has written to store
	 *         negative value for error
	 */
	int do_write(IoSubTask* io);

	int save_meta_data();
	void delete_snapshot(shard_id_t shard_id, uint32_t snap_seq_to_del, uint32_t prev_snap_seq, uint32_t next_snap_seq);
	int recovery_replica(replica_id_t  rep_id, const std::string &from_store_ip, int32_t from_store_id,
					  const std::string& from_ssd_uuid, int64_t object_size, uint16_t meta_ver);
	int delete_replica(replica_id_t rep_id);

	int get_snap_list(volume_id_t volume_id, int64_t offset, std::vector<int>& snap_list);
	int delete_obj(struct lmt_key* , struct lmt_entry* entry);
	virtual int commit_batch() { return ioengine->submit_batch(); };
private:
	ThreadPool cow_thread_pool; //TODO: use std::async replace
	int format_disk();
	int read_store_head();
	int initialize_store_head();
	int load_meta_data();

	/** these two function convert physical object id (i.e. object in disk space) and offset in disk
	 *  Note: don't confuse these function with vol_offset_to_block_idx, which do convert
	 *        in volume space
	 */
	inline int64_t obj_id_to_offset(int64_t obj_id) { return (obj_id << head.objsize_order) + head.meta_size; }
	inline int64_t offset_to_obj_id(int64_t offset) { return (offset - head.meta_size) >> head.objsize_order; }
	void begin_cow(lmt_key* key, lmt_entry *objEntry, lmt_entry *dstEntry);
	void do_cow_entry(lmt_key* key, lmt_entry *objEntry, lmt_entry *dstEntry);
	void begin_cow_scc(lmt_key* key, lmt_entry *objEntry, lmt_entry *dstEntry);
	static void* end_cow_scc(void *ctx);
	int delete_obj_snapshot(uint64_t volume_id, int64_t slba, uint32_t snap_seq, uint32_t prev_snap_seq, uint32_t next_snap_seq);
	int recovery_write(lmt_key* key, lmt_entry * head, uint32_t snap_seq, void* buf, size_t length, off_t offset);
	int finish_recovery_object(lmt_key* key, lmt_entry * head, size_t length, off_t offset, int failed);
	void post_load_fix();
	void post_load_check();
	int rpc_alloc_block_impl(SubTask* t);
	int rpc_delete_block_impl(SubTask* t);
	int rpc_change_block_status_impl(SubTask* t);
	PfMessageStatus  _alloc_block(const lmt_key& key, PfMessageHead* req_cmd, lmt_entry** entry);
	friend class PfRedoLog;
};

void delete_matched_entry(struct lmt_entry **head_ref, std::function<bool(struct lmt_entry *)> match,
                          std::function<void(struct lmt_entry *)> free_func);
#endif // flash_store_h__
