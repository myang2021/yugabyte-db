// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
#ifndef YB_CLIENT_BATCHER_H_
#define YB_CLIENT_BATCHER_H_

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "yb/client/async_rpc.h"
#include "yb/client/error_collector.h"
#include "yb/client/transaction.h"

#include "yb/common/consistent_read_point.h"
#include "yb/common/transaction.h"

#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"

#include "yb/util/async_util.h"
#include "yb/util/atomic.h"
#include "yb/util/debug-util.h"
#include "yb/util/locks.h"
#include "yb/util/status.h"

namespace yb {

namespace client {
namespace internal {

// Batcher state changes sequentially in the order listed below, with the exception that kAborted
// could be reached from any state.
YB_DEFINE_ENUM(
    BatcherState,
    (kGatheringOps)       // Initial state, while we adding operations to the batcher.
    (kResolvingTablets)   // Flush was invoked on batcher, waiting until tablets for all operations
                          // are resolved and move to the next state.
                          // Could change to kComplete in case of failure.
    (kTransactionPrepare) // Preparing associated transaction for flushing operations of this
                          // batcher, for instance it picks status tablet and fills
                          // transaction metadata for this batcher.
                          // When there is no associated transaction or no operations moves to the
                          // next state immediately.
    (kTransactionReady)   // Transaction is ready, sending operations to appropriate tablets and
                          // wait for response. When there is no transaction - we still sending
                          // operations marking transaction as auto ready.
    (kComplete)           // Batcher complete.
    (kAborted)            // Batcher was aborted.
    );

// A Batcher is the class responsible for collecting row operations, routing them to the
// correct tablet server, and possibly batching them together for better efficiency.
//
// It is a reference-counted class: the client session creating the batch holds one
// reference, and all of the in-flight operations hold others. This allows the client
// session to be destructed while ops are still in-flight, without the async callbacks
// attempting to access a destructed Batcher.
//
// This class is not thread safe, i.e. it could be filled only from one thread, then flushed.
//
// The batcher changes state step by step, with appropriate operation done in the current step.
// For instance on gathering step it does NOT lookup tablets, and on transaction prepare state
// it only waits for transaction to be ready.
//
// Before calling FlushAsync all Batcher functions should be called sequentially, so no concurrent
// access to Batcher state is happening. FlushAsync is doing all tablets lookups and this doesn't
// modify Batcher state (only updates individual operations independently) until all lookups are
// done. After all tablets lookups are completed, Batcher changes its state and calls
// ExecuteOperations. This results in asynchronous calls to ProcessReadResponse/ProcessWriteResponse
// as operations are completed, but these functions only read Batcher state and update individual
// operations independently.
class Batcher : public Runnable, public std::enable_shared_from_this<Batcher> {
 public:
  // Create a new batcher associated with the given session.
  //
  // Creates a weak_ptr to 'session'.
  Batcher(YBClient* client,
          const YBSessionPtr& session,
          YBTransactionPtr transaction,
          ConsistentReadPoint* read_point,
          bool force_consistent_read);
  ~Batcher();

  // Set the timeout for this batcher.
  //
  // The timeout is currently set on all of the RPCs, but in the future will be relative
  // to when the Flush call is made (eg even if the lookup of the TS takes a long time, it
  // may time out before even sending an op). TODO: implement that
  void SetDeadline(CoarseTimePoint deadline);

  // Add a new operation to the batch. Requires that the batch has not yet been flushed.
  // TODO: in other flush modes, this may not be the case -- need to
  // update this when they're implemented.
  //
  // NOTE: If this returns not-OK, does not take ownership of 'write_op'.
  void Add(std::shared_ptr<YBOperation> yb_op);

  bool Has(const std::shared_ptr<YBOperation>& yb_op) const;

  // Return true if any operations are still pending. An operation is no longer considered
  // pending once it has either errored or succeeded.  Operations are considering pending
  // as soon as they are added, even if Flush has not been called.
  bool HasPendingOperations() const;

  // Return the number of buffered operations. These are only those operations which are
  // "corked" (i.e not yet flushed). Once Flush has been called, this returns 0.
  int CountBufferedOperations() const;

  // Return the number of operations successfully added, but not yet flushed.
  // This differs from CountBufferedOperations that can decrease before flush due to tablet lookup
  // errors after addition.
  int GetAddedNotFlushedOperationsCount() const;

  // Flush any buffered operations. The callback will be called once there are no
  // more pending operations from this Batcher. If all of the operations succeeded,
  // then the callback will receive Status::OK. Otherwise, it will receive failed status,
  // and the caller must inspect the ErrorCollector to retrieve more detailed
  // information on which operations failed.
  // If is_within_transaction_retry is true, all operations to be flushed by this batcher have
  // been already flushed, meaning we are now retrying them within the same session and the
  // associated transaction (if any) already expects them.
  void FlushAsync(StatusFunctor callback, IsWithinTransactionRetry is_within_transaction_retry);

  CoarseTimePoint deadline() const {
    return deadline_;
  }

