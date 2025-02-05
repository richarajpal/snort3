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

#include <functional>

#include "flow/flow_control.h"
#include "flow/prune_stats.h"
#include "framework/data_bus.h"
#include "log/messages.h"
#include "main/snort_config.h"
#include "main/snort_types.h"
#include "managers/inspector_manager.h"
#include "profiler/profiler_defs.h"
#include "protocols/packet.h"
#include "protocols/tcp.h"
#include "stream/flush_bucket.h"

#include "stream_ha.h"
#include "stream_module.h"

using namespace snort;

//-------------------------------------------------------------------------
// stats
//-------------------------------------------------------------------------

THREAD_LOCAL ProfileStats s5PerfStats;
THREAD_LOCAL FlowControl* flow_con = nullptr;

static BaseStats g_stats;
THREAD_LOCAL BaseStats stream_base_stats;

// FIXIT-L dependency on stats define in another file
const PegInfo base_pegs[] =
{
    { CountType::SUM, "flows", "total sessions" },
    { CountType::SUM, "total_prunes", "total sessions pruned" },
    { CountType::SUM, "idle_prunes", " sessions pruned due to timeout" },
    { CountType::SUM, "excess_prunes", "sessions pruned due to excess" },
    { CountType::SUM, "uni_prunes", "uni sessions pruned" },
    { CountType::SUM, "preemptive_prunes", "sessions pruned during preemptive pruning" },
    { CountType::SUM, "memcap_prunes", "sessions pruned due to memcap" },
    { CountType::SUM, "ha_prunes", "sessions pruned by high availability sync" },
    { CountType::END, nullptr, nullptr }
};

// FIXIT-L dependency on stats define in another file
void base_sum()
{
    if ( !flow_con )
        return;

    stream_base_stats.flows = flow_con->get_flows();
    stream_base_stats.prunes = flow_con->get_total_prunes();
    stream_base_stats.timeout_prunes = flow_con->get_prunes(PruneReason::IDLE);
    stream_base_stats.excess_prunes = flow_con->get_prunes(PruneReason::EXCESS);
    stream_base_stats.uni_prunes = flow_con->get_prunes(PruneReason::UNI);
    stream_base_stats.preemptive_prunes = flow_con->get_prunes(PruneReason::PREEMPTIVE);
    stream_base_stats.memcap_prunes = flow_con->get_prunes(PruneReason::MEMCAP);
    stream_base_stats.ha_prunes = flow_con->get_prunes(PruneReason::HA);

    sum_stats((PegCount*)&g_stats, (PegCount*)&stream_base_stats,
        array_size(base_pegs)-1);
    base_reset();
}

void base_stats()
{
    show_stats((PegCount*)&g_stats, base_pegs, array_size(base_pegs)-1, MOD_NAME);
}

void base_reset()
{
    if ( flow_con )
        flow_con->clear_counts();

    memset(&stream_base_stats, 0, sizeof(stream_base_stats));
}

//-------------------------------------------------------------------------
// runtime support
//-------------------------------------------------------------------------

static inline bool is_eligible(Packet* p)
{
#ifdef NDEBUG
    UNUSED(p);
#endif
    assert(!(p->ptrs.decode_flags & DECODE_ERR_CKSUM_IP));
    assert(!(p->packet_flags & PKT_REBUILT_STREAM));
    assert(p->ptrs.ip_api.is_valid());

    return true;
}

//-------------------------------------------------------------------------
// inspector stuff
//-------------------------------------------------------------------------

class StreamBase : public Inspector
{
public:
    StreamBase(const StreamModuleConfig*);

    void show(SnortConfig*) override;

    void tinit() override;
    void tterm() override;

    void eval(Packet*) override;

public:
    StreamModuleConfig config;
};

StreamBase::StreamBase(const StreamModuleConfig* c)
{ config = *c; }

void StreamBase::tinit()
{
    assert(!flow_con && config.flow_cache_cfg.max_flows);

    // this is temp added to suppress the compiler error only
    flow_con = new FlowControl(config.flow_cache_cfg);
    InspectSsnFunc f;

    StreamHAManager::tinit();

    if ( (f = InspectorManager::get_session(PROTO_BIT__IP)) )
        flow_con->init_proto(PktType::IP, f);

    if ( (f = InspectorManager::get_session(PROTO_BIT__ICMP)) )
        flow_con->init_proto(PktType::ICMP, f);

    if ( (f = InspectorManager::get_session(PROTO_BIT__TCP)) )
        flow_con->init_proto(PktType::TCP, f);

    if ( (f = InspectorManager::get_session(PROTO_BIT__UDP)) )
        flow_con->init_proto(PktType::UDP, f);

    if ( (f = InspectorManager::get_session(PROTO_BIT__PDU)) )
        flow_con->init_proto(PktType::PDU, f);

    if ( (f = InspectorManager::get_session(PROTO_BIT__FILE)) )
        flow_con->init_proto(PktType::FILE, f);

    if ( config.flow_cache_cfg.max_flows > 0 )
        flow_con->init_exp(config.flow_cache_cfg.max_flows);

    FlushBucket::set(config.footprint);
}

