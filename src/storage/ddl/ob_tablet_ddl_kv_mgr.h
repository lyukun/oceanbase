/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OCEANBASE_STORAGE_OB_TABLET_DDL_KV_MGR_H
#define OCEANBASE_STORAGE_OB_TABLET_DDL_KV_MGR_H

#include "common/ob_tablet_id.h"
#include "lib/allocator/ob_concurrent_fifo_allocator.h"
#include "lib/lock/ob_tc_rwlock.h"
#include "lib/ob_define.h"
#include "share/scn.h"
#include "share/ob_ls_id.h"
#include "storage/ob_i_table.h"
#include "storage/meta_mem/ob_tablet_pointer.h"

namespace oceanbase
{
namespace storage
{

class ObDDLKV;
class ObDDLKVHandle;
class ObDDLKVsHandle;
struct ObTabletDDLParam;

class ObTabletDDLKvMgr final
{
public:
  ObTabletDDLKvMgr();
  ~ObTabletDDLKvMgr();
  int init(const share::ObLSID &ls_id, const common::ObTabletID &tablet_id); // init before memtable mgr
  int ddl_start(const ObITable::TableKey &table_key, const share::SCN &start_log_ts, const int64_t cluster_version, const int64_t execution_id, const share::SCN &checkpoint_scn);
  int ddl_prepare(const share::SCN &start_scn, const share::SCN &commit_scn, const uint64_t table_id = 0, const int64_t ddl_task_id = 0); // schedule build a major sstable
  int ddl_commit(const share::SCN &start_scn, const share::SCN &prepare_scn, const bool is_replay); // try wait build major sstable
  int wait_ddl_commit(const share::SCN &start_scn, const share::SCN &prepare_scn);
  int get_ddl_param(ObTabletDDLParam &ddl_param);
  int get_or_create_ddl_kv(const share::SCN &scn, ObDDLKVHandle &kv_handle); // used in active ddl kv guard
  int get_freezed_ddl_kv(const share::SCN &freeze_scn, ObDDLKVHandle &kv_handle); // locate ddl kv with exeact freeze log ts
  int get_ddl_kvs(const bool frozen_only, ObDDLKVsHandle &ddl_kvs_handle); // get all freeze ddl kvs
  int freeze_ddl_kv(const share::SCN &freeze_scn = share::SCN::invalid_scn()); // freeze the active ddl kv, when memtable freeze or ddl commit
  int release_ddl_kvs(const share::SCN &rec_scn); // release persistent ddl kv, used in ddl merge task for free ddl kv
  int check_has_effective_ddl_kv(bool &has_ddl_kv); // used in ddl log handler for checkpoint
  int get_ddl_kv_min_scn(share::SCN &min_scn); // for calculate rec_scn of ls
  share::SCN get_start_scn() const { return start_scn_; }
  bool is_started() const { return start_scn_.is_valid(); }
  int set_commit_success();
  bool is_commit_success() const { return is_commit_success_; }
  common::ObTabletID get_tablet_id() const { return tablet_id_; }
  int cleanup();
  int online();
  bool is_execution_id_older(const int64_t execution_id);
  int register_to_tablet(const share::SCN &ddl_start_scn, ObDDLKvMgrHandle &kv_mgr_handle);
  int unregister_from_tablet(const share::SCN &ddl_start_scn, ObDDLKvMgrHandle &kv_mgr_handle);
  OB_INLINE void inc_ref() { ATOMIC_INC(&ref_cnt_); }
  OB_INLINE int64_t dec_ref() { return ATOMIC_SAF(&ref_cnt_, 1 /* just sub 1 */); }
  OB_INLINE int64_t get_ref() const { return ATOMIC_LOAD(&ref_cnt_); }
  OB_INLINE void reset() { destroy(); }
  TO_STRING_KV(K_(is_inited), K_(is_commit_success), K_(ls_id), K_(tablet_id), K_(table_key),
      K_(cluster_version), K_(start_scn), K_(max_freeze_scn),
      K_(table_id), K_(execution_id), K_(ddl_task_id), K_(head), K_(tail), K_(ref_cnt));

private:
  int64_t get_idx(const int64_t pos) const;
  int64_t get_count() const;
  int alloc_ddl_kv(ObDDLKV *&kv);
  void free_ddl_kv(const int64_t idx);
  int get_active_ddl_kv_impl(ObDDLKVHandle &kv_handle);
  void try_get_ddl_kv_unlock(const share::SCN &scn, ObDDLKV *&kv);
  int update_tablet(const share::SCN &start_scn, const int64_t snapshot_version, const share::SCN &ddl_checkpoint_scn);
  void cleanup_unlock();
  void destroy();
private:
  static const int64_t MAX_DDL_KV_CNT_IN_STORAGE = 64;
  bool is_inited_;
  bool is_commit_success_;
  share::ObLSID ls_id_;
  common::ObTabletID tablet_id_;
  ObITable::TableKey table_key_;
  int64_t cluster_version_;
  share::SCN start_scn_;
  share::SCN max_freeze_scn_;
  uint64_t table_id_; // used for ddl checksum
  int64_t execution_id_; // used for ddl checksum
  int64_t ddl_task_id_; // used for ddl checksum
  ObDDLKV *ddl_kvs_[MAX_DDL_KV_CNT_IN_STORAGE];
  int64_t head_;
  int64_t tail_;
  common::TCRWLock lock_;
  volatile int64_t ref_cnt_ CACHE_ALIGNED;
  DISALLOW_COPY_AND_ASSIGN(ObTabletDDLKvMgr);
};

}  // end namespace storage
}  // end namespace oceanbase

#endif  // OCEANBASE_STORAGE_OB_PG_DDL_KV_MGR_H
