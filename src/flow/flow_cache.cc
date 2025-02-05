//--------------------------------------------------------------------------
// Copyright (C) 2014-2019 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2013-2013 Sourcefire, Inc.
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
// flow_cache.cc author Russ Combs <rucombs@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "flow/flow_cache.h"

#include "hash/zhash.h"
#include "helpers/flag_context.h"
#include "ips_options/ips_flowbits.h"
#include "memory/memory_cap.h"
#include "packet_io/active.h"
#include "time/packet_time.h"
#include "utils/stats.h"

#include "flow.h"
#include "flow_key.h"
#include "flow_uni_list.h"
#include "ha.h"
#include "session.h"

using namespace snort;

#define SESSION_CACHE_FLAG_PURGING  0x01

//-------------------------------------------------------------------------
// FlowCache stuff
//-------------------------------------------------------------------------

FlowCache::FlowCache(const FlowCacheConfig& cfg) : config(cfg)
{
    hash_table = new ZHash(config.max_flows, sizeof(FlowKey));
    hash_table->set_keyops(FlowKey::hash, FlowKey::compare);

    uni_flows = new FlowUniList;
    uni_ip_flows = new FlowUniList;

    flags = 0x0;

    assert(prune_stats.get_total() == 0);
}

FlowCache::~FlowCache()
{
    delete uni_flows;
    delete uni_ip_flows;
    delete hash_table;
}

void FlowCache::push(Flow* flow)
{
    void* key = hash_table->push(flow);
    flow->key = (FlowKey*)key;
}

unsigned FlowCache::get_count()
{
    return hash_table ? hash_table->get_count() : 0;
}

Flow* FlowCache::find(const FlowKey* key)
{
    Flow* flow = (Flow*)hash_table->find(key);

    if ( flow )
    {
        time_t t = packet_time();

        if ( flow->last_data_seen < t )
            flow->last_data_seen = t;
    }

    return flow;
}

// always prepend
void FlowCache::link_uni(Flow* flow)
{
    if ( flow->pkt_type == PktType::IP )
        uni_ip_flows->link_uni(flow);
    else
        uni_flows->link_uni(flow);
}

// but remove from any point
void FlowCache::unlink_uni(Flow* flow)
{
    if ( flow->pkt_type == PktType::IP )
        uni_ip_flows->unlink_uni(flow);
    else
        uni_flows->unlink_uni(flow);
}

Flow* FlowCache::get(const FlowKey* key)
{
    time_t timestamp = packet_time();
    Flow* flow = (Flow*)hash_table->get(key);

    if ( !flow )
    {
        if ( !prune_stale(timestamp, nullptr) )
        {
            if ( !prune_unis(key->pkt_type) )
                prune_excess(nullptr);
        }

        flow = (Flow*)hash_table->get(key);
        assert(flow);

        if ( flow->session && flow->pkt_type != key->pkt_type )
            flow->term();
        else
            flow->reset();
        link_uni(flow);
    }

    if ( flow->session && flow->pkt_type != key->pkt_type )
        flow->term();

    memory::MemoryCap::update_allocations(config.proto[to_utype(key->pkt_type)].cap_weight);
    flow->last_data_seen = timestamp;

    return flow;
}

int FlowCache::release(Flow* flow, PruneReason reason, bool do_cleanup)
{
    flow->reset(do_cleanup);
    prune_stats.update(reason);
    return remove(flow);
}

int FlowCache::remove(Flow* flow)
{
    if ( flow->next )
        unlink_uni(flow);

    bool deleted = hash_table->remove(flow->key);

    // FIXIT-M This check is added for offload case where both Flow::reset
    // and Flow::retire try remove the flow from hash. Flow::reset should
    // just mark the flow as pending instead of trying to remove it.
    if ( deleted )
        memory::MemoryCap::update_deallocations(config.proto[to_utype(flow->key->pkt_type)].cap_weight);

    return deleted;
}

int FlowCache::retire(Flow* flow)
{
    flow->reset(true);
    flow->term();
    prune_stats.update(PruneReason::NONE);
    return remove(flow);
}

unsigned FlowCache::prune_stale(uint32_t thetime, const Flow* save_me)
{
    ActiveSuspendContext act_susp;

    unsigned pruned = 0;
    auto flow = static_cast<Flow*>(hash_table->first());

    while ( flow and pruned <= cleanup_flows )
    {
#if 0
        // FIXIT-RC this loops forever if 1 flow in cache
        if (flow == save_me)
        {
            break;
            if ( hash_table->get_count() == 1 )
                break;

            hash_table->touch();
        }
#else
        // Reached the current flow. This *should* be the newest flow
        if ( flow == save_me )
        {
            // assert( flow->last_data_seen + config.pruning_timeout >= thetime );
            // bool rv = hash_table->touch(); assert( !rv );
            break;
        }
#endif
        if ( flow->is_suspended() )
            break;

        if ( flow->last_data_seen + config.pruning_timeout >= thetime )
            break;

        flow->ssn_state.session_flags |= SSNFLAG_TIMEDOUT;
        release(flow, PruneReason::IDLE);
        ++pruned;

        flow = static_cast<Flow*>(hash_table->first());
    }

    return pruned;
}

