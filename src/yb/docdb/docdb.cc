// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/docdb/docdb.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

#include "yb/common/hybrid_time.h"
#include "yb/common/row_mark.h"
#include "yb/common/transaction.h"

#include "yb/docdb/conflict_resolution.h"
#include "yb/docdb/cql_operation.h"
#include "yb/docdb/docdb-internal.h"
#include "yb/docdb/docdb.messages.h"
#include "yb/docdb/docdb_debug.h"
#include "yb/dockv/doc_kv_util.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/docdb_types.h"
#include "yb/dockv/intent.h"
#include "yb/docdb/intent_aware_iterator.h"
#include "yb/docdb/pgsql_operation.h"
#include "yb/docdb/rocksdb_writer.h"
#include "yb/dockv/subdocument.h"
#include "yb/dockv/value.h"
#include "yb/dockv/value_type.h"

#include "yb/gutil/casts.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/rocksutil/write_batch_formatter.h"

#include "yb/server/hybrid_clock.h"

#include "yb/util/bitmap.h"
#include "yb/util/bytes_formatter.h"
#include "yb/util/enums.h"
#include "yb/util/fast_varint.h"
#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/pb_util.h"
#include "yb/util/status.h"
#include "yb/util/status_format.h"
#include "yb/util/status_log.h"

#include "yb/yql/cql/ql/util/errcodes.h"

using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

using namespace std::placeholders;

DEFINE_UNKNOWN_int32(cdc_max_stream_intent_records, 1680,
             "Max number of intent records allowed in single cdc batch. ");

namespace yb {
namespace docdb {

using dockv::KeyBytes;
using dockv::KeyEntryType;
using dockv::KeyEntryTypeAsChar;

namespace {

// key should be valid prefix of doc key, ending with some complete primitive value or group end.
Status ApplyIntent(
    RefCntPrefix key, dockv::IntentTypeSet intent_types, LockBatchEntries* keys_locked) {
  RSTATUS_DCHECK(!intent_types.None(), InternalError, "Empty intent types is not allowed");
  // Have to strip kGroupEnd from end of key, because when only hash key is specified, we will
  // get two kGroupEnd at end of strong intent.
  RETURN_NOT_OK(dockv::RemoveGroupEndSuffix(&key));
  keys_locked->push_back({std::move(key), intent_types});
  return Status::OK();
}

struct DetermineKeysToLockResult {
  LockBatchEntries lock_batch;
  bool need_read_snapshot;

