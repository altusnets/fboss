/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/lib/usb/BaseWedgeI2CBus.h"

#include <glog/logging.h>

#include "fboss/lib/usb/UsbError.h"

using folly::MutableByteRange;
using std::lock_guard;

namespace facebook {
namespace fboss {

void BaseWedgeI2CBus::open() {
  dev_->open();

  selectedPort_ = NO_PORT;
  verifyBus(true);
  initBus();

  VLOG(4) << "successfully opened wedge CP2112 I2C bus";
}

void BaseWedgeI2CBus::close() {
  dev_->close();
}

void BaseWedgeI2CBus::read(uint8_t address, int offset, int len, uint8_t* buf) {
  CHECK_LE(offset, 255);

  // CP2112 uses addresses in the on-the-wire format, while we generally
  // pass them around in Linux standard format.
  address <<= 1;

  // Note that we don't use the writeRead() command, since this
  // locks up the CP2112 chip if it times out.  We perform a separate write,
  // followed by a read.  This releases the I2C bus between operations, but
  // that's okay since there aren't any other master devices on the bus.

  // Also note that we can't read more than 128 bytes at a time.
  dev_->writeByte(address, offset);
  if (len > 128) {
    dev_->read(address, MutableByteRange(buf, 128));
    dev_->writeByte(address, offset + 128);
    dev_->read(address, MutableByteRange(buf + 128, len - 128));
  } else {
    dev_->read(address, MutableByteRange(buf, len));
  }
}

void BaseWedgeI2CBus::write(
    uint8_t address,
    int offset,
    int len,
    const uint8_t* buf) {
  CHECK_LE(offset, 255);

  // CP2112 uses addresses in the on-the-wire format, while we generally
  // pass them around in Linux standard format.
  address <<= 1;

  // The CP2112 can only write 61 bytes at a time, and we burn one for
  // the offset
  CHECK_LE(len, 60);

  // XXX:  surely there's an easier way to do this?
  uint8_t output[61]; // USB buffer size;
  output[0] = offset;
  memcpy(output + 1, buf, len);
  dev_->write(address, MutableByteRange(output, len + 1));
}

void BaseWedgeI2CBus::moduleRead(
    unsigned int module,
    uint8_t address,
    int offset,
    int len,
    uint8_t* buf) {
  selectQsfp(module);
  CHECK_NE(selectedPort_, NO_PORT);

  read(address, offset, len, buf);

  // TODO: remove this after we ensure exclusive access to cp2112 chip
  unselectQsfp();
}

void BaseWedgeI2CBus::moduleWrite(
    unsigned int module,
    uint8_t address,
    int offset,
    int len,
    const uint8_t* buf) {
  selectQsfp(module);
  CHECK_NE(selectedPort_, NO_PORT);

  write(address, offset, len, buf);

  // TODO: remove this after we ensure exclusive access to cp2112 chip
  unselectQsfp();
}

bool BaseWedgeI2CBus::isPresent(unsigned int module) {
  uint8_t buf = 0;
  try {
    moduleRead(module, TransceiverI2CApi::ADDR_QSFP, 0, sizeof(buf), &buf);
  } catch (const I2cError& ex) {
    /*
     * This can either mean that we failed to open the USB device
     * because it was already in use, or that the I2C read failed.
     * At some point we might want to return more a more accurate
     * status value to higher-level functions.
     */
    return false;
  }
  return true;
}

void BaseWedgeI2CBus::scanPresence(
    std::map<int32_t, ModulePresence>& presences) {
  for (auto& presence : presences) {
    uint8_t buf = 0;
    try {
      moduleRead(
          presence.first + 1,
          TransceiverI2CApi::ADDR_QSFP,
          0,
          sizeof(buf),
          &buf);
    } catch (const I2cError& ex) {
      /*
       * This can either mean that we failed to open the USB device
       * because it was already in use, or that the I2C read failed.
       * At some point we might want to return more a more accurate
       * status value to higher-level functions.
       */
      presence.second = ModulePresence::ABSENT;
      continue;
    }
    presence.second = ModulePresence::PRESENT;
  }
}

void BaseWedgeI2CBus::selectQsfp(unsigned int port) {
  VLOG(4) << "selecting QSFP " << port;
  CHECK_GT(port, 0);
  if (port != selectedPort_) {
    selectQsfpImpl(port);
  }
}

void BaseWedgeI2CBus::unselectQsfp() {
  VLOG(4) << "unselecting all QSFPs";
  if (selectedPort_ != NO_PORT) {
    selectQsfpImpl(NO_PORT);
  }
}

} // namespace fboss
} // namespace facebook
