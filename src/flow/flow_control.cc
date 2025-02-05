//--------------------------------------------------------------------------
// Copyright (C) 2014-2019 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <daq_common.h>

#include "flow_control.h"

#include "detection/detection_engine.h"
#include "main/snort_config.h"
#include "managers/inspector_manager.h"
#include "memory/memory_cap.h"
#include "packet_io/active.h"
#include "packet_tracer/packet_tracer.h"
#include "protocols/icmp4.h"
#include "protocols/tcp.h"
#include "protocols/udp.h"
#include "protocols/vlan.h"
#include "stream/stream.h"
#include "utils/util.h"

#include "expect_cache.h"
#include "flow_cache.h"
#include "ha.h"
#include "session.h"

using namespace snort;

FlowControl::FlowControl(const FlowCacheConfig& fc)
{
    cache = new FlowCache(fc);

    mem = (Flow*)snort_calloc(fc.max_flows, sizeof(Flow));

    for ( unsigned i = 0; i < fc.max_flows; ++i )
        cache->push(mem + i);
}

FlowControl::~FlowControl()
{
    DetectionEngine de;

    delete cache;
    snort_free(mem);
    delete exp_cache;
}

//-------------------------------------------------------------------------
// count foo
//-------------------------------------------------------------------------

PegCount FlowControl::get_total_prunes() const
{
    auto cache = get_cache();
    return cache ? cache->get_total_prunes() : 0;
}

PegCount FlowControl::get_prunes(PruneReason reason) const
{
    auto cache = get_cache();
    return cache ? cache->get_prunes(reason) : 0;
}

void FlowControl::clear_counts()
{
    for ( int i = 0; i < to_utype(PktType::MAX); ++i )
    {
        if ( cache )
            cache->reset_stats();

        num_flows = 0;
    }
}

//-------------------------------------------------------------------------
// cache foo
//-------------------------------------------------------------------------

Flow* FlowControl::find_flow(const FlowKey* key)
{
    if ( auto cache = get_cache() )
        return cache->find(key);

    return nullptr;
}

Flow* FlowControl::new_flow(const FlowKey* key)
{
    if ( auto cache = get_cache() )
        return cache->get(key);

    return nullptr;
}

// FIXIT-L cache* can be put in flow so that lookups by
// packet type are obviated for existing / initialized flows
void FlowControl::delete_flow(const FlowKey* key)
{
    FlowCache* cache = get_cache();

    if ( !cache )
        return;

    if ( auto flow = cache->find(key) )
        cache->release(flow, PruneReason::HA);
}

void FlowControl::delete_flow(Flow* flow, PruneReason reason)
{
    if ( auto cache = get_cache() )
        cache->release(flow, reason);
}

void FlowControl::purge_flows ()
{
    if ( auto cache = get_cache() )
        cache->purge();
}

// hole for memory manager/prune handler
bool FlowControl::prune_one(PruneReason reason, bool do_cleanup)
{
    auto cache = get_cache();
    return cache ? cache->prune_one(reason, do_cleanup) : false;
}

void FlowControl::timeout_flows(time_t cur_time)
{
    if ( types.empty() )
        return;

    ActiveSuspendContext act_susp;
    FlowCache* fc = get_cache();

    if ( ++next >= types.size() )
        next = 0;

    if ( fc )
        fc->timeout(1, cur_time);
}

void FlowControl::preemptive_cleanup()
{
    // FIXIT-RC is there a possibility of this looping forever?
    while ( memory::MemoryCap::over_threshold() )
    {
        if ( !prune_one(PruneReason::PREEMPTIVE, true) )
            break;
    }
}

Flow* FlowControl::stale_flow_cleanup(FlowCache* cache, Flow* flow, Packet* p)
{
    if ( p->pkth->flags & DAQ_PKT_FLAG_NEW_FLOW )
    {
        if (PacketTracer::is_active())
            PacketTracer::log("Session: deleting snort session, reason: stale and not cleaned \n");

        cache->release(flow, PruneReason::STALE);
        flow = nullptr;
    }

    return flow;
}

