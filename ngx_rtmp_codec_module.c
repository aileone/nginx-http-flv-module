
/*
 * Copyright (C) Roman Arutyunyan
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp_codec_module.h"
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_bitop.h"
#include "hls/ngx_rtmp_hls_module.h"


#define NGX_RTMP_CODEC_META_OFF     0
#define NGX_RTMP_CODEC_META_ON      1
#define NGX_RTMP_CODEC_META_COPY    2


static void * ngx_rtmp_codec_create_app_conf(ngx_conf_t *cf);
static char * ngx_rtmp_codec_merge_app_conf(ngx_conf_t *cf,
       void *parent, void *child);
static ngx_int_t ngx_rtmp_codec_postconfiguration(ngx_conf_t *cf);
static ngx_int_t ngx_rtmp_codec_reconstruct_meta(ngx_rtmp_session_t *s);
static ngx_int_t ngx_rtmp_codec_copy_meta(ngx_rtmp_session_t *s,
       ngx_rtmp_header_t *h, ngx_chain_t *in);
static ngx_int_t ngx_rtmp_codec_prepare_meta(ngx_rtmp_session_t *s,
       uint32_t timestamp);
static void ngx_rtmp_codec_parse_aac_header(ngx_rtmp_session_t *s,
       ngx_chain_t *in);
static void ngx_rtmp_codec_parse_avc_header(ngx_rtmp_session_t *s,
       ngx_chain_t *in);

static void ngx_rtmp_codec_parse_hevc_header(ngx_rtmp_session_t *s,
       ngx_chain_t *in);
#if (NGX_DEBUG)
static void ngx_rtmp_codec_dump_header(ngx_rtmp_session_t *s, const char *type,
       ngx_chain_t *in);
#endif


static void ngx_rtmp_codec_parse_avc_header_compat(ngx_rtmp_session_t *s,
        ngx_chain_t **in, ngx_chain_t *sps);
static ngx_int_t ngx_rtmp_codec_parse_avc_header_in_keyframe(
        ngx_rtmp_session_t *s, ngx_chain_t *in, ngx_buf_t *out);


typedef struct {
    ngx_uint_t                      meta;
} ngx_rtmp_codec_app_conf_t;


static ngx_conf_enum_t ngx_rtmp_codec_meta_slots[] = {
    { ngx_string("off"),            NGX_RTMP_CODEC_META_OFF  },
    { ngx_string("on"),             NGX_RTMP_CODEC_META_ON   },
    { ngx_string("copy"),           NGX_RTMP_CODEC_META_COPY },
    { ngx_null_string,              0 }
};


static ngx_command_t  ngx_rtmp_codec_commands[] = {

    { ngx_string("meta"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_codec_app_conf_t, meta),
      &ngx_rtmp_codec_meta_slots },

      ngx_null_command
};


static ngx_rtmp_module_t  ngx_rtmp_codec_module_ctx = {
    NULL,                                   /* preconfiguration */
    ngx_rtmp_codec_postconfiguration,       /* postconfiguration */
    NULL,                                   /* create main configuration */
    NULL,                                   /* init main configuration */
    NULL,                                   /* create server configuration */
    NULL,                                   /* merge server configuration */
    ngx_rtmp_codec_create_app_conf,         /* create app configuration */
    ngx_rtmp_codec_merge_app_conf           /* merge app configuration */
};


ngx_module_t  ngx_rtmp_codec_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_codec_module_ctx,             /* module context */
    ngx_rtmp_codec_commands,                /* module directives */
    NGX_RTMP_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    NULL,                                   /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


static const char *
audio_codecs[] = {
    "",
    "ADPCM",
    "MP3",
    "LinearLE",
    "Nellymoser16",
    "Nellymoser8",
    "Nellymoser",
    "G711A",
    "G711U",
    "",
    "AAC",
    "Speex",
    "",
    "",
    "MP3-8K",
    "DeviceSpecific",
    "Uncompressed"
};


static const char *
video_codecs[] = {
    "",
    "Jpeg",
    "Sorenson-H263",
    "ScreenVideo",
    "On2-VP6",
    "On2-VP6-Alpha",
    "ScreenVideo2",
    "H264",
    "",
    "",
    "",
    "",
    "H265",
};


u_char *
ngx_rtmp_get_audio_codec_name(ngx_uint_t id)
{
    return (u_char *)(id < sizeof(audio_codecs) / sizeof(audio_codecs[0])
        ? audio_codecs[id]
        : "");
}


u_char *
ngx_rtmp_get_video_codec_name(ngx_uint_t id)
{
    return (u_char *)(id < sizeof(video_codecs) / sizeof(video_codecs[0])
        ? video_codecs[id]
        : "");
}


static ngx_uint_t
ngx_rtmp_codec_get_next_version()
{
    ngx_uint_t          v;
    static ngx_uint_t   version;

    do {
        v = ++version;
    } while (v == 0);

    return v;
}


static ngx_int_t
ngx_rtmp_codec_disconnect(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
        ngx_chain_t *in)
{
    ngx_rtmp_codec_ctx_t               *ctx;
    ngx_rtmp_core_srv_conf_t           *cscf;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);
    if (ctx == NULL) {
        return NGX_OK;
    }

    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    if (ctx->avc_header) {
        ngx_rtmp_free_shared_chain(cscf, ctx->avc_header);
        ctx->avc_header = NULL;
    }

    if (ctx->aac_header) {
        ngx_rtmp_free_shared_chain(cscf, ctx->aac_header);
        ctx->aac_header = NULL;
    }

    if (ctx->meta) {
        ngx_rtmp_free_shared_chain(cscf, ctx->meta);
        ctx->meta = NULL;
    }

    return NGX_OK;
}


