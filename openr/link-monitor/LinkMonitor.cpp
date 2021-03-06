/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <fbzmq/service/logging/LogSample.h>
#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/futures/Future.h>
#include <folly/gen/Base.h>
#include <folly/system/ThreadName.h>
#include <thrift/lib/cpp/protocol/TProtocolTypes.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp/util/EnumUtils.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/spark/Spark.h>

namespace fb303 = facebook::fb303;

namespace {

const std::string kConfigKey{"link-monitor-config"};

/**
 * Transformation function to convert measured rtt (in us) to a metric value
 * to be used. Metric can never be zero.
 */
int32_t
getRttMetric(int64_t rttUs) {
  return std::max((int)(rttUs / 100), (int)1);
}

void
printLinkMonitorState(openr::thrift::LinkMonitorState const& state) {
  VLOG(1) << "LinkMonitor state .... ";
  VLOG(1) << "\tnodeLabel: " << *state.nodeLabel_ref();
  VLOG(1) << "\tisOverloaded: "
          << (*state.isOverloaded_ref() ? "true" : "false");
  if (not state.overloadedLinks_ref()->empty()) {
    VLOG(1) << "\toverloadedLinks: "
            << folly::join(",", *state.overloadedLinks_ref());
  }
  if (not state.linkMetricOverrides_ref()->empty()) {
    VLOG(1) << "\tlinkMetricOverrides: ";
    for (auto const& [key, val] : *state.linkMetricOverrides_ref()) {
      VLOG(1) << "\t\t" << key << ": " << val;
    }
  }
}

} // anonymous namespace

namespace openr {

//
// LinkMonitor code
//
LinkMonitor::LinkMonitor(
    std::shared_ptr<const Config> config,
    fbnl::NetlinkProtocolSocket* nlSock,
    KvStore* kvStore,
    PersistentStore* configStore,
    bool enablePerfMeasurement,
    messaging::ReplicateQueue<thrift::InterfaceDatabase>& intfUpdatesQueue,
    messaging::ReplicateQueue<thrift::PrefixUpdateRequest>& prefixUpdatesQueue,
    messaging::ReplicateQueue<thrift::PeerUpdateRequest>& peerUpdatesQueue,
    messaging::ReplicateQueue<LogSample>& logSampleQueue,
    messaging::RQueue<thrift::SparkNeighborEvent> neighborUpdatesQueue,
    messaging::RQueue<fbnl::NetlinkEvent> netlinkEventsQueue,
    bool assumeDrained,
    bool overrideDrainState,
    std::chrono::seconds adjHoldTime)
    : nodeId_(config->getNodeName()),
      enablePerfMeasurement_(enablePerfMeasurement),
      enableV4_(config->isV4Enabled()),
      enableSegmentRouting_(config->isSegmentRoutingEnabled()),
      prefixForwardingType_(*config->getConfig().prefix_forwarding_type_ref()),
      prefixForwardingAlgorithm_(
          *config->getConfig().prefix_forwarding_algorithm_ref()),
      useRttMetric_(*config->getLinkMonitorConfig().use_rtt_metric_ref()),
      linkflapInitBackoff_(std::chrono::milliseconds(
          *config->getLinkMonitorConfig().linkflap_initial_backoff_ms_ref())),
      linkflapMaxBackoff_(std::chrono::milliseconds(
          *config->getLinkMonitorConfig().linkflap_max_backoff_ms_ref())),
      ttlKeyInKvStore_(config->getKvStoreKeyTtl()),
      areas_(config->getAreas()),
      interfaceUpdatesQueue_(intfUpdatesQueue),
      prefixUpdatesQueue_(prefixUpdatesQueue),
      peerUpdatesQueue_(peerUpdatesQueue),
      logSampleQueue_(logSampleQueue),
      expBackoff_(Constants::kInitialBackoff, Constants::kMaxBackoff, true),
      configStore_(configStore),
      nlSock_(nlSock) {
  // Check non-empty module ptr
  CHECK(kvStore);
  CHECK(configStore_);
  CHECK(nlSock_);

  // Schedule callback to advertise the initial set of adjacencies and prefixes
  adjHoldTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    LOG(INFO) << "Hold time expired. Advertising adjacencies and addresses";
    // Advertise adjacencies and addresses after hold-timeout
    advertiseAdjacencies();
    advertiseRedistAddrs();
  });

  // Create throttled adjacency advertiser
  advertiseAdjacenciesThrottled_ = std::make_unique<AsyncThrottle>(
      getEvb(), Constants::kLinkThrottleTimeout, [this]() noexcept {
        // will advertise to all areas but will not trigger a adj key update
        // if nothing changed.
        advertiseAdjacencies();
      });

  // Create throttled interfaces and addresses advertiser
  advertiseIfaceAddrThrottled_ = std::make_unique<AsyncThrottle>(
      getEvb(), Constants::kLinkThrottleTimeout, [this]() noexcept {
        advertiseIfaceAddr();
      });
  // Create timer. Timer is used for immediate or delayed executions.
  advertiseIfaceAddrTimer_ = folly::AsyncTimeout::make(
      *getEvb(), [this]() noexcept { advertiseIfaceAddr(); });

  // Create config-store client
  LOG(INFO) << "Loading link-monitor state";
  auto state =
      configStore_->loadThriftObj<thrift::LinkMonitorState>(kConfigKey).get();
  if (state.hasValue()) {
    LOG(INFO) << "Loaded link-monitor state from disk.";
    state_ = state.value();
    printLinkMonitorState(state_);
  } else {
    // no persistent store found, use assumeDrained
    state_.isOverloaded_ref() = assumeDrained;
    LOG(WARNING) << folly::sformat(
        "Failed to load link-monitor state from disk. Setting node as {}",
        assumeDrained ? "DRAINED" : "UNDRAINED");
  }
  // overrideDrainState provided, use assumeDrained
  if (overrideDrainState) {
    state_.isOverloaded_ref() = assumeDrained;
    LOG(WARNING) << folly::sformat(
        "FLAGS_override_drain_state == true, setting drain state based on FLAGS_assume_drained to {}",
        assumeDrained ? "DRAINED" : "UNDRAINED");
  }

  //  Create KvStore client
  kvStoreClient_ = std::make_unique<KvStoreClientInternal>(
      this, nodeId_, kvStore, std::nullopt /* persist key timer */);

  if (enableSegmentRouting_) {
    // create range allocator to get unique node labels
    for (auto const& kv : areas_) {
      auto const& areaId = kv.first;
      rangeAllocator_.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(areaId),
          std::forward_as_tuple(
              AreaId{areaId},
              nodeId_,
              Constants::kNodeLabelRangePrefix.toString(),
              kvStoreClient_.get(),
              [&](std::optional<int32_t> newVal) noexcept {
                state_.nodeLabel_ref() = newVal ? newVal.value() : 0;
                advertiseAdjacencies();
              }, /* callback */
              std::chrono::milliseconds(100), /* minBackoffDur */
              std::chrono::seconds(2), /* maxBackoffDur */
              false /* override owner */,
              nullptr, /* checkValueInUseCb */
              Constants::kRangeAllocTtl));

      // Delay range allocation until we have formed all of our adjcencies
      auto startAllocTimer =
          folly::AsyncTimeout::make(*getEvb(), [this, areaId]() noexcept {
            std::optional<int32_t> initValue;
            if (*state_.nodeLabel_ref() != 0) {
              initValue = *state_.nodeLabel_ref();
            }
            rangeAllocator_.at(areaId).startAllocator(
                Constants::kSrGlobalRange, initValue);
          });
      startAllocTimer->scheduleTimeout(adjHoldTime);
      startAllocationTimers_.emplace_back(std::move(startAllocTimer));
    }
  }

  // start initial dump timer
  adjHoldTimer_->scheduleTimeout(adjHoldTime);

  // Add fiber to process the neighbor events
  addFiberTask([q = std::move(neighborUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto maybeEvent = q.get();
      if (maybeEvent.hasError()) {
        LOG(INFO) << "Terminating neighbor update processing fiber";
        break;
      }
      processNeighborEvent(std::move(maybeEvent).value());
    }
  });

  // Add fiber to process the LINK/ADDR events from platform
  addFiberTask([q = std::move(netlinkEventsQueue), this]() mutable noexcept {
    while (true) {
      auto maybeEvent = q.get();
      if (maybeEvent.hasError()) {
        LOG(INFO) << "Terminating netlink events processing fiber";
        break;
      }
      processNetlinkEvent(std::move(maybeEvent).value());
    }
  });

  // TODO: Add fiber to process KvStore InitialSync events
  // processKvStoreSyncEvent();

  // Schedule periodic timer for InterfaceDb re-sync from Netlink Platform
  interfaceDbSyncTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    auto success = syncInterfaces();
    if (success) {
      VLOG(2) << "InterfaceDb Sync is successful";
      expBackoff_.reportSuccess();
      interfaceDbSyncTimer_->scheduleTimeout(Constants::kPlatformSyncInterval);
    } else {
      fb303::fbData->addStatValue(
          "link_monitor.thrift.failure.getAllLinks", 1, fb303::SUM);

      if (ifIndexToName_.empty()) {
        // initial sync failed, immediately file re-sync
        // instead of applying exponetial backoff
        LOG(ERROR) << "Initial interfaceDb sync failed, re-sync immediately";

        interfaceDbSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
      } else {
        // Apply exponential backoff and schedule next run
        expBackoff_.reportError();
        interfaceDbSyncTimer_->scheduleTimeout(
            expBackoff_.getTimeRemainingUntilRetry());
        LOG(ERROR)
            << "InterfaceDb Sync failed, apply exponential backoff and retry in "
            << expBackoff_.getTimeRemainingUntilRetry().count() << " ms";
      }
    }
  });

  // schedule immediate with small timeout
  interfaceDbSyncTimer_->scheduleTimeout(std::chrono::milliseconds(100));

  // Initialize stats keys
  fb303::fbData->addStatExportType("link_monitor.neighbor_up", fb303::SUM);
  fb303::fbData->addStatExportType("link_monitor.neighbor_down", fb303::SUM);
  fb303::fbData->addStatExportType(
      "link_monitor.advertise_adjacencies", fb303::SUM);
  fb303::fbData->addStatExportType("link_monitor.advertise_links", fb303::SUM);
  fb303::fbData->addStatExportType(
      "link_monitor.thrift.failure.getAllLinks", fb303::SUM);
}

