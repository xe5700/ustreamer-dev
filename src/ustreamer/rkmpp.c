#include "rkmpp.h"


#ifdef WITH_RKMPP
static mpp_encoder_s *_mpp_encoder_init(
	const char *name, const char *path, unsigned output_format,
	unsigned fps, bool allow_dma, MppEncCfg cfg);

static int _mpp_encoder_prepare(mpp_encoder_s *enc, const frame_s *frame);

static int _mpp_encoder_init_buffers(
	mpp_encoder_s *enc, const char *name, enum v4l2_buf_type type,
	m2m_buffer_s **bufs_ptr, unsigned *n_bufs_ptr, bool dma);

static void _mpp_encoder_cleanup(mpp_encoder_s *enc);
static int _mpp_encoder_compress_raw(mpp_encoder_s *enc, const frame_s *src, frame_s *dest);

#define E_LOG_ERROR(_msg, ...)		LOG_ERROR("%s: " _msg, enc->name, ##__VA_ARGS__)
#define E_LOG_PERROR(_msg, ...)		LOG_PERROR("%s: " _msg, enc->name, ##__VA_ARGS__)
#define E_LOG_INFO(_msg, ...)		LOG_INFO("%s: " _msg, enc->name, ##__VA_ARGS__)
#define E_LOG_VERBOSE(_msg, ...)	LOG_VERBOSE("%s: " _msg, enc->name, ##__VA_ARGS__)
#define E_LOG_DEBUG(_msg, ...)		LOG_DEBUG("%s: " _msg, enc->name, ##__VA_ARGS__)

mpp_encoder_s *mpp_h264_encoder_init(const char *name, const char *path, unsigned bitrate, unsigned gop)
{
    // #	define OPTION(_key, _value) {#_key, V4L2_CID_MPEG_VIDEO_##_key, _value}

    // 	m2m_option_s options[] = {
    // 		OPTION(BITRATE, bitrate * 1000),
    // 		// OPTION(BITRATE_PEAK, bitrate * 1000),
    // 		OPTION(H264_I_PERIOD, gop),
    // 		OPTION(H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE),
    // 		OPTION(H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_4_0),
    // 		OPTION(REPEAT_SEQ_HEADER, 1),
    // 		OPTION(H264_MIN_QP, 16),
    // 		OPTION(H264_MAX_QP, 32),
    // 		{NULL, 0, 0},
    // 	};

    // #	undef OPTION
    MppEncCfg cfg;
    mpp_enc_cfg_init(&cfg);
    bitrate*=1000;
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bitrate);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bitrate * 17 / 16);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bitrate * 15 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", gop);
    // FIXME: 30 or 0? https://github.com/6by9/yavta/blob/master/yavta.c#L2100
    // По логике вещей правильно 0, но почему-то на низких разрешениях типа 640x480
    // енкодер через несколько секунд перестает производить корректные фреймы.
    return _mpp_encoder_init(name, path, MPP_VIDEO_CodingAVC, 30, true, cfg);
}

mpp_encoder_s *mpp_mjpeg_encoder_init(const char *name, const char *path, unsigned quality) {
	const double b_min = 25;
	const double b_max = 20000;
	const double step = 25;
	double bitrate = log10(quality) * (b_max - b_min) / 2 + b_min;
	bitrate = step * round(bitrate / step);
	bitrate *= 1000; // From Kbps
	assert(bitrate > 0);

	// m2m_option_s options[] = {
	// 	{"BITRATE", V4L2_CID_MPEG_VIDEO_BITRATE, bitrate},
	// 	{NULL, 0, 0},
	// };

    MppEncCfg cfg;
    mpp_enc_cfg_init(&cfg);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bitrate);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bitrate);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bitrate);
	// FIXME: То же самое про 30 or 0, но еще даже не проверено на низких разрешениях
	return _mpp_encoder_init(name, path, MPP_VIDEO_CodingMJPEG, 30, true, cfg);
}


