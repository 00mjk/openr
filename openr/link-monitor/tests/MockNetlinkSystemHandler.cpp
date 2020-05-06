/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MockNetlinkSystemHandler.h"

#include <algorithm>
#include <functional>
#include <thread>
#include <utility>

#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/futures/Promise.h>
#include <folly/gen/Base.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/NetworkUtil.h>
#include <openr/nl/NetlinkTypes.h>

extern "C" {
#include <net/if.h>
}

using apache::thrift::FRAGILE;

namespace openr {

MockNetlinkSystemHandler::MockNetlinkSystemHandler(
    fbzmq::Context& context, const std::string& platformPublisherUrl) {
  VLOG(3) << "Building Mock NL Db";

  nlSock_ = std::make_unique<fbnl::FakeNetlinkProtocolSocket>(&evl_);

  platformPublisher_ = std::make_unique<PlatformPublisher>(
      context, PlatformPublisherUrl{platformPublisherUrl}, nlSock_.get());
}

void
MockNetlinkSystemHandler::getAllLinks(std::vector<thrift::Link>& linkDb) {
  VLOG(3) << "Query links from Netlink according to link name";
  SYNCHRONIZED(linkDb_) {
    for (const auto link : linkDb_) {
      thrift::Link linkEntry;
      linkEntry.ifName = link.first;
      linkEntry.ifIndex = link.second.ifIndex;
      linkEntry.isUp = link.second.isUp;
      for (const auto network : link.second.networks) {
        linkEntry.networks.push_back(thrift::IpPrefix(
            FRAGILE, toBinaryAddress(network.first), network.second));
      }
      linkDb.push_back(linkEntry);
    }
  }
}

void
MockNetlinkSystemHandler::sendLinkEvent(
    const std::string& ifName, const uint64_t ifIndex, const bool isUp) {
  // Update linkDb_
  SYNCHRONIZED(linkDb_) {
    if (!linkDb_.count(ifName)) {
      fbnl::LinkAttribute newLinkEntry;
      newLinkEntry.isUp = isUp;
      newLinkEntry.ifIndex = ifIndex;
      linkDb_.emplace(ifName, newLinkEntry);
    } else {
      auto& link = linkDb_.at(ifName);
      link.isUp = isUp;
      CHECK_EQ(link.ifIndex, ifIndex) << "Interface index changed";
    }
  }

  // Send event to NetlinkProtocolSocket
  fbnl::LinkBuilder builder;
  builder.setLinkName(ifName);
  builder.setIfIndex(ifIndex);
  builder.setFlags(isUp ? IFF_RUNNING : 0);
  nlSock_->addLink(builder.build()).get();
}

void
MockNetlinkSystemHandler::sendAddrEvent(
    const std::string& ifName, const std::string& prefix, const bool isValid) {
  const auto ipNetwork = folly::IPAddress::createNetwork(prefix, -1, false);

  // Update linkDb_
  std::optional<int> ifIndex;
  SYNCHRONIZED(linkDb_) {
    auto& link = linkDb_.at(ifName);
    ifIndex = link.ifIndex;
    if (isValid) {
      link.networks.insert(ipNetwork);
    } else {
      link.networks.erase(ipNetwork);
    }
  }

  // Send event to NetlinkProtocolSocket
  CHECK(ifIndex.has_value()) << "Uknown interface";
  fbnl::IfAddressBuilder builder;
  builder.setIfIndex(ifIndex.value());
  builder.setPrefix(ipNetwork);
  builder.setValid(isValid);
  if (isValid) {
    nlSock_->addIfAddress(builder.build()).get();
  } else {
    nlSock_->deleteIfAddress(builder.build()).get();
  }
}

void
MockNetlinkSystemHandler::stop() {
  platformPublisher_->stop();
}

} // namespace openr
