/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/platforms/wedge/WedgeTomahawkPlatform.h"
#include "fboss/agent/platforms/common/PlatformProductInfo.h"

namespace facebook {
namespace fboss {

WedgeTomahawkPlatform::WedgeTomahawkPlatform(
    std::unique_ptr<PlatformProductInfo> productInfo)
    : WedgePlatform(std::move(productInfo)) {
  asic_ = std::make_unique<TomahawkAsic>();
}
const PortQueue& WedgeTomahawkPlatform::getDefaultPortQueueSettings(
    cfg::StreamType /*streamType*/) const {
  throw FbossError("PortQueue setting is not supported");
}

const PortQueue& WedgeTomahawkPlatform::getDefaultControlPlaneQueueSettings(
    cfg::StreamType /*streamType*/) const {
  throw FbossError("PortQueue setting is not supported");
}
} // namespace fboss
} // namespace facebook
