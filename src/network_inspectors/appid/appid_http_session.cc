//--------------------------------------------------------------------------
// Copyright (C) 2017-2018 Cisco and/or its affiliates. All rights reserved.
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

// appid_http_session.cc author davis mcpherson <davmcphe@cisco.com>
// Created on: Apr 19, 2017

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "appid_http_session.h"

#include "profiler/profiler.h"

#include "app_info_table.h"
#include "appid_config.h"
#include "appid_debug.h"
#include "appid_session.h"
#include "detector_plugins/http_url_patterns.h"
#include "http_xff_fields.h"
#ifdef ENABLE_APPID_THIRD_PARTY
#include "tp_lib_handler.h"
#endif

using namespace snort;

static const char* httpFieldName[ MAX_HTTP_FIELD_ID ] = // for use in debug messages
{
    "useragent",
    "host",
    "referer",
    "uri",
    "cookie",
    "req_body",
    "content_type",
    "location",
    "body",
};

snort::ProfileStats httpPerfStats;

AppIdHttpSession::AppIdHttpSession(AppIdSession& asd)
    : asd(asd),
    host(nullptr),
    url(nullptr),
    uri(nullptr),
    referer(nullptr),
    useragent(nullptr),
    via(nullptr),
    cookie(nullptr),
    body(nullptr),
    response_code(nullptr),
    content_type(nullptr),
    location(nullptr),
    req_body(nullptr),
    server(nullptr),
    x_working_with(nullptr)
{
    http_matchers = HttpPatternMatchers::get_instance();
}

AppIdHttpSession::~AppIdHttpSession()
{
    delete xff_addr;
    if (host)
        delete host;
    if (url)
        delete url;
    if (uri)
        delete uri;
    if (referer)
        delete referer;
    if (useragent)
        delete useragent;
    if (via)
        delete via;
    if (cookie)
        delete cookie;
    if (body)
        delete body;
    if (response_code)
        delete response_code;
    if (content_type)
        delete content_type;
    if (location)
        delete location;
    if (req_body)
        delete req_body;
    if (server)
        delete server;
    if (x_working_with)
        delete x_working_with;
}

void AppIdHttpSession::free_chp_matches(ChpMatchDescriptor& cmd, unsigned num_matches)
{
    for (unsigned i = 0; i <= num_matches; i++)
        if ( !cmd.chp_matches[i].empty() )
            cmd.chp_matches[i].clear();
}

int AppIdHttpSession::initial_chp_sweep(ChpMatchDescriptor& cmd)
{
    CHPApp* cah = nullptr;

    for (unsigned i = 0; i <= MAX_KEY_PATTERN; i++)
    {
        if (cmd.buffer[i] && cmd.length[i])
        {
            cmd.cur_ptype = (HttpFieldIds)i;
            http_matchers->scan_key_chp(cmd);
        }
    }

    if (cmd.match_tally.empty())
    {
        free_chp_matches(cmd, MAX_KEY_PATTERN);
        return 0;
    }

    int longest = 0;
    for (auto& item: cmd.match_tally)
    {
        // Only those items with key_pattern_countdown field reduced to zero are a full match
        if (item.key_pattern_countdown)
            continue;
        if (longest < item.key_pattern_length_sum)
        {
            // We've found a new longest pattern set
            longest = item.key_pattern_length_sum;
            cah = item.chpapp;
        }
    }

    if ( !cah )
    {
        free_chp_matches(cmd, MAX_KEY_PATTERN);
        return 0;
    }

    /***************************************************************
       candidate has been chosen and it is pointed to by cah
       we will preserve any match sets until the calls to scanCHP()
     ***************************************************************/
    for (unsigned i = 0; i < MAX_HTTP_FIELD_ID; i++)
    {
        ptype_scan_counts[i] = cah->ptype_scan_counts[i];
        ptype_req_counts[i] = cah->ptype_req_counts[i] + cah->ptype_rewrite_insert_used[i];
        if (i > 3 && !cah->ptype_scan_counts[i]
            && !asd.get_session_flags(APPID_SESSION_SPDY_SESSION))
        {
            asd.clear_session_flags(APPID_SESSION_CHP_INSPECTING);
#ifdef ENABLE_APPID_THIRD_PARTY
            if (asd.tpsession)
                asd.tpsession->clear_attr(TP_ATTR_CONTINUE_MONITORING);
#endif
        }
    }
    chp_candidate = cah->appIdInstance;
    app_type_flags = cah->app_type_flags;
    num_matches = cah->num_matches;
    num_scans = cah->num_scans;

#ifdef ENABLE_APPID_THIRD_PARTY
    if (asd.tpsession)
    {
        if ((ptype_scan_counts[RSP_CONTENT_TYPE_FID]))
            asd.tpsession->set_attr(TP_ATTR_COPY_RESPONSE_CONTENT);
        else
            asd.tpsession->clear_attr(TP_ATTR_COPY_RESPONSE_CONTENT);

        if ((ptype_scan_counts[RSP_LOCATION_FID]))
            asd.tpsession->set_attr(TP_ATTR_COPY_RESPONSE_LOCATION);
        else
            asd.tpsession->clear_attr(TP_ATTR_COPY_RESPONSE_LOCATION);

        if ((ptype_scan_counts[RSP_BODY_FID]))
            asd.tpsession->set_attr(TP_ATTR_COPY_RESPONSE_BODY);
        else
            asd.tpsession->clear_attr(TP_ATTR_COPY_RESPONSE_BODY);
    }
#endif

    return 1;
}