  std::string ToString() const {
    return YB_STRUCT_TO_STRING(lock_batch, need_read_snapshot);
  }
};

Result<DetermineKeysToLockResult> DetermineKeysToLock(
    const std::vector<std::unique_ptr<DocOperation>>& doc_write_ops,
    const ArenaList<LWKeyValuePairPB>& read_pairs,
    IsolationLevel isolation_level,
    dockv::OperationKind operation_kind,
    RowMarkType row_mark_type,
    bool transactional_table,
    dockv::PartialRangeKeyIntents partial_range_key_intents) {
  DetermineKeysToLockResult result;
  boost::container::small_vector<RefCntPrefix, 8> doc_paths;
  boost::container::small_vector<size_t, 32> key_prefix_lengths;
  result.need_read_snapshot = false;
  for (const unique_ptr<DocOperation>& doc_op : doc_write_ops) {
    doc_paths.clear();
    IsolationLevel level;
    RETURN_NOT_OK(doc_op->GetDocPaths(GetDocPathsMode::kLock, &doc_paths, &level));
    if (isolation_level != IsolationLevel::NON_TRANSACTIONAL) {
      level = isolation_level;
    }
    auto intent_types = GetIntentTypeSet(level, operation_kind, row_mark_type);
    if (isolation_level == IsolationLevel::SERIALIZABLE_ISOLATION &&
        operation_kind == dockv::OperationKind::kWrite &&
        doc_op->RequireReadSnapshot()) {
      intent_types = dockv::IntentTypeSet(
          {dockv::IntentType::kStrongRead, dockv::IntentType::kStrongWrite});
    }

    for (const auto& doc_path : doc_paths) {
      key_prefix_lengths.clear();
      RETURN_NOT_OK(dockv::SubDocKey::DecodePrefixLengths(
          doc_path.as_slice(), &key_prefix_lengths));
      // At least entire doc_path should be returned, so empty key_prefix_lengths is an error.
      if (key_prefix_lengths.empty()) {
        return STATUS_FORMAT(Corruption, "Unable to decode key prefixes from: $0",
                             doc_path.as_slice().ToDebugHexString());
      }
      // We will acquire strong lock on the full doc_path, so remove it from list of weak locks.
      key_prefix_lengths.pop_back();
      auto partial_key = doc_path;
      // Acquire weak lock on empty key for transactional tables,
      // unless specified key is already empty.
      if (doc_path.size() > 0 && transactional_table) {
        partial_key.Resize(0);
        RETURN_NOT_OK(ApplyIntent(
            partial_key, MakeWeak(intent_types), &result.lock_batch));
      }
      for (auto prefix_length : key_prefix_lengths) {
        partial_key.Resize(prefix_length);
        RETURN_NOT_OK(ApplyIntent(
            partial_key, MakeWeak(intent_types), &result.lock_batch));
      }

      RETURN_NOT_OK(ApplyIntent(doc_path, intent_types, &result.lock_batch));
    }

    if (doc_op->RequireReadSnapshot()) {
      result.need_read_snapshot = true;
    }
  }

  if (!read_pairs.empty()) {
    const auto read_intent_types = GetIntentTypeSet(isolation_level, operation_kind, row_mark_type);
    RETURN_NOT_OK(EnumerateIntents(
        read_pairs,
        [&result, &read_intent_types](
            dockv::AncestorDocKey ancestor_doc_key, dockv::FullDocKey, Slice, KeyBytes* key,
            dockv::LastKey) {
          return ApplyIntent(
              RefCntPrefix(key->AsSlice()),
              ancestor_doc_key ? MakeWeak(read_intent_types) : read_intent_types,
              &result.lock_batch);
        }, partial_range_key_intents));
  }

  return result;
}

// Collapse keys_locked into a unique set of keys with intent_types representing the union of
// intent_types originally present. In other words, suppose keys_locked is originally the following:
// [
//   (k1, {kWeakRead, kWeakWrite}),
//   (k1, {kStrongRead}),
//   (k2, {kWeakRead}),
//   (k3, {kStrongRead}),
//   (k2, {kStrongWrite}),
// ]
// Then after calling FilterKeysToLock we will have:
// [
//   (k1, {kWeakRead, kWeakWrite, kStrongRead}),
//   (k2, {kWeakRead}),
//   (k3, {kStrongRead, kStrongWrite}),
// ]
// Note that only keys which appear in order in keys_locked will be collapsed in this manner.
void FilterKeysToLock(LockBatchEntries *keys_locked) {
  if (keys_locked->empty()) {
    return;
  }

  std::sort(keys_locked->begin(), keys_locked->end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.key < rhs.key;
            });

  auto w = keys_locked->begin();
  for (auto it = keys_locked->begin(); ++it != keys_locked->end();) {
    if (it->key == w->key) {
      w->intent_types |= it->intent_types;
    } else {
      ++w;
      *w = *it;
    }
  }

