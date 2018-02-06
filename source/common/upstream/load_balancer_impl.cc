#include "common/upstream/load_balancer_impl.h"

#include <cstdint>
#include <string>
#include <vector>

#include "envoy/runtime/runtime.h"
#include "envoy/stats/stats.h"
#include "envoy/upstream/upstream.h"

#include "common/common/assert.h"

namespace Envoy {
namespace Upstream {

static const std::string RuntimeZoneEnabled = "upstream.zone_routing.enabled";
static const std::string RuntimeMinClusterSize = "upstream.zone_routing.min_cluster_size";
static const std::string RuntimePanicThreshold = "upstream.healthy_panic_threshold";

uint32_t LoadBalancerBase::choosePriority(uint64_t hash,
                                          const std::vector<uint32_t>& per_priority_load) {
  hash = hash % 100 + 1; // 1-100
  uint32_t aggregate_percentage_load = 0;
  // As with tryChooseLocalLocalityHosts, this can be refactored for efficiency
  // but O(N) is good enough for now given the expected number of priorities is
  // small.
  for (size_t priority = 0; priority < per_priority_load.size(); ++priority) {
    aggregate_percentage_load += per_priority_load[priority];
    if (hash <= aggregate_percentage_load) {
      return priority;
    }
  }
  // The percentages should always add up to 100 but we have to have a return for the compiler.
  NOT_REACHED;
}

LoadBalancerBase::LoadBalancerBase(const PrioritySet& priority_set, ClusterStats& stats,
                                   Runtime::Loader& runtime, Runtime::RandomGenerator& random)
    : stats_(stats), runtime_(runtime), random_(random), priority_set_(priority_set) {
  for (auto& host_set : priority_set_.hostSetsPerPriority()) {
    recalculatePerPriorityState(host_set->priority());
  }
  priority_set_.addMemberUpdateCb(
      [this](uint32_t priority, const HostVector&, const HostVector&) -> void {
        recalculatePerPriorityState(priority);
      });
}

void LoadBalancerBase::recalculatePerPriorityState(uint32_t priority) {
  per_priority_load_.resize(priority_set_.hostSetsPerPriority().size());
  per_priority_health_.resize(priority_set_.hostSetsPerPriority().size());

  // Determine the health of the newly modified priority level.
  // Health ranges from 0-100, and is the ratio of healthy hosts to total hosts, modified by the
  // somewhat arbitrary overprovision factor of 1.4.
  // Eventually the overprovision factor will likely be made configurable.
  HostSet& host_set = *priority_set_.hostSetsPerPriority()[priority];
  per_priority_health_[priority] = 0;
  if (host_set.hosts().size() > 0) {
    per_priority_health_[priority] =
        std::min<uint32_t>(100, 140 * host_set.healthyHosts().size() / host_set.hosts().size());
  }

  // Now that we've updated health for the changed priority level, we need to caculate percentage
  // load for all priority levels.
  //
  // First, determine if the load needs to be scaled relative to health. For example if there are
  // 3 host sets with 20% / 20% / 10% health they will get 40% / 40% / 20% load to ensure total load
  // adds up to 100.
  const uint32_t total_health = std::min<uint32_t>(
      std::accumulate(per_priority_health_.begin(), per_priority_health_.end(), 0), 100);
  if (total_health == 0) {
    // Everything is terrible. Send all load to P=0.
    // In this one case sumEntries(per_priority_load_) != 100 since we sinkhole all traffic in P=0.
    per_priority_load_[0] = 100;
    return;
  }
  size_t total_load = 100;
  for (size_t i = 0; i < per_priority_health_.size(); ++i) {
    // Now assign as much load as possible to the high priority levels and cease assigning load when
    // total_load runs out.
    per_priority_load_[i] =
        std::min<uint32_t>(total_load, per_priority_health_[i] * 100 / total_health);
    total_load -= per_priority_load_[i];
  }
  if (total_load != 0) {
    // Account for rounding errors.
    ASSERT(total_load < per_priority_load_.size());
    per_priority_load_[0] += total_load;
  }
}

const HostSet& LoadBalancerBase::chooseHostSet() {
  const uint32_t priority = choosePriority(random_.random(), per_priority_load_);
  return *priority_set_.hostSetsPerPriority()[priority];
}

ZoneAwareLoadBalancerBase::ZoneAwareLoadBalancerBase(const PrioritySet& priority_set,
                                                     const PrioritySet* local_priority_set,
                                                     ClusterStats& stats, Runtime::Loader& runtime,
                                                     Runtime::RandomGenerator& random)
    : LoadBalancerBase(priority_set, stats, runtime, random),
      local_priority_set_(local_priority_set) {
  ASSERT(!priority_set.hostSetsPerPriority().empty());
  resizePerPriorityState();
  priority_set_.addMemberUpdateCb(
      [this](uint32_t priority, const HostVector&, const HostVector&) -> void {
        // Make sure per_priority_state_ is as large as priority_set_.hostSetsPerPriority()
        resizePerPriorityState();
        // If P=0 changes, regenerate locality routing structures. Locality based routing is
        // disabled at all other levels.
        if (local_priority_set_ && priority == 0) {
          regenerateLocalityRoutingStructures();
        }
      });
  if (local_priority_set_) {
    // Multiple priorities are unsupported for local priority sets.
    // In order to support priorities correctly, one would have to make some assumptions about
    // routing (all local Envoys fail over at the same time) and use all priorities when computing
    // the locality routing structure.
    ASSERT(local_priority_set_->hostSetsPerPriority().size() == 1);
    local_priority_set_member_update_cb_handle_ = local_priority_set_->addMemberUpdateCb(
        [this](uint32_t priority, const HostVector&, const HostVector&) -> void {
          ASSERT(priority == 0);
          UNREFERENCED_PARAMETER(priority);
          // If the set of local Envoys changes, regenerate routing for P=0 as it does priority
          // based routing.
          regenerateLocalityRoutingStructures();
        });
  }
}

ZoneAwareLoadBalancerBase::~ZoneAwareLoadBalancerBase() {
  if (local_priority_set_member_update_cb_handle_ != nullptr) {
    local_priority_set_member_update_cb_handle_->remove();
  }
}

void ZoneAwareLoadBalancerBase::regenerateLocalityRoutingStructures() {
  ASSERT(local_priority_set_);
  stats_.lb_recalculate_zone_structures_.inc();
  // resizePerPriorityState should ensure these stay in sync.
  ASSERT(per_priority_state_.size() == priority_set_.hostSetsPerPriority().size());

  // We only do locality routing for P=0
  uint32_t priority = 0;
  PerPriorityState& state = *per_priority_state_[priority];
  // Do not perform any calculations if we cannot perform locality routing based on non runtime
  // params.
  if (earlyExitNonLocalityRouting()) {
    state.locality_routing_state_ = LocalityRoutingState::NoLocalityRouting;
    return;
  }
  HostSet& host_set = *priority_set_.hostSetsPerPriority()[priority];
  ASSERT(host_set.healthyHostsPerLocality().hasLocalLocality());
  const size_t num_localities = host_set.healthyHostsPerLocality().get().size();
  ASSERT(num_localities > 0);

  // It is worth noting that all of the percentages calculated are orthogonal from
  // how much load this priority level receives, percentageLoad(priority).
  //
  // If the host sets are such that 20% of load is handled locally and 80% is residual, and then
  // half the hosts in all host sets go unhealthy, this priority set will
  // still send half of the incoming load to the local locality and 80% to residual.
  //
  // Basically, fariness across localities within a priority is guaranteed. Fairness across
  // localities across priorities is not.
  uint64_t local_percentage[num_localities];
  calculateLocalityPercentage(localHostSet().healthyHostsPerLocality(), local_percentage);
  uint64_t upstream_percentage[num_localities];
  calculateLocalityPercentage(host_set.healthyHostsPerLocality(), upstream_percentage);

  // If we have lower percent of hosts in the local cluster in the same locality,
  // we can push all of the requests directly to upstream cluster in the same locality.
  if (upstream_percentage[0] >= local_percentage[0]) {
    state.locality_routing_state_ = LocalityRoutingState::LocalityDirect;
    return;
  }

  state.locality_routing_state_ = LocalityRoutingState::LocalityResidual;

  // If we cannot route all requests to the same locality, calculate what percentage can be routed.
  // For example, if local percentage is 20% and upstream is 10%
  // we can route only 50% of requests directly.
  state.local_percent_to_route_ = upstream_percentage[0] * 10000 / local_percentage[0];

  // Local locality does not have additional capacity (we have already routed what we could).
  // Now we need to figure out how much traffic we can route cross locality and to which exact
  // locality we should route. Percentage of requests routed cross locality to a specific locality
  // needed be proportional to the residual capacity upstream locality has.
  //
  // residual_capacity contains capacity left in a given locality, we keep accumulating residual
  // capacity to make search for sampled value easier.
  // For example, if we have the following upstream and local percentage:
  // local_percentage: 40000 40000 20000
  // upstream_percentage: 25000 50000 25000
  // Residual capacity would look like: 0 10000 5000. Now we need to sample proportionally to
  // bucket sizes (residual capacity). For simplicity of finding where specific
  // sampled value is, we accumulate values in residual capacity. This is what it will look like:
  // residual_capacity: 0 10000 15000
  // Now to find a locality to route (bucket) we could simply iterate over residual_capacity
  // searching where sampled value is placed.
  state.residual_capacity_.resize(num_localities);

  // Local locality (index 0) does not have residual capacity as we have routed all we could.
  state.residual_capacity_[0] = 0;
  for (size_t i = 1; i < num_localities; ++i) {
    // Only route to the localities that have additional capacity.
    if (upstream_percentage[i] > local_percentage[i]) {
      state.residual_capacity_[i] =
          state.residual_capacity_[i - 1] + upstream_percentage[i] - local_percentage[i];
    } else {
      // Locality with index "i" does not have residual capacity, but we keep accumulating previous
      // values to make search easier on the next step.
      state.residual_capacity_[i] = state.residual_capacity_[i - 1];
    }
  }
}

void ZoneAwareLoadBalancerBase::resizePerPriorityState() {
  const uint32_t size = priority_set_.hostSetsPerPriority().size();
  while (per_priority_state_.size() < size) {
    // Note for P!=0, PerPriorityState is created with NoLocalityRouting and never changed.
    per_priority_state_.push_back(PerPriorityStatePtr{new PerPriorityState});
  }
}

bool ZoneAwareLoadBalancerBase::earlyExitNonLocalityRouting() {
  // We only do locality routing for P=0.
  HostSet& host_set = *priority_set_.hostSetsPerPriority()[0];
  if (host_set.healthyHostsPerLocality().get().size() < 2) {
    return true;
  }

  // lb_local_cluster_not_ok is bumped for "Local host set is not set or it is
  // panic mode for local cluster".
  if (!host_set.healthyHostsPerLocality().hasLocalLocality() ||
      host_set.healthyHostsPerLocality().get()[0].empty()) {
    stats_.lb_local_cluster_not_ok_.inc();
    return true;
  }

  // Same number of localities should be for local and upstream cluster.
  if (host_set.healthyHostsPerLocality().get().size() !=
      localHostSet().healthyHostsPerLocality().get().size()) {
    stats_.lb_zone_number_differs_.inc();
    return true;
  }

  // Do not perform locality routing for small clusters.
  uint64_t min_cluster_size = runtime_.snapshot().getInteger(RuntimeMinClusterSize, 6U);
  if (host_set.healthyHosts().size() < min_cluster_size) {
    stats_.lb_zone_cluster_too_small_.inc();
    return true;
  }

  return false;
}

bool LoadBalancerBase::isGlobalPanic(const HostSet& host_set, Runtime::Loader& runtime) {
  uint64_t global_panic_threshold =
      std::min<uint64_t>(100, runtime.snapshot().getInteger(RuntimePanicThreshold, 50));
  double healthy_percent = host_set.hosts().size() == 0
                               ? 0
                               : 100.0 * host_set.healthyHosts().size() / host_set.hosts().size();

  // If the % of healthy hosts in the cluster is less than our panic threshold, we use all hosts.
  if (healthy_percent < global_panic_threshold) {
    return true;
  }

  return false;
}

void ZoneAwareLoadBalancerBase::calculateLocalityPercentage(
    const HostsPerLocality& hosts_per_locality, uint64_t* ret) {
  uint64_t total_hosts = 0;
  for (const auto& locality_hosts : hosts_per_locality.get()) {
    total_hosts += locality_hosts.size();
  }

  size_t i = 0;
  for (const auto& locality_hosts : hosts_per_locality.get()) {
    ret[i++] = total_hosts > 0 ? 10000ULL * locality_hosts.size() / total_hosts : 0;
  }
}

const HostVector& ZoneAwareLoadBalancerBase::tryChooseLocalLocalityHosts(const HostSet& host_set) {
  PerPriorityState& state = *per_priority_state_[host_set.priority()];
  ASSERT(state.locality_routing_state_ != LocalityRoutingState::NoLocalityRouting);

  // At this point it's guaranteed to be at least 2 localities & local exists.
  const size_t number_of_localities = host_set.healthyHostsPerLocality().get().size();
  ASSERT(number_of_localities >= 2U);
  ASSERT(host_set.healthyHostsPerLocality().hasLocalLocality());

  // Try to push all of the requests to the same locality first.
  if (state.locality_routing_state_ == LocalityRoutingState::LocalityDirect) {
    stats_.lb_zone_routing_all_directly_.inc();
    return host_set.healthyHostsPerLocality().get()[0];
  }

  ASSERT(state.locality_routing_state_ == LocalityRoutingState::LocalityResidual);

  // If we cannot route all requests to the same locality, we already calculated how much we can
  // push to the local locality, check if we can push to local locality on current iteration.
  if (random_.random() % 10000 < state.local_percent_to_route_) {
    stats_.lb_zone_routing_sampled_.inc();
    return host_set.healthyHostsPerLocality().get()[0];
  }

  // At this point we must route cross locality as we cannot route to the local locality.
  stats_.lb_zone_routing_cross_zone_.inc();

  // This is *extremely* unlikely but possible due to rounding errors when calculating
  // locality percentages. In this case just select random locality.
  if (state.residual_capacity_[number_of_localities - 1] == 0) {
    stats_.lb_zone_no_capacity_left_.inc();
    return host_set.healthyHostsPerLocality().get()[random_.random() % number_of_localities];
  }

  // Random sampling to select specific locality for cross locality traffic based on the additional
  // capacity in localities.
  uint64_t threshold = random_.random() % state.residual_capacity_[number_of_localities - 1];

  // This potentially can be optimized to be O(log(N)) where N is the number of localities.
  // Linear scan should be faster for smaller N, in most of the scenarios N will be small.
  // TODO(htuch): is there a bug here when threshold == 0? Seems like we pick
  // local locality in that situation. Probably should start iterating at 1.
  int i = 0;
  while (threshold > state.residual_capacity_[i]) {
    i++;
  }

  return host_set.healthyHostsPerLocality().get()[i];
}

const HostVector& ZoneAwareLoadBalancerBase::hostsToUse() {
  const HostSet& host_set = chooseHostSet();

  // If the selected host set has insufficient healthy hosts, return all hosts.
  if (isGlobalPanic(host_set, runtime_)) {
    stats_.lb_healthy_panic_.inc();
    return host_set.hosts();
  }

  // If we've latched that we can't do priority-based routing, return healthy hosts for the selected
  // host set.
  if (per_priority_state_[host_set.priority()]->locality_routing_state_ ==
      LocalityRoutingState::NoLocalityRouting) {
    return host_set.healthyHosts();
  }

  // Determine if the load balancer should do zone based routing for this pick.
  if (!runtime_.snapshot().featureEnabled(RuntimeZoneEnabled, 100)) {
    return host_set.healthyHosts();
  }

  if (isGlobalPanic(localHostSet(), runtime_)) {
    stats_.lb_local_cluster_not_ok_.inc();
    // If the local Envoy instances are in global panic, do not do locality
    // based routing.
    return host_set.healthyHosts();
  }

  return tryChooseLocalLocalityHosts(host_set);
}

HostConstSharedPtr RoundRobinLoadBalancer::chooseHost(LoadBalancerContext*) {
  const HostVector& hosts_to_use = hostsToUse();
  if (hosts_to_use.empty()) {
    return nullptr;
  }

  return hosts_to_use[rr_index_++ % hosts_to_use.size()];
}

LeastRequestLoadBalancer::LeastRequestLoadBalancer(const PrioritySet& priority_set,
                                                   const PrioritySet* local_priority_set,
                                                   ClusterStats& stats, Runtime::Loader& runtime,
                                                   Runtime::RandomGenerator& random)
    : ZoneAwareLoadBalancerBase(priority_set, local_priority_set, stats, runtime, random) {
  priority_set.addMemberUpdateCb(
      [this](uint32_t, const HostVector&, const HostVector& hosts_removed) -> void {
        if (last_host_) {
          for (const HostSharedPtr& host : hosts_removed) {
            if (host == last_host_) {
              hits_left_ = 0;
              last_host_.reset();

              break;
            }
          }
        }
      });
}

HostConstSharedPtr LeastRequestLoadBalancer::chooseHost(LoadBalancerContext*) {
  bool is_weight_imbalanced = stats_.max_host_weight_.value() != 1;
  bool is_weight_enabled = runtime_.snapshot().getInteger("upstream.weight_enabled", 1UL) != 0;

  if (is_weight_imbalanced && hits_left_ > 0 && is_weight_enabled) {
    --hits_left_;

    return last_host_;
  } else {
    // To avoid hit stale last_host_ when all hosts become weight balanced.
    hits_left_ = 0;
    last_host_.reset();
  }

  const HostVector& hosts_to_use = hostsToUse();
  if (hosts_to_use.empty()) {
    return nullptr;
  }

  // Make weighed random if we have hosts with non 1 weights.
  if (is_weight_imbalanced & is_weight_enabled) {
    last_host_ = hosts_to_use[random_.random() % hosts_to_use.size()];
    hits_left_ = last_host_->weight() - 1;

    return last_host_;
  } else {
    HostSharedPtr host1 = hosts_to_use[random_.random() % hosts_to_use.size()];
    HostSharedPtr host2 = hosts_to_use[random_.random() % hosts_to_use.size()];
    if (host1->stats().rq_active_.value() < host2->stats().rq_active_.value()) {
      return host1;
    } else {
      return host2;
    }
  }
}

HostConstSharedPtr RandomLoadBalancer::chooseHost(LoadBalancerContext*) {
  const HostVector& hosts_to_use = hostsToUse();
  if (hosts_to_use.empty()) {
    return nullptr;
  }

  return hosts_to_use[random_.random() % hosts_to_use.size()];
}

} // namespace Upstream
} // namespace Envoy