void
LinkMonitor::stop() {
  // Stop KvStoreClient first
  kvStoreClient_->stop();

  // Invoke stop method of super class
  OpenrEventBase::stop();
}

void
LinkMonitor::neighborUpEvent(const thrift::SparkNeighborEvent& event) {
  const auto& info = *event.info_ref();
  const auto& neighborAddrV4 = *info.transportAddressV4_ref();
  const auto& neighborAddrV6 = *info.transportAddressV6_ref();
  const auto& localIfName = *info.localIfName_ref();
  const auto& remoteIfName = *info.remoteIfName_ref();
  const auto& remoteNodeName = *info.nodeName_ref();
  const auto& area = *info.area_ref();
  const auto kvStoreCmdPort = *info.kvStoreCmdPort_ref();
  const auto openrCtrlThriftPort = *info.openrCtrlThriftPort_ref();
  const auto rttUs = *info.rttUs_ref();

  // current unixtime
  auto now = std::chrono::system_clock::now();
  int64_t timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();

  int64_t weight = 1;
  if (interfaces_.count(localIfName)) {
    weight = interfaces_.at(localIfName).getWeight();
  }

  thrift::Adjacency newAdj = createThriftAdjacency(
      remoteNodeName /* neighbor node name */,
      localIfName /* local ifName neighbor discovered on */,
      toString(neighborAddrV6) /* nextHopV6 */,
      toString(neighborAddrV4) /* nextHopV4 */,
      useRttMetric_ ? getRttMetric(rttUs) : 1 /* metric */,
      enableSegmentRouting_ ? *info.label_ref() : 0 /* adjacency-label */,
      false /* overload bit */,
      useRttMetric_ ? rttUs : 0 /* rtt */,
      timestamp,
      weight,
      remoteIfName);

  SYSLOG(INFO) << "Neighbor " << remoteNodeName << " is up on interface "
               << localIfName << ". Remote Interface: " << remoteIfName
               << ", metric: " << *newAdj.metric_ref() << ", rttUs: " << rttUs
               << ", addrV4: " << toString(neighborAddrV4)
               << ", addrV6: " << toString(neighborAddrV6)
               << ", area: " << area;
  fb303::fbData->addStatValue("link_monitor.neighbor_up", 1, fb303::SUM);

  std::string repUrl{""};
  std::string peerAddr{""};
  if (!mockMode_) {
    // peer address used for KvStore external sync over ZMQ
    repUrl = folly::sformat(
        "tcp://[{}%{}]:{}",
        toString(neighborAddrV6),
        localIfName,
        kvStoreCmdPort);
    // peer address used for KvStore external sync over thrift
    peerAddr = folly::sformat("{}%{}", toString(neighborAddrV6), localIfName);
  } else {
    // use inproc address
    repUrl = folly::sformat("inproc://{}-kvstore-cmd-global", remoteNodeName);
    // TODO: address value of peerAddr under system test environment
    peerAddr = folly::sformat("::1%{}", localIfName);
  }

  CHECK(not repUrl.empty()) << "Got empty repUrl";
  CHECK(not peerAddr.empty()) << "Got empty peerAddr";

  // two cases upon this event:
  // 1) the min interface changes: the previous min interface's connection will
  // be overridden by KvStoreClientInternal, thus no need to explicitly remove
  // it 2) does not change: the existing connection to a neighbor is retained
  auto peerSpec = createPeerSpec(repUrl, peerAddr, openrCtrlThriftPort);
  const auto adjId = std::make_pair(remoteNodeName, localIfName);
  adjacencies_[adjId] =
      AdjacencyValue(area, peerSpec, std::move(newAdj), false);

  // Advertise KvStore peers immediately
  advertiseKvStorePeers(area, {{remoteNodeName, peerSpec}});

  // Advertise new adjancies in a throttled fashion
  advertiseAdjacenciesThrottled_->operator()();
}

