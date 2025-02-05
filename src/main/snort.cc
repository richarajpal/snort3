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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "snort.h"

#include <daq.h>
#include <sys/stat.h>
#include <syslog.h>

#include "actions/ips_actions.h"
#include "codecs/codec_api.h"
#include "connectors/connectors.h"
#include "detection/fp_config.h"
#include "file_api/file_service.h"
#include "filters/rate_filter.h"
#include "filters/sfrf.h"
#include "filters/sfthreshold.h"
#include "flow/ha.h"
#include "framework/mpse.h"
#include "helpers/process.h"
#include "host_tracker/host_cache.h"
#include "ips_options/ips_options.h"
#include "log/log.h"
#include "log/messages.h"
#include "loggers/loggers.h"
#include "main.h"
#include "main/shell.h"
#include "managers/codec_manager.h"
#include "managers/inspector_manager.h"
#include "managers/ips_manager.h"
#include "managers/event_manager.h"
#include "managers/module_manager.h"
#include "managers/mpse_manager.h"
#include "managers/plugin_manager.h"
#include "managers/script_manager.h"
#include "memory/memory_cap.h"
#include "network_inspectors/network_inspectors.h"
#include "packet_io/active.h"
#include "packet_io/sfdaq.h"
#include "packet_io/trough.h"
#include "parser/cmd_line.h"
#include "parser/parser.h"
#include "profiler/profiler.h"
#include "search_engines/search_engines.h"
#include "service_inspectors/service_inspectors.h"
#include "side_channel/side_channel.h"
#include "stream/stream_inspectors.h"
#include "target_based/sftarget_reader.h"
#include "time/periodic.h"
#include "utils/util.h"

#ifdef PIGLET
#include "piglet/piglet.h"
#include "piglet/piglet_manager.h"
#include "piglet_plugins/piglet_plugins.h"
#endif

#ifdef SHELL
#include "ac_shell_cmd.h"
#include "control_mgmt.h"
#endif

#include "build.h"
#include "snort_config.h"
#include "thread_config.h"

using namespace snort;
using namespace std;

static SnortConfig* snort_cmd_line_conf = nullptr;
static pid_t snort_main_thread_pid = 0;

//-------------------------------------------------------------------------
// initialization
//-------------------------------------------------------------------------