void AppIdHttpSession::init_chp_match_descriptor(ChpMatchDescriptor& cmd)
{
    cmd.buffer[REQ_AGENT_FID] = useragent ? useragent->c_str() : nullptr;
    cmd.buffer[REQ_HOST_FID] = host ? host->c_str() : nullptr;
    cmd.buffer[REQ_REFERER_FID] = referer ? referer->c_str() : nullptr;
    cmd.buffer[REQ_URI_FID] = uri ? uri->c_str() : nullptr;
    cmd.buffer[REQ_COOKIE_FID] = cookie ? cookie->c_str() : nullptr;
    cmd.buffer[REQ_BODY_FID] = req_body ? req_body->c_str() : nullptr;
    cmd.buffer[RSP_CONTENT_TYPE_FID] = content_type ? content_type->c_str() : nullptr;
    cmd.buffer[RSP_LOCATION_FID] = location ? location->c_str() : nullptr;
    cmd.buffer[RSP_BODY_FID] = body ? body->c_str() : nullptr;

    cmd.length[REQ_AGENT_FID] = useragent ? useragent->size() : 0;
    cmd.length[REQ_HOST_FID] = host ? host->size() : 0;
    cmd.length[REQ_REFERER_FID] = referer ? referer->size() : 0;
    cmd.length[REQ_URI_FID] = uri ? uri->size() : 0;
    cmd.length[REQ_COOKIE_FID] = cookie ? cookie->size() : 0;
    cmd.length[REQ_BODY_FID] = req_body ? req_body->size() : 0;
    cmd.length[RSP_CONTENT_TYPE_FID] = content_type ? content_type->size() : 0;
    cmd.length[RSP_LOCATION_FID] = location ? location->size() : 0;
    cmd.length[RSP_BODY_FID] = body ? body->size() : 0;
}

