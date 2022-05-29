
#include "m2m.h"


// typedef struct {
// 	int				fd;
// 	m2m_buffer_s	*input_bufs;
// 	unsigned		n_input_bufs;
// 	m2m_buffer_s	*output_bufs;
// 	unsigned		n_output_bufs;

// 	unsigned	width;
// 	unsigned	height;
// 	unsigned	input_format;
// 	unsigned	stride;
// 	bool		dma;
// 	bool		ready;

// 	int last_online;
// } mpp_encoder_runtime_s;

#define PKT_SIZE    1024*4
#ifdef WITH_RKMPP
	#include "vpu.h"
	#include "rk_mpi.h"
	#include "rk_type.h"
	#include "vpu_api.h"
	#include "mpp_err.h"
	#include "mpp_task.h"
	#include "mpp_meta.h"
	#include "mpp_frame.h"
	#include "mpp_buffer.h"
	#include "mpp_packet.h"
	#include "rk_mpi_cmd.h"
	#include "rk_mpi.h"

	typedef struct {
		char			*name;
		char			*path;
		unsigned		output_format;
		unsigned		fps;
		bool			allow_dma;
    	MppBufferGroup buf_grp;
		MppCtx ctx;
		MppApi *api;
		MppPacket pkt;
		MppBuffer pkt_buf;
		MppBuffer frm_buf;
		MppFrame frm;
		MppEncCfg cfg;
    	FILE *fp_input;
    	MppBuffer md_info;
    	
		// RGA *mRGA;

		// sp_dev *mDev;
		// sp_plane **mPlanes;
		// sp_crtc *mCrtc;
		// sp_plane *mTestPlane;
		// mpp_encoder_runtime_s *run;
	} mpp_encoder_s;
	int mpp_encoder_compress(mpp_encoder_s *enc, const frame_s *src, frame_s *dest, bool force_key);
#endif