void Snort::init(int argc, char** argv)
{
    init_signals();
    ThreadConfig::init();

#if defined(NOCOREFILE)
    SetNoCores();
#else
    StoreSnortInfoStrings();
#endif

    InitProtoNames();
    SFAT_Init();

    load_actions();
    load_codecs();
    load_connectors();
    load_ips_options();
    load_loggers();
#ifdef PIGLET
    load_piglets();
#endif
    load_search_engines();
    load_stream_inspectors();
    load_network_inspectors();
    load_service_inspectors();

    snort_cmd_line_conf = parse_cmd_line(argc, argv);
    SnortConfig::set_conf(snort_cmd_line_conf);

    LogMessage("--------------------------------------------------\n");
    LogMessage("%s  Snort++ %s-%s\n", get_prompt(), VERSION, BUILD);
    LogMessage("--------------------------------------------------\n");

#ifdef PIGLET
    Piglet::Manager::init();
#endif

    SideChannelManager::pre_config_init();

    ModuleManager::init();
    ScriptManager::load_scripts(snort_cmd_line_conf->script_paths);
    PluginManager::load_plugins(snort_cmd_line_conf->plugin_path);

    if ( snort_cmd_line_conf->logging_flags & LOGGING_FLAG__SHOW_PLUGINS )
    {
        ModuleManager::dump_modules();
        PluginManager::dump_plugins();
    }

    FileService::init();

    parser_init();
    SnortConfig* sc = ParseSnortConf(snort_cmd_line_conf);

    /* Merge the command line and config file confs to take care of
     * command line overriding config file.
     * Set the global snort_conf that will be used during run time */
    sc->merge(snort_cmd_line_conf);
    SnortConfig::set_conf(sc);

#ifdef PIGLET
    if ( !Piglet::piglet_mode() )
#endif
    CodecManager::instantiate();

#ifdef PIGLET
    if ( !Piglet::piglet_mode() )
#endif
    if ( !sc->output.empty() )
        EventManager::instantiate(sc->output.c_str(), sc);

    HighAvailabilityManager::configure(sc->ha_config);

    if (SnortConfig::alert_before_pass())
        sc->rule_order = "reset block drop alert pass log";

    sc->setup();
    FileService::post_init();

    // Must be after CodecManager::instantiate()
    if ( !InspectorManager::configure(sc) )
        ParseError("can't initialize inspectors");
    else if ( SnortConfig::log_verbose() )
        InspectorManager::print_config(sc);

    ModuleManager::reset_stats(sc);

    if (sc->file_mask != 0)
        umask(sc->file_mask);
    else
        umask(077);    /* set default to be sane */

    /* Need to do this after dynamic detection stuff is initialized, too */
    IpsManager::global_init(sc);

    sc->post_setup();

    const MpseApi* search_api = sc->fast_pattern_config->get_search_api();
    const MpseApi* offload_search_api = sc->fast_pattern_config->get_offload_search_api();

    MpseManager::activate_search_engine(search_api, sc);

    if ((offload_search_api != nullptr) and (offload_search_api != search_api))
        MpseManager::activate_search_engine(offload_search_api, sc);

    SFAT_Start();

#ifdef PIGLET
    if ( !Piglet::piglet_mode() )
#endif
    /* Finish up the pcap list and put in the queues */
    Trough::setup();

    // FIXIT-L refactor stuff done here and in snort_config.cc::VerifyReload()
    if ( sc->bpf_filter.empty() && !sc->bpf_file.empty() )
        sc->bpf_filter = read_infile("bpf_file", sc->bpf_file.c_str());

    if ( !sc->bpf_filter.empty() )
        LogMessage("Snort BPF option: %s\n", sc->bpf_filter.c_str());

    parser_term(sc);

    Active::init(sc);

    LogMessage("%s\n", LOG_DIV);

    SFDAQ::init(sc->daq_config);
}

// this function should only include initialization that must be done as a
// non-root user such as creating log files.  other initialization stuff should
// be in the main initialization function since, depending on platform and
// configuration, this may be running in a background thread while passing
// packets in a fail open mode in the main thread.  we don't want big delays
// here to cause excess latency or dropped packets in that thread which may
// be the case if all threads are pinned to a single cpu/core.
//
// clarification: once snort opens/starts the DAQ, packets are queued for snort
// and must be disposed of quickly or the queue will overflow and packets will
// be dropped so the fail open thread does the remaining initialization while
// the main thread passes packets.  prior to opening and starting the DAQ,
// packet passing is done by the driver/hardware.  the goal then is to put as
// much initialization stuff in Snort::init() as possible and to restrict this
// function to those things that depend on DAQ startup or non-root user/group.

bool Snort::drop_privileges()
{
    /* Enter the chroot jail if necessary. */
    if (!SnortConfig::get_conf()->chroot_dir.empty() &&
        !EnterChroot(SnortConfig::get_conf()->chroot_dir, SnortConfig::get_conf()->log_dir))
        return false;

    /* Drop privileges if requested. */
    if (SnortConfig::get_uid() != -1 || SnortConfig::get_gid() != -1)
    {
        if (!SFDAQ::can_run_unprivileged())
        {
            ParseError("Cannot drop privileges - at least one of the configured DAQ modules does not support unprivileged operation.\n");
            return false;
        }
        if (!SetUidGid(SnortConfig::get_uid(), SnortConfig::get_gid()))
            return false;
    }

    initializing = false;
    privileges_dropped = true;

    return true;
}

void Snort::do_pidfile()
{
    static bool pid_file_created = false;

    if (SnortConfig::create_pid_file() && !pid_file_created)
    {
        CreatePidFile(snort_main_thread_pid);
        pid_file_created = true;
    }
}

//-------------------------------------------------------------------------
// termination
//-------------------------------------------------------------------------

