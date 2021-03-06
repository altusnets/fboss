// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <boost/container/flat_set.hpp>
#include <list>

#include <folly/IPAddress.h>
#include <folly/Optional.h>
#include "fboss/agent/gen-cpp2/switch_config_constants.h"
#include "fboss/agent/state/AclEntry.h"
#include "fboss/agent/state/NodeBase.h"
#include "fboss/agent/state/RouteNextHop.h"
#include "fboss/agent/types.h"

namespace facebook {
namespace fboss {

using boost::container::flat_set;

struct TunnelUdpPorts {
  uint32_t udpSrcPort;
  uint32_t udpDstPort;
  TunnelUdpPorts(uint32_t src, uint32_t dst)
      : udpSrcPort(src), udpDstPort(dst) {}
};

struct MirrorTunnel {
  folly::IPAddress srcIp, dstIp;
  folly::MacAddress srcMac, dstMac;
  folly::Optional<TunnelUdpPorts> udpPorts;
  uint8_t ttl;
  uint16_t greProtocol;
  static constexpr auto kTTL = 255;
  static constexpr auto kProto = 0x88be;

  MirrorTunnel()
      : srcMac(folly::MacAddress::ZERO),
        dstMac(folly::MacAddress::ZERO),
        ttl(kTTL),
        greProtocol(kProto) {}

  MirrorTunnel(
      const folly::IPAddress& srcIp,
      const folly::IPAddress& dstIp,
      const folly::MacAddress& srcMac,
      const folly::MacAddress& dstMac,
      const folly::Optional<TunnelUdpPorts>& udpPorts = folly::none,
      uint8_t ttl = kTTL,
      uint16_t proto = kProto)
      : srcIp(srcIp),
        dstIp(dstIp),
        srcMac(srcMac),
        dstMac(dstMac),
        udpPorts(udpPorts),
        ttl(ttl),
        greProtocol(proto) {}

  bool operator==(const MirrorTunnel& rhs) const {
    return srcIp == rhs.srcIp && dstIp == rhs.dstIp && srcMac == rhs.srcMac &&
        dstMac == rhs.dstMac && ttl == rhs.ttl &&
        greProtocol == rhs.greProtocol;
  }

  bool operator<(const MirrorTunnel& rhs) const {
    return std::tie(srcIp, dstIp, srcMac, dstMac, ttl, greProtocol) <
        std::tie(
               rhs.srcIp,
               rhs.dstIp,
               rhs.srcMac,
               rhs.dstMac,
               rhs.ttl,
               rhs.greProtocol);
  }

  folly::dynamic toFollyDynamic() const;

  static MirrorTunnel fromFollyDynamic(const folly::dynamic& json);
};

struct MirrorFields {
  MirrorFields(
      const std::string& name,
      const folly::Optional<PortID>& egressPort,
      const folly::Optional<folly::IPAddress>& destinationIp,
      const folly::Optional<folly::IPAddress>& srcIp = folly::none,
      const folly::Optional<TunnelUdpPorts>& udpPorts = folly::none,
      const uint8_t& dscp = cfg::switch_config_constants::DEFAULT_MIRROR_DSCP_,
      const bool truncate = false)
      : name(name),
        egressPort(egressPort),
        destinationIp(destinationIp),
        srcIp(srcIp),
        udpPorts(udpPorts),
        dscp(dscp),
        truncate(truncate) {
    if (egressPort.hasValue()) {
      configHasEgressPort = true;
    }
  }

  template <typename Fn>
  void forEachChild(Fn /* unused */) {}

  std::string name;
  folly::Optional<PortID> egressPort;
  folly::Optional<folly::IPAddress> destinationIp;
  folly::Optional<folly::IPAddress> srcIp;
  folly::Optional<TunnelUdpPorts> udpPorts;
  uint8_t dscp;
  bool truncate;
  folly::Optional<MirrorTunnel> resolvedTunnel;
  bool configHasEgressPort{false};

  folly::dynamic toFollyDynamic() const;
};

class Mirror : public NodeBaseT<Mirror, MirrorFields> {
 public:
  Mirror(
      std::string name,
      folly::Optional<PortID> egressPort,
      folly::Optional<folly::IPAddress> destinationIp,
      folly::Optional<folly::IPAddress> srcIp = folly::none,
      folly::Optional<TunnelUdpPorts> udpPorts = folly::none,
      uint8_t dscp = cfg::switch_config_constants::DEFAULT_MIRROR_DSCP_,
      bool truncate = false);
  std::string getID() const;
  folly::Optional<PortID> getEgressPort() const;
  folly::Optional<folly::IPAddress> getDestinationIp() const;
  folly::Optional<folly::IPAddress> getSrcIp() const;
  folly::Optional<TunnelUdpPorts> getTunnelUdpPorts() const;
  folly::Optional<MirrorTunnel> getMirrorTunnel() const;
  uint8_t getDscp() const;
  bool getTruncate() const;
  void setEgressPort(PortID egressPort);
  void setMirrorTunnel(const MirrorTunnel& tunnel);
  bool configHasEgressPort() const;
  bool isResolved() const;

  static std::shared_ptr<Mirror> fromFollyDynamic(const folly::dynamic& json);
  folly::dynamic toFollyDynamic() const override;

  bool operator==(const Mirror& rhs) const;
  bool operator!=(const Mirror& rhs) const;

 private:
  // Inherit the constructors required for clone()

  using NodeBaseT::NodeBaseT;
  friend class CloneAllocator;
};
} // namespace fboss
} // namespace facebook