#define NGX_RTMP_CODEC_NON_SEQ_HEADER    0
#define NGX_RTMP_CODEC_SINGLE_SEQ_HEADER 1
#define NGX_RTMP_CODEC_COMBO_SEQ_HEADER  2


static ngx_inline ngx_int_t
ngx_rtmp_codec_video_is_combined_nals(ngx_chain_t *in, ngx_rtmp_session_t *s)
{
    ngx_rtmp_codec_ctx_t     *ctx;
    uint8_t                  src_nal_type, nal_type;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);
    if (ctx == NULL || ctx->avc_nal_bytes == 0) {
        return NGX_ERROR;
    }

    if (ngx_rtmp_get_video_frame_type(in) != NGX_RTMP_VIDEO_KEY_FRAME) {
        return NGX_ERROR;
    }

    /**
      * +--------------------------------------------+---------------+
      * |   17   |   00   |   00   |   00   |   00   |0|1|2|3|4|5|6|7|
      * +--------------------------------------------+-+-+-+-+-+-+-+-+
      * |frt|cdid|pkt-type|     composition time     |F|NRI|  Type   |
      * +------------------------------------------------------------+
      **/

    if (in->buf->pos + 5 + ctx->avc_nal_bytes <= in->buf->last) {
        src_nal_type = in->buf->pos[5 + ctx->avc_nal_bytes];

        if (ctx->video_codec_id == NGX_RTMP_VIDEO_H264) {
            nal_type = src_nal_type & 0x1f;

            return (nal_type >= NGX_RTMP_NALU_SPS
                    && nal_type <= NGX_RTMP_NALU_PPS) ? NGX_OK : NGX_ERROR;
        }
    }

    return NGX_ERROR;
}


static ngx_inline ngx_int_t
ngx_rtmp_get_codec_header_type(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
        ngx_chain_t *in)
{
    if (ngx_rtmp_is_codec_header(in)) {
        ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "codec: a sequence header in chain");

        return NGX_RTMP_CODEC_SINGLE_SEQ_HEADER;
    }

    if (h->type == NGX_RTMP_MSG_VIDEO &&
        ngx_rtmp_codec_video_is_combined_nals(in, s) == NGX_OK)
    {
        ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "codec: an AVC NALU or an AAC raw packet in chain");

        return NGX_RTMP_CODEC_COMBO_SEQ_HEADER;
    }

    return NGX_RTMP_CODEC_NON_SEQ_HEADER;
}


ngx_int_t
ngx_rtmp_codec_parse_avc_header_in_keyframe(ngx_rtmp_session_t *s,
    ngx_chain_t *in, ngx_buf_t *out)
{
    ngx_rtmp_codec_ctx_t    *ctx;
    u_char                  *p;
    uint8_t                 fmt, ftype, htype, src_nal_type, nal_type;
    uint32_t                rlen, len;
    ngx_buf_t               *b;
    ngx_uint_t              nal_bytes;
    ngx_uint_t              has_sps = 0;
    ngx_uint_t              left;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);
    if (ctx == NULL
        || ctx->video_codec_id != NGX_RTMP_VIDEO_H264
        || ctx->avc_nal_bytes == 0
        || out == NULL)
    {
        return NGX_ERROR;
    }

#if (NGX_DEBUG)
    ngx_rtmp_codec_dump_header(s, "avc_in_keyframe", in);