  rpc::Messenger* messenger() const;

  rpc::ProxyCache& proxy_cache() const;

  const std::shared_ptr<AsyncRpcMetrics>& async_rpc_metrics() const {
    return async_rpc_metrics_;
  }

  ConsistentReadPoint* read_point() {
    return read_point_;
  }

  void SetForceConsistentRead(ForceConsistentRead value) {
    force_consistent_read_ = value;
  }

  YBTransactionPtr transaction() const;

  const InFlightOpsGroupsWithMetadata& in_flight_ops() const { return ops_info_; }

  void set_allow_local_calls_in_curr_thread(bool flag) { allow_local_calls_in_curr_thread_ = flag; }

  bool allow_local_calls_in_curr_thread() const { return allow_local_calls_in_curr_thread_; }

  const std::string& proxy_uuid() const;

  const ClientId& client_id() const;

  std::pair<RetryableRequestId, RetryableRequestId> NextRequestIdAndMinRunningRequestId(
      const TabletId& tablet_id);
  void RequestFinished(const TabletId& tablet_id, RetryableRequestId request_id);

  void SetRejectionScoreSource(RejectionScoreSourcePtr rejection_score_source) {
    rejection_score_source_ = rejection_score_source;
  }

  double RejectionScore(int attempt_num);

  // Returns errors occurred due tablet resolution or flushing operations to tablet server(s).
  // Caller takes ownership of the returned errors.
  CollectedErrors GetAndClearPendingErrors();

  std::string LogPrefix() const;

  // This is a status error string used when there are multiple errors that need to be fetched
  // from the error collector.
  static const std::string kErrorReachingOutToTServersMsg;

 private:
  friend class RefCountedThreadSafe<Batcher>;
  friend class AsyncRpc;
  friend class WriteRpc;
  friend class ReadRpc;

  void Flushed(const InFlightOps& ops, const Status& status, FlushExtraResult flush_extra_result);

  // Combines new error to existing ones. I.e. updates combined error with new status.
  void CombineError(const InFlightOp& in_flight_op);

  void FlushFinished();
  void AllLookupsDone();
  std::shared_ptr<AsyncRpc> CreateRpc(
      const BatcherPtr& self, RemoteTablet* tablet, const InFlightOpsGroup& group,
      bool allow_local_calls_in_curr_thread, bool need_consistent_read);

  // Calls/Schedules flush_callback_ and resets it to free resources.
  void RunCallback();

  // Log an error where an Rpc callback has response count mismatch.
  void AddOpCountMismatchError();

  // Cleans up an RPC response, scooping out any errors and passing them up
  // to the batcher.
  void ProcessReadResponse(const ReadRpc &rpc, const Status &s);
  void ProcessWriteResponse(const WriteRpc &rpc, const Status &s);

  // Process RPC status.
  void ProcessRpcStatus(const AsyncRpc &rpc, const Status &s);

  // Async Callbacks.
  void TabletLookupFinished(InFlightOp* op, Result<internal::RemoteTabletPtr> result);

  void TransactionReady(const Status& status);

  // initial - whether this method is called first time for this batch.
  void ExecuteOperations(Initial initial);

  void Abort(const Status& status);

  void Run() override;

  std::map<PartitionKey, Status> CollectOpsErrors();

  BatcherState state_ = BatcherState::kGatheringOps;

  YBClient* const client_;
  std::weak_ptr<YBSession> weak_session_;

  // Errors are reported into this error collector.
  ErrorCollector error_collector_;

  Status combined_error_;

  // If state is kFlushing, this member will be set to the user-provided
  // callback. Once there are no more in-flight operations, the callback
  // will be called exactly once (and the state changed to kFlushed).
  StatusFunctor flush_callback_;

  // All buffered or in-flight ops.
  // Added to this set during apply, removed during Finished of AsyncRpc.
  std::vector<std::shared_ptr<YBOperation>> ops_;
  std::vector<InFlightOp> ops_queue_;
  InFlightOpsGroupsWithMetadata ops_info_;

  // The absolute deadline for all in-flight ops.
  CoarseTimePoint deadline_;

  // Number of outstanding lookups across all in-flight ops.
  std::atomic<size_t> outstanding_lookups_{0};
  std::atomic<size_t> outstanding_rpcs_{0};

  // If true, we might allow the local calls to be run in the same IPC thread.
  bool allow_local_calls_in_curr_thread_ = true;

  std::shared_ptr<yb::client::internal::AsyncRpcMetrics> async_rpc_metrics_;

  YBTransactionPtr transaction_;

  // The consistent read point for this batch if it is specified.
  ConsistentReadPoint* read_point_ = nullptr;

  // Force consistent read on transactional table, even we have only single shard commands.
  ForceConsistentRead force_consistent_read_;

  RejectionScoreSourcePtr rejection_score_source_;

  DISALLOW_COPY_AND_ASSIGN(Batcher);
};

}  // namespace internal
}  // namespace client
}  // namespace yb
#endif  // YB_CLIENT_BATCHER_H_