void Snort::term()
{
    /* This function can be called more than once.  For example,
     * once from the SIGINT signal handler, and once recursively
     * as a result of calling pcap_close() below.  We only need
     * to perform the cleanup once, however.  So the static
     * variable already_exiting will act as a flag to prevent
     * double-freeing any memory.  Not guaranteed to be
     * thread-safe, but it will prevent the simple cases.
     */
    static bool already_exiting = false;
    if ( already_exiting )
        return;

    already_exiting = true;
    initializing = false;  // just in case we cut out early

    memory::MemoryCap::print();

    term_signals();
    IpsManager::global_term(SnortConfig::get_conf());
    SFAT_Cleanup();

#ifdef PIGLET
    if ( !Piglet::piglet_mode() )
#endif
    Trough::cleanup();

    ClosePidFile();

    /* remove pid file */
    if ( !SnortConfig::get_conf()->pid_filename.empty() )
    {
        int ret = unlink(SnortConfig::get_conf()->pid_filename.c_str());

        if (ret != 0)
        {
            ErrorMessage("Could not remove pid file %s: %s\n",
                SnortConfig::get_conf()->pid_filename.c_str(), get_error(errno));
        }
    }

    //MpseManager::print_search_engine_stats();

    Periodic::unregister_all();

    LogMessage("%s  Snort exiting\n", get_prompt());

    /* free allocated memory */
    if (SnortConfig::get_conf() == snort_cmd_line_conf)
    {
        delete snort_cmd_line_conf;
        snort_cmd_line_conf = nullptr;
        SnortConfig::set_conf(nullptr);
    }
    else
    {
        delete snort_cmd_line_conf;
        snort_cmd_line_conf = nullptr;

        delete SnortConfig::get_conf();
        SnortConfig::set_conf(nullptr);
    }

    CleanupProtoNames();
    HighAvailabilityManager::term();
    SideChannelManager::term();
    ModuleManager::term();
    PluginManager::release_plugins();
    ScriptManager::release_scripts();
}

void Snort::clean_exit(int)
{
    term();
    closelog();
}

//-------------------------------------------------------------------------
// public methods
//-------------------------------------------------------------------------

bool Snort::initializing = true;
bool Snort::reloading = false;
bool Snort::privileges_dropped = false;

bool Snort::is_starting()
{ return initializing; }

bool Snort::is_reloading()
{ return reloading; }

bool Snort::has_dropped_privileges()
{ return privileges_dropped; }

void Snort::setup(int argc, char* argv[])
{
    set_main_thread();

    // must be done before any other files are opened because we
    // will try to grab file descriptor 3 (if --enable-stdlog)
    OpenLogger();

    init(argc, argv);

    if ( SnortConfig::daemon_mode() )
        daemonize();

    // this must follow daemonization
    snort_main_thread_pid = gettid();

    /* Change groups */
    InitGroups(SnortConfig::get_uid(), SnortConfig::get_gid());

    set_quick_exit(false);

    memory::MemoryCap::calculate(ThreadConfig::get_instance_max());
    memory::MemoryCap::print();
    host_cache.print_config();

    TimeStart();
}

void Snort::cleanup()
{
    TimeStop();

    SFDAQ::term();
    FileService::close();

    if ( !SnortConfig::test_mode() )  // FIXIT-M ideally the check is in one place
        PrintStatistics();

    CloseLogger();
    ThreadConfig::term();
    clean_exit(0);
}

void Snort::reload_failure_cleanup(SnortConfig* sc)
{
    parser_term(sc);
    delete sc;
    reloading = false;
}

