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


#include "encoder.h"


static const struct {
	const char *name;
	const encoder_type_e type;
} _ENCODER_TYPES[] = {
	{"CPU",			ENCODER_TYPE_CPU},
	{"HW",			ENCODER_TYPE_HW},
	{"M2M-VIDEO",	ENCODER_TYPE_M2M_VIDEO},
	{"M2M-IMAGE",	ENCODER_TYPE_M2M_IMAGE},
	{"M2M-MJPEG",	ENCODER_TYPE_M2M_VIDEO},
	{"M2M-JPEG",	ENCODER_TYPE_M2M_IMAGE},
	{"OMX",			ENCODER_TYPE_M2M_IMAGE},
	{"RKMPP",		ENCODER_TYPE_RKMPP},
	{"NOOP",		ENCODER_TYPE_NOOP},
};


static void *_worker_job_init(void *v_enc);
static void _worker_job_destroy(void *v_job);
static bool _worker_run_job(worker_s *wr);


#define ER(_next)	enc->run->_next


encoder_s *encoder_init(void) {
	encoder_runtime_s *run;
	A_CALLOC(run, 1);
	run->type = ENCODER_TYPE_CPU;
	run->quality = 80;
	A_MUTEX_INIT(&run->mutex);

	encoder_s *enc;
	A_CALLOC(enc, 1);
	enc->type = run->type;
	enc->n_workers = get_cores_available();
	enc->run = run;
	return enc;
}

void encoder_destroy(encoder_s *enc) {
	if (ER(m2ms)) {
		for (unsigned index = 0; index < ER(n_m2ms); ++index) {
			if (ER(m2ms[index])) {
				m2m_encoder_destroy(ER(m2ms[index]));
			}
		}
		free(ER(m2ms));
	}
	A_MUTEX_DESTROY(&ER(mutex));
	free(enc->run);
	free(enc);
}

encoder_type_e encoder_parse_type(const char *str) {
	for (unsigned index = 0; index < ARRAY_LEN(_ENCODER_TYPES); ++index) {
		if (!strcasecmp(str, _ENCODER_TYPES[index].name)) {
			return _ENCODER_TYPES[index].type;
		}
	}
	return ENCODER_TYPE_UNKNOWN;
}

const char *encoder_type_to_string(encoder_type_e type) {
	for (unsigned index = 0; index < ARRAY_LEN(_ENCODER_TYPES); ++index) {
		if (_ENCODER_TYPES[index].type == type) {
			return _ENCODER_TYPES[index].name;
		}
	}
	return _ENCODER_TYPES[0].name;
}

workers_pool_s *encoder_workers_pool_init(encoder_s *enc, device_s *dev) {
#	define DR(_next) dev->run->_next

	encoder_type_e type = (ER(cpu_forced) ? ENCODER_TYPE_CPU : enc->type);
	unsigned quality = dev->jpeg_quality;
	unsigned n_workers = min_u(enc->n_workers, DR(n_bufs));
	bool cpu_forced = false;

	if (is_jpeg(DR(format)) && type != ENCODER_TYPE_HW) {
		LOG_INFO("Switching to HW encoder: the input is (M)JPEG ...");
		type = ENCODER_TYPE_HW;
	}

	if (type == ENCODER_TYPE_HW) {
		if (!is_jpeg(DR(format))) {
			LOG_INFO("Switching to CPU encoder: the input format is not (M)JPEG ...");
			goto use_cpu;
		}
		quality = DR(jpeg_quality);
		n_workers = 1;

	} else if (type == ENCODER_TYPE_M2M_VIDEO || type == ENCODER_TYPE_M2M_IMAGE) {
		LOG_DEBUG("Preparing M2M-%s encoder ...", (type == ENCODER_TYPE_M2M_VIDEO ? "VIDEO" : "IMAGE"));
		if (ER(m2ms) == NULL) {
			A_CALLOC(ER(m2ms), n_workers);
		}
		for (; ER(n_m2ms) < n_workers; ++ER(n_m2ms)) {
			// Начинаем с нуля и доинициализируем на следующих заходах при необходимости
			char name[32];
			snprintf(name, 32, "JPEG-%u", ER(n_m2ms));
			if (type == ENCODER_TYPE_M2M_VIDEO) {
				ER(m2ms[ER(n_m2ms)]) = m2m_mjpeg_encoder_init(name, enc->m2m_path, quality);
			} else {
				ER(m2ms[ER(n_m2ms)]) = m2m_jpeg_encoder_init(name, enc->m2m_path, quality);
			}
		}

	} else if (type == ENCODER_TYPE_NOOP) {
		n_workers = 1;
		quality = 0;
	}

	goto ok;

	use_cpu:
		type = ENCODER_TYPE_CPU;
		quality = dev->jpeg_quality;

	ok:
		if (type == ENCODER_TYPE_NOOP) {
			LOG_INFO("Using JPEG NOOP encoder");
		} else if (quality == 0) {
			LOG_INFO("Using JPEG quality: encoder default");
		} else {
			LOG_INFO("Using JPEG quality: %u%%", quality);
		}

		A_MUTEX_LOCK(&ER(mutex));
		ER(type) = type;
		ER(quality) = quality;
		if (cpu_forced) {
			ER(cpu_forced) = true;
		}
		A_MUTEX_UNLOCK(&ER(mutex));

		long double desired_interval = 0;
		if (dev->desired_fps > 0 && (dev->desired_fps < dev->run->hw_fps || dev->run->hw_fps == 0)) {
			desired_interval = (long double)1 / dev->desired_fps;
		}

		return workers_pool_init(
			"JPEG", "jw", n_workers, desired_interval,
			_worker_job_init, (void *)enc,
			_worker_job_destroy,
			_worker_run_job);

#	undef DR
}