//-------------------------------------------------------------------------
// packet foo
//-------------------------------------------------------------------------

void FlowControl::set_key(FlowKey* key, Packet* p)
{
    const ip::IpApi& ip_api = p->ptrs.ip_api;
    uint32_t mplsId;
    uint16_t vlanId;
    uint16_t addressSpaceId;
    PktType type = p->type();
    IpProtocol ip_proto = p->get_ip_proto_next();

    if ( p->proto_bits & PROTO_BIT__VLAN )
        vlanId = layer::get_vlan_layer(p)->vid();
    else
        vlanId = 0;

    if ( p->proto_bits & PROTO_BIT__MPLS )
        mplsId = p->ptrs.mplsHdr.label;
    else
        mplsId = 0;

    addressSpaceId = p->pkth->address_space_id;

    if ( (p->ptrs.decode_flags & DECODE_FRAG) )
    {
        key->init(type, ip_proto, ip_api.get_src(), ip_api.get_dst(), ip_api.id(),
            vlanId, mplsId, addressSpaceId);
    }
    else if ( type == PktType::ICMP )
    {
        key->init(type, ip_proto, ip_api.get_src(), p->ptrs.icmph->type, ip_api.get_dst(), 0,
            vlanId, mplsId, addressSpaceId);
    }
    else
    {
        key->init(type, ip_proto, ip_api.get_src(), p->ptrs.sp, ip_api.get_dst(), p->ptrs.dp,
            vlanId, mplsId, addressSpaceId);
    }
}

static bool is_bidirectional(const Flow* flow)
{
    constexpr unsigned bidir = SSNFLAG_SEEN_CLIENT | SSNFLAG_SEEN_SERVER;
    return (flow->ssn_state.session_flags & bidir) == bidir;
}

static void init_roles_ip(const Packet* p, Flow* flow)
{
    flow->ssn_state.direction = FROM_CLIENT;
    flow->client_ip.set(*p->ptrs.ip_api.get_src());
    flow->server_ip.set(*p->ptrs.ip_api.get_dst());
}

static void init_roles_tcp(const Packet* p, Flow* flow)
{
    if ( p->ptrs.tcph->is_syn_only() )
    {
        flow->ssn_state.direction = FROM_CLIENT;
        flow->client_ip.set(*p->ptrs.ip_api.get_src());
        flow->client_port = p->ptrs.sp;
        flow->server_ip.set(*p->ptrs.ip_api.get_dst());
        flow->server_port = p->ptrs.dp;
    }
    else if ( p->ptrs.tcph->is_syn_ack() )
    {
        flow->ssn_state.direction = FROM_SERVER;
        flow->client_ip.set(*p->ptrs.ip_api.get_dst());
        flow->client_port = p->ptrs.dp;
        flow->server_ip.set(*p->ptrs.ip_api.get_src());
        flow->server_port = p->ptrs.sp;
    }
    else if (p->ptrs.sp > p->ptrs.dp)
    {
        flow->ssn_state.direction = FROM_CLIENT;
        flow->client_ip.set(*p->ptrs.ip_api.get_src());
        flow->client_port = p->ptrs.sp;
        flow->server_ip.set(*p->ptrs.ip_api.get_dst());
        flow->server_port = p->ptrs.dp;
    }
    else
    {
        flow->ssn_state.direction = FROM_SERVER;
        flow->client_ip.set(*p->ptrs.ip_api.get_dst());
        flow->client_port = p->ptrs.dp;
        flow->server_ip.set(*p->ptrs.ip_api.get_src());
        flow->server_port = p->ptrs.sp;
    }
}