void AppIdHttpSession::process_chp_buffers()
{
    ChpMatchDescriptor cmd;

    init_chp_match_descriptor(cmd);
    if ( chp_hold_flow )
        chp_finished = false;

    if ( !chp_candidate )
    {
        // remove artifacts from previous matches before we start again.
        for (auto f : http_fields)
            f.field.clear();

        if ( !initial_chp_sweep(cmd) )
            chp_finished = true; // this is a failure case.
    }

    if ( !chp_finished && chp_candidate )
    {
        char* user = nullptr;
        char* version = nullptr;

        for (unsigned i = 0; i < MAX_HTTP_FIELD_ID; i++)
        {
            if ( !ptype_scan_counts[i] )
                continue;

            if ( cmd.buffer[i] && cmd.length[i] )
            {
                int num_found = 0;
                cmd.cur_ptype = (HttpFieldIds)i;
                AppId ret = http_matchers->scan_chp(cmd, &version, &user, &num_found, this,
                    asd.config->mod_config);
                total_found += num_found;
                if (!ret || num_found < ptype_req_counts[i])
                {
                    // No match at all or the required matches for the field was NOT made
                    if (!num_matches)
                    {
                        // num_matches == 0 means: all must succeed
                        // give up early
                        chp_candidate = 0;
                        break;
                    }
                }
            }
            else if ( !num_matches )
            {
                // num_matches == 0 means: all must succeed  give up early
                chp_candidate = 0;
                break;
            }

            // Decrement the expected scan count toward 0.
            ptype_scan_counts[i] = 0;
            num_scans--;
            // if we have reached the end of the list of scans (which have something to do), then
            // num_scans == 0
            if (num_scans == 0)
            {
                // we finished the last scan
                // either the num_matches value was zero and we failed early-on or we need to check
                // for the min.
                if (num_matches &&
                    total_found < num_matches)
                {
                    // There was a minimum scans match count (num_matches != 0)
                    // And we did not reach that minimum
                    chp_candidate = 0;
                    break;
                }
                // All required matches were met.
                chp_finished = true;
                break;
            }
        }

        free_chp_matches(cmd, MAX_PATTERN_TYPE);

        if ( !chp_candidate )
        {
            chp_finished = true;
            if ( version )
            {
                snort_free(version);
                version = nullptr;
            }

            if ( user )
            {
                snort_free(user);
                user = nullptr;
            }

            cmd.free_rewrite_buffers();
            memset(ptype_scan_counts, 0, sizeof(ptype_scan_counts));

            // Make it possible for other detectors to run.
            skip_simple_detect = false;
            return;
        }

        if (chp_candidate && chp_finished)
        {
            AppId chp_final = chp_alt_candidate ? chp_alt_candidate
                : CHP_APPIDINSTANCE_TO_ID(chp_candidate);

            if (app_type_flags & APP_TYPE_SERVICE)
                asd.set_service_appid_data(chp_final, nullptr, version);

            if (app_type_flags & APP_TYPE_CLIENT)
                asd.set_client_appid_data(chp_final, version);

            if ( app_type_flags & APP_TYPE_PAYLOAD )
                asd.set_payload_appid_data((AppId)chp_final, version);

            if ( version )
            {
                snort_free(version);
                version = nullptr;
            }

            if ( user )
            {
                if (app_type_flags & APP_TYPE_SERVICE)
                    asd.client.update_user(chp_final, user);
                else
                    asd.client.update_user(asd.service.get_id(), user);
                user = nullptr;
                asd.set_session_flags(APPID_SESSION_LOGIN_SUCCEEDED);
            }

            for (unsigned i = 0; i < MAX_HTTP_FIELD_ID; i++)
                if ( cmd.chp_rewritten[i] )
                {
                    if (appidDebug->is_active())
                        LogMessage("AppIdDbg %s Rewritten %s: %s\n",
                            appidDebug->get_debug_session(),
                            httpFieldName[i], cmd.chp_rewritten[i]);

                    http_fields[i].field = cmd.chp_rewritten[i];
                    cmd.chp_rewritten[i] = nullptr;
                }

            chp_candidate = 0;
            //if we're doing safesearch rewrites, we want to continue to hold the flow
            if (!rebuilt_offsets)
                chp_hold_flow = 0;
            asd.scan_flags &= ~SCAN_HTTP_VIA_FLAG;
            asd.scan_flags &= ~SCAN_HTTP_USER_AGENT_FLAG;
            asd.scan_flags &= ~SCAN_HTTP_HOST_URL_FLAG;
            memset(ptype_scan_counts, 0, sizeof(ptype_scan_counts));
        }
        else /* if we have a candidate, but we're not finished */
        {
            if ( user )
            {
                snort_free(user);
                user = nullptr;
            }

            cmd.free_rewrite_buffers();
        }
    }
}

