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

// rna_inspector.cc author Masud Hasan <mashasan@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "rna_inspector.h"

#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

#include "log/messages.h"
#include "managers/inspector_manager.h"
#include "protocols/packet.h"

#include "rna_event_handler.h"

#ifdef UNIT_TEST
#include "catch/snort_catch.h"
#endif

using namespace snort;
using namespace std;

THREAD_LOCAL RnaStats rna_stats;
THREAD_LOCAL ProfileStats rna_perf_stats;

//-------------------------------------------------------------------------
// class stuff
//-------------------------------------------------------------------------

RnaInspector::RnaInspector(RnaModule* mod)
{
    mod_conf = mod->get_config();
    load_rna_conf();
    pnd = new RnaPnd(mod_conf? mod_conf->enable_logger : false);
}

RnaInspector::~RnaInspector()
{
    delete pnd;
    delete rna_conf;
    delete mod_conf;
}

bool RnaInspector::configure(SnortConfig*)
{
    DataBus::subscribe( STREAM_ICMP_NEW_FLOW_EVENT, new RnaIcmpEventHandler(*pnd) );
    DataBus::subscribe( STREAM_IP_NEW_FLOW_EVENT, new RnaIpEventHandler(*pnd) );
    DataBus::subscribe( STREAM_UDP_NEW_FLOW_EVENT, new RnaUdpEventHandler(*pnd) );
    DataBus::subscribe( STREAM_TCP_SYN_EVENT, new RnaTcpSynEventHandler(*pnd) );
    DataBus::subscribe( STREAM_TCP_SYN_ACK_EVENT, new RnaTcpSynAckEventHandler(*pnd) );
    DataBus::subscribe( STREAM_TCP_MIDSTREAM_EVENT, new RnaTcpMidstreamEventHandler(*pnd) );

    return true;
}

void RnaInspector::eval(Packet* p)
{
    Profile profile(rna_perf_stats);
    ++rna_stats.other_packets;

    assert( !p->flow );
    assert( !(BIT((unsigned)p->type()) & PROTO_BIT__ANY_SSN) );

    // Handling untracked sessions, e.g., non-IP packets
    // pnd->analyze_flow_non_ip(p);
    UNUSED(p);
}

void RnaInspector::show(SnortConfig*)
{
    LogMessage("RNA Configuration\n");

    if (mod_conf)
    {
        if (!mod_conf->rna_conf_path.empty())
            LogMessage("    Config path:            %s\n", mod_conf->rna_conf_path.c_str());
        if (!mod_conf->rna_util_lib_path.empty())
            LogMessage("    Library path:           %s\n", mod_conf->rna_util_lib_path.c_str());
        if (!mod_conf->fingerprint_dir.empty())
            LogMessage("    Fingerprint dir:        %s\n", mod_conf->fingerprint_dir.c_str());
        if (!mod_conf->custom_fingerprint_dir.empty())
            LogMessage("    Custom fingerprint dir: %s\n",
                mod_conf->custom_fingerprint_dir.c_str());
        LogMessage("    Enable logger:          %d\n", mod_conf->enable_logger);
    }

    if (rna_conf)
    {
        LogMessage("    Update timeout:         %u secs\n", rna_conf->update_timeout);
        LogMessage("    Max host client apps:   %u\n", rna_conf->max_host_client_apps);
        LogMessage("    Max payloads:           %u\n", rna_conf->max_payloads);
        LogMessage("    Max host services:      %u\n", rna_conf->max_host_services);
        LogMessage("    Max host service info:  %u\n", rna_conf->max_host_service_info);
        LogMessage("    Banner grab:            %d\n", rna_conf->enable_banner_grab);
    }

    LogMessage("\n");
}

void RnaInspector::tinit()
{
    // thread local initialization
}

void RnaInspector::tterm()
{
    // thread local cleanup
}

void RnaInspector::load_rna_conf()
{
    if (rna_conf)
        delete rna_conf;
    rna_conf = new RnaConfig; // initialize with defaults

    if (!mod_conf)
        return;

    ifstream in_stream(mod_conf->rna_conf_path);
    if (!in_stream)
        return;

    uint32_t line_num = 0;

    while (in_stream)
    {
        string line;
        getline(in_stream, line);
        ++line_num;
        if (line.empty() or line.front() == '#')
            continue;

        string config_type;
        string config_key;
        string config_value;
        istringstream line_stream(line);

        line_stream >> config_type >> config_key >> config_value;
        if (config_type.empty() or config_key.empty() or config_value.empty())
        {
            WarningMessage("RNA: Empty configuration items at line %u from %s\n",
                line_num, mod_conf->rna_conf_path.c_str());
            continue;
        }

        if (config_type == "pnd" and config_key == "UpdateTimeout")
            rna_conf->update_timeout = stoi(config_value);
        else if (config_type == "config" and config_key == "MaxHostClientApps")
            rna_conf->max_host_client_apps = stoi(config_value);
        else if (config_type == "config" and config_key == "MaxPayloads")
            rna_conf->max_payloads = stoi(config_value);
        else if (config_type == "config" and config_key == "MaxHostServices")
            rna_conf->max_host_services = stoi(config_value);
        else if (config_type == "config" and config_key == "MaxHostServiceInfo")
            rna_conf->max_host_service_info = stoi(config_value);
        else if (config_type == "protoid" and config_key == "BannerGrab" and config_value != "0")
            rna_conf->enable_banner_grab = true;
    }

    in_stream.close();
}

//-------------------------------------------------------------------------
// api stuff
//-------------------------------------------------------------------------

static Module* rna_mod_ctor()
{ return new RnaModule; }

static void rna_mod_dtor(Module* m)
{ delete m; }

static void rna_inspector_pinit()
{
    // global initialization
}

static void rna_inspector_pterm()
{
    // global cleanup
}

static Inspector* rna_inspector_ctor(Module* m)
{ return new RnaInspector((RnaModule*)m); }

static void rna_inspector_dtor(Inspector* p)
{ delete p; }

static const InspectApi rna_inspector_api =
{
    {
        PT_INSPECTOR,
        sizeof(InspectApi),
        INSAPI_VERSION,
        0,
        API_RESERVED,
        API_OPTIONS,
        RNA_NAME,
        RNA_HELP,
        rna_mod_ctor,
        rna_mod_dtor
    },
    IT_CONTROL,
    PROTO_BIT__ALL ^ PROTO_BIT__ANY_SSN,
    nullptr, // buffers
    nullptr, // service
    rna_inspector_pinit,
    rna_inspector_pterm,
    nullptr, // pre-config tinit
    nullptr, // pre-config tterm
    rna_inspector_ctor,
    rna_inspector_dtor,
    nullptr, // ssn
    nullptr  // reset
};

#ifdef BUILDING_SO
SO_PUBLIC const BaseApi* snort_plugins[] =
#else
const BaseApi* nin_rna[] =
#endif
{
    &rna_inspector_api.base,
    nullptr
};

#ifdef UNIT_TEST
TEST_CASE("RNA inspector", "[rna_inspector]")
{
    SECTION("inspector show")
    {
        RnaModule mod;
        RnaInspector ins(&mod);
        ins.show(nullptr);
    }
}
#endif