unsigned FlowCache::prune_unis(PktType pkt_type)
{
    ActiveSuspendContext act_susp;

    // we may have many or few unis; need to find reasonable ratio
    // FIXIT-M max_uni should be based on typical ratios seen in perfmon
    const unsigned max_uni = (config.max_flows >> 2) + 1;
    unsigned pruned = 0;
    FlowUniList* uni_list;

    if ( pkt_type == PktType::IP )
        uni_list = uni_ip_flows;
    else
        uni_list = uni_flows;

    Flow* flow = uni_list->get_oldest_uni();
    while ( (uni_list->get_count() > max_uni) && flow && (pruned < cleanup_flows) )
    {
        Flow* prune_me = flow;
        flow = prune_me->prev;

        if ( prune_me->was_blocked() )
            continue;

        release(prune_me, PruneReason::UNI);
        ++pruned;
    }

    return pruned;
}

unsigned FlowCache::prune_excess(const Flow* save_me)
{
    ActiveSuspendContext act_susp;

    auto max_cap = config.max_flows - cleanup_flows;
    assert(max_cap > 0);

    unsigned pruned = 0;
    unsigned blocks = 0;

    // initially skip offloads but if that doesn't work the hash table is iterated from the
    // beginning again. prune offloads at that point.
    unsigned ignore_offloads = hash_table->get_count();

    while ( hash_table->get_count() > max_cap and hash_table->get_count() > blocks )
    {
        auto flow = static_cast<Flow*>(hash_table->first());
        assert(flow); // holds true because hash_table->get_count() > 0

        if ( (save_me and flow == save_me) or flow->was_blocked() or
            (flow->is_suspended() and ignore_offloads) )
        {
            // check for non-null save_me above to silence analyzer
            // "called C++ object pointer is null" here
            if ( flow->was_blocked() )
                ++blocks;

            // FIXIT-M we should update last_data_seen upon touch to ensure
            // the hash_table LRU list remains sorted by time
            if ( !hash_table->touch() )
                break;
        }
        else
        {
            flow->ssn_state.session_flags |= SSNFLAG_PRUNED;
            release(flow, PruneReason::EXCESS);
            ++pruned;
        }
        if ( ignore_offloads > 0 )
            --ignore_offloads;
    }

    if (!pruned and hash_table->get_count() > max_cap)
    {
        prune_one(PruneReason::EXCESS, true);
        ++pruned;
    }

    return pruned;
}

bool FlowCache::prune_one(PruneReason reason, bool do_cleanup)
{

    // so we don't prune the current flow (assume current == MRU)
    if ( hash_table->get_count() <= 1 )
        return false;

    // ZHash returns in LRU order, which is updated per packet via find --> move_to_front call
    auto flow = static_cast<Flow*>(hash_table->first());
    assert(flow);

    flow->ssn_state.session_flags |= SSNFLAG_PRUNED;
    release(flow, reason, do_cleanup);

    return true;
}

unsigned FlowCache::timeout(unsigned num_flows, time_t thetime)
{
    ActiveSuspendContext act_susp;
    unsigned retired = 0;

    auto flow = static_cast<Flow*>(hash_table->current());

    if ( !flow )
        flow = static_cast<Flow*>(hash_table->first());

    while ( flow and (retired < num_flows) )
    {
        if ( flow->is_hard_expiration() )
        {
            if ( flow->expire_time > (uint64_t) thetime )
                break;
        }
        else if ( flow->last_data_seen + config.proto[to_utype(flow->key->pkt_type)].nominal_timeout > thetime )
            break;

        if ( HighAvailabilityManager::in_standby(flow) or
            flow->is_suspended() )
        {
            flow = static_cast<Flow*>(hash_table->next());
            continue;
        }

        flow->ssn_state.session_flags |= SSNFLAG_TIMEDOUT;
        release(flow, PruneReason::IDLE);

        ++retired;

        flow = static_cast<Flow*>(hash_table->current());
    }

    return retired;
}

// Remove all flows from the hash table.
unsigned FlowCache::purge()
{
    ActiveSuspendContext act_susp;
    FlagContext<decltype(flags)>(flags, SESSION_CACHE_FLAG_PURGING);

    unsigned retired = 0;

    while ( auto flow = static_cast<Flow*>(hash_table->first()) )
    {
        retire(flow);
        ++retired;
    }

    while ( Flow* flow = (Flow*)hash_table->pop() )
        flow->term();

    return retired;
}