int AppIdHttpSession::process_http_packet(AppidSessionDirection direction)
{
    snort::Profile http_profile_context(httpPerfStats);
    AppId service_id = APP_ID_NONE;
    AppId client_id = APP_ID_NONE;
    AppId payload_id = APP_ID_NONE;
    bool have_tp = asd.tpsession;

    // For fragmented HTTP headers, do not process if none of the fields are set.
    // These fields will get set when the HTTP header is reassembled.

    if ( !useragent && !host && !referer && !uri )
    {
        if (!skip_simple_detect)
            asd.clear_http_flags();
        return 0;
    }

    if ( direction == APP_ID_FROM_RESPONDER &&
        !asd.get_session_flags(APPID_SESSION_RESPONSE_CODE_CHECKED) )
    {
        if ( response_code )
        {
            asd.set_session_flags(APPID_SESSION_RESPONSE_CODE_CHECKED);
            constexpr auto RESPONSE_CODE_LENGTH = 3;
            if (response_code->size() != RESPONSE_CODE_LENGTH)
            {
                if (appidDebug->is_active())
                    LogMessage("AppIdDbg %s Bad http response code.\n",
                        appidDebug->get_debug_session());
                asd.reset_session_data();
                return 0;
            }
        }
#if RESPONSE_CODE_PACKET_THRESHHOLD
        else if (++(response_code_packets) == RESPONSE_CODE_PACKET_THRESHHOLD)
        {
            set_session_flags(APPID_SESSION_RESPONSE_CODE_CHECKED);
            /* didn't receive response code in first X packets. Stop processing this session */
            asd.reset_session_data();
            if (appidDebug->is_active())
                LogMessage("AppIdDbg %s No response code received\n",
                    appidDebug->get_debug_session());
            return 0;
        }
#endif
    }

    if (asd.service.get_id() == APP_ID_NONE)
    {
        asd.service.set_id(APP_ID_HTTP);
        asd.set_session_flags(APPID_SESSION_SERVICE_DETECTED | APPID_SESSION_HTTP_SESSION);
        asd.service_disco_state = APPID_DISCO_STATE_FINISHED;
    }

    if (appidDebug->is_active())
        LogMessage("AppIdDbg %s chp_finished %d chp_hold_flow %d\n",
            appidDebug->get_debug_session(),
            chp_finished, chp_hold_flow);

    if (!chp_finished || chp_hold_flow)
        process_chp_buffers();

    if (!skip_simple_detect)  // true if processCHP found match
    {
        if (!asd.get_session_flags(APPID_SESSION_APP_REINSPECT))
        {
            // Scan Server Header for Vendor & Version
            // FIXIT-M: Should we be checking the scan_flags even when
            //     tp_appid_module is off?
            if ( (have_tp && (asd.scan_flags & SCAN_HTTP_VENDOR_FLAG) && server)
                || (!have_tp && server) )
            {
                if ( asd.service.get_id() == APP_ID_NONE || asd.service.get_id() == APP_ID_HTTP )
                {
                    //AppIdServiceSubtype* local_subtype = nullptr;
                    char* vendorVersion = nullptr;
                    char* vendor = nullptr;

                    http_matchers->get_server_vendor_version(server->c_str(), server->size(),
                        &vendorVersion, &vendor, &asd.subtype);
                    if (vendor || vendorVersion)
                    {
                        asd.service.set_vendor(vendor);
                        asd.service.set_version(vendorVersion);
                        asd.scan_flags &= ~SCAN_HTTP_VENDOR_FLAG;

                        snort_free(vendor);
                        snort_free(vendorVersion);
                    }
#if 0
                    if (local_subtype)  // FIXIT-W always false
                    {
                        AppIdServiceSubtype** tmpSubtype;

                        for (tmpSubtype = &asd.subtype; *tmpSubtype; tmpSubtype =
                            &(*tmpSubtype)->next)
                            ;

                        *tmpSubtype = local_subtype;
                    }
#endif
                }
            }

            if (is_webdav)
            {
                if (appidDebug->is_active() and asd.payload.get_id() != APP_ID_WEBDAV)
                    LogMessage("AppIdDbg %s Data is webdav\n", appidDebug->get_debug_session());
                asd.set_payload_appid_data(APP_ID_WEBDAV, nullptr);
            }

            // Scan User-Agent for Browser types or Skype
            if ( (asd.scan_flags & SCAN_HTTP_USER_AGENT_FLAG)
                && asd.client.get_id() <= APP_ID_NONE && useragent )
            {
                char* version = nullptr;

                http_matchers->identify_user_agent(useragent->c_str(), useragent->size(),
                    service_id, client_id, &version);
                if (appidDebug->is_active())
                {
                    if (service_id > APP_ID_NONE and service_id != APP_ID_HTTP and
                        asd.service.get_id() != service_id)
                        LogMessage("AppIdDbg %s User Agent is service %d\n",
                            appidDebug->get_debug_session(), service_id);
                    if (client_id > APP_ID_NONE and client_id != APP_ID_HTTP and
                        asd.client.get_id() != client_id)
                        LogMessage("AppIdDbg %s User Agent is client %d\n",
                            appidDebug->get_debug_session(), client_id);
                }
                asd.set_service_appid_data(service_id, nullptr, nullptr);
                asd.set_client_appid_data(client_id, version);
                asd.scan_flags &= ~SCAN_HTTP_USER_AGENT_FLAG;
                snort_free(version);
            }

            /* Scan Via Header for squid */
            if ( !asd.is_payload_appid_set() && (asd.scan_flags & SCAN_HTTP_VIA_FLAG) && via )
            {
                payload_id = http_matchers->get_appid_by_pattern(via->c_str(), via->size(),
                    nullptr);
                if (appidDebug->is_active() && payload_id > APP_ID_NONE &&
                    asd.payload.get_id() != payload_id)
                    LogMessage("AppIdDbg %s VIA is payload %d\n", appidDebug->get_debug_session(),
                        payload_id);
                asd.set_payload_appid_data((AppId)payload_id, nullptr);
                asd.scan_flags &= ~SCAN_HTTP_VIA_FLAG;
            }
        }

        /* Scan X-Working-With HTTP header */
        // FIXIT-M: Should we be checking the scan_flags even when
        //     tp_appid_module is off?
        if ( (have_tp && (asd.scan_flags & SCAN_HTTP_XWORKINGWITH_FLAG) &&
            x_working_with) || (!have_tp && x_working_with))
        {
            AppId appId;
            char* version = nullptr;

            appId = http_matchers->scan_header_x_working_with(x_working_with->c_str(),
                x_working_with->size(), &version);
            if ( appId )
            {
                if (direction == APP_ID_FROM_INITIATOR)
                {
                    if (appidDebug->is_active() && client_id > APP_ID_NONE && client_id !=
                        APP_ID_HTTP && asd.client.get_id() != client_id)
                        LogMessage("AppIdDbg %s X is client %d\n", appidDebug->get_debug_session(),
                            appId);

                    asd.set_client_appid_data(appId, version);
                }
                else
                {
                    if (appidDebug->is_active() && service_id > APP_ID_NONE && service_id !=
                        APP_ID_HTTP && asd.service.get_id() != service_id)
                        LogMessage("AppIdDbg %s X service %d\n", appidDebug->get_debug_session(),
                            appId);
                    asd.set_service_appid_data(appId, nullptr, version);
                }
                asd.scan_flags &= ~SCAN_HTTP_XWORKINGWITH_FLAG;
            }

            snort_free(version);
        }

        // Scan Content-Type Header for multimedia types and scan contents
        // FIXIT-M: Should we be checking the scan_flags even when
        //     tp_appid_module is off?
        if ( (have_tp && (asd.scan_flags & SCAN_HTTP_CONTENT_TYPE_FLAG)
            && content_type && !asd.is_payload_appid_set())
            || (!have_tp && !asd.is_payload_appid_set() && content_type) )
        {
            payload_id = http_matchers->get_appid_by_content_type(content_type->c_str(),
                content_type->size());
            if (appidDebug->is_active() && payload_id > APP_ID_NONE
                && asd.payload.get_id() != payload_id)
                LogMessage("AppIdDbg %s Content-Type is payload %d\n",
                    appidDebug->get_debug_session(),
                    payload_id);
            asd.set_payload_appid_data((AppId)payload_id, nullptr);
            asd.scan_flags &= ~SCAN_HTTP_CONTENT_TYPE_FLAG;
        }

        if (asd.scan_flags & SCAN_HTTP_HOST_URL_FLAG)
        {
            AppId referredPayloadAppId = 0;
            char* version = nullptr;
            char* my_host = host ? snort_strdup(host->c_str()) : nullptr;
            const char* refStr = referer ? referer->c_str() : nullptr;
            const char* urlStr = url ? url->c_str() : nullptr;
            if ( http_matchers->get_appid_from_url(my_host, urlStr, &version,
                refStr, &client_id, &service_id, &payload_id,
                &referredPayloadAppId, false) )
            {
                // do not overwrite a previously-set client or service
                if (asd.client.get_id() <= APP_ID_NONE)
                {
                    if (appidDebug->is_active() && client_id > APP_ID_NONE && client_id !=
                        APP_ID_HTTP && asd.client.get_id() != client_id)
                        LogMessage("AppIdDbg %s URL is client %d\n",
                            appidDebug->get_debug_session(),
                            client_id);
                    asd.set_client_appid_data(client_id, nullptr);
                }

                if (asd.service.get_id() <= APP_ID_NONE)
                {
                    if (appidDebug->is_active() && service_id > APP_ID_NONE && service_id !=
                        APP_ID_HTTP && asd.service.get_id() != service_id)
                        LogMessage("AppIdDbg %s URL is service %d\n",
                            appidDebug->get_debug_session(),
                            service_id);
                    asd.set_service_appid_data(service_id, nullptr, nullptr);
                }

                // DO overwrite a previously-set data
                if (appidDebug->is_active() && payload_id > APP_ID_NONE &&
                    asd.payload.get_id() != payload_id)
                    LogMessage("AppIdDbg %s URL is payload %d\n", appidDebug->get_debug_session(),
                        payload_id);
                asd.set_payload_appid_data((AppId)payload_id, version);
                asd.set_referred_payload_app_id_data(referredPayloadAppId);
            }

            asd.scan_flags &= ~SCAN_HTTP_HOST_URL_FLAG;
            if ( version )
                snort_free(version);
            if ( my_host )
                snort_free(my_host);
        }

        if (asd.client.get_id() == APP_ID_APPLE_CORE_MEDIA)
        {
            AppInfoTableEntry* entry;
            AppId tp_payload_app_id = asd.get_tp_payload_app_id();
            if (tp_payload_app_id > APP_ID_NONE)
            {
                entry = asd.app_info_mgr->get_app_info_entry(tp_payload_app_id);
                // only move tpPayloadAppId to client if client app id is valid
                if (entry && entry->clientId > APP_ID_NONE)
                {
                    asd.misc_app_id = asd.client.get_id();
                    asd.client.set_id(tp_payload_app_id);
                }
            }
            else if (asd.payload.get_id() > APP_ID_NONE)
            {
                entry =  asd.app_info_mgr->get_app_info_entry(asd.payload.get_id());
                // only move payload_app_id to client if it has a ClientAppid
                if (entry && entry->clientId > APP_ID_NONE)
                {
                    asd.misc_app_id = asd.client.get_id();
                    asd.client.set_id(asd.payload.get_id());
                }
            }
        }

        asd.clear_http_flags();
    }  // end DON'T skip_simple_detect

    return 0;
}