#endif

    p = in->buf->pos;
    if (ngx_rtmp_hls_copy(s, &fmt, &p, 1, &in) != NGX_OK) {
        return NGX_ERROR;
    }

    /* 1: keyframe (IDR)
     * 2: inter frame
     * 3: disposable inter frame */

    ftype = (fmt & 0xf0) >> 4;

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
            "codec: ftype=%uD", ftype);

    if (ftype != NGX_RTMP_FRAME_IDR) {
        return NGX_ERROR;
    }

    /* H.264 HDR/PICT */
    if (ngx_rtmp_hls_copy(s, &htype, &p, 1, &in) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
            "codec: htype=%uD", htype);

    /* proceed only with PICT */
    if (htype != 1) {
        return NGX_OK;
    }

    b = out;

    *b->last++ = ((ftype & 0x0f) << 4) | NGX_RTMP_NALU_SPS;
    *b->last++ = 0x00;

    /* 3 bytes: decoder delay */
    if (ngx_rtmp_hls_copy(s, NULL, &p, 3, &in) != NGX_OK) {
        return NGX_ERROR;
    }

    /* 3 bytes: composition time */
    *b->last++ = 0x00;
    *b->last++ = 0x00;
    *b->last++ = 0x00;

    /* 1 byte: configuration version */
    *b->last++ = 0x01;

    /* 1 byte: avc profile indication */
    *b->last++ = ctx->avc_profile;

    /* 1 byte: profile compatibility */
    *b->last++ = ctx->avc_compat;

    /* 1 byte: avc level indication */
    *b->last++ = ctx->avc_level;

    /* 1 byte: reserved 6 bits + lengthSizeMinusOne 2 bits */
    *b->last++ = 0xff;

    /* num of SPS */
    *b->last++ = 0xe1;

    /* H.264 nal -> len + body */
    /* body: nal_type(1) +  */

    nal_bytes = ctx->avc_nal_bytes;

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
            "codec: nal_bytes=%uD", nal_bytes);

    left = NGX_RTMP_SPS_MAX_LENGTH;

    while (in) {
        if (ngx_rtmp_hls_copy(s, &rlen, &p, nal_bytes, &in) != NGX_OK) {
            return NGX_OK;
        }

        len = 0;
        ngx_rtmp_rmemcpy(&len, &rlen, nal_bytes);

        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "codec: len=%uD", len);

        if (len == 0) {
            ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                    "codec: skip, len=%uD", len);

            continue;
        }

        if (ngx_rtmp_hls_copy(s, &src_nal_type, &p, 1, &in) != NGX_OK) {
            return NGX_OK;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "codec: src_nal_type=%uD", src_nal_type);

        /**
          * +---------------+
          * |0|1|2|3|4|5|6|7|
          * +-+-+-+-+-+-+-+-+
          * |F|NRI|  Type   |
          * +---------------+
          **/
        nal_type = src_nal_type & 0x1f;

        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "codec: nal_type=%uD", nal_type);

        if (!(nal_type >= NGX_RTMP_NALU_SPS
                && nal_type <= NGX_RTMP_NALU_PPS))
        {
            if (ngx_rtmp_hls_copy(s, NULL, &p, len - 1, &in) != NGX_OK) {
                return NGX_ERROR;
            }

            ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "codec: skip non-sps or non-pps, nal_type=%uD", nal_type);

            continue;
        }

        left = NGX_RTMP_SPS_MAX_LENGTH - (b->last - b->pos);

        /* avc: nal len(2B) + nal data (len Byte) + pps num(1B) */
        if (len + 3 > left) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                    "codec: avc too big sps or pps, "
                    "nal_type: %uD, left=%uD, len=%uD",
                    nal_type, left, len);

            return NGX_ERROR;
        }

        /* nal length */
        *b->last++ = len >> 8;
        *b->last++ = len & 0xff;
        *b->last++ = src_nal_type;

        if (ngx_rtmp_hls_copy(s, b->last, &p, len - 1, &in) != NGX_OK) {
            return NGX_ERROR;
        }

        b->last += len - 1;

        /* NALs MUST be SPS + PPS */
        *b->last++ = 0x01;
        has_sps = 1;

        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "codec: has_sps=%uD", has_sps);
    }

    return (has_sps == 1) ? NGX_OK : NGX_ERROR;
}


void
ngx_rtmp_codec_parse_avc_header_compat(ngx_rtmp_session_t *s,
        ngx_chain_t **in, ngx_chain_t *sps)
{
    if (in == NULL || *in == NULL || sps == NULL) {
        return;
    }

    if (ngx_rtmp_codec_parse_avc_header_in_keyframe(s, *in,
            sps->buf) != NGX_OK)
    {
        return;
    }

    *in = sps;

    ngx_rtmp_codec_parse_avc_header(s, *in);
}


static ngx_int_t
ngx_rtmp_codec_av(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
        ngx_chain_t *in)
{
    ngx_rtmp_core_srv_conf_t           *cscf;
    ngx_rtmp_codec_ctx_t               *ctx;
    ngx_chain_t                       **header;
    ngx_buf_t                           sps_buf;
    u_char                              buffer[NGX_RTMP_SPS_MAX_LENGTH];
    ngx_chain_t                         sps;
    ngx_uint_t                          seq_header_type;
    uint8_t                             fmt;
    static ngx_uint_t                   sample_rates[] =
                                        { 5512, 11025, 22050, 44100 };

    if (h->type != NGX_RTMP_MSG_AUDIO && h->type != NGX_RTMP_MSG_VIDEO) {
        return NGX_OK;
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);
    if (ctx == NULL) {
        ctx = ngx_pcalloc(s->connection->pool, sizeof(ngx_rtmp_codec_ctx_t));
        ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_codec_module);
    }

    /* save codec */
    if (in->buf->last - in->buf->pos < 1) {
        return NGX_OK;
    }

   cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    fmt =  in->buf->pos[0];
    if (h->type == NGX_RTMP_MSG_AUDIO) {
        ctx->audio_codec_id = (fmt & 0xf0) >> 4;
        ctx->audio_channels = (fmt & 0x01) + 1;
        ctx->sample_size = (fmt & 0x02) ? 2 : 1;

        if (ctx->sample_rate == 0) {
            ctx->sample_rate = sample_rates[(fmt & 0x0c) >> 2];
        }

    } else {
        // parse VideoTagHeader avc=7 hevc=12
        ctx->video_codec_id = (fmt & 0x0f);
    }

    /* save AVC/AAC header */
    if (in->buf->last - in->buf->pos < 3) {
        return NGX_OK;
    }

     /* PacketType = 0, FLV TAG MUST be sequence header */
    /* PacketType = 1, FLV TAG MAY be AVC NALU or AAC Raw */
    seq_header_type = ngx_rtmp_get_codec_header_type(s, h, in);
    if (seq_header_type == NGX_RTMP_CODEC_NON_SEQ_HEADER) {
        return NGX_OK;
    }

    header = NULL;

    sps_buf.start = buffer;
    sps_buf.end   = buffer + sizeof(buffer);
    sps_buf.pos   = sps_buf.start;
    sps_buf.last  = sps_buf.pos;
    sps.buf       = &sps_buf;
    sps.next      = NULL;

    /* MUST be audio / video sequence header */
    if (h->type == NGX_RTMP_MSG_AUDIO) {
        if (ctx->audio_codec_id == NGX_RTMP_AUDIO_AAC) {
            header = &ctx->aac_header;
            ngx_rtmp_codec_parse_aac_header(s, in);
        }
    } else {
        if (ctx->video_codec_id == NGX_RTMP_VIDEO_H264) {
            header = &ctx->avc_header;
            
            if (seq_header_type == NGX_RTMP_CODEC_COMBO_SEQ_HEADER) {
                ngx_rtmp_codec_parse_avc_header_compat(s, &in, &sps);
            } else {
                ngx_rtmp_codec_parse_avc_header(s, in);
            }
        }
        if (ctx->video_codec_id == NGX_RTMP_VIDEO_H265) {
            header = &ctx->avc_header;
            ngx_rtmp_codec_parse_hevc_header(s, in);
        }
    }

    if (header == NULL) {
        return NGX_OK;
    }

    if (*header) {
        ngx_rtmp_free_shared_chain(cscf, *header);
    }

    *header = ngx_rtmp_append_shared_bufs(cscf, NULL, in);

    return NGX_OK;
}


