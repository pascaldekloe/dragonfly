// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/engine_shard_set.h"

extern "C" {
#include "redis/object.h"
#include "redis/zmalloc.h"
}

#include "base/flags.h"
#include "base/logging.h"
#include "server/blocking_controller.h"
#include "server/server_state.h"
#include "server/tiered_storage.h"
#include "server/transaction.h"
#include "util/fiber_sched_algo.h"
#include "util/varz.h"

using namespace std;

ABSL_FLAG(string, spill_file_prefix, "", "");

ABSL_FLAG(uint32_t, hz, 100,
          "Base frequency at which the server performs other background tasks. "
          "Warning: not advised to decrease in production.");

ABSL_FLAG(bool, cache_mode, false,
          "If true, the backend behaves like a cache, "
          "by evicting entries when getting close to maxmemory limit");

// memory defragmented related flags
ABSL_FLAG(float, mem_defrag_threshold, 0.7,
          "Minimum percentage of used memory relative to maxmemory cap before running "
          "defragmentation");

ABSL_FLAG(float, mem_defrag_waste_threshold, 0.2,
          "The ratio of wasted/commited memory above which we run defragmentation");

ABSL_FLAG(float, mem_defrag_page_utilization_threshold, 0.8,
          "memory page under utilization threshold. Ratio between used and commited size, below "
          "this, memory in this page will defragmented");