// FIXIT-H - Implement this function when (reconfigurable) XFF is supported.
void AppIdHttpSession::update_http_xff_address(struct XffFieldValue* xff_fields,
    uint32_t numXffFields)
{
    UNUSED(xff_fields);
    UNUSED(numXffFields);
#if 0
    static const char* defaultXffPrecedence[] =
    {
        HTTP_XFF_FIELD_X_FORWARDED_FOR,
        HTTP_XFF_FIELD_TRUE_CLIENT_IP
    };

    // XFF precedence configuration cannot change for a session. Do not get it again if we already
    // got it.
    char** xffPrecedence = _dpd.sessionAPI->get_http_xff_precedence(p->stream_session, p->flags,
        &numXffFields);
    if (!xffPrecedence)
    {
        xffPrecedence = defaultXffPrecedence;
        numXffFields = sizeof(defaultXffPrecedence) / sizeof(defaultXffPrecedence[0]);
    }

    xffPrecedence = malloc(numXffFields * sizeof(char*));

    for (unsigned j = 0; j < numXffFields; j++)
        xffPrecedence[j] = strndup(xffPrecedence[j], UINT8_MAX);

    if (appidDebug->is_active())
    {
        for (unsigned i = 0; i < numXffFields; i++)
            LogMessage("AppIdDbg %s XFF %s : %s\n", appidDebug->get_debug_session(),
                xff_fields[i].field.c_str(), xff_fields[i].value.empty() ? "(empty)" :
                xff_fields[i].value);
    }

    // xffPrecedence array is sorted based on precedence
    for (unsigned i = 0; (i < numXffFields) && xffPrecedence[i]; i++)
    {
        for (unsigned j = 0; j < numXffFields; j++)
        {
            if (xff_addr)
            {
                delete xff_addr;
                xff_addr = nullptr;
            }

            if (strncasecmp(xff_fields[j].field.c_str(), xffPrecedence[i], UINT8_MAX) == 0)
            {
                if (xff_fields[j].value.empty())
                    return;

                // For a comma-separated list of addresses, pick the last address
                // FIXIT-L: change to select last address port from 2.9.10-42..not tested

                // FIXIT_H: - this code is wrong. We can't have
                // tmp-xff_fields[j].value when tmp=0.

                // xff_addr = new snort::SfIp();
                // char* xff_addr_str = nullptr;
                // char* tmp = strchr(xff_fields[j].value, ',');

                // if (tmp)
                // {
                //     xff_addr_str = tmp + 1;
                // }
                // else
                // {
                //     xff_fields[j].value[tmp - xff_fields[j].value] = '\0';
                //     xff_addr_str = xff_fields[j].value;
                // }

                // if (xff_addr->set(xff_addr_str) != SFIP_SUCCESS)
                // {
                //     delete xff_addr;
                //     xff_addr = nullptr;
                // }
                break;
            }
        }

        if (xff_addr)
            break;
    }
#endif
}

