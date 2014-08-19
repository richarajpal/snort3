/*
** Copyright (C) 2002-2013 Sourcefire, Inc.
** Copyright (C) 1998-2002 Martin Roesch <roesch@sourcefire.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public LicenseUpdateMPLSStats
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
// cd_mpls.cc author Josh Rosenbaum <jrosenba@cisco.com>



#include "framework/codec.h"
#include "codecs/decode_module.h"
#include "network_inspectors/perf_monitor/perf.h"
#include "snort.h"
#include "protocols/mpls.h"
#include "codecs/codec_events.h"
#include "packet_io/active.h"
#include "protocols/protocol_ids.h"
#include "protocols/mpls.h"
#include "codecs/sf_protocols.h"
#include "main/snort_config.h"
#include "main/snort.h"

namespace
{
#define CD_MPLS_NAME "mpls"

static const Parameter mpls_params[] =
{
    { "enable_mpls_multicast", Parameter::PT_BOOL, nullptr, "false",
      "enables support for MPLS multicast" },

    { "enable_mpls_overlapping_ip", Parameter::PT_BOOL, nullptr, "false",
      "enable if private network addresses overlap and must be differentiated by MPLS label(s)" },

    { "max_mpls_stack_depth", Parameter::PT_INT, "-1:", "-1",
      "set MPLS stack depth" },

    { "mpls_payload_type", Parameter::PT_ENUM, "eth | ip4 | ip6", "ip4",
      "set encapsulated payload type" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};


// rules which will loaded into snort.
// You can now reference these rules by calling a codec_event
// in your main codec's functions
static const RuleMap mpls_rules[] =
{
    { DECODE_BAD_MPLS, "(" CD_MPLS_NAME ") Bad MPLS Frame" },
    { DECODE_BAD_MPLS_LABEL0, "(" CD_MPLS_NAME ") MPLS Label 0 Appears in Nonbottom Header" },
    { DECODE_BAD_MPLS_LABEL1, "(" CD_MPLS_NAME ") MPLS Label 1 Appears in Bottom Header" },
    { DECODE_BAD_MPLS_LABEL2, "(" CD_MPLS_NAME ") MPLS Label 2 Appears in Nonbottom Header" },
    { DECODE_BAD_MPLS_LABEL3, "(" CD_MPLS_NAME ") MPLS Label 3 Appears in Header" },
    { DECODE_MPLS_RESERVED_LABEL, "(" CD_MPLS_NAME ") MPLS Label 4, 5,.. or 15 Appears in Header" },
    { DECODE_MPLS_LABEL_STACK, "(" CD_MPLS_NAME ") Too Many MPLS headers" },
    { 0, nullptr }
};

class MplsModule : public DecodeModule
{
public:
    MplsModule() : DecodeModule(CD_MPLS_NAME, mpls_params) {};

    const RuleMap* get_rules() const
    { return mpls_rules; }

    bool set(const char*, Value& v, SnortConfig* sc)
    {
        if ( v.is("enable_mpls_multicast") )
        {
            if ( v.get_bool() )
                sc->run_flags |= RUN_FLAG__MPLS_MULTICAST; // FIXIT move to existing bitfield
        }
        else if ( v.is("enable_mpls_overlapping_ip") )
        {
            if ( v.get_bool() )
                sc->run_flags |= RUN_FLAG__MPLS_OVERLAPPING_IP; // FIXIT move to existing bitfield
        }
        else if ( v.is("max_mpls_stack_depth") )
        {
            sc->mpls_stack_depth = v.get_long();
        }
        else if ( v.is("mpls_payload_type") )
        {
            sc->mpls_payload_type = v.get_long() + 1;
        }
        else
            return false;

        return true;
    }
};

class MplsCodec : public Codec
{
public:
    MplsCodec() : Codec(CD_MPLS_NAME){};
    ~MplsCodec(){};

    virtual PROTO_ID get_proto_id() { return PROTO_MPLS; };
    virtual void get_protocol_ids(std::vector<uint16_t>& v);
    virtual bool decode(const uint8_t *raw_pkt, const uint32_t& raw_len,
        Packet *, uint16_t &lyr_len, uint16_t &next_prot_id);    

};


constexpr uint16_t ETHERTYPE_MPLS_UNICAST = 0x8847;
constexpr uint16_t ETHERTYPE_MPLS_MULTICAST = 0x8848;
constexpr int MPLS_HEADER_LEN = 4;
constexpr int NUM_RESERVED_LABELS = 16;
constexpr int MPLS_PAYLOADTYPE_ERROR = -1;

} // namespace

static int checkMplsHdr(uint32_t, uint8_t, uint8_t, uint8_t, Packet *);


void MplsCodec::get_protocol_ids(std::vector<uint16_t>& v)
{
    v.push_back(ETHERTYPE_MPLS_UNICAST);
    v.push_back(ETHERTYPE_MPLS_MULTICAST);
}


bool MplsCodec::decode(const uint8_t *raw_pkt, const uint32_t& raw_len,
        Packet *p, uint16_t &lyr_len, uint16_t &next_prot_id)
{
    const uint32_t* tmpMplsHdr;
    uint32_t mpls_h;
    uint32_t label;
    lyr_len= 0;

    uint8_t exp;
    uint8_t bos = 0;
    uint8_t ttl;
    uint8_t chainLen = 0;
    uint32_t stack_len = raw_len;

    int iRet = 0;

    UpdateMPLSStats(&sfBase, raw_len, Active_PacketWasDropped());
    tmpMplsHdr = (const uint32_t *) raw_pkt;

    while (!bos)
    {
        if(stack_len < MPLS_HEADER_LEN)
        {
            codec_events::decoder_event(p, DECODE_BAD_MPLS);
            return false;
        }

        mpls_h  = ntohl(*tmpMplsHdr);
        ttl = (uint8_t)(mpls_h & 0x000000FF);
        mpls_h = mpls_h>>8;
        bos = (uint8_t)(mpls_h & 0x00000001);
        exp = (uint8_t)(mpls_h & 0x0000000E);
        label = (mpls_h>>4) & 0x000FFFFF;

        if((label<NUM_RESERVED_LABELS)&&((iRet = checkMplsHdr(label, exp, bos, ttl, p)) < 0))
            return false;

        if( bos )
        {
            p->mplsHdr.label = label;
            p->mplsHdr.exp = exp;
            p->mplsHdr.bos = bos;
            p->mplsHdr.ttl = ttl;
            /**
            p->mpls = &(p->mplsHdr);
      **/
            p->proto_bits |= PROTO_BIT__MPLS;
            if(!iRet)
            {
                iRet = ScMplsPayloadType();
            }
        }
        tmpMplsHdr++;
        stack_len -= MPLS_HEADER_LEN;

        if ((ScMplsStackDepth() != -1) && (chainLen++ >= ScMplsStackDepth()))
        {
            codec_events::decoder_event(p, DECODE_MPLS_LABEL_STACK);

            p->proto_bits &= ~PROTO_BIT__MPLS;
            return false;
        }
    }   /* while bos not 1, peel off more labels */

    lyr_len = (uint8_t*)tmpMplsHdr - raw_pkt;

    switch (iRet)
    {
        case MPLS_PAYLOADTYPE_IPV4:
            next_prot_id = ETHERTYPE_IPV4;
            break;

        case MPLS_PAYLOADTYPE_IPV6:
            next_prot_id = ETHERTYPE_IPV6;
            break;

        case MPLS_PAYLOADTYPE_ETHERNET:
            next_prot_id = ETHERTYPE_TRANS_ETHER_BRIDGING;
            break;

        default:
            break;
    }

    return true;
}