void mpp_encoder_destroy(mpp_encoder_s *enc) {
	E_LOG_INFO("Destroying encoder ...");
	_mpp_encoder_cleanup(enc);
    if (enc->ctx) {
        mpp_destroy(enc->ctx);
        enc->ctx = NULL;
    }

    if (enc->cfg) {
        mpp_enc_cfg_deinit(enc->cfg);
        enc->cfg = NULL;
    }

	free(enc->path);
	free(enc->name);
	free(enc);
}


static mpp_encoder_s *_mpp_encoder_init(
    const char *name, const char *path, unsigned output_format,
    unsigned fps, bool allow_dma, MppEncCfg options)
{

    LOG_INFO("%s: Initializing encoder ...", name);

    // mpp_encoder_runtime_s *run;
    // A_CALLOC(run, 1);
    // run->last_online = -1;
    // run->fd = -1;

    mpp_encoder_s *enc;
    A_CALLOC(enc, 1);
    assert(enc->name = strdup(name));
    // if (path == NULL) {
    // 	assert(enc->path = strdup(output_format == V4L2_PIX_FMT_JPEG ? "/dev/video31" : "/dev/video11"));
    // } else {
    // 	assert(enc->path = strdup(path));
    // }
    enc->output_format = output_format;
    enc->fps = fps;
    enc->allow_dma = allow_dma;
    // enc->run = run;
    enc->cfg = options;
    // memcpy(enc->cfg, &options, sizeof(MppEncCfg));
    enc->pkt_buf = (char *)malloc(PKT_SIZE);

    int ret = 0;
    ret = mpp_packet_init(&enc->pkt, enc->pkt_buf, PKT_SIZE);
    if (ret)
    {
        E_LOG_ERROR("failed to exec mpp_packet_init ret %d", ret);
        return NULL;
    }
    ret = mpp_create(&enc->ctx, &enc->api);
    if (ret != MPP_OK)
    {
        E_LOG_ERROR("failed to exec mpp_create ret %d", ret);
        return NULL;
    }
    MppPollType timeout = MPP_POLL_BLOCK;
    ret = enc->api->control(enc->ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    if (MPP_OK != ret)
    {
        E_LOG_ERROR("mpi control set output timeout %d ret %d\n", timeout, ret);
        return NULL;
    }

    ret = mpp_init(enc->ctx, MPP_CTX_ENC, enc->output_format);

    if (MPP_OK != ret)
    {
        E_LOG_ERROR("mpp_init failed\n");
        return NULL;
    }

    // if (!strncmp(path, "/dev/video", 10))
    // {
    //     mpp_log("open camera device");
    //     enc->cam_ctx = camera_source_init(path, 4, p->width, p->height, p->fmt);
    //     mpp_log("new framecap ok");
    //     if (p->cam_ctx == NULL)
    //         mpp_err("open %s fail", path);
    // }
    // else
    // {
    //     enc->fp_input = fopen(path, "rb");
    //     if (NULL == enc->fp_input)
    //     {
    //         mpp_err("failed to open input file %s\n", path);
    //         mpp_err("create default yuv image for test\n");
    //     }
    // }

    MppEncCfg cfg = enc->cfg;

    mpp_enc_cfg_set_s32(cfg, "prep:format", enc->output_format);
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_init", -1);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 51);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 10);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", 51);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 10);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 2);
    switch (output_format)
    {
    case MPP_VIDEO_CodingAVC : {
        /*
        * H.264 profile_idc parameter
        * 66  - Baseline profile
        * 77  - Main profile
        * 100 - High profile
        */
        mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
        /*
        * H.264 level_idc parameter
        * 10 / 11 / 12 / 13    - qcif@15fps / cif@7.5fps / cif@15fps / cif@30fps
        * 20 / 21 / 22         - cif@30fps / half-D1@@25fps / D1@12.5fps
        * 30 / 31 / 32         - D1@25fps / 720p@30fps / 720p@60fps
        * 40 / 41 / 42         - 1080p@30fps / 1080p@30fps / 1080p@60fps
        * 50 / 51 / 52         - 4K@30fps
        */
        mpp_enc_cfg_set_s32(cfg, "h264:level", 41);
        mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
        mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
        mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);
        break;
    }
    case MPP_VIDEO_CodingMJPEG : {
        mpp_enc_cfg_set_s32(cfg, "jpeg:q_factor", 80);
        mpp_enc_cfg_set_s32(cfg, "jpeg:qf_max", 99);
        mpp_enc_cfg_set_s32(cfg, "jpeg:qf_min", 1);
        break;
    }
    default:{
        break;
    }
    }
    // mpp_env_get_u32("split_mode", cfg, MPP_ENC_SPLIT_NONE);
    // mpp_env_get_u32("split_arg", cfg, 0);
    
	ret = enc->api->control(enc->ctx, MPP_ENC_SET_CFG, cfg);
	if (ret != MPP_OK) {
		LOGE("failed to set enc config: %d\n", ret);
		goto err;
	}
    return enc;
    err:
        return -1;
}