void AppIdHttpSession::set_url(const char* url)
{
    if ( this->url )
        delete this->url;
    if ( url )
        this->url = new std::string(url);   // FIXIT-M null terminated?
    else
        this->url = nullptr;
}

void AppIdHttpSession::set_referer(char* referer)
{
    if ( this->referer )
        delete this->referer;
    if ( referer )
        this->referer = new std::string(referer);
    else
        this->referer = nullptr;
}

const char* AppIdHttpSession::get_new_url()
{
    return http_fields[REQ_URI_FID].field.empty()
           ? nullptr : http_fields[REQ_URI_FID].field.c_str();
}

const char* AppIdHttpSession::get_new_cookie()
{
    return http_fields[REQ_COOKIE_FID].field.empty()
           ? nullptr : http_fields[REQ_COOKIE_FID].field.c_str();
}

const char* AppIdHttpSession::get_new_field(HttpFieldIds fieldId)
{
    return http_fields[fieldId].field.empty() ? nullptr : http_fields[fieldId].field.c_str();
}

uint16_t AppIdHttpSession::get_field_offset(HttpFieldIds fid)
{
    return http_fields[fid].field.empty() ? 0 : http_fields[fid].start_offset;
}

void AppIdHttpSession::set_field_offset(HttpFieldIds fid, uint16_t value)
{
    http_fields[fid].start_offset = value;
}