  ++w;
  keys_locked->erase(w, keys_locked->end());
}

}  // namespace

Result<PrepareDocWriteOperationResult> PrepareDocWriteOperation(
    const std::vector<std::unique_ptr<DocOperation>>& doc_write_ops,
    const ArenaList<LWKeyValuePairPB>& read_pairs,
    const scoped_refptr<Histogram>& write_lock_latency,
    const scoped_refptr<Counter>& failed_batch_lock,
    const IsolationLevel isolation_level,
    const dockv::OperationKind operation_kind,
    const RowMarkType row_mark_type,
    bool transactional_table,
    bool write_transaction_metadata,
    CoarseTimePoint deadline,
    dockv::PartialRangeKeyIntents partial_range_key_intents,
    SharedLockManager *lock_manager) {
  PrepareDocWriteOperationResult result;

  auto determine_keys_to_lock_result = VERIFY_RESULT(DetermineKeysToLock(
      doc_write_ops, read_pairs, isolation_level, operation_kind, row_mark_type,
      transactional_table, partial_range_key_intents));
  VLOG_WITH_FUNC(4) << "determine_keys_to_lock_result=" << determine_keys_to_lock_result.ToString();
  if (determine_keys_to_lock_result.lock_batch.empty() && !write_transaction_metadata) {
    LOG(ERROR) << "Empty lock batch, doc_write_ops: " << yb::ToString(doc_write_ops)
               << ", read pairs: " << AsString(read_pairs);
    return STATUS(Corruption, "Empty lock batch");
  }
  result.need_read_snapshot = determine_keys_to_lock_result.need_read_snapshot;

  FilterKeysToLock(&determine_keys_to_lock_result.lock_batch);
  VLOG_WITH_FUNC(4) << "filtered determine_keys_to_lock_result="
                    << determine_keys_to_lock_result.ToString();
  const MonoTime start_time = (write_lock_latency != nullptr) ? MonoTime::Now() : MonoTime();
  result.lock_batch = LockBatch(
      lock_manager, std::move(determine_keys_to_lock_result.lock_batch), deadline);
  auto lock_status = result.lock_batch.status();
  if (!lock_status.ok()) {
    if (failed_batch_lock != nullptr) {
      failed_batch_lock->Increment();
    }
    return lock_status.CloneAndAppend(
        Format("Timeout: $0", deadline - ToCoarse(start_time)));
  }
  if (write_lock_latency != nullptr) {
    const MonoDelta elapsed_time = MonoTime::Now().GetDeltaSince(start_time);
    write_lock_latency->Increment(elapsed_time.ToMicroseconds());
  }

  return result;
}

Status AssembleDocWriteBatch(const vector<unique_ptr<DocOperation>>& doc_write_ops,
                             CoarseTimePoint deadline,
                             const ReadHybridTime& read_time,
                             const DocDB& doc_db,
                             LWKeyValueWriteBatchPB* write_batch,
                             InitMarkerBehavior init_marker_behavior,
                             std::atomic<int64_t>* monotonic_counter,
                             HybridTime* restart_read_ht,
                             const string& table_name) {
  DCHECK_ONLY_NOTNULL(restart_read_ht);
  DocWriteBatch doc_write_batch(doc_db, init_marker_behavior, monotonic_counter);
  DocOperationApplyData data = {&doc_write_batch, deadline, read_time, restart_read_ht};
  for (const unique_ptr<DocOperation>& doc_op : doc_write_ops) {
    Status s = doc_op->Apply(data);
    if (s.IsQLError() && doc_op->OpType() == DocOperation::Type::QL_WRITE_OPERATION) {
      std::string error_msg;
      if (ql::GetErrorCode(s) == ql::ErrorCode::CONDITION_NOT_SATISFIED) {
        // Generating the error message here because 'table_name'
        // is not available on the lower level - in doc_op->Apply().
        error_msg = Format("Condition on table $0 was not satisfied.", table_name);
      } else {
        error_msg = s.message().ToBuffer();
      }
      // Ensure we set appropriate error in the response object for QL errors.
      const auto& resp = down_cast<QLWriteOperation*>(doc_op.get())->response();
      resp->set_status(QLResponsePB::YQL_STATUS_QUERY_ERROR);
      resp->set_error_message(std::move(error_msg));
      continue;
    }

    RETURN_NOT_OK(s);
  }
  doc_write_batch.MoveToWriteBatchPB(write_batch);
  return Status::OK();
}

namespace {

Status NotEnoughBytes(size_t present, size_t required, const Slice& full) {
  return STATUS_FORMAT(
      Corruption, "Not enough bytes in external intents $0 while $1 expected, full: $2",
      present, required, full.ToDebugHexString());
}

Status PrepareApplyExternalIntentsBatch(
    HybridTime commit_ht,
    const Slice& original_input_value,
    rocksdb::WriteBatch* regular_batch,
    IntraTxnWriteId* write_id) {
  auto input_value = original_input_value;
  DocHybridTimeBuffer doc_ht_buffer;
  RETURN_NOT_OK(input_value.consume_byte(KeyEntryTypeAsChar::kUuid));
  RETURN_NOT_OK(Uuid::FromSlice(input_value.Prefix(kUuidSize)));
  input_value.remove_prefix(kUuidSize);
  RETURN_NOT_OK(input_value.consume_byte(KeyEntryTypeAsChar::kExternalIntents));
  for (;;) {
    auto key_size = VERIFY_RESULT(util::FastDecodeUnsignedVarInt(&input_value));
    if (key_size == 0) {
      break;
    }
    if (input_value.size() < key_size) {
      return NotEnoughBytes(input_value.size(), key_size, original_input_value);
    }
    auto output_key = input_value.Prefix(key_size);
    input_value.remove_prefix(key_size);
    auto value_size = VERIFY_RESULT(util::FastDecodeUnsignedVarInt(&input_value));
    if (input_value.size() < value_size) {
      return NotEnoughBytes(input_value.size(), value_size, original_input_value);
    }
    auto output_value = input_value.Prefix(value_size);
    input_value.remove_prefix(value_size);
    std::array<Slice, 2> key_parts = {{
        output_key,
        doc_ht_buffer.EncodeWithValueType(commit_ht, *write_id),
    }};
    std::array<Slice, 1> value_parts = {{
        output_value,
    }};
    regular_batch->Put(key_parts, value_parts);
    ++*write_id;
  }

  return Status::OK();
}

// Reads all stored external intents for provided transactions and prepares batches that will apply
// them into regular db and remove from intents db.
Status PrepareApplyExternalIntents(
    ExternalTxnApplyState* apply_external_transactions,
    rocksdb::WriteBatch* regular_batch,
    rocksdb::DB* intents_db,
    rocksdb::WriteBatch* intents_batch) {
  if (apply_external_transactions->empty()) {
    return Status::OK();
  }

  KeyBytes key_prefix;
  KeyBytes key_upperbound;
  Slice key_upperbound_slice;

  auto iter = CreateRocksDBIterator(
      intents_db, &KeyBounds::kNoBounds, BloomFilterMode::DONT_USE_BLOOM_FILTER,
      /* user_key_for_filter= */ boost::none,
      rocksdb::kDefaultQueryId, /* read_filter= */ nullptr, &key_upperbound_slice);

  for (auto& apply : *apply_external_transactions) {
    key_prefix.Clear();
    key_prefix.AppendKeyEntryType(KeyEntryType::kExternalTransactionId);
    key_prefix.AppendRawBytes(apply.first.AsSlice());

    key_upperbound = key_prefix;
    key_upperbound.AppendKeyEntryType(KeyEntryType::kMaxByte);
    key_upperbound_slice = key_upperbound.AsSlice();

    IntraTxnWriteId& write_id = apply.second.write_id;

    iter.Seek(key_prefix);
    while (iter.Valid()) {
      const Slice input_key(iter.key());

      if (!input_key.starts_with(key_prefix.AsSlice())) {
        break;
      }

      if (regular_batch) {
        RETURN_NOT_OK(PrepareApplyExternalIntentsBatch(
            apply.second.commit_ht, iter.value(), regular_batch, &write_id));
      }
      if (intents_batch) {
        intents_batch->SingleDelete(input_key);
      }

      iter.Next();
    }
    RETURN_NOT_OK(iter.status());
  }

  return Status::OK();
}

ExternalTxnApplyState ProcessApplyExternalTransactions(const LWKeyValueWriteBatchPB& put_batch) {
  ExternalTxnApplyState result;
  for (const auto& apply : put_batch.apply_external_transactions()) {
    auto txn_id = CHECK_RESULT(FullyDecodeTransactionId(apply.transaction_id()));
    auto commit_ht = HybridTime(apply.commit_hybrid_time());
    result.emplace(
        txn_id,
        ExternalTxnApplyStateData{
          .commit_ht = commit_ht
        });
  }

  return result;
}

} // namespace

IntraTxnWriteId ExternalTxnIntentsState::GetWriteIdAndIncrement(const TransactionId& txn_id) {
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  return map_[txn_id]++;
}

void ExternalTxnIntentsState::EraseEntry(const TransactionId& txn_id) {
  std::lock_guard<decltype(mutex_)> lock(mutex_);
  map_.erase(txn_id);
}

bool AddExternalPairToWriteBatch(
    const LWKeyValuePairPB& kv_pair,
    HybridTime hybrid_time,
    ExternalTxnApplyState* apply_external_transactions,
    rocksdb::WriteBatch* regular_write_batch,
    rocksdb::WriteBatch* intents_write_batch,
    ExternalTxnIntentsState* external_txns_intents_state) {
  DocHybridTimeBuffer doc_ht_buffer;
  dockv::DocHybridTimeWordBuffer inverted_doc_ht_buffer;

  CHECK(!kv_pair.key().empty());
  CHECK(!kv_pair.value().empty());

  if (kv_pair.key()[0] != KeyEntryTypeAsChar::kExternalTransactionId) {
    return true;
  }

  // We replicate encoded SubDocKeys without a HybridTime at the end, and only append it here.
  // The reason for this is that the HybridTime timestamp is only picked at the time of
  // appending  an entry to the tablet's Raft log. Also this is a good way to save network
  // bandwidth.
  //
  // "Write id" is the final component of our HybridTime encoding (or, to be more precise,
  // DocHybridTime encoding) that helps disambiguate between different updates to the
  // same key (row/column) within a transaction. We set it based on the position of the write
  // operation in its write batch.
  Slice key_value = kv_pair.value();
  // This entry contains external intents.
  Slice key = kv_pair.key();
  key.consume_byte();
  auto txn_id = CHECK_RESULT(DecodeTransactionId(&key));
  auto it = apply_external_transactions->find(txn_id);
  if (it != apply_external_transactions->end()) {
    // The same write operation could contain external intents and instruct us to apply them.
    CHECK_OK(PrepareApplyExternalIntentsBatch(
        it->second.commit_ht, key_value, regular_write_batch, &it->second.write_id));
    if (external_txns_intents_state) {
      external_txns_intents_state->EraseEntry(txn_id);
    }
    return false;
  }

  int write_id = 0;
  if (external_txns_intents_state) {
    write_id = external_txns_intents_state->GetWriteIdAndIncrement(txn_id);
  }

  hybrid_time = kv_pair.has_external_hybrid_time() ?
      HybridTime(kv_pair.external_hybrid_time()) : hybrid_time;
  std::array<Slice, 2> key_parts = {{
      Slice(kv_pair.key()),
      doc_ht_buffer.EncodeWithValueType(hybrid_time, write_id),
  }};
  key_parts[1] = dockv::InvertEncodedDocHT(key_parts[1], &inverted_doc_ht_buffer);
  constexpr size_t kNumValueParts = 1;
  intents_write_batch->Put(key_parts, { &key_value, kNumValueParts });

  return false;
}

// Usually put_batch contains only records that should be applied to regular DB.
// So apply_external_transactions will be empty and regular_entry will be true.
//
// But in general case on consumer side of CDC put_batch could contain various kinds of records,
// that should be applied into regular and intents db.
// They are:
// apply_external_transactions
//   The list of external transactions that should be applied.
//   For each such transaction we should lookup for existing external intents (stored in intents DB)
//   and convert them to Put command in regular_write_batch plus SingleDelete command in
//   intents_write_batch.
// write_pairs
//   Could contain regular entries, that should be stored into regular DB as is.
//   Also pair could contain external intents, that should be stored into intents DB.
//   But if apply_external_transactions contains transaction for those external intents, then
//   those intents will be applied directly to regular DB, avoiding unnecessary write to intents DB.
//   This case is very common for short running transactions.
bool PrepareExternalWriteBatch(
    const LWKeyValueWriteBatchPB& put_batch,
    HybridTime hybrid_time,
    rocksdb::DB* intents_db,
    rocksdb::WriteBatch* regular_write_batch,
    rocksdb::WriteBatch* intents_write_batch,
    ExternalTxnIntentsState* external_txns_intents_state) {
  CHECK(put_batch.read_pairs().empty());

  auto apply_external_transactions = ProcessApplyExternalTransactions(put_batch);

  CHECK_OK(PrepareApplyExternalIntents(
      &apply_external_transactions, regular_write_batch, intents_db, intents_write_batch));

  bool has_non_external_kvs = false;
  for (const auto& write_pair : put_batch.write_pairs()) {
    has_non_external_kvs = AddExternalPairToWriteBatch(
        write_pair, hybrid_time, &apply_external_transactions, regular_write_batch,
        intents_write_batch, external_txns_intents_state) || has_non_external_kvs;
  }
  return has_non_external_kvs;
}

Status EnumerateIntents(
    const ArenaList<LWKeyValuePairPB>& kv_pairs,
    const dockv::EnumerateIntentsCallback& functor,
    dockv::PartialRangeKeyIntents partial_range_key_intents) {
  if (kv_pairs.empty()) {
    return Status::OK();
  }
  KeyBytes encoded_key;

  auto it = kv_pairs.begin();
  for (;;) {
    const auto& kv_pair = *it;
    dockv::LastKey last_key(++it == kv_pairs.end());
    CHECK(!kv_pair.key().empty());
    CHECK(!kv_pair.value().empty());
    RETURN_NOT_OK(dockv::EnumerateIntents(
        kv_pair.key(), kv_pair.value(), functor, &encoded_key, partial_range_key_intents,
        last_key));
    if (last_key) {
      break;
    }
  }

  return Status::OK();
}

// ------------------------------------------------------------------------------------------------
// Standalone functions
// ------------------------------------------------------------------------------------------------

void AppendTransactionKeyPrefix(const TransactionId& transaction_id, KeyBytes* out) {
  out->AppendKeyEntryType(KeyEntryType::kTransactionId);
  out->AppendRawBytes(transaction_id.AsSlice());
}

Result<ApplyTransactionState> GetIntentsBatch(
    const TransactionId& transaction_id,
    const KeyBounds* key_bounds,
    const ApplyTransactionState* stream_state,
    rocksdb::DB* intents_db,
    std::vector<IntentKeyValueForCDC>* key_value_intents) {
  KeyBytes txn_reverse_index_prefix;
  Slice transaction_id_slice = transaction_id.AsSlice();
  AppendTransactionKeyPrefix(transaction_id, &txn_reverse_index_prefix);
  txn_reverse_index_prefix.AppendKeyEntryType(KeyEntryType::kMaxByte);
  Slice key_prefix = txn_reverse_index_prefix.AsSlice();
  key_prefix.remove_suffix(1);
  const Slice reverse_index_upperbound = txn_reverse_index_prefix.AsSlice();

  auto reverse_index_iter = CreateRocksDBIterator(
      intents_db, &KeyBounds::kNoBounds, BloomFilterMode::DONT_USE_BLOOM_FILTER, boost::none,
      rocksdb::kDefaultQueryId, nullptr /* read_filter */, &reverse_index_upperbound);

  BoundedRocksDbIterator intent_iter = CreateRocksDBIterator(
      intents_db, key_bounds, BloomFilterMode::DONT_USE_BLOOM_FILTER, boost::none,
      rocksdb::kDefaultQueryId);

  reverse_index_iter.Seek(key_prefix);

  DocHybridTimeBuffer doc_ht_buffer;
  IntraTxnWriteId write_id = 0;
  if (stream_state != nullptr && stream_state->active() && stream_state->write_id != 0) {
    reverse_index_iter.Seek(stream_state->key);
    write_id = stream_state->write_id;
    reverse_index_iter.Next();
  }
  const uint64_t& max_records = FLAGS_cdc_max_stream_intent_records;
  uint64_t cur_records = 0;

  while (reverse_index_iter.Valid()) {
    const Slice key_slice(reverse_index_iter.key());

    if (!key_slice.starts_with(key_prefix)) {
      break;
    }
    // If the key ends at the transaction id then it is transaction metadata (status tablet,
    // isolation level etc.).
    if (key_slice.size() > txn_reverse_index_prefix.size()) {
      auto reverse_index_value = reverse_index_iter.value();
      if (!reverse_index_value.empty() && reverse_index_value[0] == KeyEntryTypeAsChar::kBitSet) {
        reverse_index_value.remove_prefix(1);
        RETURN_NOT_OK(OneWayBitmap::Skip(&reverse_index_value));
      }
      // Value of reverse index is a key of original intent record, so seek it and check match.
      if ((!key_bounds || key_bounds->IsWithinBounds(reverse_index_iter.value()))) {
        // return when we have reached the batch limit.
        if (cur_records >= max_records) {
          return ApplyTransactionState{
              .key = key_slice.ToBuffer(), .write_id = write_id, .aborted = {}};
        }
        {
          intent_iter.Seek(reverse_index_value);
          if (!VERIFY_RESULT(intent_iter.CheckedValid()) ||
              intent_iter.key() != reverse_index_value) {
            LOG(WARNING) << "Unable to find intent: " << reverse_index_value.ToDebugHexString()
                         << " for " << key_slice.ToDebugHexString()
                         << ", transactionId: " << transaction_id;
            return ApplyTransactionState{};
          }

          auto intent = VERIFY_RESULT(ParseIntentKey(intent_iter.key(), transaction_id_slice));

          if (intent.types.Test(dockv::IntentType::kStrongWrite)) {
            auto decoded_value = VERIFY_RESULT(dockv::DecodeIntentValue(
                intent_iter.value(), &transaction_id_slice));
            write_id = decoded_value.write_id;

            if (decoded_value.body.starts_with(dockv::ValueEntryTypeAsChar::kRowLock)) {
              reverse_index_iter.Next();
              continue;
            }

            std::array<Slice, 1> key_parts = {{
                intent.doc_path,
            }};
            std::array<Slice, 1> value_parts = {{
                  decoded_value.body,
            }};
            std::array<Slice, 1> ht_parts = {{
                intent.doc_ht,
            }};

            auto doc_ht = VERIFY_RESULT(DocHybridTime::DecodeFromEnd(intent.doc_ht));

            IntentKeyValueForCDC intent_metadata;
            Slice(key_parts, &(intent_metadata.key_buf));
            intent_metadata.key = intent.doc_path;
            Slice(value_parts, &(intent_metadata.value_buf));
            intent_metadata.value = decoded_value.body;
            intent_metadata.reverse_index_key = key_slice.ToBuffer();
            intent_metadata.write_id = write_id;
            intent_metadata.intent_ht = doc_ht;
            intent_metadata.ht = Slice(ht_parts, &intent_metadata.ht_buf);

            (*key_value_intents).push_back(intent_metadata);

            VLOG(4) << "The size of intentKeyValues in GetIntentList "
                    << (*key_value_intents).size();
            ++cur_records;
            ++write_id;
          }
        }
      }
    }
    reverse_index_iter.Next();
  }
  RETURN_NOT_OK(reverse_index_iter.status());

  return ApplyTransactionState{};
}

std::string ApplyTransactionState::ToString() const {
  return Format(
      "{ key: $0 write_id: $1 aborted: $2 }", Slice(key).ToDebugString(), write_id, aborted);
}

void CombineExternalIntents(
    const TransactionId& txn_id,
    ExternalIntentsProvider* provider) {
  // External intents are stored in the following format:
  // key: kExternalTransactionId, txn_id
  // value: size(intent1_key), intent1_key, size(intent1_value), intent1_value, size(intent2_key)...
  // where size is encoded as varint.

  dockv::KeyBytes buffer;
  buffer.AppendKeyEntryType(KeyEntryType::kExternalTransactionId);
  buffer.AppendRawBytes(txn_id.AsSlice());
  provider->SetKey(buffer.AsSlice());
  buffer.Clear();
  buffer.AppendKeyEntryType(KeyEntryType::kUuid);
  buffer.AppendRawBytes(provider->InvolvedTablet().AsSlice());
  buffer.AppendKeyEntryType(KeyEntryType::kExternalIntents);
  while (auto key_value = provider->Next()) {
    buffer.AppendUInt64AsVarInt(key_value->first.size());
    buffer.AppendRawBytes(key_value->first);
    buffer.AppendUInt64AsVarInt(key_value->second.size());
    buffer.AppendRawBytes(key_value->second);
  }
  buffer.AppendUInt64AsVarInt(0);
  provider->SetValue(buffer.AsSlice());
}

}  // namespace docdb
}  // namespace yb