void
LinkMonitor::neighborDownEvent(const thrift::SparkNeighborEvent& event) {
  const auto& info = *event.info_ref();
  const auto& remoteNodeName = *info.nodeName_ref();
  const auto& localIfName = *info.localIfName_ref();
  const auto& area = *info.area_ref();

  SYSLOG(INFO) << "Neighbor " << remoteNodeName << " is down on interface "
               << localIfName;
  fb303::fbData->addStatValue("link_monitor.neighbor_down", 1, fb303::SUM);

  const auto adjId = std::make_pair(remoteNodeName, localIfName);
  auto adjValueIt = adjacencies_.find(adjId);
  if (adjValueIt != adjacencies_.end()) {
    // remove such adjacencies
    adjacencies_.erase(adjValueIt);
  }
  // advertise both peers and adjacencies
  advertiseKvStorePeers(area);
  advertiseAdjacencies(area);
}

void
LinkMonitor::neighborRestartingEvent(const thrift::SparkNeighborEvent& event) {
  const auto& info = *event.info_ref();
  const auto& remoteNodeName = *info.nodeName_ref();
  const auto& localIfName = *info.localIfName_ref();
  const auto& area = *info.area_ref();

  SYSLOG(INFO) << "Neighbor " << remoteNodeName
               << " is restarting on interface " << localIfName;
  fb303::fbData->addStatValue(
      "link_monitor.neighbor_restarting", 1, fb303::SUM);

  // update adjacencies_ restarting-bit and advertise peers
  const auto adjId = std::make_pair(remoteNodeName, localIfName);
  auto adjValueIt = adjacencies_.find(adjId);
  if (adjValueIt != adjacencies_.end()) {
    adjValueIt->second.isRestarting = true;
  }
  advertiseKvStorePeers(area);
}

void
LinkMonitor::neighborRttChangeEvent(const thrift::SparkNeighborEvent& event) {
  const auto& info = *event.info_ref();
  const auto& remoteNodeName = *info.nodeName_ref();
  const auto& localIfName = *info.localIfName_ref();
  const auto& rttUs = *info.rttUs_ref();
  int32_t newRttMetric = getRttMetric(rttUs);

  VLOG(1) << "Metric value changed for neighbor " << remoteNodeName
          << " on interface: " << localIfName << " to " << newRttMetric;

  auto it = adjacencies_.find({remoteNodeName, localIfName});
  if (it != adjacencies_.end()) {
    auto& adj = it->second.adjacency;
    adj.metric_ref() = newRttMetric;
    adj.rtt_ref() = rttUs;
    advertiseAdjacenciesThrottled_->operator()();
  }
}

std::unordered_map<std::string, thrift::PeerSpec>
LinkMonitor::getPeersFromAdjacencies(
    const std::unordered_map<AdjacencyKey, AdjacencyValue>& adjacencies,
    const std::string& area) {
  std::unordered_map<std::string, std::string> neighborToIface;
  for (const auto& [adjKey, adjValue] : adjacencies) {
    if (adjValue.area != area or adjValue.isRestarting) {
      continue;
    }
    const auto& nodeName = adjKey.first;
    const auto& iface = adjKey.second;

    // Look up for node
    auto it = neighborToIface.find(nodeName);
    if (it == neighborToIface.end()) {
      // Add nbr-iface if not found
      neighborToIface.emplace(nodeName, iface);
    } else if (it->second > iface) {
      // Update iface if it is smaller (minimum interface)
      it->second = iface;
    }
  }

  std::unordered_map<std::string, thrift::PeerSpec> peers;
  for (const auto& kv : neighborToIface) {
    peers.emplace(kv.first, adjacencies.at(kv).peerSpec);
  }
  return peers;
}

