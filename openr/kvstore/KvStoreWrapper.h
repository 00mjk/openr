/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>

#include <fbzmq/zmq/Zmq.h>
#include <folly/Memory.h>
#include <folly/Optional.h>

#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/monitor/LogSample.h>

namespace openr {

/**
 * A utility class to wrap and interact with KvStore. It exposes the APIs to
 * send commands to and receive publications from KvStore.
 * Mainly used for testing.
 *
 * Not thread-safe, use from the same thread only.
 */
class KvStoreWrapper {
 public:
  KvStoreWrapper(
      fbzmq::Context& zmqContext,
      std::shared_ptr<const Config> config,
      std::optional<messaging::RQueue<thrift::PeerUpdateRequest>>
          peerUpdatesQueue = std::nullopt,
      bool enableKvStoreThrift = false);

  ~KvStoreWrapper() {
    stop();
  }

  /**
   * Synchronous APIs to run and stop KvStore. This creates a thread
   * and stop it on destruction.
   *
   * Synchronous => function call with return only after thread is
   *                running/stopped completely.
   */
  void run() noexcept;
  void stop();

  /**
   * Get reader for KvStore updates queue
   */
  messaging::RQueue<thrift::Publication>
  getReader() {
    return kvStoreUpdatesQueue_.getReader();
  }

  /**
   * Get reader for KvStore Initial Sync queue
   */
  messaging::RQueue<KvStoreSyncEvent>
  getInitialSyncEventsReader() {
    return kvStoreSyncEventsQueue_.getReader();
  }

  void
  openQueue() {
    kvStoreUpdatesQueue_.open();
    kvStoreSyncEventsQueue_.open();
  }

  void
  closeQueue() {
    kvStoreUpdatesQueue_.close();
    kvStoreSyncEventsQueue_.close();
  }

  /**
   * APIs to set key-value into the KvStore. Returns true on success else
   * returns false.
   */
  bool setKey(
      AreaId const& area,
      std::string key,
      thrift::Value value,
      std::optional<std::vector<std::string>> nodeIds = std::nullopt);

  /**
   * API to retrieve an existing key-value from KvStore. Returns empty if none
   * exists.
   */
  std::optional<thrift::Value> getKey(AreaId const& area, std::string key);

  /**
   * APIs to set key-values into the KvStore. Returns true on success else
   * returns false.
   */
  bool setKeys(
      AreaId const& area,
      const std::vector<std::pair<std::string, thrift::Value>>& keyVals,
      std::optional<std::vector<std::string>> nodeIds = std::nullopt);

  /**
   * API to get dump from KvStore.
   * if we pass a prefix, only return keys that match it
   */
  std::unordered_map<std::string /* key */, thrift::Value> dumpAll(
      AreaId const& area, std::optional<KvStoreFilters> filters = std::nullopt);

  /**
   * API to get dump hashes from KvStore.
   * if we pass a prefix, only return keys that match it
   */
  std::unordered_map<std::string /* key */, thrift::Value> dumpHashes(
      AreaId const& area, std::string const& prefix = "");

  /**
   * API to get key vals on which hash differs from provided keyValHashes.
   */
  std::unordered_map<std::string /* key */, thrift::Value> syncKeyVals(
      AreaId const& area, thrift::KeyVals const& keyValHashes);

  /**
   * API to listen for a publication on PUB queue.
   */
  thrift::Publication recvPublication();

  /*
   * API to read initial sync event from kvStoreSyncEventsQueue
   */
  KvStoreSyncEvent recvSyncEvent();

  /*
   * Get flooding topology information
   */
  thrift::SptInfos getFloodTopo(AreaId const& area);

  /**
   * APIs to manage (add/remove) KvStore peers. Returns true on success else
   * returns false.
   */
  bool addPeer(AreaId const& area, std::string peerName, thrift::PeerSpec spec);
  bool delPeer(AreaId const& area, std::string peerName);

  std::optional<KvStorePeerState> getPeerState(
      AreaId const& area, std::string const& peerName);

  /**
   * APIs to get existing peers of a KvStore.
   */
  std::unordered_map<std::string /* peerName */, thrift::PeerSpec> getPeers(
      AreaId const& area);

  /**
   * Utility function to get peer-spec for owned KvStore
   */
  thrift::PeerSpec
  getPeerSpec() const {
    return createPeerSpec(
        globalCmdUrl, /* cmdUrl for ZMQ */
        "", /* peerAddr for thrift */
        0 /* port for thrift */);
  }

  /**
   * Get counters from KvStore
   */
  std::map<std::string, int64_t>
  getCounters() {
    return kvStore_->getCounters().get();
  }

  KvStore*
  getKvStore() {
    return kvStore_.get();
  }

  const std::string
  getNodeId() {
    return this->nodeId;
  }

 private:
  const std::string nodeId;

  // Global URLs could be created outside of kvstore, mainly for testing
  const std::string globalCmdUrl;

  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  apache::thrift::CompactSerializer serializer_;

  // Queue for streaming KvStore updates
  messaging::ReplicateQueue<thrift::Publication> kvStoreUpdatesQueue_;
  messaging::RQueue<thrift::Publication> kvStoreUpdatesQueueReader_{
      kvStoreUpdatesQueue_.getReader()};

  // Queue to get KvStore Initial Sync Updates
  messaging::ReplicateQueue<KvStoreSyncEvent> kvStoreSyncEventsQueue_;
  messaging::RQueue<KvStoreSyncEvent> kvStoreSyncEventsQueueReader_{
      kvStoreSyncEventsQueue_.getReader()};

  // Queue for publishing the event log
  messaging::ReplicateQueue<LogSample> logSampleQueue_;

  // Queue for streaming peer updates from LM
  messaging::ReplicateQueue<thrift::PeerUpdateRequest> dummyPeerUpdatesQueue_;

  // KvStore owned by this wrapper.
  std::unique_ptr<KvStore> kvStore_;

  // Thread in which KvStore will be running.
  std::thread kvStoreThread_;

  // enable kvStore over thrift or not
  const bool enableKvStoreThrift_{false};
};

} // namespace openr