static void init_roles_udp(const Packet* p, Flow* flow)
{
    flow->ssn_state.direction = FROM_CLIENT;
    flow->client_ip.set(*p->ptrs.ip_api.get_src());
    flow->client_port = p->ptrs.sp;
    flow->server_ip.set(*p->ptrs.ip_api.get_dst());
    flow->server_port = p->ptrs.dp;
}

static void init_roles_user(const Packet* p, Flow* flow)
{
    if ( p->ptrs.decode_flags & DECODE_C2S )
    {
        flow->ssn_state.direction = FROM_CLIENT;
        flow->client_ip.set(*p->ptrs.ip_api.get_src());
        flow->client_port = p->ptrs.sp;
        flow->server_ip.set(*p->ptrs.ip_api.get_dst());
        flow->server_port = p->ptrs.dp;
    }
    else
    {
        flow->ssn_state.direction = FROM_SERVER;
        flow->client_ip.set(*p->ptrs.ip_api.get_dst());
        flow->client_port = p->ptrs.dp;
        flow->server_ip.set(*p->ptrs.ip_api.get_src());
        flow->server_port = p->ptrs.sp;
    }
}

// FIXIT-L init_roles should take const Packet*
static void init_roles(Packet* p, Flow* flow)
{
    switch ( flow->pkt_type )
    {
    case PktType::IP:
    case PktType::ICMP:
        init_roles_ip(p, flow);
        break;

    case PktType::TCP:
        init_roles_tcp(p, flow);
        break;

    case PktType::UDP:
        init_roles_udp(p, flow);
        break;

    case PktType::PDU:
    case PktType::FILE:
        init_roles_user(p, flow);
        break;

    default:
        break;
    }
    if ( flow->ssn_state.direction == FROM_CLIENT )
        p->packet_flags |= PKT_FROM_CLIENT;
    else
        p->packet_flags |= PKT_FROM_SERVER;
}

//-------------------------------------------------------------------------
// proto
//-------------------------------------------------------------------------

void FlowControl::init_proto(PktType type, InspectSsnFunc get_ssn)
{
    assert(get_ssn);

    proto[to_utype(type)].get_ssn = get_ssn;
    types.emplace_back(type);
}

// FIXIT-P apply more filtering logic here, eg require_3whs
// delegate to stream inspectors but that requires binding
// can't use session because goal is to avoid instantiation
static bool want_flow(PktType type, Packet* p)
{
    if ( type != PktType::TCP )
        return true;

    if ( p->ptrs.tcph->is_rst() )
        // guessing direction based on ports is misleading
        return false;

    if ( !p->ptrs.tcph->is_syn_only() or SnortConfig::get_conf()->track_on_syn() or
        (p->ptrs.decode_flags & DECODE_WSCALE) )
        return true;

    p->packet_flags |= PKT_FROM_CLIENT;
    return false;
}

bool FlowControl::process(PktType type, Packet* p, bool* new_flow)
{
    if ( !proto[to_utype(type)].get_ssn )
        return false;

    FlowKey key;
    set_key(&key, p);
    Flow* flow = cache->find(&key);

    if (flow)
        flow = stale_flow_cleanup(cache, flow, p);

    if ( !flow )
    {
        flow = HighAvailabilityManager::import(*p, key);

        if ( !flow )
        {
            if ( !want_flow(type, p) )
                return true;

            flow = cache->get(&key);

            if ( !flow )
                return true;

            if ( new_flow )
                *new_flow = true;
        }
    }

    if ( !flow->session )
    {
        flow->init(type);
        flow->session = proto[to_utype(type)].get_ssn(flow);
    }

    num_flows += process(flow, p);

    // FIXIT-M refactor to unlink_uni immediately after session
    // is processed by inspector manager (all flows)
    if ( flow->next && is_bidirectional(flow) )
        cache->unlink_uni(flow);

    return true;
}

