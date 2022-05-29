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


#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <assert.h>

#include "../../libs/tools.h"
#include "../../libs/logging.h"
#include "../../libs/frame.h"
#include "../../libs/memsink.h"
#include "../../libs/unjpeg.h"
#include "../m2m.h"
#include "../rkmpp.h"
#include "../encoder.h"


typedef struct {
	memsink_s		*sink;
	frame_s			*tmp_src;
	frame_s			*dest;
	void			*enc;
	encoder_type_e	enc_type;
	atomic_bool		online;
} h264_stream_s;


h264_stream_s *h264_stream_init(memsink_s *sink, const char *path, unsigned bitrate, unsigned gop, encoder_type_e enc_type);
void h264_stream_destroy(h264_stream_s *h264);
void h264_stream_process(h264_stream_s *h264, const frame_s *frame, bool force_key);