int mpp_encoder_compress(mpp_encoder_s *enc, const frame_s *src, frame_s *dest, bool force_key) {
    int ret = 0;
    ret = _mpp_encoder_prepare(enc, src);
    if (ret) {
        E_LOG_ERROR("encode set config failed\n");
        goto error;
    }
    ret = _mpp_encoder_compress_raw(enc,src, dest);
    if (ret) {
        E_LOG_ERROR("encode compress failed\n");
        goto error;
    }
    error:
        _mpp_encoder_cleanup(enc);
        return -1;
}
static int _mpp_encoder_prepare(mpp_encoder_s *enc, const frame_s *frame) {
    int ret = 0;
	bool dma = (enc->allow_dma && frame->dma_fd >= 0);

	E_LOG_INFO("Configuring encoder: DMA=%d ...", dma);

	_mpp_encoder_cleanup(enc);
    size_t mpp_buf_size = mpp_buffer_get_size(enc->frm_buf);
    ret = mpp_buffer_get(NULL, &enc->frm_buf, frame->used);
    if(mpp_buf_size < frame->used){
        ret = mpp_buffer_get(NULL, &enc->frm_buf, frame->used);
        if(ret){
            E_LOG_ERROR("mpp_frame_init failed\n");
            goto error;
        }
    }
    MppFrame m_frame = &enc->frm;
    mpp_frame_init(&m_frame);
    mpp_frame_set_width(m_frame, frame->width);
    mpp_frame_set_height(m_frame, frame->height);
    RK_U32 ver_stride = frame->used / frame->stride;
    mpp_frame_set_ver_stride(m_frame, ver_stride);
    int depth=8;
    MppFrameFormat m_frame_format;
    {
        MppFrameFormat f;

        switch (frame->format)
        {
            case V4L2_PIX_FMT_ABGR32:
                depth=32;
                f = MPP_FMT_ABGR8888;
                break;
            case V4L2_PIX_FMT_ARGB32:
                depth=32;
                f = MPP_FMT_ARGB8888;
                break;
            case V4L2_PIX_FMT_BGR32:
                depth=32;
                f = MPP_FMT_BGR101010;
                break;
            case V4L2_PIX_FMT_BGR24:
                depth=24;
                f = MPP_FMT_BGR888;
                break;
            case V4L2_PIX_FMT_BGRA32:
                depth=32;
                f = MPP_FMT_BGRA8888;
                break;
            case V4L2_PIX_FMT_RGB32:
                depth=32;
                f = MPP_FMT_RGB101010;
                break;
            case V4L2_PIX_FMT_RGB444:
                depth=8;
                f = MPP_FMT_RGB444;
                break;
            case V4L2_PIX_FMT_RGB555:
                depth=8;
                f = MPP_FMT_RGB555;
                break;
            case V4L2_PIX_FMT_RGB565:
                depth=8;
                f = MPP_FMT_RGB565;
                break;
            case V4L2_PIX_FMT_RGB24:
                depth=24;
                f = MPP_FMT_RGB888;
                break;
            case V4L2_PIX_FMT_RGBA32:
                depth=32;
                f = MPP_FMT_RGBA8888;
                break;
            case V4L2_PIX_FMT_YUV411P:
                depth=8;
                f = MPP_FMT_YUV411SP;
                break;
            case V4L2_PIX_FMT_NV12M:
                depth=8;
                f = MPP_FMT_YUV420SP;
                break;
            case V4L2_PIX_FMT_YUV422P:
                depth=8;
                f = MPP_FMT_YUV422P;
                break;
            case V4L2_PIX_FMT_YUV444:
                depth=8;
                f = MPP_FMT_YUV444P;
                break;
        }
        m_frame_format = f;
    }
    RK_U32 hor_stride = frame->stride * 8 / depth;
    mpp_frame_set_hor_stride(m_frame, frame->stride * 8 / depth);
    mpp_frame_set_fmt(m_frame, m_frame_format);
    // mpp_packet_init(&enc->pkt, enc->pkt_buf);
    E_LOG_DEBUG("Apply input size: %dx%d(%dx%d)", frame->width, frame->height, hor_stride, ver_stride);
	E_LOG_DEBUG("Encoder state: *** READY ***");
    ret = enc->api->control(enc->ctx, MPP_ENC_SET_CFG, enc->cfg);
    if (ret != MPP_OK) {
		E_LOG_ERROR("failed to set enc config: %d\n", ret);
		goto error;
	}
	return 0;
	error:
		_mpp_encoder_cleanup(enc);
		E_LOG_ERROR("Encoder destroyed due an error (prepare)");
		return -1;
}


