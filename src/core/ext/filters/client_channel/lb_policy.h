/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_LB_POLICY_H
#define GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_LB_POLICY_H

#include <grpc/support/port_platform.h>

#include "src/core/ext/filters/client_channel/client_channel_channelz.h"
#include "src/core/ext/filters/client_channel/client_channel_factory.h"
#include "src/core/ext/filters/client_channel/subchannel.h"
#include "src/core/ext/filters/client_channel/subchannel_pool_interface.h"
#include "src/core/lib/gprpp/abstract.h"
#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/iomgr/combiner.h"
#include "src/core/lib/iomgr/polling_entity.h"
#include "src/core/lib/transport/connectivity_state.h"

extern grpc_core::DebugOnlyTraceFlag grpc_trace_lb_policy_refcount;

namespace grpc_core {

/// Interface for load balancing policies.
///
/// Note: All methods with a "Locked" suffix must be called from the
/// combiner passed to the constructor.
///
/// Any I/O done by the LB policy should be done under the pollset_set
/// returned by \a interested_parties().
class LoadBalancingPolicy : public InternallyRefCounted<LoadBalancingPolicy> {
 public:
  struct Args {
    /// The combiner under which all LB policy calls will be run.
    /// Policy does NOT take ownership of the reference to the combiner.
    // TODO(roth): Once we have a C++-like interface for combiners, this
    // API should change to take a smart pointer that does pass ownership
    // of a reference.
    grpc_combiner* combiner = nullptr;
    /// Used to create channels and subchannels.
    grpc_client_channel_factory* client_channel_factory = nullptr;
    /// Subchannel pool.
    RefCountedPtr<SubchannelPoolInterface> subchannel_pool;
    /// Channel args from the resolver.
    /// Note that the LB policy gets the set of addresses from the
    /// GRPC_ARG_SERVER_ADDRESS_LIST channel arg.
    grpc_channel_args* args = nullptr;
    /// Load balancing config from the resolver.
    grpc_json* lb_config = nullptr;
  };

  /// State used for an LB pick.
  struct PickState {
    /// Initial metadata associated with the picking call.
    grpc_metadata_batch* initial_metadata = nullptr;
    /// Pointer to bitmask used for selective cancelling. See
    /// \a CancelMatchingPicksLocked() and \a GRPC_INITIAL_METADATA_* in
    /// grpc_types.h.
    uint32_t* initial_metadata_flags = nullptr;
    /// Storage for LB token in \a initial_metadata, or nullptr if not used.
    grpc_linked_mdelem lb_token_mdelem_storage;
    /// Closure to run when pick is complete, if not completed synchronously.
    /// If null, pick will fail if a result is not available synchronously.
    grpc_closure* on_complete = nullptr;
    // Callback set by lb policy to be notified of trailing metadata.
    // The callback must be scheduled on grpc_schedule_on_exec_ctx.
    grpc_closure* recv_trailing_metadata_ready = nullptr;
    // The address that will be set to point to the original
    // recv_trailing_metadata_ready callback, to be invoked by the LB
    // policy's recv_trailing_metadata_ready callback when complete.
    // Must be non-null if recv_trailing_metadata_ready is non-null.
    grpc_closure** original_recv_trailing_metadata_ready = nullptr;
    // If this is not nullptr, then the client channel will point it to the
    // call's trailing metadata before invoking recv_trailing_metadata_ready.
    // If this is nullptr, then the callback will still be called.
    // The lb does not have ownership of the metadata.
    grpc_metadata_batch** recv_trailing_metadata = nullptr;
    /// Will be set to the selected subchannel, or nullptr on failure or when
    /// the LB policy decides to drop the call.
    RefCountedPtr<ConnectedSubchannel> connected_subchannel;
    /// Will be populated with context to pass to the subchannel call, if
    /// needed.
    grpc_call_context_element subchannel_call_context[GRPC_CONTEXT_COUNT] = {};
    /// Next pointer.  For internal use by LB policy.
    PickState* next = nullptr;
  };

  // Not copyable nor movable.
  LoadBalancingPolicy(const LoadBalancingPolicy&) = delete;
  LoadBalancingPolicy& operator=(const LoadBalancingPolicy&) = delete;

  /// Returns the name of the LB policy.
  virtual const char* name() const GRPC_ABSTRACT;

  /// Updates the policy with a new set of \a args and a new \a lb_config from
  /// the resolver. Note that the LB policy gets the set of addresses from the
  /// GRPC_ARG_SERVER_ADDRESS_LIST channel arg.
  virtual void UpdateLocked(const grpc_channel_args& args,
                            grpc_json* lb_config) GRPC_ABSTRACT;

  /// Finds an appropriate subchannel for a call, based on data in \a pick.
  /// \a pick must remain alive until the pick is complete.
  ///
  /// If a result is known immediately, returns true, setting \a *error
  /// upon failure.  Otherwise, \a pick->on_complete will be invoked once
  /// the pick is complete with its error argument set to indicate success
  /// or failure.
  ///
  /// If \a pick->on_complete is null and no result is known immediately,
  /// a synchronous failure will be returned (i.e., \a *error will be
  /// set and true will be returned).
  virtual bool PickLocked(PickState* pick, grpc_error** error) GRPC_ABSTRACT;

