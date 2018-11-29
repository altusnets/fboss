/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/hw/bcm/BcmPort.h"

#include <chrono>
#include <map>

#include <folly/Conv.h>
#include <folly/logging/xlog.h>
#include "common/stats/MonotonicCounter.h"
#include "common/stats/ServiceData.h"

#include "fboss/agent/SwitchStats.h"
#include "fboss/agent/hw/bcm/BcmError.h"
#include "fboss/agent/hw/bcm/BcmMirrorTable.h"
#include "fboss/agent/hw/bcm/BcmPlatformPort.h"
#include "fboss/agent/hw/bcm/BcmPortGroup.h"
#include "fboss/agent/hw/bcm/BcmPortQueueManager.h"
#include "fboss/agent/hw/bcm/BcmStatsConstants.h"
#include "fboss/agent/hw/bcm/BcmSwitch.h"
#include "fboss/agent/hw/bcm/gen-cpp2/hardware_stats_constants.h"
#include "fboss/agent/state/Port.h"
#include "fboss/agent/state/PortMap.h"
#include "fboss/agent/state/SwitchState.h"

extern "C" {
#include <opennsl/link.h>
#include <opennsl/port.h>
#include <opennsl/stat.h>
}

using std::chrono::duration_cast;
using std::chrono::seconds;
using std::chrono::system_clock;
using std::string;
using std::shared_ptr;

using facebook::stats::MonotonicCounter;