void
LinkMonitor::advertiseKvStorePeers(
    const std::string& area,
    const std::unordered_map<std::string, thrift::PeerSpec>& upPeers) {
  // Prepare peer update request
  thrift::PeerUpdateRequest req;
  *req.area_ref() = area;

  // Get old and new peer list. Also update local state
  const auto oldPeers = std::move(peers_[area]);
  const auto newPeers = getPeersFromAdjacencies(adjacencies_, area);

  // Get list of peers to delete
  std::vector<std::string> toDelPeers;
  for (const auto& [nodeName, peer] : oldPeers) {
    if (newPeers.find(nodeName) == newPeers.end()) {
      toDelPeers.emplace_back(nodeName);
      logPeerEvent("DEL_PEER", nodeName, peer.tPeerSpec);
    }
  }

  // Delete old peers
  if (toDelPeers.size() > 0) {
    thrift::PeerDelParams params;
    *params.peerNames_ref() = std::move(toDelPeers);
    req.peerDelParams_ref() = std::move(params);
  }

  // Get list of peers to add
  std::unordered_map<std::string, thrift::PeerSpec> toAddPeers;
  for (const auto& [nodeName, peerSpec] : newPeers) {
    const auto& oldPeerVal = oldPeers.find(nodeName);

    // In parallel link case, inherit previous kvstore sync state
    // TODO: We should just reserve previous KvStore Peer, instead of getting
    // PeerSpec with smallest name
    // There can only be one kvstore session with a remote node, no need to
    // pull down existing kvstore session with new spec. This causes extra
    // initial syncs.
    bool initialSynced =
        oldPeerVal == oldPeers.end() ? false : oldPeerVal->second.initialSynced;

    peers_[area].emplace(nodeName, KvStorePeerValue(peerSpec, initialSynced));

    // if old peer spec is same as new peer spec, skip
    if (oldPeerVal != oldPeers.end() &&
        oldPeerVal->second.tPeerSpec == peerSpec) {
      continue;
    }

    // send out peer-add to kvstore if
    // 1. it's a new peer (not exist in old-peers)
    // 2. old-peer but peer-spec changed (e.g parallel link case)
    toAddPeers.emplace(nodeName, peerSpec);
    logPeerEvent("ADD_PEER", nodeName, peerSpec);
  }

  for (const auto& [name, spec] : upPeers) {
    // upPeer MUST already be in current state peers_
    CHECK(peers_.at(area).count(name));

    if (toAddPeers.count(name)) {
      // already added, skip it
      continue;
    }
    if (spec != peers_.at(area).at(name).tPeerSpec) {
      // spec does not match, skip it
      continue;
    }
    toAddPeers.emplace(name, spec);
  }

  // Add new peers
  if (toAddPeers.size() > 0) {
    thrift::PeerAddParams params;
    *params.peers_ref() = std::move(toAddPeers);
    req.peerAddParams_ref() = std::move(params);
  }

  if (req.peerDelParams_ref().has_value() ||
      req.peerAddParams_ref().has_value()) {
    peerUpdatesQueue_.push(std::move(req));
  }
}

void
LinkMonitor::advertiseKvStorePeers(
    const std::unordered_map<std::string, thrift::PeerSpec>& upPeers) {
  // Get old and new peer list. Also update local state
  for (const auto& [areaId, _] : areas_) {
    advertiseKvStorePeers(areaId, upPeers);
  }
}

void
LinkMonitor::advertiseAdjacencies(const std::string& area) {
  if (adjHoldTimer_->isScheduled()) {
    return;
  }

  // Cancel throttle timeout if scheduled
  if (advertiseAdjacenciesThrottled_->isActive()) {
    advertiseAdjacenciesThrottled_->cancel();
  }

  // Extract information from `adjacencies_`
  auto adjDb = buildAdjacencyDatabase(area);

  LOG(INFO) << "Updating adjacency database in KvStore with "
            << adjDb.adjacencies_ref()->size() << " entries in area: " << area;

  // Persist `adj:node_Id` key into KvStore via KvStoreClientInternal
  const auto keyName = Constants::kAdjDbMarker.toString() + nodeId_;
  std::string adjDbStr = writeThriftObjStr(adjDb, serializer_);
  kvStoreClient_->persistKey(AreaId{area}, keyName, adjDbStr, ttlKeyInKvStore_);

  // Config is most likely to have changed. Update it in `ConfigStore`
  configStore_->storeThriftObj(kConfigKey, state_); // not awaiting on result

  // Update some flat counters
  fb303::fbData->addStatValue(
      "link_monitor.advertise_adjacencies", 1, fb303::SUM);
  fb303::fbData->setCounter("link_monitor.adjacencies", adjacencies_.size());
  for (const auto& [_, adjValue] : adjacencies_) {
    auto& adj = adjValue.adjacency;
    fb303::fbData->setCounter(
        "link_monitor.metric." + *adj.otherNodeName_ref(), *adj.metric_ref());
  }
}
void
LinkMonitor::advertiseAdjacencies() {
  // advertise to all areas. Once area configuration per link is implemented
  // then adjacencies can be advertised to a specific area
  for (const auto& [areaId, _] : areas_) {
    // Update KvStore
    advertiseAdjacencies(areaId);
  }
}

void
LinkMonitor::advertiseIfaceAddr() {
  auto retryTime = getRetryTimeOnUnstableInterfaces();

  advertiseInterfaces();
  advertiseRedistAddrs();

  // Cancel throttle timeout if scheduled
  if (advertiseIfaceAddrThrottled_->isActive()) {
    advertiseIfaceAddrThrottled_->cancel();
  }

  // Schedule new timeout if needed to advertise UP but UNSTABLE interfaces
  // once their backoff is clear.
  if (retryTime.count() != 0) {
    advertiseIfaceAddrTimer_->scheduleTimeout(retryTime);
    VLOG(2) << "advertiseIfaceAddr timer scheduled in " << retryTime.count()
            << " ms";
  }
}