void StreamBase::tterm()
{
    StreamHAManager::tterm();
    FlushBucket::clear();
}

void StreamBase::show(SnortConfig*)
{
    LogMessage("Stream Base config:\n");
    LogMessage("    Max flows: %d\n", config.flow_cache_cfg.max_flows);
    LogMessage("    Pruning timeout: %d\n", config.flow_cache_cfg.pruning_timeout);
}

void StreamBase::eval(Packet* p)
{
    Profile profile(s5PerfStats);

    if ( !is_eligible(p) )
        return;

    switch ( p->type() )
    {
    case PktType::NONE:
        break;

    case PktType::IP:
        if ( p->has_ip() and ((p->ptrs.decode_flags & DECODE_FRAG) or
            !SnortConfig::get_conf()->ip_frags_only()) )
        {
            bool new_flow = false;
            flow_con->process(PktType::IP, p, &new_flow);
            if ( new_flow )
                DataBus::publish(STREAM_IP_NEW_FLOW_EVENT, p);
        }
        break;

    case PktType::TCP:
        if ( p->ptrs.tcph )
            flow_con->process(PktType::TCP, p);
        break;

    case PktType::UDP:
        if ( p->ptrs.decode_flags & DECODE_FRAG )
            flow_con->process(PktType::IP, p);

        if ( p->ptrs.udph )
        {
            bool new_flow = false;
            flow_con->process(PktType::UDP, p, &new_flow);
            if ( new_flow )
                DataBus::publish(STREAM_UDP_NEW_FLOW_EVENT, p);
        }
        break;

    case PktType::ICMP:
        if ( p->ptrs.icmph )
        {
            bool new_flow = false;
            if ( !flow_con->process(PktType::ICMP, p, &new_flow) )
                flow_con->process(PktType::IP, p, &new_flow);
            if ( new_flow )
                DataBus::publish(STREAM_ICMP_NEW_FLOW_EVENT, p);
        }
        break;

    case PktType::PDU:
        flow_con->process(PktType::PDU, p);
        break;

    case PktType::FILE:
        flow_con->process(PktType::FILE, p);
        break;

    case PktType::MAX:
        break;
    };
}

#if 0
// FIXIT-L add method to get exp cache?
LogMessage("            Expected Flows\n");
LogMessage("                  Expected: %lu\n", exp_cache->get_expects());
LogMessage("                  Realized: %lu\n", exp_cache->get_realized());
LogMessage("                    Pruned: %lu\n", exp_cache->get_prunes());
LogMessage("                 Overflows: %lu\n", exp_cache->get_overflows());
#endif

//-------------------------------------------------------------------------
// api stuff
//-------------------------------------------------------------------------

static Module* mod_ctor()
{ return new StreamModule; }

static void mod_dtor(Module* m)
{ delete m; }

static Inspector* base_ctor(Module* m)
{
    StreamModule* mod = (StreamModule*)m;
    return new StreamBase(mod->get_data());
}

static void base_dtor(Inspector* p)
{
    delete p;
}

static void base_tterm()
{
    // this can't happen sooner because the counts haven't been harvested yet
    delete flow_con;
    flow_con = nullptr;
}

static const InspectApi base_api =
{
    {
        PT_INSPECTOR,
        sizeof(InspectApi),
        INSAPI_VERSION,
        0,
        API_RESERVED,
        API_OPTIONS,
        MOD_NAME,
        MOD_HELP,
        mod_ctor,
        mod_dtor
    },
    IT_STREAM,
    PROTO_BIT__ANY_SSN,
    nullptr, // buffers
    nullptr, // service
    nullptr, // init
    nullptr, // term
    nullptr, // tinit
    base_tterm,
    base_ctor,
    base_dtor,
    nullptr, // ssn
    nullptr  // reset
};

const BaseApi* nin_stream_base = &base_api.base;
