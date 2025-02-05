//--------------------------------------------------------------------------
// Copyright (C) 2019-2019 Cisco and/or its affiliates. All rights reserved.
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

// flow_test.cc author Prajwal Srinivas <psreenat@cisco.com>
// unit test main

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "protocols/ip.h"
#include "protocols/layer.h"
#include "protocols/packet.h"
#include "flow/flow.h"
#include "flow/flow_stash.h"
#include "flow/ha.h"
#include "main/snort_debug.h"
#include "framework/inspector.h"
#include "framework/data_bus.h"
#include "memory/memory_cap.h"
#include "detection/detection_engine.h"

#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/TestHarness.h>

using namespace snort;

Packet::Packet(bool) { }
Packet::~Packet() { }

void Inspector::rem_ref() {}

void Inspector::add_ref() {}

void memory::MemoryCap::update_allocations(size_t n) {}

void memory::MemoryCap::update_deallocations(size_t n) {}

bool memory::MemoryCap::free_space(size_t n) { return false; }

bool HighAvailabilityManager::active() { return false; }

FlowHAState::FlowHAState() = default;

void FlowHAState::reset() {}

snort::FlowStash::~FlowStash() {}

void FlowStash::reset() {}

void DetectionEngine::onload(Flow* flow) {}

Packet* DetectionEngine::set_next_packet(Packet* parent) { return nullptr; }

IpsContext* DetectionEngine::get_context() { return nullptr; }

DetectionEngine::DetectionEngine() = default;

DetectionEngine::~DetectionEngine() {}

bool snort::layer::set_outer_ip_api(const Packet* const p,
    ip::IpApi& api,
    int8_t& curr_layer)
{ return false; }

uint8_t ip::IpApi::ttl() const { return 0; }

const Layer* snort::layer::get_mpls_layer(const Packet* const p) { return nullptr; }

void snort::DataBus::publish(const char* key, Packet* p, Flow* f) {}

TEST_GROUP(nondefault_timeout)
{
    void setup() override
    {
    }

    void teardown() override
    {
    }
};

TEST(nondefault_timeout, hard_expiration)
{
    uint64_t validate = 100;
    Packet pkt(false);
    Flow *flow = new Flow();
    DAQ_PktHdr_t pkthdr;

    pkt.pkth = &pkthdr;
    pkthdr.ts.tv_sec = 0;

    flow->set_default_session_timeout(validate, true);
    flow->set_hard_expiration();
    flow->set_expire(&pkt, validate);
    
    CHECK( flow->is_hard_expiration() == true);
    CHECK( flow->expire_time == validate );
    
    delete flow;
}

int main(int argc, char** argv)
{
    int return_value = CommandLineTestRunner::RunAllTests(argc, argv);
    return return_value;
}