void
LinkMonitor::advertiseInterfaces() {
  fb303::fbData->addStatValue("link_monitor.advertise_links", 1, fb303::SUM);

  // Create interface database
  thrift::InterfaceDatabase ifDb;
  *ifDb.thisNodeName_ref() = nodeId_;
  for (auto& [ifName, interface] : interfaces_) {
    // Perform regex match
    if (not anyAreaShouldDiscoverOnIface(ifName)) {
      continue;
    }
    // Get interface info and override active status
    auto interfaceInfo = interface.getInterfaceInfo();
    interfaceInfo.isUp_ref() = interface.isActive();
    ifDb.interfaces_ref()->emplace(ifName, std::move(interfaceInfo));
  }

  // publish new interface database to other modules (Fib & Spark)
  interfaceUpdatesQueue_.push(std::move(ifDb));
}

void
LinkMonitor::advertiseRedistAddrs() {
  if (adjHoldTimer_->isScheduled()) {
    return;
  }
  std::unordered_map<std::string, std::vector<thrift::PrefixEntry>>
      areaPrefixes;

  // Add redistribute addresses
  for (auto& [_, interface] : interfaces_) {
    // Ignore in-active interfaces
    if (not interface.isActive()) {
      continue;
    }

    std::vector<thrift::PrefixEntry> ifacePrefixes;
    // Add all prefixes of this interface
    for (auto& prefix : interface.getGlobalUnicastNetworks(enableV4_)) {
      prefix.forwardingType_ref() = prefixForwardingType_;
      prefix.forwardingAlgorithm_ref() = prefixForwardingAlgorithm_;
      // Tags
      {
        auto& tags = prefix.tags_ref().value();
        tags.emplace("INTERFACE_SUBNET");
        tags.emplace(folly::sformat("{}:{}", nodeId_, interface.getIfName()));
      }
      // Metrics
      {
        auto& metrics = prefix.metrics_ref().value();
        metrics.path_preference_ref() = Constants::kDefaultPathPreference;
        metrics.source_preference_ref() = Constants::kDefaultSourcePreference;
      }
      ifacePrefixes.emplace_back(std::move(prefix));
    }

    for (auto const& [areaId, areaConf] : areas_) {
      if (areaConf.shouldRedistributeIface(interface.getIfName())) {
        auto& prefixList = areaPrefixes[areaId];
        prefixList.insert(
            prefixList.end(), ifacePrefixes.begin(), ifacePrefixes.end());
      }
    }
  }

  for (auto const& [areaId, _] : areas_) {
    LOG_IF(INFO, !areaPrefixes.count(areaId) || areaPrefixes.at(areaId).empty())
        << "Advertising empty LOOPBACK addresses for area: " << areaId;
  }

  for (auto& [areaId, _] : areas_) {
    // Advertise via prefix manager client
    thrift::PrefixUpdateRequest request;
    request.set_cmd(thrift::PrefixUpdateCommand::SYNC_PREFIXES_BY_TYPE);
    request.set_type(openr::thrift::PrefixType::LOOPBACK);
    // default construct syncing empty set if we found nothing
    request.set_prefixes(std::move(areaPrefixes[areaId]));
    request.set_dstAreas({areaId});
    // publish prefixes to prefix manager
    prefixUpdatesQueue_.push(std::move(request));
  }
}

std::chrono::milliseconds
LinkMonitor::getRetryTimeOnUnstableInterfaces() {
  std::chrono::milliseconds minRemainMs{0};
  for (auto& [_, interface] : interfaces_) {
    if (interface.isActive()) {
      continue;
    }

    const auto& curRemainMs = interface.getBackoffDuration();
    if (curRemainMs.count() > 0) {
      VLOG(2) << "Interface " << interface.getIfName()
              << " is in backoff state for " << curRemainMs.count() << "ms";
      minRemainMs = std::min(linkflapMaxBackoff_, curRemainMs);
    }
  }

  return minRemainMs;
}

thrift::AdjacencyDatabase
LinkMonitor::buildAdjacencyDatabase(const std::string& area) {
  // prepare adjacency database
  thrift::AdjacencyDatabase adjDb;

  *adjDb.thisNodeName_ref() = nodeId_;
  adjDb.isOverloaded_ref() = *state_.isOverloaded_ref();
  adjDb.nodeLabel_ref() = enableSegmentRouting_ ? *state_.nodeLabel_ref() : 0;
  *adjDb.area_ref() = area;

  for (const auto& [_, adjValue] : adjacencies_) {
    // ignore unrelated area
    if (adjValue.area != area) {
      continue;
    }
    // NOTE: copy on purpose
    auto adj = folly::copy(adjValue.adjacency);

    // Set link overload bit
    adj.isOverloaded_ref() =
        state_.overloadedLinks_ref()->count(*adj.ifName_ref()) > 0;

    // Override metric with link metric if it exists
    adj.metric_ref() = folly::get_default(
        *state_.linkMetricOverrides_ref(),
        *adj.ifName_ref(),
        *adj.metric_ref());

    // Override metric with adj metric if it exists
    thrift::AdjKey adjKey;
    *adjKey.nodeName_ref() = *adj.otherNodeName_ref();
    *adjKey.ifName_ref() = *adj.ifName_ref();
    adj.metric_ref() = folly::get_default(
        *state_.adjMetricOverrides_ref(), adjKey, *adj.metric_ref());

    adjDb.adjacencies_ref()->emplace_back(std::move(adj));
  }

  // Add perf information if enabled
  if (enablePerfMeasurement_) {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, nodeId_, "ADJ_DB_UPDATED");
    adjDb.perfEvents_ref() = perfEvents;
  } else {
    DCHECK(!adjDb.perfEvents_ref().has_value());
  }

  return adjDb;
}

InterfaceEntry* FOLLY_NULLABLE
LinkMonitor::getOrCreateInterfaceEntry(const std::string& ifName) {
  // Return null if ifName doesn't quality regex match criteria
  if (not anyAreaShouldDiscoverOnIface(ifName) &&
      not anyAreaShouldRedistributeIface(ifName)) {
    return nullptr;
  }

  // Return existing element if any
  auto it = interfaces_.find(ifName);
  if (it != interfaces_.end()) {
    return &(it->second);
  }

  // Create one and return it's reference
  auto res = interfaces_.emplace(
      ifName,
      InterfaceEntry(
          ifName,
          linkflapInitBackoff_,
          linkflapMaxBackoff_,
          *advertiseIfaceAddrThrottled_,
          *advertiseIfaceAddrTimer_));

  return &(res.first->second);
}