#undef NGX_RTMP_CODEC_NON_SEQ_HEADER
#undef NGX_RTMP_CODEC_SINGLE_SEQ_HEADER
#undef NGX_RTMP_CODEC_COMBO_SEQ_HEADER


static void
ngx_rtmp_codec_parse_aac_header(ngx_rtmp_session_t *s, ngx_chain_t *in)
{
    ngx_uint_t              idx;
    ngx_rtmp_codec_ctx_t   *ctx;
    ngx_rtmp_bit_reader_t   br;

    static ngx_uint_t      aac_sample_rates[] =
        { 96000, 88200, 64000, 48000,
          44100, 32000, 24000, 22050,
          16000, 12000, 11025,  8000,
           7350,     0,     0,     0 };

#if (NGX_DEBUG)
    ngx_rtmp_codec_dump_header(s, "aac", in);
#endif

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);

    ngx_rtmp_bit_init_reader(&br, in->buf->pos, in->buf->last);

    ngx_rtmp_bit_read(&br, 16);

    ctx->aac_profile = (ngx_uint_t) ngx_rtmp_bit_read(&br, 5);
    if (ctx->aac_profile == 31) {
        ctx->aac_profile = (ngx_uint_t) ngx_rtmp_bit_read(&br, 6) + 32;
    }

    idx = (ngx_uint_t) ngx_rtmp_bit_read(&br, 4);
    if (idx == 15) {
        ctx->sample_rate = (ngx_uint_t) ngx_rtmp_bit_read(&br, 24);
    } else {
        ctx->sample_rate = aac_sample_rates[idx];
    }

    ctx->aac_chan_conf = (ngx_uint_t) ngx_rtmp_bit_read(&br, 4);

    if (ctx->aac_profile == 5 || ctx->aac_profile == 29) {
        
        if (ctx->aac_profile == 29) {
            ctx->aac_ps = 1;
        }

        ctx->aac_sbr = 1;

        idx = (ngx_uint_t) ngx_rtmp_bit_read(&br, 4);
        if (idx == 15) {
            ctx->sample_rate = (ngx_uint_t) ngx_rtmp_bit_read(&br, 24);
        } else {
            ctx->sample_rate = aac_sample_rates[idx];
        }

        ctx->aac_profile = (ngx_uint_t) ngx_rtmp_bit_read(&br, 5);
        if (ctx->aac_profile == 31) {
            ctx->aac_profile = (ngx_uint_t) ngx_rtmp_bit_read(&br, 6) + 32;
        }
    }

    /* MPEG-4 Audio Specific Config

       5 bits: object type
       if (object type == 31)
         6 bits + 32: object type
       4 bits: frequency index
       if (frequency index == 15)
         24 bits: frequency
       4 bits: channel configuration

       if (object_type == 5)
           4 bits: frequency index
           if (frequency index == 15)
             24 bits: frequency
           5 bits: object type
           if (object type == 31)
             6 bits + 32: object type
             
       var bits: AOT Specific Config
     */

    ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "codec: aac header profile=%ui, "
                   "sample_rate=%ui, chan_conf=%ui",
                   ctx->aac_profile, ctx->sample_rate, ctx->aac_chan_conf);
}


