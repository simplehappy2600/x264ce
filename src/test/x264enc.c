 /*****************************************************************************
 * Copyright (C) 2012
 * Authors: simplehappy2600@126.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *****************************************************************************/

#include "common/osdep.h"
#include "x264.h"

typedef struct X264Context {
	x264_param_t    params;
	x264_t         *enc;
	x264_picture_t  pic;
	
	int width;
	int height;
	int fps;
	
} X264Context;

typedef struct AVFrame{
    //int     csp;       /* colorspace */
    int     width;     /* width of the picture */
    int     height;    /* height of the picture */
    int     planes;    /* number of planes */
    uint8_t *plane[4]; /* pointers for each plane */
    int     stride[4]; /* strides for each plane */
} AVFrame;

typedef struct AVPacket{
	uint8_t *data;
	int size;
} AVPacket;

static void X264_log(void *p, int level, const char *fmt, va_list args)
{
	//TODO
}

int X264_init(X264Context *x4)
{
	x264_param_t *param = &x4->params;

	x264_param_default_preset(param, "veryfast", "zerolatency");

	param->i_csp = X264_CSP_I420;

	param->i_threads = 1;
	param->i_width = x4->width;
	param->i_height = x4->height;
	param->i_fps_num = x4->fps;
	param->i_fps_den = 1;
	// Intra refres:
	param->i_keyint_max = x4->fps;
	param->b_intra_refresh = 1;
	// //Rate control:
	param->rc.i_rc_method = X264_RC_CRF;
	param->rc.f_rf_constant = 25;
	param->rc.f_rf_constant_max = 35;
	//For streaming:
	param->b_repeat_headers = 1;
	param->b_annexb = 1;
	x264_param_apply_profile(param, "baseline");

	x4->enc = x264_encoder_open(param);

	//x264_picture_t pic_in, pic_out;
	//x264_picture_alloc(&pic_in, X264_CSP_I420, width, height);
			
}

int X264_frame(X264Context *x4, AVPacket *pkt, const AVFrame *frame, int *got_packet)
{
	x264_nal_t* nal;
	int nnal;
	x264_picture_t pic_out;

	x264_picture_init(&x4->pic);
	x4->pic.img.i_csp   = x4->params.i_csp;
	x4->pic.img.i_plane = frame->planes;
	for (int i = 0; i < x4->pic.img.i_plane; i++) {
		x4->pic.img.plane[i]    = frame->plane[i];
		x4->pic.img.i_stride[i] = frame->stride[i];
	}
	//x4->pic.i_pts  = frame->pts;
	//
	x4->pic.i_type = X264_TYPE_AUTO;	

	int frame_size = x264_encoder_encode(x4->enc, &nal, &nnal, &x4->pic, &pic_out);
	if (frame_size > 0){
		pkt->data = nal[0].p_payload;
		pkt->size = frame_size;
	}
}

int X264_close(X264Context *x4)
{
	if (x4->enc){
		x264_encoder_close(x4->enc);
	}		
}

//--------------------------------------------------------------------
FILE *logfile = NULL;

static void x264log(const char *psz_fmt, ... )
{
	va_list arg;
        va_start( arg, psz_fmt );
#ifdef WINCE
	vfprintf( logfile, psz_fmt, arg );
#else
	vfprintf( stderr, psz_fmt, arg );
#endif
        va_end( arg );
}

int main()
{
#ifdef WINCE
	char *input_filename = "Storage Card\\akiyo_qcif.yuv";
	char *output_filename = "Storage Card\\out.264";
#else
	char *input_filename = "../../seq/akiyo_qcif.yuv";
	char *output_filename = "../../seq/akiyo_qcif.264";
#endif

#ifdef WINCE
    	logfile = fopen("Storage Card\\log.txt", "w");
#endif

	FILE *fin = NULL; 
	FILE *fout = NULL;
	
	int csp = X264_CSP_I420;
	int width = 176;
	int height = 144;
	int fps = 30;

	AVFrame frame;
	memset(&frame, 0, sizeof(frame));
	frame.width = width;
	frame.height = height;
	frame.planes = 3;
	frame.plane[0] = malloc(width*height);
	frame.plane[1] = malloc(width*height/4);
	frame.plane[2] = malloc(width*height/4);
	frame.stride[0] = 176;
	frame.stride[1] = 176/2;
	frame.stride[2] = 176/2;
		

	do{


		fin = fopen(input_filename, "rb");
		if (fin == NULL){
			x264log("can not open file %s\n", input_filename);		
			break;
		}
		else{			
			x264log("in file is %s\n", input_filename);		
		}		
		fout = fopen(output_filename, "w+b");
		if (fout == NULL){
			x264log("can not open file %s\n", output_filename);
		}
		else{
			x264log("out file is %s\n", output_filename);		
		}
		

		X264Context ctx;
		ctx.width = width;
		ctx.height = height;
		ctx.fps = fps;

		X264_init(&ctx);

		//read a frame
		int i = 200;
		while(i--){
			//read a frame
			int error = 0;
			fread(frame.plane[0], frame.width*frame.height, 1, fin);
			
			fread(frame.plane[1], frame.width*frame.height/4, 1, fin);
			fread(frame.plane[2], frame.width*frame.height/4, 1, fin);
			
			AVPacket pkt = {NULL, 0};
			//encode
			X264_frame(&ctx, &pkt, &frame, NULL);

			//out
			//if (pkt.size > 0){				
#ifdef TEST_FRAME_SIZE
				//fwrite(&pkt.size, sizeof(pkt.size), 1, fout);
				x264log("frame %d size: %d\n", i, pkt.size);
#endif				
				if(fwrite(pkt.data, pkt.size, 1, fout)){
					//
				}
				else{
					break;
				}
			//}
		}

		X264_close(&ctx);

	}while(0);

	for (int i=0; i<3; i++){
		free(frame.plane[i]);
	}

	if (fin){
		fclose(fin);
	}
	if (fout){
		fclose(fout);
	}
#ifdef WINCE
	if (logfile){
		fclose(logfile);
	}
#endif
}