// FIXIT-M refactor this so startup and reload call the same core function to
// instantiate things that can be reloaded
SnortConfig* Snort::get_reload_config(const char* fname)
{
    reloading = true;
    ModuleManager::reset_errors();
    reset_parse_errors();
    trim_heap();

    parser_init();
    SnortConfig* sc = ParseSnortConf(snort_cmd_line_conf, fname, false);
    sc->merge(snort_cmd_line_conf);

    if ( get_parse_errors() || ModuleManager::get_errors() || !sc->verify() )
    {
        reload_failure_cleanup(sc);
        return nullptr;
    }

    sc->setup();

#ifdef SHELL
    ControlMgmt::reconfigure_controls();
#endif

    if ( get_parse_errors() or !InspectorManager::configure(sc) )
    {
        reload_failure_cleanup(sc);
        return nullptr;
    }

    FileService::verify_reload(sc);
    if ( get_reload_errors() )
    {
        reload_failure_cleanup(sc);
        return nullptr;
    }

    if ((sc->file_mask != 0) && (sc->file_mask != SnortConfig::get_conf()->file_mask))
        umask(sc->file_mask);

    // FIXIT-L is this still needed?
    /* Transfer any user defined rule type outputs to the new rule list */
    {
        RuleListNode* cur = SnortConfig::get_conf()->rule_lists;

        for (; cur != nullptr; cur = cur->next)
        {
            RuleListNode* rnew = sc->rule_lists;

            for (; rnew != nullptr; rnew = rnew->next)
            {
                if (strcasecmp(cur->name, rnew->name) == 0)
                {
                    EventManager::copy_outputs(
                        rnew->RuleList->AlertList, cur->RuleList->AlertList);

                    EventManager::copy_outputs(
                        rnew->RuleList->LogList, cur->RuleList->LogList);
                    break;
                }
            }
        }
    }

    sc->post_setup();

    if ( sc->fast_pattern_config->get_search_api() !=
        SnortConfig::get_conf()->fast_pattern_config->get_search_api() )
    {
        MpseManager::activate_search_engine(sc->fast_pattern_config->get_search_api(), sc);
    }

    InspectorManager::update_policy(sc);
    reloading = false;
    parser_term(sc);

    return sc;
}

SnortConfig* Snort::get_updated_policy(SnortConfig* other_conf, const char* fname, const char* iname)
{
    reloading = true;

    SnortConfig* sc = new SnortConfig(other_conf);

    if ( fname )
    {
        Shell sh = Shell(fname);
        sh.configure(sc, false, true);

        if ( ModuleManager::get_errors() || !sc->verify() )
        {
            sc->cloned = true;
            InspectorManager::update_policy(other_conf);
            delete sc;
            set_default_policy(other_conf);
            reloading = false;
            return nullptr;
        }
    }

    if ( iname )
    {
        if ( !InspectorManager::delete_inspector(sc, iname) )
        {
            sc->cloned = true;
            InspectorManager::update_policy(other_conf);
            delete sc;
            set_default_policy(other_conf);
            reloading = false;
            return nullptr;
        }
    }

    if ( !InspectorManager::configure(sc, true) )
    {
        sc->cloned = true;
        InspectorManager::update_policy(other_conf);
        delete sc;
        set_default_policy(other_conf);
        reloading = false;
        return nullptr;
    }

    other_conf->cloned = true;
    sc->policy_map->get_inspection_policy()->clone_dbus(other_conf, iname);
    InspectorManager::update_policy(sc);
    reloading = false;
    return sc;
}

SnortConfig* Snort::get_updated_module(SnortConfig* other_conf, const char* name)
{
    reloading = true;

    SnortConfig* sc = new SnortConfig(other_conf);

    if ( name )
    {
        ModuleManager::reset_errors();
        ModuleManager::reload_module(name, sc);
        if ( ModuleManager::get_errors() || !sc->verify() )
        {
            sc->cloned = true;
            InspectorManager::update_policy(other_conf);
            delete sc;
            set_default_policy(other_conf);
            reloading = false;
            return nullptr;
        }
    }

    if ( !InspectorManager::configure(sc, true) )
    {
        sc->cloned = true;
        InspectorManager::update_policy(other_conf);
        delete sc;
        set_default_policy(other_conf);
        reloading = false;
        return nullptr;
    }

    other_conf->cloned = true;
    sc->policy_map->get_inspection_policy()->clone_dbus(other_conf, name);
    InspectorManager::update_policy(sc);
    reloading = false;
    return sc;
}