bool
LinkMonitor::syncInterfaces() {
  VLOG(1) << "Syncing Interface DB from Netlink Platform";

  // Retrieve latest link snapshot from NetlinkProtocolSocket
  std::vector<LinkEntry> links;
  try {
    links = getAllLinks().get();
  } catch (const std::exception& e) {
    LOG(ERROR) << "Failed to sync linkDb from NetlinkProtocolSocket. Error: "
               << folly::exceptionStr(e);
    return false;
  }

  // Make updates in InterfaceEntry objects
  for (const auto& link : links) {
    // update cache of ifIndex -> ifName mapping
    //  1) if ifIndex exists, override it with new ifName;
    //  2) if ifIndex does NOT exist, cache the ifName;
    ifIndexToName_[link.ifIndex] = link.ifName;

    // Get interface entry
    auto interfaceEntry = getOrCreateInterfaceEntry(link.ifName);
    if (not interfaceEntry) {
      continue;
    }

    const std::unordered_set<folly::CIDRNetwork> oldNetworks =
        interfaceEntry->getNetworks(); // NOTE: Copy intended
    std::unordered_set<folly::CIDRNetwork> newNetworks;
    for (const auto& network : link.networks) {
      newNetworks.emplace(network);
    }

    // Update link attributes
    const bool wasUp = interfaceEntry->isUp();
    interfaceEntry->updateAttrs(link.ifIndex, link.isUp, link.weight);
    logLinkEvent(
        interfaceEntry->getIfName(),
        wasUp,
        interfaceEntry->isUp(),
        interfaceEntry->getBackoffDuration());

    // Remove old addresses if they are not in new
    for (auto const& oldNetwork : oldNetworks) {
      if (newNetworks.count(oldNetwork) == 0) {
        interfaceEntry->updateAddr(oldNetwork, false);
      }
    }

    // Add new addresses if they are not in old
    for (auto const& newNetwork : newNetworks) {
      if (oldNetworks.count(newNetwork) == 0) {
        interfaceEntry->updateAddr(newNetwork, true);
      }
    }
  }
  return true;
}

void
LinkMonitor::processNetlinkEvent(fbnl::NetlinkEvent&& event) {
  if (auto* link = std::get_if<fbnl::Link>(&event)) {
    VLOG(3) << "Received Link Event from NetlinkProtocolSocket...";

    auto ifName = link->getLinkName();
    auto ifIndex = link->getIfIndex();
    auto isUp = link->isUp();

    // Cache interface index name mapping
    // ATTN: will create new ifIndex -> ifName mapping if it is unknown link
    //       `[]` operator is used in purpose
    ifIndexToName_[ifIndex] = ifName;

    auto interfaceEntry = getOrCreateInterfaceEntry(ifName);
    if (interfaceEntry) {
      const bool wasUp = interfaceEntry->isUp();
      interfaceEntry->updateAttrs(ifIndex, isUp, Constants::kDefaultAdjWeight);
      logLinkEvent(
          interfaceEntry->getIfName(),
          wasUp,
          interfaceEntry->isUp(),
          interfaceEntry->getBackoffDuration());
    }
  } else if (auto* addr = std::get_if<fbnl::IfAddress>(&event)) {
    VLOG(3) << "Received Address Event from NetlinkProtocolSocket...";

    auto ifIndex = addr->getIfIndex();
    auto prefix = addr->getPrefix(); // std::optional<folly::CIDRNetwork>
    auto isValid = addr->isValid();

    // Check for interface name
    auto it = ifIndexToName_.find(ifIndex);
    if (it == ifIndexToName_.end()) {
      LOG(ERROR) << "Address event for unknown iface index: " << ifIndex;
      return;
    }

    // Cached ifIndex -> ifName mapping
    auto interfaceEntry = getOrCreateInterfaceEntry(it->second);
    if (interfaceEntry) {
      interfaceEntry->updateAddr(prefix.value(), isValid);
    }
  }
}

void
LinkMonitor::processNeighborEvent(thrift::SparkNeighborEvent&& event) {
  const auto& info = *event.info_ref();
  const auto& neighborAddrV4 = *info.transportAddressV4_ref();
  const auto& neighborAddrV6 = *info.transportAddressV6_ref();
  const auto& localIfName = *info.localIfName_ref();
  const auto& remoteIfName = *info.remoteIfName_ref();
  const auto& remoteNodeName = *info.nodeName_ref();
  const auto& area = *info.area_ref();

  VLOG(1) << "Received neighbor event for " << remoteNodeName << " from "
          << remoteIfName << " at " << localIfName << " with addrs "
          << toString(neighborAddrV6) << " and "
          << (enableV4_ ? toString(neighborAddrV4) : "") << " Area:" << area
          << " Event Type: "
          << apache::thrift::util::enumNameSafe(*event.eventType_ref());

  switch (*event.eventType_ref()) {
  case thrift::SparkNeighborEventType::NEIGHBOR_UP:
  case thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED: {
    logNeighborEvent(event);
    neighborUpEvent(event);
    break;
  }
  case thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING: {
    logNeighborEvent(event);
    neighborRestartingEvent(event);
    break;
  }
  case thrift::SparkNeighborEventType::NEIGHBOR_DOWN: {
    logNeighborEvent(event);
    neighborDownEvent(event);
    break;
  }
  case thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE: {
    if (!useRttMetric_) {
      break;
    }
    logNeighborEvent(event);
    neighborRttChangeEvent(event);
    break;
  }
  default:
    LOG(ERROR) << "Unknown event type " << (int32_t)*event.eventType_ref();
  }
}