static void
ngx_rtmp_codec_parse_avc_header(ngx_rtmp_session_t *s, ngx_chain_t *in)
{
    ngx_uint_t              profile_idc, width, height, crop_left, crop_right,
                            crop_top, crop_bottom, frame_mbs_only, n, cf_idc,
                            num_ref_frames;
    ngx_rtmp_codec_ctx_t   *ctx;
    ngx_rtmp_bit_reader_t   br;

#if (NGX_DEBUG)
    ngx_rtmp_codec_dump_header(s, "avc", in);
#endif

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);

    ngx_rtmp_bit_init_reader(&br, in->buf->pos, in->buf->last);

    ngx_rtmp_bit_read(&br, 48);

    ctx->avc_profile = (ngx_uint_t) ngx_rtmp_bit_read_8(&br);
    ctx->avc_compat = (ngx_uint_t) ngx_rtmp_bit_read_8(&br);
    ctx->avc_level = (ngx_uint_t) ngx_rtmp_bit_read_8(&br);

    /* nal bytes */
    ctx->avc_nal_bytes = (ngx_uint_t) ((ngx_rtmp_bit_read_8(&br) & 0x03) + 1);

    /* nnals */
    if ((ngx_rtmp_bit_read_8(&br) & 0x1f) == 0) {
        return;
    }

    /* nal size */
    ngx_rtmp_bit_read(&br, 16);

    /* nal type */
    if (ngx_rtmp_bit_read_8(&br) != 0x67) {
        return;
    }

    /* SPS */

    /* profile idc */
    profile_idc = (ngx_uint_t) ngx_rtmp_bit_read(&br, 8);

    /* flags */
    ngx_rtmp_bit_read(&br, 8);

    /* level idc */
    ngx_rtmp_bit_read(&br, 8);

    /* SPS id */
    ngx_rtmp_bit_read_golomb(&br);

    if (profile_idc == 100 || profile_idc == 110 ||
        profile_idc == 122 || profile_idc == 244 || profile_idc == 44 ||
        profile_idc == 83 || profile_idc == 86 || profile_idc == 118)
    {
        /* chroma format idc */
        cf_idc = (ngx_uint_t) ngx_rtmp_bit_read_golomb(&br);
        
        if (cf_idc == 3) {

            /* separate color plane */
            ngx_rtmp_bit_read(&br, 1);
        }

        /* bit depth luma - 8 */
        ngx_rtmp_bit_read_golomb(&br);

        /* bit depth chroma - 8 */
        ngx_rtmp_bit_read_golomb(&br);

        /* qpprime y zero transform bypass */
        ngx_rtmp_bit_read(&br, 1);

        /* seq scaling matrix present */
        if (ngx_rtmp_bit_read(&br, 1)) {

            for (n = 0; n < (cf_idc != 3 ? 8u : 12u); n++) {

                /* seq scaling list present */
                if (ngx_rtmp_bit_read(&br, 1)) {

                    /* TODO: scaling_list()
                    if (n < 6) {
                    } else {
                    }
                    */
                }
            }
        }
    }

    /* log2 max frame num */
    ngx_rtmp_bit_read_golomb(&br);

    /* pic order cnt type */
    switch (ngx_rtmp_bit_read_golomb(&br)) {
    case 0:

        /* max pic order cnt */
        ngx_rtmp_bit_read_golomb(&br);
        break;

    case 1:

        /* delta pic order alwys zero */
        ngx_rtmp_bit_read(&br, 1);

        /* offset for non-ref pic */
        ngx_rtmp_bit_read_golomb(&br);

        /* offset for top to bottom field */
        ngx_rtmp_bit_read_golomb(&br);

        /* num ref frames in pic order */
        num_ref_frames = (ngx_uint_t) ngx_rtmp_bit_read_golomb(&br);

        for (n = 0; n < num_ref_frames; n++) {

            /* offset for ref frame */
            ngx_rtmp_bit_read_golomb(&br);
        }
    }

    /* num ref frames */
    ctx->avc_ref_frames = (ngx_uint_t) ngx_rtmp_bit_read_golomb(&br);

    /* gaps in frame num allowed */
    ngx_rtmp_bit_read(&br, 1);

    /* pic width in mbs - 1 */
    width = (ngx_uint_t) ngx_rtmp_bit_read_golomb(&br);

    /* pic height in map units - 1 */
    height = (ngx_uint_t) ngx_rtmp_bit_read_golomb(&br);

    /* frame mbs only flag */
    frame_mbs_only = (ngx_uint_t) ngx_rtmp_bit_read(&br, 1);

    if (!frame_mbs_only) {

        /* mbs adaprive frame field */
        ngx_rtmp_bit_read(&br, 1);
    }

    /* direct 8x8 inference flag */
    ngx_rtmp_bit_read(&br, 1);

    /* frame cropping */
    if (ngx_rtmp_bit_read(&br, 1)) {

        crop_left = (ngx_uint_t) ngx_rtmp_bit_read_golomb(&br);
        crop_right = (ngx_uint_t) ngx_rtmp_bit_read_golomb(&br);
        crop_top = (ngx_uint_t) ngx_rtmp_bit_read_golomb(&br);
        crop_bottom = (ngx_uint_t) ngx_rtmp_bit_read_golomb(&br);

    } else {

        crop_left = 0;
        crop_right = 0;
        crop_top = 0;
        crop_bottom = 0;
    }

    ctx->width = (width + 1) * 16 - (crop_left + crop_right) * 2;
    ctx->height = (2 - frame_mbs_only) * (height + 1) * 16 -
                  (crop_top + crop_bottom) * 2;

    ngx_log_debug7(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "codec: avc header "
                   "profile=%ui, compat=%ui, level=%ui, "
                   "nal_bytes=%ui, ref_frames=%ui, width=%ui, height=%ui",
                   ctx->avc_profile, ctx->avc_compat, ctx->avc_level,
                   ctx->avc_nal_bytes, ctx->avc_ref_frames,
                   ctx->width, ctx->height);
}


