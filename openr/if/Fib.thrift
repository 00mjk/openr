/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace php Openr
namespace py openr.Fib

include "Network.thrift"
include "Lsdb.thrift"

struct RouteDatabase {
  1: string thisNodeName
  3: optional Lsdb.PerfEvents perfEvents;
  4: list<Network.UnicastRoute> unicastRoutes
  5: list<Network.MplsRoute> mplsRoutes
}

// Perf log buffer maintained by Fib
struct PerfDatabase {
  1: string thisNodeName
  2: list<Lsdb.PerfEvents> eventInfo
}

enum FibCommand {
  ROUTE_DB_GET = 1,
  PERF_DB_GET = 2,
}

struct FibRequest {
  1: FibCommand cmd
}