// NOTE: add commands which set/unset overload bit or metric values will
// immediately advertise new adjacencies into the KvStore.
folly::SemiFuture<folly::Unit>
LinkMonitor::setNodeOverload(bool isOverloaded) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), isOverloaded]() mutable {
    std::string cmd =
        isOverloaded ? "SET_NODE_OVERLOAD" : "UNSET_NODE_OVERLOAD";
    if (*state_.isOverloaded_ref() == isOverloaded) {
      LOG(INFO) << "Skip cmd: [" << cmd << "]. Node already in target state: ["
                << (isOverloaded ? "OVERLOADED" : "NOT OVERLOADED") << "]";
    } else {
      state_.isOverloaded_ref() = isOverloaded;
      SYSLOG(INFO) << (isOverloaded ? "Setting" : "Unsetting")
                   << " overload bit for node";
      advertiseAdjacencies();
    }
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::setInterfaceOverload(
    std::string interfaceName, bool isOverloaded) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        interfaceName,
                        isOverloaded]() mutable {
    std::string cmd =
        isOverloaded ? "SET_LINK_OVERLOAD" : "UNSET_LINK_OVERLOAD";
    if (0 == interfaces_.count(interfaceName)) {
      LOG(ERROR) << "Skip cmd: [" << cmd
                 << "] due to unknown interface: " << interfaceName;
      p.setValue();
      return;
    }

    if (isOverloaded && state_.overloadedLinks_ref()->count(interfaceName)) {
      LOG(INFO) << "Skip cmd: [" << cmd << "]. Interface: " << interfaceName
                << " is already overloaded";
      p.setValue();
      return;
    }

    if (!isOverloaded && !state_.overloadedLinks_ref()->count(interfaceName)) {
      LOG(INFO) << "Skip cmd: [" << cmd << "]. Interface: " << interfaceName
                << " is currently NOT overloaded";
      p.setValue();
      return;
    }

    if (isOverloaded) {
      state_.overloadedLinks_ref()->insert(interfaceName);
      SYSLOG(INFO) << "Setting overload bit for interface " << interfaceName;
    } else {
      state_.overloadedLinks_ref()->erase(interfaceName);
      SYSLOG(INFO) << "Unsetting overload bit for interface " << interfaceName;
    }
    advertiseAdjacenciesThrottled_->operator()();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::setLinkMetric(
    std::string interfaceName, std::optional<int32_t> overrideMetric) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), interfaceName, overrideMetric]() mutable {
        std::string cmd = overrideMetric.has_value() ? "SET_LINK_METRIC"
                                                     : "UNSET_LINK_METRIC";
        if (0 == interfaces_.count(interfaceName)) {
          LOG(ERROR) << "Skip cmd: [" << cmd
                     << "] due to unknown interface: " << interfaceName;
          p.setValue();
          return;
        }

        if (overrideMetric.has_value() &&
            state_.linkMetricOverrides_ref()->count(interfaceName) &&
            state_.linkMetricOverrides_ref()[interfaceName] ==
                overrideMetric.value()) {
          LOG(INFO) << "Skip cmd: " << cmd
                    << ". Overridden metric: " << overrideMetric.value()
                    << " already set for interface: " << interfaceName;
          p.setValue();
          return;
        }

        if (!overrideMetric.has_value() &&
            !state_.linkMetricOverrides_ref()->count(interfaceName)) {
          LOG(INFO) << "Skip cmd: " << cmd
                    << ". No overridden metric found for interface: "
                    << interfaceName;
          p.setValue();
          return;
        }

        if (overrideMetric.has_value()) {
          state_.linkMetricOverrides_ref()[interfaceName] =
              overrideMetric.value();
          SYSLOG(INFO) << "Overriding metric for interface " << interfaceName
                       << " to " << overrideMetric.value();
        } else {
          state_.linkMetricOverrides_ref()->erase(interfaceName);
          SYSLOG(INFO) << "Removing metric override for interface "
                       << interfaceName;
        }
        advertiseAdjacenciesThrottled_->operator()();
        p.setValue();
      });
  return sf;
}