unsigned FlowControl::process(Flow* flow, Packet* p)
{
    unsigned news = 0;

    flow->previous_ssn_state = flow->ssn_state;

    p->flow = flow;
    p->disable_inspect = flow->is_inspection_disabled();

    last_pkt_type = p->type();
    preemptive_cleanup();

    flow->set_direction(p);
    flow->session->precheck(p);

    if ( flow->flow_state != Flow::FlowState::SETUP )
    {
        set_inspection_policy(SnortConfig::get_conf(), flow->inspection_policy_id);
        set_ips_policy(SnortConfig::get_conf(), flow->ips_policy_id);
        set_network_policy(SnortConfig::get_conf(), flow->network_policy_id);
    }

    else
    {
        if (PacketTracer::is_active())
            PacketTracer::log("Session: new snort session\n");

        init_roles(p, flow);
        DataBus::publish(FLOW_STATE_SETUP_EVENT, p);

        if ( flow->flow_state == Flow::FlowState::SETUP ||
            (flow->flow_state == Flow::FlowState::INSPECT &&
             (!flow->ssn_client || !flow->session->setup(p))) )
            flow->set_state(Flow::FlowState::ALLOW);

        ++news;
    }

    // This requires the packet direction to be set
    if ( p->proto_bits & PROTO_BIT__MPLS )
        flow->set_mpls_layer_per_dir(p);

    switch ( flow->flow_state )
    {
    case Flow::FlowState::SETUP:
        flow->set_state(Flow::FlowState::ALLOW);
        break;

    case Flow::FlowState::INSPECT:
        assert(flow->ssn_client);
        assert(flow->ssn_server);
        break;

    case Flow::FlowState::ALLOW:
        if ( news )
            Stream::stop_inspection(flow, p, SSN_DIR_BOTH, -1, 0);
        else
            DetectionEngine::disable_all(p);

        p->ptrs.decode_flags |= DECODE_PKT_TRUST;
        break;

    case Flow::FlowState::BLOCK:
        if ( news )
            Stream::drop_traffic(p, SSN_DIR_BOTH);
        else
            p->active->block_again();

        DetectionEngine::disable_all(p);
        break;

    case Flow::FlowState::RESET:
        if ( news )
            Stream::drop_traffic(p, SSN_DIR_BOTH);
        else
            p->active->reset_again();

        Stream::blocked_flow(p);
        DetectionEngine::disable_all(p);
        break;
    }

    return news;
}

//-------------------------------------------------------------------------
// expected
//-------------------------------------------------------------------------

void FlowControl::init_exp(uint32_t max)
{
    max >>= 9;

    if ( !max )
        max = 2;

    exp_cache = new ExpectCache(max);
}

bool FlowControl::expected_flow(Flow* flow, Packet* p)
{
    bool ignore = exp_cache->check(p, flow);

    if ( ignore )
    {
        flow->ssn_state.ignore_direction = ignore;
        DetectionEngine::disable_all(p);
    }

    return ignore;
}

int FlowControl::add_expected(
    const Packet* ctrlPkt, PktType type, IpProtocol ip_proto,
    const SfIp *srcIP, uint16_t srcPort,
    const SfIp *dstIP, uint16_t dstPort,
    char direction, FlowData* fd)
{
    return exp_cache->add_flow(
        ctrlPkt, type, ip_proto, srcIP, srcPort, dstIP, dstPort,
        direction, fd);
}

int FlowControl::add_expected(
    const Packet* ctrlPkt, PktType type, IpProtocol ip_proto,
    const SfIp *srcIP, uint16_t srcPort,
    const SfIp *dstIP, uint16_t dstPort,
    SnortProtocolId snort_protocol_id, FlowData* fd)
{
    return exp_cache->add_flow(
        ctrlPkt, type, ip_proto, srcIP, srcPort, dstIP, dstPort,
        SSN_DIR_BOTH, fd, snort_protocol_id);
}

bool FlowControl::is_expected(Packet* p)
{
    return exp_cache->is_expected(p);
}