//add by adwpc for hevc
static void
ngx_rtmp_codec_parse_hevc_header(ngx_rtmp_session_t *s, ngx_chain_t *in)
{
    ngx_uint_t              i, j, narrs, nal_type, nnal, nnall;
    ngx_rtmp_codec_ctx_t   *ctx;
    ngx_rtmp_bit_reader_t   br;

#if (NGX_DEBUG)
    ngx_rtmp_hex_dump(s->connection->log, "ngx_rtmp_codec_parse_hevc_header", in->buf->start, in->buf->end);
    ngx_rtmp_codec_dump_header(s, "ngx_rtmp_codec_parse_hevc_header in:", in);
#endif
    // HEVCDecoderConfigurationRecord
    // http://ffmpeg.org/doxygen/trunk/hevc_8c_source.html#l00040

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);

    ngx_rtmp_bit_init_reader(&br, in->buf->pos, in->buf->last);

    //skip tag header and configurationVersion(1 byte)
    ngx_rtmp_bit_read(&br, 48);

    /* unsigned int(2) general_profile_space; */
    /* unsigned int(1) general_tier_flag; */
    /* unsigned int(5) general_profile_idc; */
    ctx->avc_profile = (ngx_uint_t) ((ngx_rtmp_bit_read_8(&br) & 0x1f) >> 5);

    //unsigned int(32) general_profile_compatibility_flags;
    ctx->avc_compat = (ngx_uint_t) ngx_rtmp_bit_read_32(&br);
    //unsigned int(48) general_constraint_indicator_flags;
    ngx_rtmp_bit_read(&br, 48);
    //unsigned int(8) general_level_idc; 
    ctx->avc_level = (ngx_uint_t) ngx_rtmp_bit_read_8(&br);

    /* bit(4) reserved = ‘1111’b; */
    /* unsigned int(12) min_spatial_segmentation_idc; */
    /* bit(6) reserved = ‘111111’b; */
    /* unsigned int(2) parallelismType; */
    /* bit(6) reserved = ‘111111’b; */
    /* unsigned int(2) chroma_format_idc; */
    /* bit(5) reserved = ‘11111’b; */
    /* unsigned int(3) bit_depth_luma_minus8; */
    /* bit(5) reserved = ‘11111’b; */
    /* unsigned int(3) bit_depth_chroma_minus8; */
    ngx_rtmp_bit_read(&br, 48);

    /* bit(16) avgFrameRate; */
    ctx->frame_rate = (ngx_uint_t) ngx_rtmp_bit_read_16(&br);

    /* bit(2) constantFrameRate; */
    ctx->avc_ref_frames = (ngx_uint_t) ngx_rtmp_bit_read(&br, 2);
    /* bit(3) numTemporalLayers; */
    /* bit(1) temporalIdNested; */
    ngx_rtmp_bit_read(&br, 4);

    /* unsigned int(2) lengthSizeMinusOne; */
    ctx->avc_nal_bytes = (ngx_uint_t)ngx_rtmp_bit_read(&br, 2) + 1;
    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0, "codec: hevc h265 ctx->avc_nal_bytes=%ui", ctx->avc_nal_bytes);

    /* unsigned int(8) numOfArrays; 04 */
    narrs = (ngx_uint_t)ngx_rtmp_bit_read_8(&br);
    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0, "codec: hevc header narrs=%ui ", narrs);

    //parse vps sps pps ..
    for ( j = 0; j < narrs; j++) {
        //bit(1) array_completeness;
        nal_type = (ngx_uint_t)ngx_rtmp_bit_read_8(&br) & 0x3f;
        nnal = (ngx_uint_t)ngx_rtmp_bit_read_16(&br);
        ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0, "codec: hevc nal_type=%ui nnal=%ui", nal_type, nnal);
        for (i = 0; i < nnal; i++) {
            nnall = (ngx_uint_t)ngx_rtmp_bit_read_16(&br);
            ngx_rtmp_bit_read(&br, nnall*8);
            ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0, "codec: hevc nnall=%ui",  nnall);
            //vps-32 sps-33 pps-34
        }
    }


    /* todo ctx->avc_ref_frames =  and so on*/
    ngx_log_debug8(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "codec: hevc header "
                   "profile=%ui, compat=%ui, level=%ui, "
                   "nal_bytes=%ui, ref_frames=%ui, frame_rate=%ui, width=%ui, height=%ui",
                   ctx->avc_profile, ctx->avc_compat, ctx->avc_level,
                   ctx->avc_nal_bytes, ctx->avc_ref_frames, ctx->frame_rate,
                   ctx->width, ctx->height);
}



#if (NGX_DEBUG)
static void
ngx_rtmp_codec_dump_header(ngx_rtmp_session_t *s, const char *type,
    ngx_chain_t *in)
{
    u_char buf[256], *p, *pp;
    u_char hex[] = "0123456789abcdef";

    for (pp = buf, p = in->buf->pos;
         p < in->buf->last && pp < buf + sizeof(buf) - 1;
         ++p)
    {
        *pp++ = hex[*p >> 4];
        *pp++ = hex[*p & 0x0f];
    }

    *pp = 0;

    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "codec: %s header %s", type, buf);
}
#endif