folly::SemiFuture<folly::Unit>
LinkMonitor::setAdjacencyMetric(
    std::string interfaceName,
    std::string adjNodeName,
    std::optional<int32_t> overrideMetric) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        interfaceName,
                        adjNodeName,
                        overrideMetric]() mutable {
    std::string cmd = overrideMetric.has_value() ? "SET_ADJACENCY_METRIC"
                                                 : "UNSET_ADJACENCY_METRIC";
    thrift::AdjKey adjKey;
    *adjKey.ifName_ref() = interfaceName;
    *adjKey.nodeName_ref() = adjNodeName;

    // Invalid adj encountered. Ignore
    if (!adjacencies_.count(std::make_pair(adjNodeName, interfaceName))) {
      LOG(ERROR) << "Skip cmd: [" << cmd << "] due to unknown adj: ["
                 << adjNodeName << ":" << interfaceName << "]";
      p.setValue();
      return;
    }

    if (overrideMetric.has_value() &&
        state_.adjMetricOverrides_ref()->count(adjKey) &&
        state_.adjMetricOverrides_ref()[adjKey] == overrideMetric.value()) {
      LOG(INFO) << "Skip cmd: " << cmd
                << ". Overridden metric: " << overrideMetric.value()
                << " already set for: [" << adjNodeName << ":" << interfaceName
                << "]";
      p.setValue();
      return;
    }

    if (!overrideMetric.has_value() &&
        !state_.adjMetricOverrides_ref()->count(adjKey)) {
      LOG(INFO) << "Skip cmd: " << cmd << ". No overridden metric found for: ["
                << adjNodeName << ":" << interfaceName << "]";
      p.setValue();
      return;
    }

    if (overrideMetric.has_value()) {
      state_.adjMetricOverrides_ref()[adjKey] = overrideMetric.value();
      SYSLOG(INFO) << "Overriding metric for adjacency: [" << adjNodeName << ":"
                   << interfaceName << "] to " << overrideMetric.value();
    } else {
      state_.adjMetricOverrides_ref()->erase(adjKey);
      SYSLOG(INFO) << "Removing metric override for adjacency: [" << adjNodeName
                   << ":" << interfaceName << "]";
    }
    advertiseAdjacenciesThrottled_->operator()();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>>
LinkMonitor::getInterfaces() {
  VLOG(2) << "Dump Links requested, replying withV " << interfaces_.size()
          << " links";

  folly::Promise<std::unique_ptr<thrift::DumpLinksReply>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable {
    // reply with the dump of known interfaces and their states
    thrift::DumpLinksReply reply;
    *reply.thisNodeName_ref() = nodeId_;
    reply.isOverloaded_ref() = *state_.isOverloaded_ref();

    // Fill interface details
    for (auto& [ifName, interface] : interfaces_) {
      auto ifDetails = thrift::InterfaceDetails(
          apache::thrift::FRAGILE,
          interface.getInterfaceInfo(),
          state_.overloadedLinks_ref()->count(ifName) > 0,
          0 /* custom metric value */,
          0 /* link flap back off time */);

      // Add metric override if any
      folly::Optional<int32_t> maybeMetric;
      if (state_.linkMetricOverrides_ref()->count(ifName) > 0) {
        maybeMetric.assign(state_.linkMetricOverrides_ref()->at(ifName));
      }
      apache::thrift::fromFollyOptional(
          ifDetails.metricOverride_ref(), maybeMetric);

      // Add link-backoff
      auto backoffMs = interface.getBackoffDuration();
      if (backoffMs.count() != 0) {
        ifDetails.linkFlapBackOffMs_ref() = backoffMs.count();
      } else {
        ifDetails.linkFlapBackOffMs_ref().reset();
      }

      reply.interfaceDetails_ref()->emplace(ifName, std::move(ifDetails));
    }
    p.setValue(std::make_unique<thrift::DumpLinksReply>(std::move(reply)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>>
LinkMonitor::getAdjacencies(thrift::AdjacenciesFilter filter) {
  VLOG(2) << "Dump adj requested, reply with " << adjacencies_.size()
          << " adjs";

  folly::Promise<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), filter = std::move(filter)]() mutable {
        auto res = std::make_unique<std::vector<thrift::AdjacencyDatabase>>();
        if (filter.get_selectAreas().empty()) {
          for (auto const& [areaId, _] : areas_) {
            res->push_back(buildAdjacencyDatabase(areaId));
          }
        } else {
          for (auto const& areaId : filter.get_selectAreas()) {
            res->push_back(buildAdjacencyDatabase(areaId));
          }
        }
        p.setValue(std::move(res));
      });
  return sf;
}

folly::SemiFuture<std::vector<LinkEntry>>
LinkMonitor::getAllLinks() {
  VLOG(2) << "Querying all links and their addresses from system";
  return collectAll(nlSock_->getAllLinks(), nlSock_->getAllIfAddresses())
      .deferValue(
          [](std::tuple<
              folly::Try<folly::Expected<std::vector<fbnl::Link>, int>>,
              folly::Try<folly::Expected<std::vector<fbnl::IfAddress>, int>>>&&
                 res) {
            std::unordered_map<int, LinkEntry> links;
            // Create links
            auto nlLinks = std::get<0>(res).value();
            if (nlLinks.hasError()) {
              throw fbnl::NlException("Failed fetching links", nlLinks.error());
            }
            for (auto& nlLink : nlLinks.value()) {
              // explicitly constuct linkEntry with EMPTY addresses
              LinkEntry link(
                  nlLink.getLinkName(), nlLink.getIfIndex(), nlLink.isUp(), {});
              links.emplace(nlLink.getIfIndex(), std::move(link));
            }

            // Add addresses
            auto nlAddrs = std::get<1>(res).value();
            if (nlAddrs.hasError()) {
              throw fbnl::NlException("Failed fetching addrs", nlAddrs.error());
            }
            for (auto& nlAddr : nlAddrs.value()) {
              auto& link = links.at(nlAddr.getIfIndex());
              link.networks.emplace_back(nlAddr.getPrefix().value());
            }

            // Convert to list and return
            std::vector<LinkEntry> result{};
            for (auto& [_, link] : links) {
              result.emplace_back(std::move(link));
            }
            return result;
          });
}

void
LinkMonitor::logNeighborEvent(thrift::SparkNeighborEvent const& event) {
  LogSample sample{};
  sample.addString(
      "event",
      apache::thrift::TEnumTraits<thrift::SparkNeighborEventType>::findName(
          *event.eventType_ref()));
  sample.addString("neighbor", *(event.info_ref()->nodeName_ref()));
  sample.addString("interface", *(event.info_ref()->localIfName_ref()));
  sample.addString("remote_interface", *(event.info_ref()->remoteIfName_ref()));
  sample.addString("area", *(event.info_ref()->area_ref()));
  sample.addInt("rtt_us", *(event.info_ref()->rttUs_ref()));

  logSampleQueue_.push(std::move(sample));
}

void
LinkMonitor::logLinkEvent(
    const std::string& iface,
    bool wasUp,
    bool isUp,
    std::chrono::milliseconds backoffTime) {
  // Do not log if no state transition
  if (wasUp == isUp) {
    return;
  }

  LogSample sample{};
  const std::string event = isUp ? "UP" : "DOWN";

  sample.addString("event", folly::sformat("IFACE_{}", event));
  sample.addString("interface", iface);
  sample.addInt("backoff_ms", backoffTime.count());

  logSampleQueue_.push(sample);

  SYSLOG(INFO) << "Interface " << iface << " is " << event
               << " and has backoff of " << backoffTime.count() << "ms";
}

void
LinkMonitor::logPeerEvent(
    const std::string& event,
    const std::string& peerName,
    const thrift::PeerSpec& peerSpec) {
  LogSample sample{};

  sample.addString("event", event);
  sample.addString("node_name", nodeId_);
  sample.addString("peer_name", peerName);
  sample.addString("cmd_url", *peerSpec.cmdUrl_ref());

  logSampleQueue_.push(sample);
}

bool
LinkMonitor::anyAreaShouldDiscoverOnIface(std::string const& iface) const {
  bool anyMatch = false;
  for (auto const& [_, areaConf] : areas_) {
    anyMatch |= areaConf.shouldDiscoverOnIface(iface);
  }
  return anyMatch;
}

bool
LinkMonitor::anyAreaShouldRedistributeIface(std::string const& iface) const {
  bool anyMatch = false;
  for (auto const& [_, areaConf] : areas_) {
    anyMatch |= areaConf.shouldRedistributeIface(iface);
  }
  return anyMatch;
}

} // namespace openr
