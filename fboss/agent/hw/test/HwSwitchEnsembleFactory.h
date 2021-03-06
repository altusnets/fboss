/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once

#include <memory>

namespace facebook {
namespace fboss {
class HwSwitchEnsemble;
/*
 * API to create appropriate HW ensemble based on the platform
 * This provides a runtime hook to dynamically plug in various
 * HwSwitch, platform implementations and then interact with
 * them through the generic HwSwitchEnsemble (and contained
 * members - HwSwitch, Platform) interfaces.
 */

std::unique_ptr<HwSwitchEnsemble> createHwEnsemble(uint32_t featuresDesired);
} // namespace fboss
} // namespace facebook