  /// Cancels \a pick.
  /// The \a on_complete callback of the pending pick will be invoked with
  /// \a pick->connected_subchannel set to null.
  virtual void CancelPickLocked(PickState* pick,
                                grpc_error* error) GRPC_ABSTRACT;

  /// Cancels all pending picks for which their \a initial_metadata_flags (as
  /// given in the call to \a PickLocked()) matches
  /// \a initial_metadata_flags_eq when ANDed with
  /// \a initial_metadata_flags_mask.
  virtual void CancelMatchingPicksLocked(uint32_t initial_metadata_flags_mask,
                                         uint32_t initial_metadata_flags_eq,
                                         grpc_error* error) GRPC_ABSTRACT;

  /// Requests a notification when the connectivity state of the policy
  /// changes from \a *state.  When that happens, sets \a *state to the
  /// new state and schedules \a closure.
  virtual void NotifyOnStateChangeLocked(grpc_connectivity_state* state,
                                         grpc_closure* closure) GRPC_ABSTRACT;

  /// Returns the policy's current connectivity state.  Sets \a error to
  /// the associated error, if any.
  virtual grpc_connectivity_state CheckConnectivityLocked(
      grpc_error** connectivity_error) GRPC_ABSTRACT;

  /// Hands off pending picks to \a new_policy.
  virtual void HandOffPendingPicksLocked(LoadBalancingPolicy* new_policy)
      GRPC_ABSTRACT;

  /// Tries to enter a READY connectivity state.
  /// TODO(roth): As part of restructuring how we handle IDLE state,
  /// consider whether this method is still needed.
  virtual void ExitIdleLocked() GRPC_ABSTRACT;

  /// Resets connection backoff.
  virtual void ResetBackoffLocked() GRPC_ABSTRACT;

  /// Populates child_subchannels and child_channels with the uuids of this
  /// LB policy's referenced children. This is not invoked from the
  /// client_channel's combiner. The implementation is responsible for
  /// providing its own synchronization.
  virtual void FillChildRefsForChannelz(
      channelz::ChildRefsList* child_subchannels,
      channelz::ChildRefsList* child_channels) GRPC_ABSTRACT;

  void Orphan() override {
    // Invoke ShutdownAndUnrefLocked() inside of the combiner.
    GRPC_CLOSURE_SCHED(
        GRPC_CLOSURE_CREATE(&LoadBalancingPolicy::ShutdownAndUnrefLocked, this,
                            grpc_combiner_scheduler(combiner_)),
        GRPC_ERROR_NONE);
  }

  /// Sets the re-resolution closure to \a request_reresolution.
  void SetReresolutionClosureLocked(grpc_closure* request_reresolution) {
    GPR_ASSERT(request_reresolution_ == nullptr);
    request_reresolution_ = request_reresolution;
  }

  grpc_pollset_set* interested_parties() const { return interested_parties_; }

  // Callers that need their own reference can call the returned
  // object's Ref() method.
  SubchannelPoolInterface* subchannel_pool() const {
    return subchannel_pool_.get();
  }

  GRPC_ABSTRACT_BASE_CLASS

 protected:
  GPRC_ALLOW_CLASS_TO_USE_NON_PUBLIC_DELETE

  explicit LoadBalancingPolicy(Args args);
  virtual ~LoadBalancingPolicy();

  grpc_combiner* combiner() const { return combiner_; }
  grpc_client_channel_factory* client_channel_factory() const {
    return client_channel_factory_;
  }

  /// Shuts down the policy.  Any pending picks that have not been
  /// handed off to a new policy via HandOffPendingPicksLocked() will be
  /// failed.
  virtual void ShutdownLocked() GRPC_ABSTRACT;

  /// Tries to request a re-resolution.
  void TryReresolutionLocked(grpc_core::TraceFlag* grpc_lb_trace,
                             grpc_error* error);

 private:
  static void ShutdownAndUnrefLocked(void* arg, grpc_error* ignored) {
    LoadBalancingPolicy* policy = static_cast<LoadBalancingPolicy*>(arg);
    policy->ShutdownLocked();
    policy->Unref();
  }

  /// Combiner under which LB policy actions take place.
  grpc_combiner* combiner_;
  /// Client channel factory, used to create channels and subchannels.
  grpc_client_channel_factory* client_channel_factory_;
  /// Subchannel pool.
  RefCountedPtr<SubchannelPoolInterface> subchannel_pool_;
  /// Owned pointer to interested parties in load balancing decisions.
  grpc_pollset_set* interested_parties_;
  /// Callback to force a re-resolution.
  grpc_closure* request_reresolution_;
};

}  // namespace grpc_core

#endif /* GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_LB_POLICY_H */