static ngx_int_t
ngx_rtmp_codec_reconstruct_meta(ngx_rtmp_session_t *s)
{
    ngx_rtmp_codec_ctx_t           *ctx;
    ngx_rtmp_core_srv_conf_t       *cscf;
    ngx_int_t                       rc;

    static struct {
        double                      width;
        double                      height;
        double                      duration;
        double                      frame_rate;
        double                      video_data_rate;
        double                      video_codec_id;
        double                      audio_data_rate;
        double                      audio_codec_id;
        u_char                      profile[32];
        u_char                      level[32];
    }                               v;

    static ngx_rtmp_amf_elt_t       out_inf[] = {

        { NGX_RTMP_AMF_STRING,
          ngx_string("Server"),
          "NGINX HTTP-FLV (https://github.com/aileone/nginx-http-flv-module)", 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("width"),
          &v.width, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("height"),
          &v.height, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("displayWidth"),
          &v.width, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("displayHeight"),
          &v.height, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("duration"),
          &v.duration, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("framerate"),
          &v.frame_rate, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("fps"),
          &v.frame_rate, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("videodatarate"),
          &v.video_data_rate, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("videocodecid"),
          &v.video_codec_id, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("audiodatarate"),
          &v.audio_data_rate, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("audiocodecid"),
          &v.audio_codec_id, 0 },

        { NGX_RTMP_AMF_STRING,
          ngx_string("profile"),
          &v.profile, sizeof(v.profile) },

        { NGX_RTMP_AMF_STRING,
          ngx_string("level"),
          &v.level, sizeof(v.level) },
    };

    static ngx_rtmp_amf_elt_t       out_elts[] = {

        { NGX_RTMP_AMF_STRING,
          ngx_null_string,
          "onMetaData", 0 },

        { NGX_RTMP_AMF_OBJECT,
          ngx_null_string,
          out_inf, sizeof(out_inf) },
    };

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);
    if (ctx == NULL) {
        return NGX_OK;
    }

    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    if (ctx->meta) {
        ngx_rtmp_free_shared_chain(cscf, ctx->meta);
        ctx->meta = NULL;
    }

    v.width = ctx->width;
    v.height = ctx->height;
    v.duration = ctx->duration;
    v.frame_rate = ctx->frame_rate;
    v.video_data_rate = ctx->video_data_rate;
    v.video_codec_id = ctx->video_codec_id;
    v.audio_data_rate = ctx->audio_data_rate;
    v.audio_codec_id = ctx->audio_codec_id;
    ngx_memcpy(v.profile, ctx->profile, sizeof(ctx->profile));
    ngx_memcpy(v.level, ctx->level, sizeof(ctx->level));

    rc = ngx_rtmp_append_amf(s, &ctx->meta, NULL, out_elts,
                             sizeof(out_elts) / sizeof(out_elts[0]));
    if (rc != NGX_OK || ctx->meta == NULL) {
        return NGX_ERROR;
    }

    return ngx_rtmp_codec_prepare_meta(s, 0);
}


static ngx_int_t
ngx_rtmp_codec_copy_meta(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
        ngx_chain_t *in)
{
    ngx_rtmp_codec_ctx_t      *ctx;
    ngx_rtmp_core_srv_conf_t  *cscf;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);

    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    if (ctx->meta) {
        ngx_rtmp_free_shared_chain(cscf, ctx->meta);
    }

    ctx->meta = ngx_rtmp_append_shared_bufs(cscf, NULL, in);

    if (ctx->meta == NULL) {
        return NGX_ERROR;
    }

    return ngx_rtmp_codec_prepare_meta(s, h->timestamp);
}


