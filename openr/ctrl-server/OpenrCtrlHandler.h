/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/OpenrCtrl.h>
#include <openr/common/OpenrEventLoop.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/zmq/Zmq.h>


namespace openr {
class OpenrCtrlHandler final : public thrift::OpenrCtrlSvIf {
 public:
  OpenrCtrlHandler(
    const std::string& nodeName,
    bool autheticatePeerCommonName,
    const std::unordered_set<std::string>& acceptablePeerCommonNames,
    std::unordered_map<
        thrift::OpenrModuleType,
        std::shared_ptr<OpenrEventLoop>>& moduleTypeToEvl,
    MonitorSubmitUrl const& monitorSubmitUrl,
    fbzmq::Context& context);

  void command(
    std::string& response,
    thrift::OpenrModuleType type,
    std::unique_ptr<std::string> request) override;

  bool hasModule(thrift::OpenrModuleType type) override;

 private:
  void authenticateConnection();
  const std::string nodeName_;
  const bool autheticatePeerCommonName_{false};
  const std::unordered_set<std::string> acceptablePeerCommonNames_;
  std::unordered_map<
      thrift::OpenrModuleType,
      std::shared_ptr<OpenrEventLoop>> moduleTypeToEvl_;
  std::unordered_map<
    thrift::OpenrModuleType,
    fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>> moduleSockets_;

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

}; // class OpenrCtrlHandler
} // namespace openr