uint16_t AppIdHttpSession::get_field_end_offset(HttpFieldIds fid)
{
    return http_fields[fid].field.empty() ? 0 : http_fields[fid].end_offset;
}

void AppIdHttpSession::set_field_end_offset(HttpFieldIds fid, uint16_t value)
{
    http_fields[fid].end_offset = value;
}

uint16_t AppIdHttpSession::get_uri_offset()
{
    return http_fields[REQ_URI_FID].start_offset;
}

uint16_t AppIdHttpSession::get_uri_end_offset()
{
    return http_fields[REQ_URI_FID].end_offset;
}

uint16_t AppIdHttpSession::get_cookie_offset()
{
    return http_fields[REQ_COOKIE_FID].start_offset;
}

uint16_t AppIdHttpSession::get_cookie_end_offset()
{
    return http_fields[REQ_COOKIE_FID].end_offset;
}

void AppIdHttpSession::update_host(const std::string* new_host)
{
    if (host)
        delete host;
    host = new_host;
}

void AppIdHttpSession::update_uri(const std::string* new_uri)
{
    if (uri)
        delete uri;
    uri = new_uri;
}

void AppIdHttpSession::update_url()
{
    if (host and uri)
    {
        if (url)
            delete url;
        url = new std::string(std::string("http://") + *host + *uri);
    }
}

void AppIdHttpSession::update_url(const std::string* new_url)
{
    if ( url )
        delete url;
    url = new_url;
}

void AppIdHttpSession::update_useragent(const std::string* new_ua)
{
    if (useragent)
        delete useragent;
    useragent = new_ua;
}

void AppIdHttpSession::update_cookie(const std::string* new_cookie)
{
    if (cookie)
        delete cookie;
    cookie = new_cookie;
}

void AppIdHttpSession::update_referer(const std::string* new_referer)
{
    if (referer)
        delete referer;
    referer = new_referer;
}

void AppIdHttpSession::update_x_working_with(const std::string* new_xww)
{
    if (x_working_with)
        delete x_working_with;
    x_working_with = new_xww;
}

void AppIdHttpSession::update_content_type(const std::string* new_content_type)
{
    if (content_type)
        delete content_type;
    content_type = new_content_type;
}

void AppIdHttpSession::update_location(const std::string* new_location)
{
    if (location)
        delete location;
    location = new_location;
}

void AppIdHttpSession::update_server(const std::string* new_server)
{
    if (server)
        delete server;
    server = new_server;
}

void AppIdHttpSession::update_via(const std::string* new_via)
{
    if (via)
        delete via;
    via = new_via;
}

void AppIdHttpSession::update_body(const std::string* new_body)
{
    if (body)
        delete body;
    body = new_body;
}