static int _mpp_encoder_compress_raw(mpp_encoder_s *enc, const frame_s *src, frame_s *dest) {
    int ret = 0;
	E_LOG_DEBUG("Compressing new frame;");
    MppFrame m_frame = &enc->frm;
    ret = mpp_buffer_get(NULL, enc->frm_buf, src->used);
    if(ret != MPP_OK){
        E_LOG_ERROR("Mpp buffer get failed;")
        goto error;
    }
    mpp_frame_set_buffer(m_frame, enc->frm_buf);
    void *p_buf = mpp_buffer_get_ptr(enc->frm_buf);
    memcpy(p_buf, src->data, src->used);
    mpp_frame_set_buf_size(m_frame,src->used);
    mpp_buffer_put(enc->frm_buf);
    enc->api->encode_put_frame(enc->ctx, enc->frm);
    size_t pos = 0;
    MppPacket pkt = enc->pkt;
    RK_U32 eoi = 1;
    // if(mpp_packet_is_partition(pkt)){
    // }
    do
    {
        enc->api->encode_get_packet(enc->ctx, &enc->pkt);
        MppMeta meta;
        // mpp_meta_get_packet(meta, MppMetaKey)
        void* ptr = mpp_packet_get_pos(pkt);
        size_t len = mpp_packet_get_length(pkt);
        if (mpp_packet_is_partition(pkt)) {
            eoi = mpp_packet_is_eoi(pkt);
        }
        frame_append_data(dest->data, ptr, len);
        //pos+=len;
        
    }while (!eoi);
    
	return 0;
	error:
        _mpp_encoder_cleanup(enc);
		return -1;
}


static void _mpp_encoder_cleanup(mpp_encoder_s *enc) {
    mpp_encoder_s* p = enc;
    if (p->frm) {
        mpp_frame_deinit(&p->frm);
        p->frm = NULL;
    }
    if (p->pkt) {
        mpp_packet_deinit(&p->pkt);
        p->pkt = NULL;
    }
}

#endif