void encoder_get_runtime_params(encoder_s *enc, encoder_type_e *type, unsigned *quality) {
	A_MUTEX_LOCK(&ER(mutex));
	*type = ER(type);
	*quality = ER(quality);
	A_MUTEX_UNLOCK(&ER(mutex));
}

static void *_worker_job_init(void *v_enc) {
	encoder_job_s *job;
	A_CALLOC(job, 1);
	job->enc = (encoder_s *)v_enc;
	job->dest = frame_init();
	return (void *)job;
}

static void _worker_job_destroy(void *v_job) {
	encoder_job_s *job = (encoder_job_s *)v_job;
	frame_destroy(job->dest);
	free(job);
}

#undef ER

static bool _worker_run_job(worker_s *wr) {
	encoder_job_s *job = (encoder_job_s *)wr->job;
	frame_s *src = &job->hw->raw;
	frame_s *dest = job->dest;

#	define ER(_next) job->enc->run->_next

	assert(ER(type) != ENCODER_TYPE_UNKNOWN);

	if (ER(type) == ENCODER_TYPE_CPU) {
		LOG_VERBOSE("Compressing JPEG using CPU: worker=%s, buffer=%u",
			wr->name, job->hw->buf.index);
		cpu_encoder_compress(src, dest, ER(quality));

	} else if (ER(type) == ENCODER_TYPE_HW || ER(type) == ENCODER_TYPE_RKMPP) {
		LOG_VERBOSE("Compressing JPEG using HW (just copying): worker=%s, buffer=%u",
			wr->name, job->hw->buf.index);
		hw_encoder_compress(src, dest);

	} else if (ER(type) == ENCODER_TYPE_M2M_VIDEO || ER(type) == ENCODER_TYPE_M2M_IMAGE) {
		LOG_VERBOSE("Compressing JPEG using M2M-%s: worker=%s, buffer=%u",
			(ER(type) == ENCODER_TYPE_M2M_VIDEO ? "VIDEO" : "IMAGE"), wr->name, job->hw->buf.index);
		if (m2m_encoder_compress(ER(m2ms[wr->number]), src, dest, false) < 0) {
			goto error;
		}

	} else if (ER(type) == ENCODER_TYPE_NOOP) {
		LOG_VERBOSE("Compressing JPEG using NOOP (do nothing): worker=%s, buffer=%u",
			wr->name, job->hw->buf.index);
		usleep(5000); // Просто чтобы работала логика desired_fps
		dest->encode_end_ts = get_now_monotonic();
	}

	LOG_VERBOSE("Compressed new JPEG: size=%zu, time=%0.3Lf, worker=%s, buffer=%u",
		job->dest->used,
		job->dest->encode_end_ts - job->dest->encode_begin_ts,
		wr->name,
		job->hw->buf.index);

	return true;

	error:
		LOG_ERROR("Compression failed: worker=%s, buffer=%u", wr->name, job->hw->buf.index);
		LOG_ERROR("Error while compressing buffer, falling back to CPU");
		A_MUTEX_LOCK(&ER(mutex));
		ER(cpu_forced) = true;
		A_MUTEX_UNLOCK(&ER(mutex));
		return false;

#	undef ER
}
