/*****************************************************************************
#                                                                            #
#    uStreamer - Lightweight and fast MJPEG-HTTP streamer.                   #
#                                                                            #
#    Copyright (C) 2018-2022  Maxim Devaev <mdevaev@gmail.com>               #
#                                                                            #
#    This program is free software: you can redistribute it and/or modify    #
#    it under the terms of the GNU General Public License as published by    #
#    the Free Software Foundation, either version 3 of the License, or       #
#    (at your option) any later version.                                     #
#                                                                            #
#    This program is distributed in the hope that it will be useful,         #
#    but WITHOUT ANY WARRANTY; without even the implied warranty of          #
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           #
#    GNU General Public License for more details.                            #
#                                                                            #
#    You should have received a copy of the GNU General Public License       #
#    along with this program.  If not, see <https://www.gnu.org/licenses/>.  #
#                                                                            #
*****************************************************************************/


#include "stream.h"


h264_stream_s *h264_stream_init(memsink_s *sink, const char *path, unsigned bitrate, unsigned gop, encoder_type_e enc_type) {
	h264_stream_s *h264;
	A_CALLOC(h264, 1);
	h264->enc_type = enc_type;
	h264->sink = sink;
	h264->tmp_src = frame_init();
	h264->dest = frame_init();
	atomic_init(&h264->online, false);
	//Add rockchip mpp support
	switch (h264->enc_type)
	{
#ifdef WITH_MPP
	case ENCODER_TYPE_MPP:
		h264->enc = (void*)mpp_h264_encoder_init("H264", path, bitrate, gop);
		break;
#endif
	default:
		h264->enc = (void*)m2m_h264_encoder_init("H264", path, bitrate, gop);
		break;
	}
	return h264;
}

void h264_stream_destroy(h264_stream_s *h264) {
	switch (h264->enc_type)
	{
#ifdef WITH_MPP
	case ENCODER_TYPE_MPP:
		mpp_encoder_destroy((mpp_encoder_s*)h264->enc);
		break;
#endif
	default:
		m2m_encoder_destroy((m2m_encoder_s*)h264->enc);
		break;
	}
	frame_destroy(h264->dest);
	frame_destroy(h264->tmp_src);
	free(h264);
}

void h264_stream_process(h264_stream_s *h264, const frame_s *frame, bool force_key) {
	if (!memsink_server_check(h264->sink, frame)) {
		return;
	}

	if (is_jpeg(frame->format)) {
		//RPI use software jpeg decode,but rockchip mpp has hardware jpeg decode.
		long double now = get_now_monotonic();
		LOG_DEBUG("H264: Input frame is JPEG; decoding ...");
		if (unjpeg(frame, h264->tmp_src, true) < 0) {
			return;
		}
		frame = h264->tmp_src;
		LOG_VERBOSE("H264: JPEG decoded; time=%.3Lf", get_now_monotonic() - now);
	}

	bool online = false;
	
	switch (h264->enc_type)
	{
#ifdef WITH_MPP
	case ENCODER_TYPE_MPP:{
		if(!mpp_encoder_compress(h264->enc, frame, h264->dest, force_key)){
			online = !memsink_server_put(h264->sink, h264->dest);
		}
		break;
	}
#endif
	default:
		if (!m2m_encoder_compress(h264->enc, frame, h264->dest, force_key)) {
			online = !memsink_server_put(h264->sink, h264->dest);
		}
		break;
	}
	atomic_store(&h264->online, online);
}