/*
 * check if reserved labels are used properly
 */
static int checkMplsHdr(
    uint32_t label, uint8_t, uint8_t bos, uint8_t, Packet *p)
{
    int iRet = 0;
    switch(label)
    {
        case 0:
        case 2:
               /* check if this label is the bottom of the stack */
               if(bos)
               {
                   if ( label == 0 )
                       iRet = MPLS_PAYLOADTYPE_IPV4;
                   else if ( label == 2 )
                       iRet = MPLS_PAYLOADTYPE_IPV6;


                   /* when label == 2, IPv6 is expected;
                    * when label == 0, IPv4 is expected */
                   if((label&&(ScMplsPayloadType() != MPLS_PAYLOADTYPE_IPV6))
                       ||((!label)&&(ScMplsPayloadType() != MPLS_PAYLOADTYPE_IPV4)))
                   {
                        if( !label )
                            codec_events::decoder_event(p, DECODE_BAD_MPLS_LABEL0);
                        else
                            codec_events::decoder_event(p, DECODE_BAD_MPLS_LABEL2);
                   }
                   break;
               }

#if 0
               /* This is valid per RFC 4182.  Just pop this label off, ignore it
                * and move on to the next one.
                */
               if( !label )
                   codec_events::decoder_event(p, DECODE_BAD_MPLS_LABEL0);
               else
                   codec_events::decoder_event(p, DECODE_BAD_MPLS_LABEL2);

               p->iph = NULL;
               p->family = NO_IP;
               return(-1);
#endif
               break;
        case 1:
               if(!bos) break;

               codec_events::decoder_event(p, DECODE_BAD_MPLS_LABEL1);

               iRet = MPLS_PAYLOADTYPE_ERROR;
               break;

      case 3:
               codec_events::decoder_event(p, DECODE_BAD_MPLS_LABEL3);

               iRet = MPLS_PAYLOADTYPE_ERROR;
               break;
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 15:
                codec_events::decoder_event(p, DECODE_MPLS_RESERVED_LABEL);
                break;
        default:
                break;
    }
    if ( !iRet )
    {
        iRet = ScMplsPayloadType();
    }
    return iRet;
}

//-------------------------------------------------------------------------
// api
//-------------------------------------------------------------------------

static Module* mod_ctor()
{
    return new MplsModule;
}

static void mod_dtor(Module* m)
{
    delete m;
}

static Codec* ctor(Module*)
{
    return new MplsCodec();
}

static void dtor(Codec *cd)
{
    delete cd;
}

static const CodecApi mpls_api =
{
    {
        PT_CODEC,
        CD_MPLS_NAME,
        CDAPI_PLUGIN_V0,
        0,
        mod_ctor,
        mod_dtor,
    },
    nullptr, // pinit
    nullptr, // pterm
    nullptr, // tinit
    nullptr, // tterm
    ctor, // ctor
    dtor, // dtor
};

#ifdef BUILDING_SO
SO_PUBLIC const BaseApi* snort_plugins[] =
{
    &mpls_api.base,
    nullptr
};
#else
const BaseApi* cd_mpls = &mpls_api.base;
#endif