namespace facebook { namespace fboss {

static const std::vector<opennsl_stat_val_t> kInPktLengthStats = {
  snmpOpenNSLReceivedPkts64Octets,
  snmpOpenNSLReceivedPkts65to127Octets,
  snmpOpenNSLReceivedPkts128to255Octets,
  snmpOpenNSLReceivedPkts256to511Octets,
  snmpOpenNSLReceivedPkts512to1023Octets,
  snmpOpenNSLReceivedPkts1024to1518Octets,
  snmpOpenNSLReceivedPkts1519to2047Octets,
  snmpOpenNSLReceivedPkts2048to4095Octets,
  snmpOpenNSLReceivedPkts4095to9216Octets,
  snmpOpenNSLReceivedPkts9217to16383Octets,
};
static const std::vector<opennsl_stat_val_t> kOutPktLengthStats = {
  snmpOpenNSLTransmittedPkts64Octets,
  snmpOpenNSLTransmittedPkts65to127Octets,
  snmpOpenNSLTransmittedPkts128to255Octets,
  snmpOpenNSLTransmittedPkts256to511Octets,
  snmpOpenNSLTransmittedPkts512to1023Octets,
  snmpOpenNSLTransmittedPkts1024to1518Octets,
  snmpOpenNSLTransmittedPkts1519to2047Octets,
  snmpOpenNSLTransmittedPkts2048to4095Octets,
  snmpOpenNSLTransmittedPkts4095to9216Octets,
  snmpOpenNSLTransmittedPkts9217to16383Octets,
};

// This allows mapping from a speed and port transmission technology
// to a broadcom supported interface
static const std::map<cfg::PortSpeed,
  std::map<TransmitterTechnology, opennsl_port_if_t> > kPortTypeMapping = {
    {cfg::PortSpeed::HUNDREDG, {
      {TransmitterTechnology::COPPER, OPENNSL_PORT_IF_CR4},
      {TransmitterTechnology::OPTICAL, OPENNSL_PORT_IF_CAUI},
      // What to default to
      {TransmitterTechnology::UNKNOWN, OPENNSL_PORT_IF_CAUI}
    }},
    {cfg::PortSpeed::FIFTYG, {
      {TransmitterTechnology::COPPER, OPENNSL_PORT_IF_CR2},
      {TransmitterTechnology::OPTICAL, OPENNSL_PORT_IF_CAUI},
      // What to default to
      {TransmitterTechnology::UNKNOWN, OPENNSL_PORT_IF_CR2}
    }},
    {cfg::PortSpeed::FORTYG, {
      {TransmitterTechnology::COPPER, OPENNSL_PORT_IF_CR4},
      {TransmitterTechnology::OPTICAL, OPENNSL_PORT_IF_XLAUI},
      // What to default to
      {TransmitterTechnology::UNKNOWN, OPENNSL_PORT_IF_XLAUI}
    }},
    {cfg::PortSpeed::TWENTYFIVEG, {
      {TransmitterTechnology::COPPER, OPENNSL_PORT_IF_CR},
      {TransmitterTechnology::OPTICAL, OPENNSL_PORT_IF_CAUI},
      // What to default to
      {TransmitterTechnology::UNKNOWN, OPENNSL_PORT_IF_CR}
    }},
    {cfg::PortSpeed::TWENTYG, {
      {TransmitterTechnology::COPPER, OPENNSL_PORT_IF_CR},
      // We don't expect 20G optics
      // What to default to
      {TransmitterTechnology::UNKNOWN, OPENNSL_PORT_IF_CR}
    }},
    {cfg::PortSpeed::XG, {
      {TransmitterTechnology::COPPER, OPENNSL_PORT_IF_CR},
      {TransmitterTechnology::OPTICAL, OPENNSL_PORT_IF_SFI},
      // What to default to
      {TransmitterTechnology::UNKNOWN, OPENNSL_PORT_IF_CR}
    }},
    {cfg::PortSpeed::GIGE, {
      {TransmitterTechnology::COPPER, OPENNSL_PORT_IF_GMII},
      // We don't expect 1G optics
      // What to default to
      {TransmitterTechnology::UNKNOWN, OPENNSL_PORT_IF_GMII}
    }}
};

void BcmPort::updateName(const std::string& newName) {
  if (newName == portName_) {
    return;
  }
  portName_ = newName;
  queueManager_->setPortName(newName);
  reinitPortStats();
}

MonotonicCounter* BcmPort::getPortCounterIf(folly::StringPiece statKey) {
  auto pcitr = portCounters_.find(statKey.str());
  return pcitr != portCounters_.end() ? &pcitr->second : nullptr;
}

void BcmPort::reinitPortStat(folly::StringPiece statKey) {
  auto stat = getPortCounterIf(statKey);

  if (!stat) {
    portCounters_.emplace(
        statKey.str(),
        MonotonicCounter({statName(statKey), stats::SUM, stats::RATE}));
  } else {
    MonotonicCounter newStat{statName(statKey), stats::SUM, stats::RATE};
    stat->swap(newStat);
  }
}

void BcmPort::reinitPortStats() {
  if (!shouldReportStats()) {
    return;
  }

  reinitPortStat(kInBytes());
  reinitPortStat(kInUnicastPkts());
  reinitPortStat(kInMulticastPkts());
  reinitPortStat(kInBroadcastPkts());
  reinitPortStat(kInDiscards());
  reinitPortStat(kInErrors());
  reinitPortStat(kInPause());
  reinitPortStat(kInIpv4HdrErrors());
  reinitPortStat(kInIpv6HdrErrors());
  reinitPortStat(kInNonPauseDiscards());

  reinitPortStat(kOutBytes());
  reinitPortStat(kOutUnicastPkts());
  reinitPortStat(kOutMulticastPkts());
  reinitPortStat(kOutBroadcastPkts());
  reinitPortStat(kOutDiscards());
  reinitPortStat(kOutErrors());
  reinitPortStat(kOutPause());
  reinitPortStat(kOutEcnCounter());

  queueManager_->setupQueueCounters();

  // (re) init out queue length
  auto statMap = fbData->getStatMap();
  const auto expType = stats::AVG;
  outQueueLen_ = statMap->getLockableStat(statName("out_queue_length"),
                                          &expType);
  // (re) init histograms
  auto histMap = fbData->getHistogramMap();
  stats::ExportedHistogram pktLenHist(1, 0, kInPktLengthStats.size());
  inPktLengths_ = histMap->getOrCreateLockableHistogram(
      statName("in_pkt_lengths"), &pktLenHist);
  outPktLengths_ = histMap->getOrCreateLockableHistogram(
      statName("out_pkt_lengths"), &pktLenHist);

  {
    auto lockedPortStatsPtr = lastPortStats_.wlock();
    *lockedPortStatsPtr = BcmPortStats(
      queueManager_->getNumQueues(cfg::StreamType::UNICAST));
  }
}

BcmPort::BcmPort(BcmSwitch* hw, opennsl_port_t port,
                 BcmPlatformPort* platformPort)
    : hw_(hw),
      port_(port),
      platformPort_(platformPort),
      unit_(hw->getUnit()),
      // we can only get the real name(ethX/Y/Z) after we first apply config
      portName_(folly::to<string>("port", platformPort_->getPortID())) {

  // Obtain the gport handle from the port handle.
  int rv = opennsl_port_gport_get(unit_, port_, &gport_);
  bcmCheckError(rv, "Failed to get gport for BCM port ", port_);

  queueManager_ = std::make_unique<BcmPortQueueManager>(
    hw_, portName_, gport_);

  pipe_ = determinePipe();

  // Initialize our stats data structures
  reinitPortStats();

  XLOG(DBG2) << "created BCM port:" << port_ << ", gport:" << gport_
             << ", FBOSS PortID:" << platformPort_->getPortID();
}

BcmPort::~BcmPort() {
  applyMirrorAction(
      ingressMirror_, MirrorAction::STOP, MirrorDirection::INGRESS);
  applyMirrorAction(egressMirror_, MirrorAction::STOP, MirrorDirection::EGRESS);
}

void BcmPort::init(bool warmBoot) {
  bool up = false;
  if (warmBoot) {
    // Get port status from HW on warm boot.
    // All ports are initially down on a cold boot.
    int linkStatus;
    opennsl_port_link_status_get(unit_, port_, &linkStatus);
    up = (linkStatus == OPENNSL_PORT_LINK_STATUS_UP);
  } else {
    // In open source code, we don't have any guarantees for the
    // state of the port at startup. Bringing them down guarantees
    // that things are in a known state.
    //
    // We should only be doing this on cold boot, since warm booting
    // should be initializing the state for us.
    auto rv = opennsl_port_enable_set(unit_, port_, false);
    bcmCheckError(rv, "failed to set port to known state: ", port_);
  }

  // Notify platform port of initial state/speed
  getPlatformPort()->linkSpeedChanged(getSpeed());
  getPlatformPort()->linkStatusChanged(up, isEnabled());

  enableLinkscan();
}

bool BcmPort::supportsSpeed(cfg::PortSpeed speed) {
  // It would be nice if we could use the port_ability api here, but
  // that struct changes based on how many lanes are active. So does
  // opennsl_port_speed_max.
  //
  // Instead, we store the speed set in the bcm config file. This will
  // not work correctly if we performed a warm boot and the config
  // file changed port speeds. However, this is not supported by
  // broadcom for warm boot so this approach should be alright.
  return speed <= getMaxSpeed();
}

opennsl_pbmp_t BcmPort::getPbmp() {
  opennsl_pbmp_t pbmp;
  OPENNSL_PBMP_PORT_SET(pbmp, port_);
  return pbmp;
}

void BcmPort::disable(const std::shared_ptr<Port>& swPort) {
  if (!isEnabled()) {
    // Already disabled
    return;
  }

  auto pbmp = getPbmp();
  for (auto entry : swPort->getVlans()) {
    auto rv = opennsl_vlan_port_remove(unit_, entry.first, pbmp);
    bcmCheckError(rv, "failed to remove disabled port ",
                  swPort->getID(), " from VLAN ", entry.first);
  }

  // Disable packet and byte counter statistic collection.
  auto rv = opennsl_port_stat_enable_set(unit_, gport_, false);
  bcmCheckError(rv, "Unexpected error disabling counter DMA on port ",
                swPort->getID());

  // Disable sFlow sampling
  disableSflow();

  rv = opennsl_port_enable_set(unit_, port_, false);
  bcmCheckError(rv, "failed to disable port ", swPort->getID());
}

void BcmPort::disableLinkscan() {
  int rv = opennsl_linkscan_mode_set(unit_, port_, OPENNSL_LINKSCAN_MODE_NONE);
  bcmCheckError(rv, "Failed to disable linkscan on port ", port_);
}

bool BcmPort::isEnabled() {
  int enabled;
  auto rv = opennsl_port_enable_get(unit_, port_, &enabled);
  bcmCheckError(rv, "Failed to determine if port is already disabled");
  return static_cast<bool>(enabled);
}

bool BcmPort::isUp() {
  if (!isEnabled()) {
    return false;
  }
  int linkStatus;
  auto rv = opennsl_port_link_status_get(hw_->getUnit(), port_, &linkStatus);
  bcmCheckError(rv, "could not find if the port ", port_, " is up or down...");
  return linkStatus == OPENNSL_PORT_LINK_STATUS_UP;
}

void BcmPort::enable(const std::shared_ptr<Port>& swPort) {
  if (isEnabled()) {
    // Port is already enabled, don't need to do anything
    return;
  }

  auto pbmp = getPbmp();
  opennsl_pbmp_t emptyPortList;
  OPENNSL_PBMP_CLEAR(emptyPortList);
  int rv;
  for (auto entry : swPort->getVlans()) {
    if (!entry.second.tagged) {
      rv = opennsl_vlan_port_add(unit_, entry.first, pbmp, pbmp);
    } else {
      rv = opennsl_vlan_port_add(unit_, entry.first, pbmp, emptyPortList);
    }
    bcmCheckError(rv, "failed to add enabled port ",
                  swPort->getID(), " to VLAN ", entry.first);
  }

  // Drop packets to/from this port that are tagged with a VLAN that this
  // port isn't a member of.
  rv = opennsl_port_vlan_member_set(unit_, port_,
                                    OPENNSL_PORT_VLAN_MEMBER_INGRESS |
                                    OPENNSL_PORT_VLAN_MEMBER_EGRESS);
  bcmCheckError(rv, "failed to set VLAN filtering on port ",
                swPort->getID());

  // Set the speed, ingress vlan, and sFlow rates before enabling
  program(swPort);

  // Enable packet and byte counter statistic collection.
  rv = opennsl_port_stat_enable_set(unit_, gport_, true);
  if (rv != OPENNSL_E_EXISTS) {
    // Don't throw an error if counter collection is already enabled
    bcmCheckError(rv, "Unexpected error enabling counter DMA on port ",
                  swPort->getID());
  }

  rv = opennsl_port_enable_set(unit_, port_, true);
  bcmCheckError(rv, "failed to enable port ", swPort->getID());
}

void BcmPort::enableLinkscan() {
  int rv = opennsl_linkscan_mode_set(unit_, port_, OPENNSL_LINKSCAN_MODE_SW);
  bcmCheckError(rv, "Failed to enable linkscan on port ", port_);
}

void BcmPort::program(const shared_ptr<Port>& port) {
  XLOG(DBG1) << "Reprogramming BcmPort for port " << port->getID();
  setIngressVlan(port);
  if (platformPort_->shouldUsePortResourceAPIs()) {
    setPortResource(port);
  } else {
    setSpeed(port);
    // Update FEC settings if needed. Note this is not only
    // on speed change as the port's default speed (say on a
    // cold boot) maybe what is desired by the config. But we
    // may still need to enable FEC
    setFEC(port);
  }

  /* update mirrors for port, mirror add/update must happen earlier than
   * updating mirrors for port */
  updateMirror(port->getIngressMirror(), MirrorDirection::INGRESS);
  updateMirror(port->getEgressMirror(), MirrorDirection::EGRESS);

  setPause(port);
  // Update Tx Setting if needed.
  setTxSetting(port);
  setSflowRates(port);
  setLoopbackMode(port);
}

void BcmPort::linkStatusChanged(const std::shared_ptr<Port>& port) {
  getPlatformPort()->linkStatusChanged(port->isUp(), port->isEnabled());
}

void BcmPort::setIngressVlan(const shared_ptr<Port>& swPort) {
  opennsl_vlan_t currVlan;
  auto rv = opennsl_port_untagged_vlan_get(unit_, port_, &currVlan);
  bcmCheckError(rv, "failed to get ingress VLAN for port ", swPort->getID());

  opennsl_vlan_t bcmVlan = swPort->getIngressVlan();
  if (bcmVlan != currVlan) {
    rv = opennsl_port_untagged_vlan_set(unit_, port_, bcmVlan);
    bcmCheckError(rv, "failed to set ingress VLAN for port ",
                  swPort->getID(), " to ", swPort->getIngressVlan());
  }
}

TransmitterTechnology BcmPort::getTransmitterTechnology(
    const std::string& name) {
  // Since we are very unlikely to switch a port from copper to optical
  // while the agent is running, don't make unnecessary attempts to figure
  // out the transmitter technology when we already know what it is.
  if (transmitterTechnology_ != TransmitterTechnology::UNKNOWN) {
    return transmitterTechnology_;
  }
  // 6pack backplane ports will report tech as unknown because this
  // information can't be retrieved via qsfp. These are actually copper,
  // and so should use that instead of any potential default value
  if (name.find("fab") == 0) {
    transmitterTechnology_ = TransmitterTechnology::COPPER;
  } else {
    folly::EventBase evb;
    transmitterTechnology_ =
        getPlatformPort()->getTransmitterTech(&evb).getVia(&evb);
  }
  return transmitterTechnology_;
}

opennsl_port_if_t BcmPort::getDesiredInterfaceMode(cfg::PortSpeed speed,
                                                   PortID id,
                                                   const std::string& name) {
  TransmitterTechnology transmitterTech = getTransmitterTechnology(name);

  // If speed or transmitter type isn't in map
  try {
    auto result = kPortTypeMapping.at(speed).at(transmitterTech);
    XLOG(DBG1) << "Getting desired interface mode for port " << id
               << " (speed=" << static_cast<int>(speed)
               << ", tech=" << static_cast<int>(transmitterTech)
               << "). RESULT=" << result;
    return result;
  } catch (const std::out_of_range& ex) {
    throw FbossError("Unsupported speed (", speed,
                     ") or transmitter technology (", transmitterTech,
                     ") setting on port ", id);
  }
}

cfg::PortSpeed BcmPort::getSpeed() const {
  int curSpeed{0};
  auto rv = opennsl_port_speed_get(unit_, port_, &curSpeed);
  bcmCheckError(
    rv, "Failed to get current speed for port ", port_);
  return cfg::PortSpeed(curSpeed);
}

cfg::PortSpeed BcmPort::getDesiredPortSpeed(
    const std::shared_ptr<Port>& swPort) {
  if (swPort->getSpeed() == cfg::PortSpeed::DEFAULT) {
    int speed;
    auto ret = opennsl_port_speed_max(unit_, port_, &speed);
    bcmCheckError(ret, "failed to get max speed for port", swPort->getID());
    return cfg::PortSpeed(speed);
  } else {
    return swPort->getSpeed();
  }
}

void BcmPort::setInterfaceMode(const shared_ptr<Port>& swPort) {
  auto desiredPortSpeed = getDesiredPortSpeed(swPort);
  opennsl_port_if_t desiredMode = getDesiredInterfaceMode(desiredPortSpeed,
                                                          swPort->getID(),
                                                          swPort->getName());

  // Check whether we have the correct interface set
  opennsl_port_if_t curMode = opennsl_port_if_t(0);
  auto ret = opennsl_port_interface_get(unit_, port_, &curMode);
  bcmCheckError(ret, "Failed to get current interface setting for port ",
                     swPort->getID());

  // HACK: we cannot call speed_set w/out also
  // calling interface_mode_set, otherwise the
  // interface mode may change unexpectedly (details
  // on T32158588). We call set_speed when the port
  // is down, so also set mode here.
  //
  // TODO(aeckert): evaluate if we still need to set
  // speed on down ports.

  bool portUp = swPort->isPortUp();
  if (curMode != desiredMode || !portUp) {
    // Changes to the interface setting only seem to take effect on the next
    // call to opennsl_port_speed_set()
    ret = opennsl_port_interface_set(unit_, port_, desiredMode);
    bcmCheckError(ret, "failed to set interface type for port ",
                       swPort->getID());
  }
}

void BcmPort::setSpeed(const shared_ptr<Port>& swPort) {
  int ret;
  cfg::PortSpeed desiredPortSpeed = getDesiredPortSpeed(swPort);
  int desiredSpeed = static_cast<int>(desiredPortSpeed);
  // Unnecessarily updating BCM port speed actually causes
  // the port to flap, even if this should be a noop, so check current
  // speed before making speed related changes. Doing so fixes
  // the interface flaps we were seeing during warm boots

  int curSpeed = static_cast<int>(getSpeed());

  // If the port is down or disabled its safe to update mode and speed to
  // desired values
  bool portUp = swPort->isPortUp();

  // Update to correct mode and speed settings if the port is down/disabled
  // or if the speed changed. Ideally we would like to always update to the
  // desired mode and speed. However these changes are disruptive, in that
  // they cause a port flap. So to avoid that, we don't update to desired
  // mode if the port is UP and running at the desired speed. Speed changes
  // though are applied to UP ports as well, since running at wrong (lower than
  // desired) speed is pretty dangerous, and can trigger non obvious outages.
  //
  // Another practical reason for not updating to the desired mode on ports that
  // are UP is that there is at least one bug whereby SDK thinks that the ports
  // are in a different mode than they actually are. We are tracking that
  // separately. Once that is resolved, we can do a audit to see that if all
  // ports are in desired mode settings, we can make mode changes a first
  // class citizen as well.
  if (!portUp || curSpeed != desiredSpeed) {
    setInterfaceMode(swPort);

    if (portUp) {
      // Changing the port speed causes traffic disruptions, but not doing
      // it would cause inconsistency.  Warn the user.
      XLOG(WARNING) << "Changing port speed on up port. This will "
                    << "disrupt traffic. Port: " << swPort->getName()
                    << " id: " << swPort->getID();
    }

    XLOG(DBG1) << "Finalizing BcmPort::setSpeed() by calling port_speed_set on "
               << "port " << swPort->getID() << " (" << swPort->getName()
               << ")";

    // Note that we call speed_set even if the speed is already set
    // properly and port is down. This is because speed_set
    // reinitializes the MAC layer of the port and allows us to pick
    // up changes in interface mode and finalize flex port
    // transitions. We ensure that the port is down for these
    // potentially unnecessary calls, as otherwise this will cause
    // port flaps on ports where link is up.
    ret = opennsl_port_speed_set(unit_, port_, desiredSpeed);
    bcmCheckError(
      ret,
      "failed to set speed to ",
      desiredSpeed,
      " from ",
      curSpeed,
      ", on port ",
      swPort->getID());
    getPlatformPort()->linkSpeedChanged(desiredPortSpeed);
  }
}

PortID BcmPort::getPortID() const {
  return platformPort_->getPortID();
}

LaneSpeeds BcmPort::supportedLaneSpeeds() const {
  return platformPort_->supportedLaneSpeeds();
}

std::shared_ptr<Port> BcmPort::getSwitchStatePort(
    const std::shared_ptr<SwitchState>& state) const {
  return state->getPort(getPortID());
}

std::shared_ptr<Port> BcmPort::getSwitchStatePortIf(
    const std::shared_ptr<SwitchState>& state) const {
  return state->getPorts()->getPortIf(getPortID());
}

void BcmPort::registerInPortGroup(BcmPortGroup* portGroup) {
  portGroup_ = portGroup;
  XLOG(DBG2) << "Port " << getPortID() << " registered in PortGroup with "
             << "controlling port "
             << portGroup->controllingPort()->getPortID();
}

std::string BcmPort::statName(folly::StringPiece name) const {
  return folly::to<string>(portName_, ".", name);
}

void BcmPort::updateStats() {
  // TODO: It would be nicer to use a monotonic clock, but unfortunately
  // the ServiceData code currently expects everyone to use system time.
  if (!shouldReportStats()) {
    return;
  }
  auto now = duration_cast<seconds>(system_clock::now().time_since_epoch());
  HwPortStats curPortStats;
  updateStat(
      now, kInBytes(), opennsl_spl_snmpIfHCInOctets, &curPortStats.inBytes_);
  updateStat(
      now,
      kInUnicastPkts(),
      opennsl_spl_snmpIfHCInUcastPkts,
      &curPortStats.inUnicastPkts_);
  updateStat(
      now,
      kInMulticastPkts(),
      opennsl_spl_snmpIfHCInMulticastPkts,
      &curPortStats.inMulticastPkts_);
  updateStat(
      now,
      kInBroadcastPkts(),
      opennsl_spl_snmpIfHCInBroadcastPkts,
      &curPortStats.inBroadcastPkts_);
  updateStat(
      now,
      kInDiscards(),
      opennsl_spl_snmpIfInDiscards,
      &curPortStats.inDiscards_);
  updateStat(
      now, kInErrors(), opennsl_spl_snmpIfInErrors, &curPortStats.inErrors_);
  updateStat(
      now,
      kInIpv4HdrErrors(),
      opennsl_spl_snmpIpInHdrErrors,
      &curPortStats.inIpv4HdrErrors_);
  updateStat(
      now,
      kInIpv6HdrErrors(),
      opennsl_spl_snmpIpv6IfStatsInHdrErrors,
      &curPortStats.inIpv6HdrErrors_);
  updateStat(
      now,
      kInPause(),
      opennsl_spl_snmpDot3InPauseFrames,
      &curPortStats.inPause_);
  // Egress Stats
  updateStat(
      now,
      kOutBytes(),
      opennsl_spl_snmpIfHCOutOctets,
      &curPortStats.outBytes_);
  updateStat(
      now,
      kOutUnicastPkts(),
      opennsl_spl_snmpIfHCOutUcastPkts,
      &curPortStats.outUnicastPkts_);
  updateStat(
      now,
      kOutMulticastPkts(),
      opennsl_spl_snmpIfHCOutMulticastPkts,
      &curPortStats.outMulticastPkts_);
  updateStat(
      now,
      kOutBroadcastPkts(),
      opennsl_spl_snmpIfHCOutBroadcastPckts,
      &curPortStats.outBroadcastPkts_);
  updateStat(
      now,
      kOutDiscards(),
      opennsl_spl_snmpIfOutDiscards,
      &curPortStats.outDiscards_);
  updateStat(
      now, kOutErrors(), opennsl_spl_snmpIfOutErrors, &curPortStats.outErrors_);
  updateStat(
      now,
      kOutPause(),
      opennsl_spl_snmpDot3OutPauseFrames,
      &curPortStats.outPause_);

  updateBcmStats(now, &curPortStats);

  setAdditionalStats(now, &curPortStats);

  auto lastPortStats = lastPortStats_.rlock()->portStats();

  // Compute non pause discards
  const auto kUninit = hardware_stats_constants::STAT_UNINITIALIZED();
  if (isMmuLossy() && lastPortStats.inDiscards_ != kUninit &&
      lastPortStats.inPause_ != kUninit) {
    // If MMU setup as lossy, all incoming pause frames will be
    // discarded and will count towards in discards. This makes in discards
    // counter somewhat useless. So instead calculate "in_non_pause_discards",
    // as std::max(0, (inDiscardsSincePrev - inPauseSincePrev)).
    // std::max(..) is used, since stats from  h/w are synced non atomically,
    // So depending on what get synced later # of pause maybe be slightly
    // higher than # of discards.
    auto inPauseSincePrev = curPortStats.inPause_ - lastPortStats.inPause_;
    auto inDiscardsSincePrev =
        curPortStats.inDiscards_ - lastPortStats.inDiscards_;
    if (inPauseSincePrev >= 0 && inDiscardsSincePrev >= 0) {
      // Account for counter rollover.
      auto inNonPauseDiscardsSincePrev =
          std::max(0L, (inDiscardsSincePrev - inPauseSincePrev));
      // Init current port stats from prev value or 0
      curPortStats.inNonPauseDiscards_ =
          (lastPortStats.inNonPauseDiscards_ == kUninit
               ? 0
               : lastPortStats.inNonPauseDiscards_);
      // Counters are cumalative
      curPortStats.inNonPauseDiscards_ += inNonPauseDiscardsSincePrev;
      auto inNonPauseDiscards = getPortCounterIf(kInNonPauseDiscards());
      inNonPauseDiscards->updateValue(now, curPortStats.inNonPauseDiscards_);
    }
  }

  {
    auto lockedLastPortStatsPtr = lastPortStats_.wlock();
    *lockedLastPortStatsPtr = BcmPortStats(curPortStats, now);
  }

  // Update the queue length stat
  uint32_t qlength;
  auto ret = opennsl_port_queued_count_get(unit_, port_, &qlength);
  if (OPENNSL_FAILURE(ret)) {
    XLOG(ERR) << "Failed to get queue length for port " << port_ << " :"
              << opennsl_errmsg(ret);
  } else {
    outQueueLen_.addValue(now.count(), qlength);
    // TODO: outQueueLen_ only exports the average queue length over the last
    // 60 seconds, 10 minutes, etc.
    // We should also export the current value.  We could use a simple counter
    // or a dynamic counter for this.
  }

  // Update the packet length histograms
  updatePktLenHist(now, &inPktLengths_, kInPktLengthStats);
  updatePktLenHist(now, &outPktLengths_, kOutPktLengthStats);
};

void BcmPort::updateStat(
    std::chrono::seconds now,
    folly::StringPiece statKey,
    opennsl_stat_val_t type,
    int64_t* statVal) {
  auto stat = getPortCounterIf(statKey);
  // Use the non-sync API to just get the values accumulated in software.
  // The Broadom SDK's counter thread syncs the HW counters to software every
  // 500000us (defined in config.bcm).
  uint64_t value;
  auto ret = opennsl_stat_get(unit_, port_, type, &value);
  if (OPENNSL_FAILURE(ret)) {
    XLOG(ERR) << "Failed to get stat " << type << " for port " << port_ << " :"
              << opennsl_errmsg(ret);
    return;
  }
  stat->updateValue(now, value);
  *statVal = value;
}

bool BcmPort::isMmuLossy() const {
  return hw_->getMmuState() == BcmSwitch::MmuState::MMU_LOSSY;
}

void BcmPort::updatePktLenHist(
    std::chrono::seconds now,
    stats::ExportedHistogramMapImpl::LockableHistogram* hist,
    const std::vector<opennsl_stat_val_t>& stats) {
  // Get the counter values
  uint64_t counters[10];
  // opennsl_stat_multi_get() unfortunately doesn't correctly const qualify
  // it's stats arguments right now.
  opennsl_stat_val_t* statsArg =
      const_cast<opennsl_stat_val_t*>(&stats.front());
  auto ret = opennsl_stat_multi_get(unit_, port_,
                                stats.size(), statsArg, counters);
  if (OPENNSL_FAILURE(ret)) {
    XLOG(ERR) << "Failed to get packet length stats for port " << port_ << " :"
              << opennsl_errmsg(ret);
    return;
  }

  // Update the histogram
  auto guard = hist->makeLockGuard();
  for (int idx = 0; idx < stats.size(); ++idx) {
    hist->addValueLocked(guard, now.count(), idx, counters[idx]);
  }
}

BcmPort::BcmPortStats::BcmPortStats(int numUnicastQueues) {
  portStats_.set_queueOutDiscardBytes_(
      std::vector<int64_t>(numUnicastQueues, 0));
  portStats_.set_queueOutBytes_(std::vector<int64_t>(numUnicastQueues, 0));
}

BcmPort::BcmPortStats::BcmPortStats(
    HwPortStats portStats,
    std::chrono::seconds timeRetrieved)
    : portStats_(portStats), timeRetrieved_(timeRetrieved) {}

HwPortStats BcmPort::BcmPortStats::portStats() const {
  return portStats_;
}

std::chrono::seconds BcmPort::BcmPortStats::timeRetrieved() const {
  return timeRetrieved_;
}

HwPortStats BcmPort::getPortStats() const {
  return lastPortStats_.rlock()->portStats();
}

std::chrono::seconds BcmPort::getTimeRetrieved() const {
  return lastPortStats_.rlock()->timeRetrieved();
}

void BcmPort::applyMirrorAction(
    const folly::Optional<std::string>& mirrorName,
    MirrorAction action,
    MirrorDirection direction) {
  if (!mirrorName) {
    return;
  }
  auto* bcmMirror = hw_->getBcmMirrorTable()->getMirrorIf(mirrorName.value());
  CHECK(bcmMirror != nullptr);
  bcmMirror->applyPortMirrorAction(getPortID(), action, direction);
}

void BcmPort::updateMirror(
    const folly::Optional<std::string>& swMirrorName,
    MirrorDirection direction) {
  applyMirrorAction(
      direction == MirrorDirection::INGRESS ? ingressMirror_ : egressMirror_,
      MirrorAction::STOP,
      direction);
  if (direction == MirrorDirection::INGRESS) {
    ingressMirror_ = swMirrorName;
  } else {
    egressMirror_ = swMirrorName;
  }
  applyMirrorAction(
      direction == MirrorDirection::INGRESS ? ingressMirror_ : egressMirror_,
      MirrorAction::START,
      direction);
}
/**
  * TODO(rsher)
  * comment back in when we move to the newer OpenNSL

bool BcmPort::isFECEnabled() {
  int value;
  opennsl_port_phy_control_get(
      unit_,
      port_,
      OPENNSL_PORT_PHY_CONTROL_FORWARD_ERROR_CORRECTION,
      &value);
  return (value == OPENNSL_PORT_PHY_CONTROL_FEC_ON);
}

*/

}} // namespace facebook::fboss