static ngx_int_t
ngx_rtmp_codec_prepare_meta(ngx_rtmp_session_t *s, uint32_t timestamp)
{
    ngx_rtmp_header_t      h;
    ngx_rtmp_codec_ctx_t  *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);

    ngx_memzero(&h, sizeof(h));
    h.csid = NGX_RTMP_CSID_AMF;
    h.msid = NGX_RTMP_MSID;
    h.type = NGX_RTMP_MSG_AMF_META;
    h.timestamp = timestamp;
    ngx_rtmp_prepare_message(s, &h, NULL, ctx->meta);

    ctx->meta_version = ngx_rtmp_codec_get_next_version();

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_codec_meta_data(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
        ngx_chain_t *in)
{
    ngx_rtmp_codec_app_conf_t      *cacf;
    ngx_rtmp_codec_ctx_t           *ctx;
    ngx_uint_t                      skip;

    static struct {
        double                      width;
        double                      height;
        double                      duration;
        double                      frame_rate;
        double                      video_data_rate;
        double                      video_codec_id_n;
        u_char                      video_codec_id_s[32];
        double                      audio_data_rate;
        double                      audio_codec_id_n;
        u_char                      audio_codec_id_s[32];
        u_char                      profile[32];
        u_char                      level[32];
    }                               v;

    static ngx_rtmp_amf_elt_t       in_video_codec_id[] = {

        { NGX_RTMP_AMF_NUMBER,
          ngx_null_string,
          &v.video_codec_id_n, 0 },

        { NGX_RTMP_AMF_STRING,
          ngx_null_string,
          &v.video_codec_id_s, sizeof(v.video_codec_id_s) },
    };

    static ngx_rtmp_amf_elt_t       in_audio_codec_id[] = {

        { NGX_RTMP_AMF_NUMBER,
          ngx_null_string,
          &v.audio_codec_id_n, 0 },

        { NGX_RTMP_AMF_STRING,
          ngx_null_string,
          &v.audio_codec_id_s, sizeof(v.audio_codec_id_s) },
    };

    static ngx_rtmp_amf_elt_t       in_inf[] = {

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("width"),
          &v.width, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("height"),
          &v.height, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("duration"),
          &v.duration, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("framerate"),
          &v.frame_rate, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("fps"),
          &v.frame_rate, 0 },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("videodatarate"),
          &v.video_data_rate, 0 },

        { NGX_RTMP_AMF_VARIANT,
          ngx_string("videocodecid"),
          in_video_codec_id, sizeof(in_video_codec_id) },

        { NGX_RTMP_AMF_NUMBER,
          ngx_string("audiodatarate"),
          &v.audio_data_rate, 0 },

        { NGX_RTMP_AMF_VARIANT,
          ngx_string("audiocodecid"),
          in_audio_codec_id, sizeof(in_audio_codec_id) },

        { NGX_RTMP_AMF_STRING,
          ngx_string("profile"),
          &v.profile, sizeof(v.profile) },

        { NGX_RTMP_AMF_STRING,
          ngx_string("level"),
          &v.level, sizeof(v.level) },
    };

    static ngx_rtmp_amf_elt_t       in_elts[] = {

        { NGX_RTMP_AMF_STRING,
          ngx_null_string,
          NULL, 0 },

        { NGX_RTMP_AMF_OBJECT,
          ngx_null_string,
          in_inf, sizeof(in_inf) },
    };

    cacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_codec_module);

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);
    if (ctx == NULL) {
        ctx = ngx_pcalloc(s->connection->pool, sizeof(ngx_rtmp_codec_ctx_t));
        ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_codec_module);
    }

    ngx_memzero(&v, sizeof(v));

    /* use -1 as a sign of unchanged data;
     * 0 is a valid value for uncompressed audio */
    v.audio_codec_id_n = -1;

    /* FFmpeg sends a string in front of actal metadata; ignore it */
    skip = !(in->buf->last > in->buf->pos
            && *in->buf->pos == NGX_RTMP_AMF_STRING);
    if (ngx_rtmp_receive_amf(s, in, in_elts + skip,
                sizeof(in_elts) / sizeof(in_elts[0]) - skip))
    {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                "codec: error parsing data frame");
        return NGX_OK;
    }

    ctx->width = (ngx_uint_t) v.width;
    ctx->height = (ngx_uint_t) v.height;
    ctx->duration = (ngx_uint_t) v.duration;
    ctx->frame_rate = (ngx_uint_t) v.frame_rate;
    ctx->video_data_rate = (ngx_uint_t) v.video_data_rate;
    ctx->video_codec_id = (ngx_uint_t) v.video_codec_id_n;
    ctx->audio_data_rate = (ngx_uint_t) v.audio_data_rate;
    ctx->audio_codec_id = (v.audio_codec_id_n == -1
            ? 0 : v.audio_codec_id_n == 0
            ? NGX_RTMP_AUDIO_UNCOMPRESSED : (ngx_uint_t) v.audio_codec_id_n);
    ngx_memcpy(ctx->profile, v.profile, sizeof(v.profile));
    ngx_memcpy(ctx->level, v.level, sizeof(v.level));

    ngx_log_debug8(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
            "codec: data frame: "
            "width=%ui height=%ui duration=%ui frame_rate=%ui "
            "video=%s (%ui) audio=%s (%ui)",
            ctx->width, ctx->height, ctx->duration, ctx->frame_rate,
            ngx_rtmp_get_video_codec_name(ctx->video_codec_id),
            ctx->video_codec_id,
            ngx_rtmp_get_audio_codec_name(ctx->audio_codec_id),
            ctx->audio_codec_id);

    switch (cacf->meta) {
        case NGX_RTMP_CODEC_META_ON:
            return ngx_rtmp_codec_reconstruct_meta(s);
        case NGX_RTMP_CODEC_META_COPY:
            return ngx_rtmp_codec_copy_meta(s, h, in);
    }

    /* NGX_RTMP_CODEC_META_OFF */

    return NGX_OK;
}


static void *
ngx_rtmp_codec_create_app_conf(ngx_conf_t *cf)
{
    ngx_rtmp_codec_app_conf_t  *cacf;

    cacf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_codec_app_conf_t));
    if (cacf == NULL) {
        return NULL;
    }

    cacf->meta = NGX_CONF_UNSET_UINT;

    return cacf;
}


static char *
ngx_rtmp_codec_merge_app_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_rtmp_codec_app_conf_t *prev = parent;
    ngx_rtmp_codec_app_conf_t *conf = child;

    ngx_conf_merge_uint_value(conf->meta, prev->meta, NGX_RTMP_CODEC_META_ON);

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_rtmp_codec_postconfiguration(ngx_conf_t *cf)
{
    ngx_rtmp_core_main_conf_t          *cmcf;
    ngx_rtmp_handler_pt                *h;
    ngx_rtmp_amf_handler_t             *ch;

    cmcf = ngx_rtmp_conf_get_module_main_conf(cf, ngx_rtmp_core_module);

    h = ngx_array_push(&cmcf->events[NGX_RTMP_MSG_AUDIO]);
    *h = ngx_rtmp_codec_av;

    h = ngx_array_push(&cmcf->events[NGX_RTMP_MSG_VIDEO]);
    *h = ngx_rtmp_codec_av;

    h = ngx_array_push(&cmcf->events[NGX_RTMP_DISCONNECT]);
    *h = ngx_rtmp_codec_disconnect;

    /* register metadata handler */
    ch = ngx_array_push(&cmcf->amf);
    if (ch == NULL) {
        return NGX_ERROR;
    }
    ngx_str_set(&ch->name, "@setDataFrame");
    ch->handler = ngx_rtmp_codec_meta_data;

    ch = ngx_array_push(&cmcf->amf);
    if (ch == NULL) {
        return NGX_ERROR;
    }
    ngx_str_set(&ch->name, "onMetaData");
    ch->handler = ngx_rtmp_codec_meta_data;


    return NGX_OK;
}
