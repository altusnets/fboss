// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/agent/hw/bcm/BcmTypes.h"

namespace facebook {
namespace fboss {

struct BcmMplsTunnelSwitchImplT {};

BcmMplsTunnelSwitchT::BcmMplsTunnelSwitchT()
    : impl_(std::make_unique<BcmMplsTunnelSwitchImplT>()) {}

BcmMplsTunnelSwitchT::~BcmMplsTunnelSwitchT() {}

BcmMplsTunnelSwitchImplT* BcmMplsTunnelSwitchT::get() {
  return impl_.get();
}

} // namespace fboss
} // namespace facebook