namespace dfly {

using namespace util;
namespace fibers = ::boost::fibers;
using absl::GetFlag;

namespace {

constexpr DbIndex kDefaultDbIndex = 0;
constexpr uint64_t kCursorDoneState = 0u;

vector<EngineShardSet::CachedStats> cached_stats;  // initialized in EngineShardSet::Init

struct ShardMemUsage {
  std::size_t commited = 0;
  std::size_t used = 0;
  std::size_t wasted_mem = 0;
};

std::ostream& operator<<(std::ostream& os, const ShardMemUsage& mem) {
  return os << "commited: " << mem.commited << " vs used " << mem.used << ", wasted memory "
            << mem.wasted_mem;
}

ShardMemUsage ReadShardMemUsage(float wasted_ratio) {
  ShardMemUsage usage;
  zmalloc_get_allocator_wasted_blocks(wasted_ratio, &usage.used, &usage.commited,
                                      &usage.wasted_mem);
  return usage;
}

}  // namespace

constexpr size_t kQueueLen = 64;

__thread EngineShard* EngineShard::shard_ = nullptr;
EngineShardSet* shard_set = nullptr;
uint64_t TEST_current_time_ms = 0;

EngineShard::Stats& EngineShard::Stats::operator+=(const EngineShard::Stats& o) {
  ooo_runs += o.ooo_runs;
  quick_runs += o.quick_runs;
  defrag_attempt_total += o.defrag_attempt_total;
  defrag_realloc_total += o.defrag_realloc_total;
  defrag_task_invocation_total += o.defrag_task_invocation_total;

  return *this;
}

void EngineShard::DefragTaskState::UpdateScanState(uint64_t cursor_val) {
  cursor = cursor_val;
  underutilized_found = false;
}

// This function checks 3 things:
// 1. Don't try memory fragmentation if we don't use "enough" memory (control by
// mem_defrag_threshold flag)
// 2. We have memory blocks that can be better utilized (there is a "wasted memory" in them).
// 3. in case the above is OK, make sure that we have a "gap" between usage and commited memory
// (control by mem_defrag_waste_threshold flag)
bool EngineShard::DefragTaskState::CheckRequired() {
  if (cursor > kCursorDoneState || underutilized_found) {
    VLOG(1) << "Already found memory utilization issue - cursor: " << cursor
            << " and underutilized_found " << underutilized_found;
    return true;
  }
  const std::size_t memory_per_shard = max_memory_limit / shard_set->size();

  const std::size_t threshold_mem = memory_per_shard * GetFlag(FLAGS_mem_defrag_threshold);
  const double waste_threshold = GetFlag(FLAGS_mem_defrag_waste_threshold);

  ShardMemUsage usage = ReadShardMemUsage(GetFlag(FLAGS_mem_defrag_page_utilization_threshold));

  if (threshold_mem < usage.commited &&
      usage.wasted_mem > (uint64_t(usage.commited * waste_threshold))) {
    VLOG(1) << "memory issue found for memory " << usage;
    underutilized_found = true;
  }

  return false;
}

bool EngineShard::DoDefrag() {
  // --------------------------------------------------------------------------
  // NOTE: This task is running with exclusive access to the shard.
  // i.e. - Since we are using shared noting access here, and all access
  // are done using fibers, This fiber is run only when no other fiber in the
  // context of the controlling thread will access this shard!
  // --------------------------------------------------------------------------

  constexpr size_t kMaxTraverses = 40;
  const float threshold = GetFlag(FLAGS_mem_defrag_page_utilization_threshold);

  auto& slice = db_slice();
  DCHECK(slice.IsDbValid(kDefaultDbIndex));
  auto [prime_table, expire_table] = slice.GetTables(kDefaultDbIndex);
  PrimeTable::Cursor cur = defrag_state_.cursor;
  uint64_t reallocations = 0;
  unsigned traverses_count = 0;
  uint64_t attempts = 0;

  do {
    cur = prime_table->Traverse(cur, [&](PrimeIterator it) {
      // for each value check whether we should move it because it
      // seats on underutilized page of memory, and if so, do it.
      bool did = it->second.DefragIfNeeded(threshold);
      attempts++;
      if (did) {
        reallocations++;
      }
    });
    traverses_count++;
  } while (traverses_count < kMaxTraverses && cur);

  defrag_state_.UpdateScanState(cur.value());
  if (reallocations > 0) {
    VLOG(1) << "shard " << slice.shard_id() << ": successfully defrag  " << reallocations
            << " times, did it in " << traverses_count << " cursor is at the "
            << (defrag_state_.cursor == kCursorDoneState ? "end" : "in progress");
  } else {
    VLOG(1) << "shard " << slice.shard_id() << ": run the defrag " << traverses_count
            << " times out of maximum " << kMaxTraverses << ", with cursor at "
            << (defrag_state_.cursor == kCursorDoneState ? "end" : "in progress")
            << " but no location for defrag were found";
  }
  stats_.defrag_realloc_total += reallocations;
  stats_.defrag_task_invocation_total++;
  stats_.defrag_attempt_total += attempts;
  return defrag_state_.cursor > kCursorDoneState;
}

// the memory defragmentation task is as follow:
//  1. Check if memory usage is high enough
//  2. Check if diff between commited and used memory is high enough
//  3. if all the above pass -> scan this shard and try to find whether we can move pointer to
//  underutilized pages values
//     if the cursor returned from scan is not in done state, schedule the task to run at high
//     priority.
//     otherwise lower the task priority so that it would not use the CPU when not required
uint32_t EngineShard::DefragTask() {
  constexpr uint32_t kRunAtLowPriority = 0u;
  const auto shard_id = db_slice().shard_id();

  if (defrag_state_.CheckRequired()) {
    VLOG(1) << shard_id << ": need to run defrag memory cursor state: " << defrag_state_.cursor
            << ", underutilzation found: " << defrag_state_.underutilized_found;
    if (DoDefrag()) {
      // we didn't finish the scan
      return util::ProactorBase::kOnIdleMaxLevel;
    }
  }
  return kRunAtLowPriority;
}

EngineShard::EngineShard(util::ProactorBase* pb, bool update_db_time, mi_heap_t* heap)
    : queue_(kQueueLen), txq_([](const Transaction* t) { return t->txid(); }), mi_resource_(heap),
      db_slice_(pb->GetIndex(), GetFlag(FLAGS_cache_mode), this) {
  fiber_q_ = fibers::fiber([this, index = pb->GetIndex()] {
    FiberProps::SetName(absl::StrCat("shard_queue", index));
    queue_.Run();
  });

  if (update_db_time) {
    uint32_t clock_cycle_ms = 1000 / std::max<uint32_t>(1, GetFlag(FLAGS_hz));
    if (clock_cycle_ms == 0)
      clock_cycle_ms = 1;

    fiber_periodic_ = fibers::fiber([this, index = pb->GetIndex(), period_ms = clock_cycle_ms] {
      FiberProps::SetName(absl::StrCat("shard_periodic", index));
      RunPeriodic(std::chrono::milliseconds(period_ms));
    });
  }

  tmp_str1 = sdsempty();

  db_slice_.UpdateExpireBase(absl::GetCurrentTimeNanos() / 1000000, 0);
  // start the defragmented task here
  defrag_task_ = pb->AddOnIdleTask([this]() { return this->DefragTask(); });
}

EngineShard::~EngineShard() {
  sdsfree(tmp_str1);
}

void EngineShard::Shutdown() {
  queue_.Shutdown();
  fiber_q_.join();

  if (tiered_storage_) {
    tiered_storage_->Shutdown();
  }

  fiber_periodic_done_.Notify();
  if (fiber_periodic_.joinable()) {
    fiber_periodic_.join();
  }

  ProactorBase::me()->RemoveOnIdleTask(defrag_task_);
}

void EngineShard::InitThreadLocal(ProactorBase* pb, bool update_db_time) {
  CHECK(shard_ == nullptr) << pb->GetIndex();

  mi_heap_t* data_heap = ServerState::tlocal()->data_heap();
  void* ptr = mi_heap_malloc_aligned(data_heap, sizeof(EngineShard), alignof(EngineShard));
  shard_ = new (ptr) EngineShard(pb, update_db_time, data_heap);

  CompactObj::InitThreadLocal(shard_->memory_resource());
  SmallString::InitThreadLocal(data_heap);

  string backing_prefix = GetFlag(FLAGS_spill_file_prefix);
  if (!backing_prefix.empty()) {
    if (pb->GetKind() != ProactorBase::IOURING) {
      LOG(ERROR) << "Only ioring based backing storage is supported. Exiting...";
      exit(1);
    }

    shard_->tiered_storage_.reset(new TieredStorage(&shard_->db_slice_));
    error_code ec = shard_->tiered_storage_->Open(backing_prefix);
    CHECK(!ec) << ec.message();  // TODO
  }
}

void EngineShard::DestroyThreadLocal() {
  if (!shard_)
    return;

  uint32_t index = shard_->db_slice_.shard_id();
  mi_heap_t* tlh = shard_->mi_resource_.heap();

  shard_->Shutdown();

  shard_->~EngineShard();
  mi_free(shard_);
  shard_ = nullptr;
  CompactObj::InitThreadLocal(nullptr);
  mi_heap_delete(tlh);
  VLOG(1) << "Shard reset " << index;
}

// Is called by Transaction::ExecuteAsync in order to run transaction tasks.
// Only runs in its own thread.
void EngineShard::PollExecution(const char* context, Transaction* trans) {
  DVLOG(2) << "PollExecution " << context << " " << (trans ? trans->DebugId() : "") << " "
           << txq_.size() << " " << continuation_trans_;

  ShardId sid = shard_id();

  uint16_t trans_mask = trans ? trans->GetLocalMask(sid) : 0;
  if (trans_mask & Transaction::AWAKED_Q) {
    DCHECK(continuation_trans_ == nullptr)
        << continuation_trans_->DebugId() << " when polling " << trans->DebugId();

    CHECK_EQ(committed_txid_, trans->GetNotifyTxid());
    bool keep = trans->RunInShard(this);
    if (keep) {
      return;
    } else {
      blocking_controller_->RemoveAwaked(trans);
    }
  }

  if (continuation_trans_) {
    if (trans == continuation_trans_)
      trans = nullptr;

    if (continuation_trans_->IsArmedInShard(sid)) {
      bool to_keep = continuation_trans_->RunInShard(this);
      DVLOG(1) << "RunContTrans: " << continuation_trans_->DebugId() << " keep: " << to_keep;
      if (!to_keep) {
        continuation_trans_ = nullptr;
      }
    }
  }

  Transaction* head = nullptr;
  string dbg_id;

  if (continuation_trans_ == nullptr) {
    while (!txq_.Empty()) {
      // we must check every iteration so that if the current transaction awakens
      // another transaction, the loop won't proceed further and will break, because we must run
      // the notified transaction before all other transactions in the queue can proceed.
      bool has_awaked_trans = blocking_controller_ && blocking_controller_->HasAwakedTransaction();
      if (has_awaked_trans)
        break;

      auto val = txq_.Front();
      head = absl::get<Transaction*>(val);

      // The fact that Tx is in the queue, already means that coordinator fiber will not progress,
      // hence here it's enough to test for run_count and check local_mask.
      bool is_armed = head->IsArmedInShard(sid);
      VLOG(2) << "Considering head " << head->DebugId() << " isarmed: " << is_armed;

      if (!is_armed)
        break;

      // It could be that head is processed and unblocks multi-hop transaction .
      // The transaction will schedule again and will arm another callback.
      // Then we will reach invalid state by running trans after this loop,
      // which is not what we want.
      // This function should not process 2 different callbacks for the same transaction.
      // Hence we make sure to reset trans if it has been processed via tx-queue.
      if (head == trans)
        trans = nullptr;
      TxId txid = head->txid();

      // committed_txid_ is strictly increasing when processed via TxQueue.
      DCHECK_LT(committed_txid_, txid);

      // We update committed_txid_ before calling RunInShard() to avoid cases where
      // a transaction stalls the execution with IO while another fiber queries this shard for
      // committed_txid_ (for example during the scheduling).
      committed_txid_ = txid;
      if (VLOG_IS_ON(2)) {
        dbg_id = head->DebugId();
      }

      bool keep = head->RunInShard(this);

      // We should not access head from this point since RunInShard callback decrements refcount.
      DLOG_IF(INFO, !dbg_id.empty()) << "RunHead " << dbg_id << ", keep " << keep;

      if (keep) {
        continuation_trans_ = head;
        break;
      }
    }       // while(!txq_.Empty())
  } else {  // if (continuation_trans_ == nullptr)
    DVLOG(1) << "Skipped TxQueue " << continuation_trans_;
  }

  // we need to run trans if it's OOO or when trans is blocked in this shard and should
  // be treated here as noop.
  bool should_run = trans_mask & (Transaction::OUT_OF_ORDER | Transaction::SUSPENDED_Q);

  // It may be that there are other transactions that touch those keys but they necessary ordered
  // after trans in the queue, hence it's safe to run trans out of order.
  if (trans && should_run) {
    DCHECK(trans != head);
    DCHECK(trans_mask & Transaction::ARMED);

    dbg_id.clear();

    if (VLOG_IS_ON(1)) {
      dbg_id = trans->DebugId();
    }
    ++stats_.ooo_runs;

    bool keep = trans->RunInShard(this);
    DLOG_IF(INFO, !dbg_id.empty()) << "Eager run " << sid << ", " << dbg_id << ", keep " << keep;
  }
}

void EngineShard::ShutdownMulti(Transaction* multi) {
  if (continuation_trans_ == multi) {
    continuation_trans_ = nullptr;
  }
}

#if 0
// There are several cases that contain proof of convergence for this shard:
// 1. txq_ empty - it means that anything that is goonna be scheduled will already be scheduled
//    with txid > notifyid.
// 2. committed_txid_ > notifyid - similarly, this shard can not affect the result with timestamp
//    notifyid.
// 3. committed_txid_ == notifyid, then if a transaction in progress (continuation_trans_ != NULL)
//    the this transaction can still affect the result, hence we require continuation_trans_ is null
//    which will point to converged result @notifyid. However, we never awake a transaction
//    when there is a multi-hop transaction in progress to avoid false positives.
//    Therefore, continuation_trans_ must always be null when calling this function.
// 4. Finally with committed_txid_ < notifyid.
//    we can check if the next in line (HeadScore) is after notifyid in that case we can also
//    conclude regarding the result convergence for this shard.
//
bool EngineShard::HasResultConverged(TxId notifyid) const {
  CHECK(continuation_trans_ == nullptr);

  if (committed_txid_ >= notifyid)
    return true;

  // This could happen if a single lpush (not in transaction) woke multi-shard blpop.
  DVLOG(1) << "HasResultConverged: cmtxid - " << committed_txid_ << " vs " << notifyid;

  // We must check for txq head - it's not an optimization - we need it for correctness.
  // If a multi-transaction has been scheduled and it does not have any presence in
  // this shard (no actual keys) and we won't check for it HasResultConverged will
  // return false. The blocked transaction will wait for this shard to progress and
  // will also block other shards from progressing (where it has been notified).
  // If this multi-transaction has presence in those shards, it won't progress there as well.
  // Therefore, we will get a deadlock. By checking txid of the head we will avoid this situation:
  // if the head.txid is after notifyid then this shard obviously converged.
  // if the head.txid <= notifyid that transaction will be able to progress in other shards.
  // and we must wait for it to finish.
  return txq_.Empty() || txq_.HeadScore() > notifyid;
}
#endif

void EngineShard::Heartbeat() {
  CacheStats();

  if (IsReplica())  // Never run expiration on replica.
    return;

  constexpr double kTtlDeleteLimit = 200;
  constexpr double kRedLimitFactor = 0.1;

  uint32_t traversed = GetMovingSum6(TTL_TRAVERSE);
  uint32_t deleted = GetMovingSum6(TTL_DELETE);
  unsigned ttl_delete_target = 5;

  if (deleted > 10) {
    // deleted should be <= traversed.
    // hence we map our delete/traversed ratio into a range [0, kTtlDeleteLimit).
    // The higher t
    ttl_delete_target = kTtlDeleteLimit * double(deleted) / (double(traversed) + 10);
  }

  ssize_t redline = (max_memory_limit * kRedLimitFactor) / shard_set->size();
  DbContext db_cntx;
  db_cntx.time_now_ms = GetCurrentTimeMs();

  for (unsigned i = 0; i < db_slice_.db_array_size(); ++i) {
    if (!db_slice_.IsDbValid(i))
      continue;

    db_cntx.db_index = i;
    auto [pt, expt] = db_slice_.GetTables(i);
    if (expt->size() > pt->size() / 4) {
      DbSlice::DeleteExpiredStats stats = db_slice_.DeleteExpiredStep(db_cntx, ttl_delete_target);

      counter_[TTL_TRAVERSE].IncBy(stats.traversed);
      counter_[TTL_DELETE].IncBy(stats.deleted);
    }

    // if our budget is below the limit
    if (db_slice_.memory_budget() < redline) {
      db_slice_.FreeMemWithEvictionStep(i, redline - db_slice_.memory_budget());
    }
  }

  // Journal entries for expired entries are not writen to socket in the loop above.
  // Trigger write to socket when loop finishes.
  if (auto journal = EngineShard::tlocal()->journal(); journal) {
    TriggerJournalWriteToSink();
  }
}

void EngineShard::RunPeriodic(std::chrono::milliseconds period_ms) {
  while (true) {
    Heartbeat();
    if (fiber_periodic_done_.WaitFor(period_ms)) {
      VLOG(1) << "finished running engine shard periodic task";
      return;
    }
  }
}

void EngineShard::CacheStats() {
  // mi_heap_visit_blocks(tlh, false /* visit all blocks*/, visit_cb, &sum);
  mi_stats_merge();

  // Used memory for this shard.
  size_t used_mem = UsedMemory();
  cached_stats[db_slice_.shard_id()].used_memory.store(used_mem, memory_order_relaxed);
  ssize_t free_mem = max_memory_limit - used_mem_current.load(memory_order_relaxed);

  size_t entries = 0;
  size_t table_memory = 0;
  for (size_t i = 0; i < db_slice_.db_array_size(); ++i) {
    DbTable* table = db_slice_.GetDBTable(i);
    if (table) {
      entries += table->prime.size();
      table_memory += (table->prime.mem_usage() + table->expire.mem_usage());
    }
  }
  size_t obj_memory = table_memory <= used_mem ? used_mem - table_memory : 0;

  size_t bytes_per_obj = entries > 0 ? obj_memory / entries : 0;
  db_slice_.SetCachedParams(free_mem / shard_set->size(), bytes_per_obj);
}

size_t EngineShard::UsedMemory() const {
  return mi_resource_.used() + zmalloc_used_memory_tl + SmallString::UsedThreadLocal();
}

BlockingController* EngineShard::EnsureBlockingController() {
  if (!blocking_controller_) {
    blocking_controller_.reset(new BlockingController(this));
  }

  return blocking_controller_.get();
}

void EngineShard::TEST_EnableHeartbeat() {
  fiber_periodic_ = fibers::fiber([this, period_ms = 1] {
    FiberProps::SetName("shard_periodic_TEST");
    RunPeriodic(std::chrono::milliseconds(period_ms));
  });
}

/**


  _____                _               ____   _                      _  ____         _
 | ____| _ __    __ _ (_) _ __    ___ / ___| | |__    __ _  _ __  __| |/ ___|   ___ | |_
 |  _|  | '_ \  / _` || || '_ \  / _ \\___ \ | '_ \  / _` || '__|/ _` |\___ \  / _ \| __|
 | |___ | | | || (_| || || | | ||  __/ ___) || | | || (_| || |  | (_| | ___) ||  __/| |_
 |_____||_| |_| \__, ||_||_| |_| \___||____/ |_| |_| \__,_||_|   \__,_||____/  \___| \__|
                |___/

 */

void EngineShardSet::Init(uint32_t sz, bool update_db_time) {
  CHECK_EQ(0u, size());
  cached_stats.resize(sz);
  shard_queue_.resize(sz);

  pp_->AwaitFiberOnAll([&](uint32_t index, ProactorBase* pb) {
    if (index < shard_queue_.size()) {
      InitThreadLocal(pb, update_db_time);
    }
  });
}

void EngineShardSet::Shutdown() {
  RunBlockingInParallel([](EngineShard*) { EngineShard::DestroyThreadLocal(); });
}

void EngineShardSet::InitThreadLocal(ProactorBase* pb, bool update_db_time) {
  EngineShard::InitThreadLocal(pb, update_db_time);
  EngineShard* es = EngineShard::tlocal();
  shard_queue_[es->shard_id()] = es->GetFiberQueue();
}

const vector<EngineShardSet::CachedStats>& EngineShardSet::GetCachedStats() {
  return cached_stats;
}

void EngineShardSet::TEST_EnableHeartBeat() {
  RunBriefInParallel([](EngineShard* shard) { shard->TEST_EnableHeartbeat(); });
}

void EngineShardSet::TEST_EnableCacheMode() {
  RunBriefInParallel([](EngineShard* shard) { shard->db_slice().TEST_EnableCacheMode(); });
}

}  // namespace dfly