void AppIdHttpSession::update_req_body(const std::string* new_req_body)
{
    if (req_body)
        delete req_body;
    req_body = new_req_body;
}

void AppIdHttpSession::update_response_code(const std::string* new_rc)
{
    if (response_code)
        delete response_code;
    response_code = new_rc;
}

void AppIdHttpSession::update_host(const uint8_t* new_host, int32_t len)
{
    if ( host )
        delete host;
    host = new std::string((const char*)new_host, len);
}

void AppIdHttpSession::update_uri(const uint8_t* new_uri, int32_t len)
{
    if ( uri )
        delete uri;
    uri = new std::string((const char*)new_uri, len);
}

void AppIdHttpSession::update_useragent(const uint8_t* new_ua, int32_t len)
{
    if ( useragent )
        delete useragent;
    useragent = new std::string((const char*)new_ua, len);
}

void AppIdHttpSession::update_cookie(const uint8_t* new_cookie, int32_t len)
{
    if ( cookie )
        delete cookie;
    cookie = new std::string((const char*)new_cookie, len);
}

void AppIdHttpSession::update_referer(const uint8_t* new_referer, int32_t len)
{
    if ( referer )
        delete referer;
    if ( new_referer and len ) referer = new std::string((const char*)new_referer, len);
    else referer = nullptr;
}

void AppIdHttpSession::update_x_working_with(const uint8_t* new_xww, int32_t len)
{
    if ( x_working_with )
        delete x_working_with;
    x_working_with = new std::string((const char*)new_xww, len);
}

void AppIdHttpSession::update_content_type(const uint8_t* new_content_type, int32_t len)
{
    if ( content_type )
        delete content_type;
    content_type = new std::string((const char*)new_content_type, len);
}

void AppIdHttpSession::update_location(const uint8_t* new_location, int32_t len)
{
    if ( location )
        delete location;
    location = new std::string((const char*)new_location, len);
}

void AppIdHttpSession::update_server(const uint8_t* new_server, int32_t len)
{
    if ( server )
        delete server;
    server = new std::string((const char*)new_server, len);
}

void AppIdHttpSession::update_via(const uint8_t* new_via, int32_t len)
{
    if ( via )
        delete via;
    via = new std::string((const char*)new_via, len);
}

void AppIdHttpSession::update_body(const uint8_t* new_body, int32_t len)
{
    if ( body )
        delete body;
    body = new std::string((const char*)new_body, len);
}

void AppIdHttpSession::update_req_body(const uint8_t* new_req_body, int32_t len)
{
    if ( req_body )
        delete req_body;
    req_body = new std::string((const char*)new_req_body, len);
}

void AppIdHttpSession::update_response_code(const char* new_rc)
{
    if ( response_code )
        delete response_code;
    response_code = new std::string((const char*)new_rc);  // FIXIT-L null term?
}

void AppIdHttpSession::reset_ptype_scan_counts()
{
    memset(ptype_scan_counts, 0, sizeof(ptype_scan_counts));
}

void AppIdHttpSession::clear_all_fields()
{
    if (host)
    {
        delete host;
        host = nullptr;
    }
    if (url)
    {
        delete url;
        url = nullptr;
    }
    if (uri)
    {
        delete uri;
        uri = nullptr;
    }
    if (referer)
    {
        delete referer;
        referer = nullptr;
    }
    if (useragent)
    {
        delete useragent;
        useragent = nullptr;
    }
    if (via)
    {
        delete via;
        via = nullptr;
    }
    if (cookie)
    {
        delete cookie;
        cookie = nullptr;
    }
    if (body)
    {
        delete body;
        body = nullptr;
    }
    if (response_code)
    {
        delete response_code;
        response_code = nullptr;
    }
    if (content_type)
    {
        delete content_type;
        content_type = nullptr;
    }
    if (location)
    {
        delete location;
        location = nullptr;
    }
    if (req_body)
    {
        delete req_body;
        req_body = nullptr;
    }
    if (server)
    {
        delete server;
        server = nullptr;
    }
    if (x_working_with)
    {
        delete x_working_with;
        x_working_with = nullptr;
    }
    if (xff_addr)
    {
        delete xff_addr;
        xff_addr = nullptr;
    }
    if (xffPrecedence)
    {
        for (unsigned i = 0; i < numXffFields; i++)
            delete xffPrecedence[i];
        delete xffPrecedence;
        xffPrecedence = NULL;
    }
}

