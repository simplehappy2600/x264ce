/*****************************************************************************
 * x264: top-level x264cli functions
 *****************************************************************************
 * Copyright (C) 2003-2011 x264 project
 *
 * Authors: Loren Merritt <lorenm@u.washington.edu>
 *          Laurent Aimar <fenrir@via.ecp.fr>
 *          Steven Walters <kemuri9@gmail.com>
 *          Jason Garrett-Glaser <darkshikari@gmail.com>
 *          Kieran Kunhya <kieran@kunhya.com>
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
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@x264.com.
 *****************************************************************************/

//#include <signal.h>
#define _GNU_SOURCE
#include <getopt.h>
#include "common/common.h"
#include "x264cli.h"
#include "input/input.h"
#include "output/output.h"
#include "filters/filters.h"

#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "x264", __VA_ARGS__ )

#ifdef DR400
#define SetConsoleTitle(t)
#include <stdio.h>
#endif

#if 1
/* Ctrl-C handler */
static volatile int b_ctrl_c = 0;
static int          b_exit_on_ctrl_c = 0;
static void sigint_handler( int a )
{
    if( b_exit_on_ctrl_c )
        exit(0);
    b_ctrl_c = 1;
}
#endif

static char UNUSED originalCTitle[200] = "";

typedef struct {
    int b_progress;
    int i_seek;
    hnd_t hin;
    hnd_t hout;
    FILE *qpfile;
    FILE *tcfile_out;
    double timebase_convert_multiplier;
    int i_pulldown;
} cli_opt_t;

/* file i/o operation structs */
cli_input_t cli_input;
static cli_output_t cli_output;

/* video filter operation struct */
#ifdef DR400
extern cli_vid_filter_t source_filter;
static cli_vid_filter_t filter;
#else
static cli_vid_filter_t filter;
#endif

static const char * const demuxer_names[] =
{
    "auto",
    "raw",
    "y4m",
#if HAVE_AVS
    "avs",
#endif
#if HAVE_LAVF
    "lavf",
#endif
#if HAVE_FFMS
    "ffms",
#endif
    0
};

static const char * const muxer_names[] =
{
    "auto",
    "raw",
    "mkv",
    "flv",
#if HAVE_GPAC
    "mp4",
#endif
    0
};

static const char * const pulldown_names[] = { "none", "22", "32", "64", "double", "triple", "euro", 0 };
static const char * const log_level_names[] = { "none", "error", "warning", "info", "debug", 0 };
static const char * const output_csp_names[] =
{
#if !X264_CHROMA_FORMAT || X264_CHROMA_FORMAT == X264_CSP_I420
    "i420",
#endif
#if !X264_CHROMA_FORMAT || X264_CHROMA_FORMAT == X264_CSP_I422
    "i422",
#endif
#if !X264_CHROMA_FORMAT || X264_CHROMA_FORMAT == X264_CSP_I444
    "i444", "rgb",
#endif
    0
};

static const char * const range_names[] = { "auto", "tv", "pc", 0 };

typedef struct
{
    int mod;
    uint8_t pattern[24];
    float fps_factor;
} cli_pulldown_t;

enum pulldown_type_e
{
    X264_PULLDOWN_22 = 1,
    X264_PULLDOWN_32,
    X264_PULLDOWN_64,
    X264_PULLDOWN_DOUBLE,
    X264_PULLDOWN_TRIPLE,
    X264_PULLDOWN_EURO
};

#define TB  PIC_STRUCT_TOP_BOTTOM
#define BT  PIC_STRUCT_BOTTOM_TOP
#define TBT PIC_STRUCT_TOP_BOTTOM_TOP
#define BTB PIC_STRUCT_BOTTOM_TOP_BOTTOM

static const cli_pulldown_t pulldown_values[] =
{
    [X264_PULLDOWN_22]     = {1,  {TB},                                   1.0},
    [X264_PULLDOWN_32]     = {4,  {TBT, BT, BTB, TB},                     1.25},
    [X264_PULLDOWN_64]     = {2,  {PIC_STRUCT_DOUBLE, PIC_STRUCT_TRIPLE}, 1.0},
    [X264_PULLDOWN_DOUBLE] = {1,  {PIC_STRUCT_DOUBLE},                    2.0},
    [X264_PULLDOWN_TRIPLE] = {1,  {PIC_STRUCT_TRIPLE},                    3.0},
    [X264_PULLDOWN_EURO]   = {24, {TBT, BT, BT, BT, BT, BT, BT, BT, BT, BT, BT, BT,
                                   BTB, TB, TB, TB, TB, TB, TB, TB, TB, TB, TB, TB}, 25.0/24.0}
};

#undef TB
#undef BT
#undef TBT
#undef BTB

// indexed by pic_struct enum
static const float pulldown_frame_duration[10] = { 0.0, 1, 0.5, 0.5, 1, 1, 1.5, 1.5, 2, 3 };

static void help( x264_param_t *defaults, int longhelp );
static int  parse( int argc, char **argv, x264_param_t *param, cli_opt_t *opt );
static int  encode( x264_param_t *param, cli_opt_t *opt );

#ifdef WINCE
FILE* logfile = NULL;
#endif

/* logging and printing for within the cli system */
static int cli_log_level;
void x264_cli_log( const char *name, int i_level, const char *fmt, ... )
{
    if( i_level > cli_log_level )
        return;
    char *s_level;
    switch( i_level )
    {
        case X264_LOG_ERROR:
            s_level = "error";
            break;
        case X264_LOG_WARNING:
            s_level = "warning";
            break;
        case X264_LOG_INFO:
            s_level = "info";
            break;
        case X264_LOG_DEBUG:
            s_level = "debug";
            break;
        default:
            s_level = "unknown";
            break;
    }
#ifdef WINCE
    fprintf( logfile, "%s [%s]: ", name, s_level );
#else
    fprintf( stderr, "%s [%s]: ", name, s_level );
#endif
    va_list arg;
    va_start( arg, fmt );
#ifdef WINCE
    vfprintf( logfile, fmt, arg );
    fflush(logfile);
#else
    vfprintf( stderr, fmt, arg );
#endif
    va_end( arg );
}

void x264_cli_printf( int i_level, const char *fmt, ... )
{
    if( i_level > cli_log_level )
        return;
    va_list arg;
    va_start( arg, fmt );
#ifdef WINCE
    vfprintf( logfile, fmt, arg );
#else
    vfprintf( stderr, fmt, arg );
#endif
    va_end( arg );
}

int main( int argc, char **argv )
{
    x264_param_t param;
    cli_opt_t opt = {0};
    int ret = 0;
	
#ifdef DR400
	filter = source_filter;
#endif

#ifdef WINCE
	logfile = fopen("Storage Card\\log.txt", "w");
#endif

    //FAIL_IF_ERROR( x264_threading_init(), "unable to initialize threading\n" )

#ifdef _WIN32
    //_setmode(_fileno(stdin), _O_BINARY);
    //_setmode(_fileno(stdout), _O_BINARY);
#endif

    //GetConsoleTitle( originalCTitle, sizeof(originalCTitle) );

    /* Parse command line */
    if( parse( argc, argv, &param, &opt ) < 0 )
        ret = -1;

    /* Restore title; it can be changed by input modules */
//    SetConsoleTitle( originalCTitle );

    /* Control-C handler */
  //  signal( SIGINT, sigint_handler );

    if( !ret )
        ret = encode( &param, &opt );

    /* clean up handles */
    if( filter.free )
        filter.free( opt.hin );
    else if( opt.hin )
        cli_input.close_file( opt.hin );
    if( opt.hout )
        cli_output.close_file( opt.hout, 0, 0 );
    if( opt.tcfile_out )
        fclose( opt.tcfile_out );
    if( opt.qpfile )
        fclose( opt.qpfile );

    //SetConsoleTitle( originalCTitle );

#ifdef WINCE
	fclose(logfile);
#endif

    return ret;
}

static char const *strtable_lookup( const char * const table[], int idx )
{
    int i = 0; while( table[i] ) i++;
    return ( ( idx >= 0 && idx < i ) ? table[ idx ] : "???" );
}

static char *stringify_names( char *buf, const char * const names[] )
{
    int i = 0;
    char *p = buf;
    for( p[0] = 0; names[i]; i++ )
    {
        p += sprintf( p, "%s", names[i] );
        if( names[i+1] )
            p += sprintf( p, ", " );
    }
    return buf;
}

static void print_csp_names( int longhelp )
{
    if( longhelp < 2 )
        return;
#   define INDENT "                                "
    printf( "                              - valid csps for `raw' demuxer:\n" );
    printf( INDENT );
    for( int i = X264_CSP_NONE+1; i < X264_CSP_CLI_MAX; i++ )
    {
        printf( "%s", x264_cli_csps[i].name );
        if( i+1 < X264_CSP_CLI_MAX )
            printf( ", " );
    }
#if HAVE_LAVF
    printf( "\n" );
    printf( "                              - valid csps for `lavf' demuxer:\n" );
    printf( INDENT );
    size_t line_len = strlen( INDENT );
    for( enum PixelFormat i = PIX_FMT_NONE+1; i < PIX_FMT_NB; i++ )
    {
        const char *pfname = av_get_pix_fmt_name( i );
        if( pfname )
        {
            size_t name_len = strlen( pfname );
            if( line_len + name_len > (80 - strlen( ", " )) )
            {
                printf( "\n" INDENT );
                line_len = strlen( INDENT );
            }
            printf( "%s", pfname );
            line_len += name_len;
            if( i+1 < PIX_FMT_NB )
            {
                printf( ", " );
                line_len += 2;
            }
        }
    }
#endif
    printf( "\n" );
}

#if 0
static void help( x264_param_t *defaults, int longhelp )
{
}
#endif

typedef enum
{
    OPT_FRAMES = 256,
    OPT_SEEK,
    OPT_QPFILE,
    OPT_THREAD_INPUT,
    OPT_QUIET,
    OPT_NOPROGRESS,
    OPT_VISUALIZE,
    OPT_LONGHELP,
    OPT_PROFILE,
    OPT_PRESET,
    OPT_TUNE,
    OPT_SLOWFIRSTPASS,
    OPT_FULLHELP,
    OPT_FPS,
    OPT_MUXER,
    OPT_DEMUXER,
    OPT_INDEX,
    OPT_INTERLACED,
    OPT_TCFILE_IN,
    OPT_TCFILE_OUT,
    OPT_TIMEBASE,
    OPT_PULLDOWN,
    OPT_LOG_LEVEL,
    OPT_VIDEO_FILTER,
    OPT_INPUT_FMT,
    OPT_INPUT_RES,
    OPT_INPUT_CSP,
    OPT_INPUT_DEPTH,
    OPT_DTS_COMPRESSION,
    OPT_OUTPUT_CSP,
    OPT_INPUT_RANGE,
    OPT_RANGE
} OptionsOPT;

static char short_options[] = "8A:B:b:f:hI:i:m:o:p:q:r:t:Vvw";
static struct option long_options[] =
{
    { "help",              no_argument, NULL, 'h' },
    { "longhelp",          no_argument, NULL, OPT_LONGHELP },
    { "fullhelp",          no_argument, NULL, OPT_FULLHELP },
    { "version",           no_argument, NULL, 'V' },
    { "profile",     required_argument, NULL, OPT_PROFILE },
    { "preset",      required_argument, NULL, OPT_PRESET },
    { "tune",        required_argument, NULL, OPT_TUNE },
    { "slow-firstpass",    no_argument, NULL, OPT_SLOWFIRSTPASS },
    { "bitrate",     required_argument, NULL, 'B' },
    { "bframes",     required_argument, NULL, 'b' },
    { "b-adapt",     required_argument, NULL, 0 },
    { "no-b-adapt",        no_argument, NULL, 0 },
    { "b-bias",      required_argument, NULL, 0 },
    { "b-pyramid",   required_argument, NULL, 0 },
    { "open-gop",          no_argument, NULL, 0 },
    { "bluray-compat",     no_argument, NULL, 0 },
    { "min-keyint",  required_argument, NULL, 'i' },
    { "keyint",      required_argument, NULL, 'I' },
    { "intra-refresh",     no_argument, NULL, 0 },
    { "scenecut",    required_argument, NULL, 0 },
    { "no-scenecut",       no_argument, NULL, 0 },
    { "nf",                no_argument, NULL, 0 },
    { "no-deblock",        no_argument, NULL, 0 },
    { "filter",      required_argument, NULL, 0 },
    { "deblock",     required_argument, NULL, 'f' },
    { "interlaced",        no_argument, NULL, OPT_INTERLACED },
    { "tff",               no_argument, NULL, OPT_INTERLACED },
    { "bff",               no_argument, NULL, OPT_INTERLACED },
    { "no-interlaced",     no_argument, NULL, OPT_INTERLACED },
    { "constrained-intra", no_argument, NULL, 0 },
    { "cabac",             no_argument, NULL, 0 },
    { "no-cabac",          no_argument, NULL, 0 },
    { "qp",          required_argument, NULL, 'q' },
    { "qpmin",       required_argument, NULL, 0 },
    { "qpmax",       required_argument, NULL, 0 },
    { "qpstep",      required_argument, NULL, 0 },
    { "crf",         required_argument, NULL, 0 },
    { "rc-lookahead",required_argument, NULL, 0 },
    { "ref",         required_argument, NULL, 'r' },
    { "asm",         required_argument, NULL, 0 },
    { "no-asm",            no_argument, NULL, 0 },
    { "sar",         required_argument, NULL, 0 },
    { "fps",         required_argument, NULL, OPT_FPS },
    { "frames",      required_argument, NULL, OPT_FRAMES },
    { "seek",        required_argument, NULL, OPT_SEEK },
    { "output",      required_argument, NULL, 'o' },
    { "muxer",       required_argument, NULL, OPT_MUXER },
    { "demuxer",     required_argument, NULL, OPT_DEMUXER },
    { "stdout",      required_argument, NULL, OPT_MUXER },
    { "stdin",       required_argument, NULL, OPT_DEMUXER },
    { "index",       required_argument, NULL, OPT_INDEX },
    { "analyse",     required_argument, NULL, 0 },
    { "partitions",  required_argument, NULL, 'A' },
    { "direct",      required_argument, NULL, 0 },
    { "weightb",           no_argument, NULL, 'w' },
    { "no-weightb",        no_argument, NULL, 0 },
    { "weightp",     required_argument, NULL, 0 },
    { "me",          required_argument, NULL, 0 },
    { "merange",     required_argument, NULL, 0 },
    { "mvrange",     required_argument, NULL, 0 },
    { "mvrange-thread", required_argument, NULL, 0 },
    { "subme",       required_argument, NULL, 'm' },
    { "psy-rd",      required_argument, NULL, 0 },
    { "no-psy",            no_argument, NULL, 0 },
    { "psy",               no_argument, NULL, 0 },
    { "mixed-refs",        no_argument, NULL, 0 },
    { "no-mixed-refs",     no_argument, NULL, 0 },
    { "no-chroma-me",      no_argument, NULL, 0 },
    { "8x8dct",            no_argument, NULL, '8' },
    { "no-8x8dct",         no_argument, NULL, 0 },
    { "trellis",     required_argument, NULL, 't' },
    { "fast-pskip",        no_argument, NULL, 0 },
    { "no-fast-pskip",     no_argument, NULL, 0 },
    { "no-dct-decimate",   no_argument, NULL, 0 },
    { "aq-strength", required_argument, NULL, 0 },
    { "aq-mode",     required_argument, NULL, 0 },
    { "deadzone-inter", required_argument, NULL, 0 },
    { "deadzone-intra", required_argument, NULL, 0 },
    { "level",       required_argument, NULL, 0 },
    { "ratetol",     required_argument, NULL, 0 },
    { "vbv-maxrate", required_argument, NULL, 0 },
    { "vbv-bufsize", required_argument, NULL, 0 },
    { "vbv-init",    required_argument, NULL, 0 },
    { "crf-max",     required_argument, NULL, 0 },
    { "ipratio",     required_argument, NULL, 0 },
    { "pbratio",     required_argument, NULL, 0 },
    { "chroma-qp-offset", required_argument, NULL, 0 },
    { "pass",        required_argument, NULL, 'p' },
    { "stats",       required_argument, NULL, 0 },
    { "qcomp",       required_argument, NULL, 0 },
    { "mbtree",            no_argument, NULL, 0 },
    { "no-mbtree",         no_argument, NULL, 0 },
    { "qblur",       required_argument, NULL, 0 },
    { "cplxblur",    required_argument, NULL, 0 },
    { "zones",       required_argument, NULL, 0 },
    { "qpfile",      required_argument, NULL, OPT_QPFILE },
    { "threads",     required_argument, NULL, 0 },
    { "sliced-threads",    no_argument, NULL, 0 },
    { "no-sliced-threads", no_argument, NULL, 0 },
    { "slice-max-size",    required_argument, NULL, 0 },
    { "slice-max-mbs",     required_argument, NULL, 0 },
    { "slices",            required_argument, NULL, 0 },
    { "thread-input",      no_argument, NULL, OPT_THREAD_INPUT },
    { "sync-lookahead",    required_argument, NULL, 0 },
    { "non-deterministic", no_argument, NULL, 0 },
    { "cpu-independent",   no_argument, NULL, 0 },
    { "psnr",              no_argument, NULL, 0 },
    { "ssim",              no_argument, NULL, 0 },
    { "quiet",             no_argument, NULL, OPT_QUIET },
    { "verbose",           no_argument, NULL, 'v' },
    { "log-level",   required_argument, NULL, OPT_LOG_LEVEL },
    { "no-progress",       no_argument, NULL, OPT_NOPROGRESS },
    { "visualize",         no_argument, NULL, OPT_VISUALIZE },
    { "dump-yuv",    required_argument, NULL, 0 },
    { "sps-id",      required_argument, NULL, 0 },
    { "aud",               no_argument, NULL, 0 },
    { "nr",          required_argument, NULL, 0 },
    { "cqm",         required_argument, NULL, 0 },
    { "cqmfile",     required_argument, NULL, 0 },
    { "cqm4",        required_argument, NULL, 0 },
    { "cqm4i",       required_argument, NULL, 0 },
    { "cqm4iy",      required_argument, NULL, 0 },
    { "cqm4ic",      required_argument, NULL, 0 },
    { "cqm4p",       required_argument, NULL, 0 },
    { "cqm4py",      required_argument, NULL, 0 },
    { "cqm4pc",      required_argument, NULL, 0 },
    { "cqm8",        required_argument, NULL, 0 },
    { "cqm8i",       required_argument, NULL, 0 },
    { "cqm8p",       required_argument, NULL, 0 },
    { "overscan",    required_argument, NULL, 0 },
    { "videoformat", required_argument, NULL, 0 },
    { "range",       required_argument, NULL, OPT_RANGE },
    { "colorprim",   required_argument, NULL, 0 },
    { "transfer",    required_argument, NULL, 0 },
    { "colormatrix", required_argument, NULL, 0 },
    { "chromaloc",   required_argument, NULL, 0 },
    { "force-cfr",         no_argument, NULL, 0 },
    { "tcfile-in",   required_argument, NULL, OPT_TCFILE_IN },
    { "tcfile-out",  required_argument, NULL, OPT_TCFILE_OUT },
    { "timebase",    required_argument, NULL, OPT_TIMEBASE },
    { "pic-struct",        no_argument, NULL, 0 },
    { "crop-rect",   required_argument, NULL, 0 },
    { "nal-hrd",     required_argument, NULL, 0 },
    { "pulldown",    required_argument, NULL, OPT_PULLDOWN },
    { "fake-interlaced",   no_argument, NULL, 0 },
    { "frame-packing",     required_argument, NULL, 0 },
    { "vf",          required_argument, NULL, OPT_VIDEO_FILTER },
    { "video-filter", required_argument, NULL, OPT_VIDEO_FILTER },
    { "input-fmt",   required_argument, NULL, OPT_INPUT_FMT },
    { "input-res",   required_argument, NULL, OPT_INPUT_RES },
    { "input-csp",   required_argument, NULL, OPT_INPUT_CSP },
    { "input-depth", required_argument, NULL, OPT_INPUT_DEPTH },
    { "dts-compress",      no_argument, NULL, OPT_DTS_COMPRESSION },
    { "output-csp",  required_argument, NULL, OPT_OUTPUT_CSP },
    { "input-range", required_argument, NULL, OPT_INPUT_RANGE },
    {0, 0, 0, 0}
};

static int select_output( const char *muxer, char *filename, x264_param_t *param )
{
    const char *ext = get_filename_extension( filename );
    if( !strcmp( filename, "-" ) || strcasecmp( muxer, "auto" ) )
        ext = muxer;

	cli_output = raw_output;
#if 0
    if( !strcasecmp( ext, "mp4" ) )
    {
#if HAVE_GPAC
        cli_output = mp4_output;
        param->b_annexb = 0;
        param->b_repeat_headers = 0;
        if( param->i_nal_hrd == X264_NAL_HRD_CBR )
        {
            x264_cli_log( "x264", X264_LOG_WARNING, "cbr nal-hrd is not compatible with mp4\n" );
            param->i_nal_hrd = X264_NAL_HRD_VBR;
        }
#else
        x264_cli_log( "x264", X264_LOG_ERROR, "not compiled with MP4 output support\n" );
        return -1;
#endif
    }
    else if( !strcasecmp( ext, "mkv" ) )
    {
        cli_output = mkv_output;
        param->b_annexb = 0;
        param->b_repeat_headers = 0;
    }
    else if( !strcasecmp( ext, "flv" ) )
    {
        cli_output = flv_output;
        param->b_annexb = 0;
        param->b_repeat_headers = 0;
    }
    else
        cli_output = raw_output;
#endif
    return 0;
}

static int select_input( const char *demuxer, char *used_demuxer, char *filename,
                         hnd_t *p_handle, video_info_t *info, cli_input_opt_t *opt )
{
    int b_auto = !strcasecmp( demuxer, "auto" );
    const char *ext = b_auto ? get_filename_extension( filename ) : "";
    int b_regular = strcmp( filename, "-" );
    if( !b_regular && b_auto )
        ext = "raw";
    b_regular = b_regular && x264_is_regular_file_path( filename );
    if( b_regular )
    {
        FILE *f = fopen( filename, "r" );
        if( f )
        {
            b_regular = x264_is_regular_file( f );
            fclose( f );
        }
    }
    const char *module = b_auto ? ext : demuxer;
	
	cli_input = raw_input;
#if 0
    if( !strcasecmp( module, "avs" ) || !strcasecmp( ext, "d2v" ) || !strcasecmp( ext, "dga" ) )
    {
#if HAVE_AVS
        cli_input = avs_input;
        module = "avs";
#else
        x264_cli_log( "x264", X264_LOG_ERROR, "not compiled with AVS input support\n" );
        return -1;
#endif
    }
    else if( !strcasecmp( module, "y4m" ) )
        cli_input = y4m_input;
    else if( !strcasecmp( module, "raw" ) || !strcasecmp( ext, "yuv" ) )
        cli_input = raw_input;
    else
    {
#if HAVE_FFMS
        if( b_regular && (b_auto || !strcasecmp( demuxer, "ffms" )) &&
            !ffms_input.open_file( filename, p_handle, info, opt ) )
        {
            module = "ffms";
            b_auto = 0;
            cli_input = ffms_input;
        }
#endif
#if HAVE_LAVF
        if( (b_auto || !strcasecmp( demuxer, "lavf" )) &&
            !lavf_input.open_file( filename, p_handle, info, opt ) )
        {
            module = "lavf";
            b_auto = 0;
            cli_input = lavf_input;
        }
#endif
#if HAVE_AVS
        if( b_regular && (b_auto || !strcasecmp( demuxer, "avs" )) &&
            !avs_input.open_file( filename, p_handle, info, opt ) )
        {
            module = "avs";
            b_auto = 0;
            cli_input = avs_input;
        }
#endif
        if( b_auto && !raw_input.open_file( filename, p_handle, info, opt ) )
        {
            module = "raw";
            b_auto = 0;
            cli_input = raw_input;
        }

        FAIL_IF_ERROR( !(*p_handle), "could not open input file `%s' via any method!\n", filename )
    }
#endif
    strcpy( used_demuxer, module );

    return 0;
}

#if 0
static int init_vid_filters( char *sequence, hnd_t *handle, video_info_t *info, x264_param_t *param, int output_csp )
{
    x264_register_vid_filters();

    /* intialize baseline filters */
    if( x264_init_vid_filter( "source", handle, &filter, info, param, NULL ) ) /* wrap demuxer into a filter */
        return -1;
    if( x264_init_vid_filter( "resize", handle, &filter, info, param, "normcsp" ) ) /* normalize csps to be of a known/supported format */
        return -1;
    if( x264_init_vid_filter( "fix_vfr_pts", handle, &filter, info, param, NULL ) ) /* fix vfr pts */
        return -1;

    /* parse filter chain */
    for( char *p = sequence; p && *p; )
    {
        int tok_len = strcspn( p, "/" );
        int p_len = strlen( p );
        p[tok_len] = 0;
        int name_len = strcspn( p, ":" );
        p[name_len] = 0;
        name_len += name_len != tok_len;
        if( x264_init_vid_filter( p, handle, &filter, info, param, p + name_len ) )
            return -1;
        p += X264_MIN( tok_len+1, p_len );
    }

    /* force end result resolution */
    if( !param->i_width && !param->i_height )
    {
        param->i_height = info->height;
        param->i_width  = info->width;
    }
    /* force the output csp to what the user specified (or the default) */
    param->i_csp = info->csp;
    int csp = info->csp & X264_CSP_MASK;
    if( output_csp == X264_CSP_I420 && (csp < X264_CSP_I420 || csp > X264_CSP_NV12) )
        param->i_csp = X264_CSP_I420;
    else if( output_csp == X264_CSP_I422 && (csp < X264_CSP_I422 || csp > X264_CSP_NV16) )
        param->i_csp = X264_CSP_I422;
    else if( output_csp == X264_CSP_I444 && (csp < X264_CSP_I444 || csp > X264_CSP_YV24) )
        param->i_csp = X264_CSP_I444;
    else if( output_csp == X264_CSP_RGB && (csp < X264_CSP_BGR || csp > X264_CSP_RGB) )
        param->i_csp = X264_CSP_RGB;
    param->i_csp |= info->csp & X264_CSP_HIGH_DEPTH;
    /* if the output range is not forced, assign it to the input one now */
    if( param->vui.b_fullrange == RANGE_AUTO )
        param->vui.b_fullrange = info->fullrange;

    if( x264_init_vid_filter( "resize", handle, &filter, info, param, NULL ) )
        return -1;

    char args[20];
    sprintf( args, "bit_depth=%d", x264_bit_depth );

    if( x264_init_vid_filter( "depth", handle, &filter, info, param, args ) )
        return -1;

    return 0;
}
#endif

static int parse_enum_name( const char *arg, const char * const *names, const char **dst )
{
    for( int i = 0; names[i]; i++ )
        if( !strcasecmp( arg, names[i] ) )
        {
            *dst = names[i];
            return 0;
        }
    return -1;
}

static int parse_enum_value( const char *arg, const char * const *names, int *dst )
{
    for( int i = 0; names[i]; i++ )
        if( !strcasecmp( arg, names[i] ) )
        {
            *dst = i;
            return 0;
        }
    return -1;
}

static int parse( int argc, char **argv, x264_param_t *param, cli_opt_t *opt )
{
    char *input_filename = NULL;
    const char *demuxer = demuxer_names[0];
    char *output_filename = NULL;
    const char *muxer = muxer_names[0];
    char *tcfile_name = NULL;
    x264_param_t defaults;
    char *profile = NULL;
    char *vid_filters = NULL;
    int b_thread_input = 0;
    int b_turbo = 1;
    int b_user_ref = 0;
    int b_user_fps = 0;
    int b_user_interlaced = 0;
    cli_input_opt_t input_opt;
    cli_output_opt_t output_opt;
    char *preset = NULL;
    char *tune = NULL;

    x264_param_default( &defaults );
    cli_log_level = defaults.i_log_level;

    memset( &input_opt, 0, sizeof(cli_input_opt_t) );
    memset( &output_opt, 0, sizeof(cli_output_opt_t) );
    input_opt.bit_depth = 8;
    input_opt.input_range = input_opt.output_range = param->vui.b_fullrange = RANGE_AUTO;
    int output_csp = defaults.i_csp;
    opt->b_progress = 1;

    /* Presets are applied before all other options. */
#if 0
    for( optind = 0;; )
    {
        int c = getopt_long( argc, argv, short_options, long_options, NULL );
        if( c == -1 )
            break;
        if( c == OPT_PRESET )
            preset = optarg;
        if( c == OPT_TUNE )
            tune = optarg;
        else if( c == '?' )
            return -1;
    }
#endif

#if 0
    if( preset && !strcasecmp( preset, "placebo" ) )
        b_turbo = 0;
#endif

    if( x264_param_default_preset( param, preset, tune ) < 0 )
        return -1;

    /*i Parse command line options */
#ifdef WINCE
	input_opt.resolution="176x144";
	output_filename = "Storage Card\\out.264";
	input_filename = "Storage Card\\akiyo_qcif.yuv";
#else
	input_opt.resolution="176x144";
	output_filename = "../seq/akiyo_qcif.264";
	input_filename = "../seq/akiyo_qcif.yuv";
#endif

#if 0
    for( optind = 0;; )
    {
        int b_error = 0;
        int long_options_index = -1;

        int c = getopt_long( argc, argv, short_options, long_options, &long_options_index );

        if( c == -1 )
        {
            break;
        }

        switch( c )
        {
            case OPT_FRAMES:
                param->i_frame_total = X264_MAX( atoi( optarg ), 0 );
                break;
            case OPT_SEEK:
                opt->i_seek = X264_MAX( atoi( optarg ), 0 );
                break;
            case 'o':
                output_filename = optarg;
                break;
            case OPT_MUXER:
                FAIL_IF_ERROR( parse_enum_name( optarg, muxer_names, &muxer ), "Unknown muxer `%s'\n", optarg )
                break;
            case OPT_DEMUXER:
                FAIL_IF_ERROR( parse_enum_name( optarg, demuxer_names, &demuxer ), "Unknown demuxer `%s'\n", optarg )
                break;
            case OPT_INDEX:
                input_opt.index_file = optarg;
                break;
            case OPT_QPFILE:
                opt->qpfile = fopen( optarg, "rb" );
                FAIL_IF_ERROR( !opt->qpfile, "can't open qpfile `%s'\n", optarg )
                if( !x264_is_regular_file( opt->qpfile ) )
                {
                    x264_cli_log( "x264", X264_LOG_ERROR, "qpfile incompatible with non-regular file `%s'\n", optarg );
                    fclose( opt->qpfile );
                    return -1;
                }
                break;
            case OPT_THREAD_INPUT:
                b_thread_input = 1;
                break;
            case OPT_QUIET:
                cli_log_level = param->i_log_level = X264_LOG_NONE;
                break;
            case 'v':
                cli_log_level = param->i_log_level = X264_LOG_DEBUG;
                break;
            case OPT_LOG_LEVEL:
                if( !parse_enum_value( optarg, log_level_names, &cli_log_level ) )
                    cli_log_level += X264_LOG_NONE;
                else
                    cli_log_level = atoi( optarg );
                param->i_log_level = cli_log_level;
                break;
            case OPT_NOPROGRESS:
                opt->b_progress = 0;
                break;
            case OPT_VISUALIZE:
#if HAVE_VISUALIZE
                param->b_visualize = 1;
                b_exit_on_ctrl_c = 1;
#else
                x264_cli_log( "x264", X264_LOG_WARNING, "not compiled with visualization support\n" );
#endif
                break;
            case OPT_TUNE:
            case OPT_PRESET:
                break;
            case OPT_PROFILE:
                profile = optarg;
                break;
            case OPT_SLOWFIRSTPASS:
                b_turbo = 0;
                break;
            case 'r':
                b_user_ref = 1;
                goto generic_option;
            case OPT_FPS:
                b_user_fps = 1;
                param->b_vfr_input = 0;
                goto generic_option;
            case OPT_INTERLACED:
                b_user_interlaced = 1;
                goto generic_option;
            case OPT_TCFILE_IN:
                tcfile_name = optarg;
                break;
            case OPT_TCFILE_OUT:
                opt->tcfile_out = fopen( optarg, "wb" );
                FAIL_IF_ERROR( !opt->tcfile_out, "can't open `%s'\n", optarg )
                break;
            case OPT_TIMEBASE:
                input_opt.timebase = optarg;
                break;
            case OPT_PULLDOWN:
                FAIL_IF_ERROR( parse_enum_value( optarg, pulldown_names, &opt->i_pulldown ), "Unknown pulldown `%s'\n", optarg )
                break;
            case OPT_VIDEO_FILTER:
                vid_filters = optarg;
                break;
            case OPT_INPUT_FMT:
                input_opt.format = optarg;
                break;
            case OPT_INPUT_RES:
                input_opt.resolution = optarg;
                break;
            case OPT_INPUT_CSP:
                input_opt.colorspace = optarg;
                break;
            case OPT_INPUT_DEPTH:
                input_opt.bit_depth = atoi( optarg );
                break;
            case OPT_DTS_COMPRESSION:
                output_opt.use_dts_compress = 1;
                break;
            case OPT_OUTPUT_CSP:
                FAIL_IF_ERROR( parse_enum_value( optarg, output_csp_names, &output_csp ), "Unknown output csp `%s'\n", optarg )
                // correct the parsed value to the libx264 csp value
#if X264_CHROMA_FORMAT
                static const uint8_t output_csp_fix[] = { X264_CHROMA_FORMAT, X264_CSP_RGB };
#else
                static const uint8_t output_csp_fix[] = { X264_CSP_I420, X264_CSP_I422, X264_CSP_I444, X264_CSP_RGB };
#endif
                param->i_csp = output_csp = output_csp_fix[output_csp];
                break;
            case OPT_INPUT_RANGE:
                FAIL_IF_ERROR( parse_enum_value( optarg, range_names, &input_opt.input_range ), "Unknown input range `%s'\n", optarg )
                input_opt.input_range += RANGE_AUTO;
                break;
            case OPT_RANGE:
                FAIL_IF_ERROR( parse_enum_value( optarg, range_names, &param->vui.b_fullrange ), "Unknown range `%s'\n", optarg );
                input_opt.output_range = param->vui.b_fullrange += RANGE_AUTO;
                break;
            default:
generic_option:
            {
                if( long_options_index < 0 )
                {
                    for( int i = 0; long_options[i].name; i++ )
                        if( long_options[i].val == c )
                        {
                            long_options_index = i;
                            break;
                        }
                    if( long_options_index < 0 )
                    {
                        /* getopt_long already printed an error message */
                        return -1;
                    }
                }

                b_error |= x264_param_parse( param, long_options[long_options_index].name, optarg );
            }
        }

        if( b_error )
        {
            const char *name = long_options_index > 0 ? long_options[long_options_index].name : argv[optind-2];
            x264_cli_log( "x264", X264_LOG_ERROR, "invalid argument: %s = %s\n", name, optarg );
            return -1;
        }
    }
#endif
    /* If first pass mode is used, apply faster settings. */
    if( b_turbo )
        x264_param_apply_fastfirstpass( param );

    /* Apply profile restrictions. */
    if( x264_param_apply_profile( param, profile ) < 0 )
        return -1;

    /* Get the file name */
#if 0
    FAIL_IF_ERROR( optind > argc - 1 || !output_filename, "No %s file. Run x264 --help for a list of options.\n",
                   optind > argc - 1 ? "input" : "output" )
#endif

    if( select_output( muxer, output_filename, param ) )
        return -1;
    FAIL_IF_ERROR( cli_output.open_file( output_filename, &opt->hout, &output_opt ), "could not open output file `%s'\n", output_filename )

    //input_filename = argv[optind++];
    video_info_t info = {0};
    char demuxername[5];

    /* set info flags to be overwritten by demuxer as necessary. */
    info.csp        = param->i_csp;
    info.fps_num    = param->i_fps_num;
    info.fps_den    = param->i_fps_den;
    info.fullrange  = input_opt.input_range == RANGE_PC;
    info.interlaced = param->b_interlaced;
    info.sar_width  = param->vui.i_sar_width;
    info.sar_height = param->vui.i_sar_height;
    info.tff        = param->b_tff;
    info.vfr        = param->b_vfr_input;

    input_opt.seek = opt->i_seek;
    input_opt.progress = opt->b_progress;
    input_opt.output_csp = output_csp;

    if( select_input( demuxer, demuxername, input_filename, &opt->hin, &info, &input_opt ) )
        return -1;

    FAIL_IF_ERROR( !opt->hin && cli_input.open_file( input_filename, &opt->hin, &info, &input_opt ),
                   "could not open input file `%s'\n", input_filename )

    x264_reduce_fraction( &info.sar_width, &info.sar_height );
    x264_reduce_fraction( &info.fps_num, &info.fps_den );
    x264_cli_log( demuxername, X264_LOG_INFO, "%dx%d%c %d:%d @ %d/%d fps (%cfr)\n", info.width,
                  info.height, info.interlaced ? 'i' : 'p', info.sar_width, info.sar_height,
                  info.fps_num, info.fps_den, info.vfr ? 'v' : 'c' );
#if 0
    if( tcfile_name )
    {
        FAIL_IF_ERROR( b_user_fps, "--fps + --tcfile-in is incompatible.\n" )
        FAIL_IF_ERROR( timecode_input.open_file( tcfile_name, &opt->hin, &info, &input_opt ), "timecode input failed\n" )
        cli_input = timecode_input;

    }
    else FAIL_IF_ERROR( !info.vfr && input_opt.timebase, "--timebase is incompatible with cfr input\n" )
#endif

    /* init threaded input while the information about the input video is unaltered by filtering */
#if 0
#if HAVE_THREAD
    if( info.thread_safe && (b_thread_input || param->i_threads > 1
        || (param->i_threads == X264_THREADS_AUTO && x264_cpu_num_processors() > 1)) )
    {
        if( thread_input.open_file( NULL, &opt->hin, &info, NULL ) )
        {
            fprintf( stderr, "x264 [error]: threaded input failed\n" );
            return -1;
        }
        cli_input = thread_input;
    }
#endif
#endif

    /* override detected values by those specified by the user */
#if 0
    if( param->vui.i_sar_width && param->vui.i_sar_height )
    {
        info.sar_width  = param->vui.i_sar_width;
        info.sar_height = param->vui.i_sar_height;
    }
#endif
#if 0
    if( b_user_fps )
    {
        info.fps_num = param->i_fps_num;
        info.fps_den = param->i_fps_den;
    }
#endif
    if( !info.vfr )
    {
        info.timebase_num = info.fps_den;
        info.timebase_den = info.fps_num;
    }
#if 0
    if( !tcfile_name && input_opt.timebase )
    {
        uint64_t i_user_timebase_num;
        uint64_t i_user_timebase_den;
        int ret = sscanf( input_opt.timebase, "%"SCNu64"/%"SCNu64, &i_user_timebase_num, &i_user_timebase_den );
        FAIL_IF_ERROR( !ret, "invalid argument: timebase = %s\n", input_opt.timebase )
        else if( ret == 1 )
        {
            i_user_timebase_num = info.timebase_num;
            i_user_timebase_den = strtoul( input_opt.timebase, NULL, 10 );
        }
        FAIL_IF_ERROR( i_user_timebase_num > UINT32_MAX || i_user_timebase_den > UINT32_MAX,
                       "timebase you specified exceeds H.264 maximum\n" )
        opt->timebase_convert_multiplier = ((double)i_user_timebase_den / info.timebase_den)
                                         * ((double)info.timebase_num / i_user_timebase_num);
        info.timebase_num = i_user_timebase_num;
        info.timebase_den = i_user_timebase_den;
        info.vfr = 1;
    }
#endif
#if 0
    if( b_user_interlaced )
    {
        info.interlaced = param->b_interlaced;
        info.tff = param->b_tff;
    }
#endif
#if 0
    if( input_opt.input_range != RANGE_AUTO )
        info.fullrange = input_opt.input_range;
#endif

#if 0
    if( init_vid_filters( vid_filters, &opt->hin, &info, param, output_csp ) )
        return -1;
#endif
//static int init( hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info, x264_param_t *param, char *opt_string )	
	source_filter.init(&opt->hin, &filter, &info, &param, output_csp);
    param->i_height = info.height;
    param->i_width  = info.width;
    /* force the output csp to what the user specified (or the default) */
    param->i_csp = info.csp;
    int csp = info.csp & X264_CSP_MASK;
    if( output_csp == X264_CSP_I420 && (csp < X264_CSP_I420 || csp > X264_CSP_NV12) )
        param->i_csp = X264_CSP_I420;
    param->i_csp |= info.csp & X264_CSP_HIGH_DEPTH;
    /* if the output range is not forced, assign it to the input one now */
    if( param->vui.b_fullrange == RANGE_AUTO )
        param->vui.b_fullrange = info.fullrange;    

    /* set param flags from the post-filtered video */
    param->b_vfr_input = info.vfr;
    param->i_fps_num = info.fps_num;
    param->i_fps_den = info.fps_den;
    param->i_timebase_num = info.timebase_num;
    param->i_timebase_den = info.timebase_den;
    param->vui.i_sar_width  = info.sar_width;
    param->vui.i_sar_height = info.sar_height;

    info.num_frames = X264_MAX( info.num_frames - opt->i_seek, 0 );
    if( (!info.num_frames || param->i_frame_total < info.num_frames)
        && param->i_frame_total > 0 )
        info.num_frames = param->i_frame_total;
    param->i_frame_total = info.num_frames;
#if 0
    if( !b_user_interlaced && info.interlaced )
    {
#if HAVE_INTERLACED
        x264_cli_log( "x264", X264_LOG_WARNING, "input appears to be interlaced, enabling %cff interlaced mode.\n"
                      "                If you want otherwise, use --no-interlaced or --%cff\n",
                      info.tff ? 't' : 'b', info.tff ? 'b' : 't' );
        param->b_interlaced = 1;
        param->b_tff = !!info.tff;
#else
        x264_cli_log( "x264", X264_LOG_WARNING, "input appears to be interlaced, but not compiled with interlaced support\n" );
#endif
    }
#endif
    /* if the user never specified the output range and the input is now rgb, default it to pc */
    csp = param->i_csp & X264_CSP_MASK;
#if 0
    if( csp >= X264_CSP_BGR && csp <= X264_CSP_RGB )
    {
        if( input_opt.output_range == RANGE_AUTO )
            param->vui.b_fullrange = RANGE_PC;
        /* otherwise fail if they specified tv */
        FAIL_IF_ERROR( !param->vui.b_fullrange, "RGB must be PC range" )
    }
#endif
    /* Automatically reduce reference frame count to match the user's target level
     * if the user didn't explicitly set a reference frame count. */
    if( !b_user_ref )
    {
        int mbs = (((param->i_width)+15)>>4) * (((param->i_height)+15)>>4);
        for( int i = 0; x264_levels[i].level_idc != 0; i++ )
            if( param->i_level_idc == x264_levels[i].level_idc )
            {
                while( mbs * 384 * param->i_frame_reference > x264_levels[i].dpb &&
                       param->i_frame_reference > 1 )
                {
                    param->i_frame_reference--;
                }
                break;
            }
    }


    return 0;
}

#if 0
static void parse_qpfile( cli_opt_t *opt, x264_picture_t *pic, int i_frame )
{
    int num = -1, qp, ret;
    char type;
    uint64_t file_pos;
    while( num < i_frame )
    {
        file_pos = ftell( opt->qpfile );
        qp = -1;
        ret = fscanf( opt->qpfile, "%d %c%*[ \t]%d\n", &num, &type, &qp );
        pic->i_type = X264_TYPE_AUTO;
        pic->i_qpplus1 = X264_QP_AUTO;
        if( num > i_frame || ret == EOF )
        {
            fseek( opt->qpfile, file_pos, SEEK_SET );
            break;
        }
        if( num < i_frame && ret >= 2 )
            continue;
        if( ret == 3 && qp >= 0 )
            pic->i_qpplus1 = qp+1;
        if     ( type == 'I' ) pic->i_type = X264_TYPE_IDR;
        else if( type == 'i' ) pic->i_type = X264_TYPE_I;
        else if( type == 'K' ) pic->i_type = X264_TYPE_KEYFRAME;
        else if( type == 'P' ) pic->i_type = X264_TYPE_P;
        else if( type == 'B' ) pic->i_type = X264_TYPE_BREF;
        else if( type == 'b' ) pic->i_type = X264_TYPE_B;
        else ret = 0;
        if( ret < 2 || qp < -1 || qp > QP_MAX )
        {
            x264_cli_log( "x264", X264_LOG_ERROR, "can't parse qpfile for frame %d\n", i_frame );
            fclose( opt->qpfile );
            opt->qpfile = NULL;
            break;
        }
    }
}
#endif

static int encode_frame( x264_t *h, hnd_t hout, x264_picture_t *pic, int64_t *last_dts )
{
    x264_picture_t pic_out;
    x264_nal_t *nal;
    int i_nal;
    int i_frame_size = 0;

    i_frame_size = x264_encoder_encode( h, &nal, &i_nal, pic, &pic_out );

    FAIL_IF_ERROR( i_frame_size < 0, "x264_encoder_encode failed\n" );

    if( i_frame_size )
    {
        i_frame_size = cli_output.write_frame( hout, nal[0].p_payload, i_frame_size, &pic_out );
        *last_dts = pic_out.i_dts;
    }

    return i_frame_size;
}

static int64_t print_status( int64_t i_start, int64_t i_previous, int i_frame, int i_frame_total, int64_t i_file, x264_param_t *param, int64_t last_ts )
{
    char buf[200];
    int64_t i_time = x264_mdate();
    if( i_previous && i_time - i_previous < UPDATE_INTERVAL )
        return i_previous;
    int64_t i_elapsed = i_time - i_start;
    double fps = i_elapsed > 0 ? i_frame * 1000000. / i_elapsed : 0;
    double bitrate;
    if( last_ts )
        bitrate = (double) i_file * 8 / ( (double) last_ts * 1000 * param->i_timebase_num / param->i_timebase_den );
    else
        bitrate = (double) i_file * 8 / ( (double) 1000 * param->i_fps_den / param->i_fps_num );
    if( i_frame_total )
    {
        int eta = i_elapsed * (i_frame_total - i_frame) / ((int64_t)i_frame * 1000000);
        sprintf( buf, "x264 [%.1f%%] %d/%d frames, %.2f fps, %.2f kb/s, eta %d:%02d:%02d",
                 100. * i_frame / i_frame_total, i_frame, i_frame_total, fps, bitrate,
                 eta/3600, (eta/60)%60, eta%60 );
    }
    else
    {
        sprintf( buf, "x264 %d frames: %.2f fps, %.2f kb/s", i_frame, fps, bitrate );
    }
    fprintf( stderr, "%s  \r", buf+5 );
    SetConsoleTitle( buf );
    fflush( stderr ); // needed in windows
    return i_time;
}

static void convert_cli_to_lib_pic( x264_picture_t *lib, cli_pic_t *cli )
{
    memcpy( lib->img.i_stride, cli->img.stride, sizeof(cli->img.stride) );
    memcpy( lib->img.plane, cli->img.plane, sizeof(cli->img.plane) );
    lib->img.i_plane = cli->img.planes;
    lib->img.i_csp = cli->img.csp;
    lib->i_pts = cli->pts;
}

#define FAIL_IF_ERROR2( cond, ... )\
if( cond )\
{\
    x264_cli_log( "x264", X264_LOG_ERROR, __VA_ARGS__ );\
    retval = -1;\
    goto fail;\
}

static int encode( x264_param_t *param, cli_opt_t *opt )
{
    x264_t *h = NULL;
    x264_picture_t pic;
    cli_pic_t cli_pic;
    const cli_pulldown_t *pulldown = NULL; // shut up gcc

    int     i_frame = 0;
    int     i_frame_output = 0;
    int64_t i_end, i_previous = 0, i_start = 0;
    int64_t i_file = 0;
    int     i_frame_size;
    int64_t last_dts = 0;
    int64_t prev_dts = 0;
    int64_t first_dts = 0;
#   define  MAX_PTS_WARNING 3 /* arbitrary */
    int     pts_warning_cnt = 0;
    int64_t largest_pts = -1;
    int64_t second_largest_pts = -1;
    int64_t ticks_per_frame;
    double  duration;
    double  pulldown_pts = 0;
    int     retval = 0;

    opt->b_progress &= param->i_log_level < X264_LOG_DEBUG;

    /* set up pulldown */
    if( opt->i_pulldown && !param->b_vfr_input )
    {
        param->b_pulldown = 1;
        param->b_pic_struct = 1;
        pulldown = &pulldown_values[opt->i_pulldown];
        param->i_timebase_num = param->i_fps_den;
        FAIL_IF_ERROR2( fmod( param->i_fps_num * pulldown->fps_factor, 1 ),
                        "unsupported framerate for chosen pulldown\n" )
        param->i_timebase_den = param->i_fps_num * pulldown->fps_factor;
    }

    h = x264_encoder_open( param );
    FAIL_IF_ERROR2( !h, "x264_encoder_open failed\n" );

    x264_encoder_parameters( h, param );

    FAIL_IF_ERROR2( cli_output.set_param( opt->hout, param ), "can't set outfile param\n" );

    i_start = x264_mdate();

    /* ticks/frame = ticks/second / frames/second */
    ticks_per_frame = (int64_t)param->i_timebase_den * param->i_fps_den / param->i_timebase_num / param->i_fps_num;
    FAIL_IF_ERROR2( ticks_per_frame < 1 && !param->b_vfr_input, "ticks_per_frame invalid: %"PRId64"\n", ticks_per_frame )
    ticks_per_frame = X264_MAX( ticks_per_frame, 1 );

    if( !param->b_repeat_headers )
    {
        // Write SPS/PPS/SEI
        x264_nal_t *headers;
        int i_nal;

        FAIL_IF_ERROR2( x264_encoder_headers( h, &headers, &i_nal ) < 0, "x264_encoder_headers failed\n" )
        FAIL_IF_ERROR2( (i_file = cli_output.write_headers( opt->hout, headers )) < 0, "error writing headers to output file\n" );
    }

    if( opt->tcfile_out )
        fprintf( opt->tcfile_out, "# timecode format v2\n" );

    /* Encode frames */
    for( ; !b_ctrl_c && (i_frame < param->i_frame_total || !param->i_frame_total); i_frame++ )
    {
        if( filter.get_frame( opt->hin, &cli_pic, i_frame + opt->i_seek ) )
            break;
        x264_picture_init( &pic );
        convert_cli_to_lib_pic( &pic, &cli_pic );

        if( !param->b_vfr_input )
            pic.i_pts = i_frame;

        if( opt->i_pulldown && !param->b_vfr_input )
        {
            pic.i_pic_struct = pulldown->pattern[ i_frame % pulldown->mod ];
            pic.i_pts = (int64_t)( pulldown_pts + 0.5 );
            pulldown_pts += pulldown_frame_duration[pic.i_pic_struct];
        }
        else if( opt->timebase_convert_multiplier )
            pic.i_pts = (int64_t)( pic.i_pts * opt->timebase_convert_multiplier + 0.5 );

        if( pic.i_pts <= largest_pts )
        {
            if( cli_log_level >= X264_LOG_DEBUG || pts_warning_cnt < MAX_PTS_WARNING )
                x264_cli_log( "x264", X264_LOG_WARNING, "non-strictly-monotonic pts at frame %d (%"PRId64" <= %"PRId64")\n",
                             i_frame, pic.i_pts, largest_pts );
            else if( pts_warning_cnt == MAX_PTS_WARNING )
                x264_cli_log( "x264", X264_LOG_WARNING, "too many nonmonotonic pts warnings, suppressing further ones\n" );
            pts_warning_cnt++;
            pic.i_pts = largest_pts + ticks_per_frame;
        }

        second_largest_pts = largest_pts;
        largest_pts = pic.i_pts;
        if( opt->tcfile_out )
            fprintf( opt->tcfile_out, "%.6f\n", pic.i_pts * ((double)param->i_timebase_num / param->i_timebase_den) * 1e3 );

#if 0
        if( opt->qpfile )
            parse_qpfile( opt, &pic, i_frame + opt->i_seek );
#endif

        prev_dts = last_dts;
        i_frame_size = encode_frame( h, opt->hout, &pic, &last_dts );
        if( i_frame_size < 0 )
        {
            b_ctrl_c = 1; /* lie to exit the loop */
            retval = -1;
        }
        else if( i_frame_size )
        {
            i_file += i_frame_size;
            i_frame_output++;
            if( i_frame_output == 1 )
                first_dts = prev_dts = last_dts;
        }

        if( filter.release_frame( opt->hin, &cli_pic, i_frame + opt->i_seek ) )
            break;

        /* update status line (up to 1000 times per input file) */
        if( opt->b_progress && i_frame_output )
            i_previous = print_status( i_start, i_previous, i_frame_output, param->i_frame_total, i_file, param, 2 * last_dts - prev_dts - first_dts );
    }
    /* Flush delayed frames */
    while( !b_ctrl_c && x264_encoder_delayed_frames( h ) )
    {
        prev_dts = last_dts;
        i_frame_size = encode_frame( h, opt->hout, NULL, &last_dts );
        if( i_frame_size < 0 )
        {
            b_ctrl_c = 1; /* lie to exit the loop */
            retval = -1;
        }
        else if( i_frame_size )
        {
            i_file += i_frame_size;
            i_frame_output++;
            if( i_frame_output == 1 )
                first_dts = prev_dts = last_dts;
        }
        if( opt->b_progress && i_frame_output )
            i_previous = print_status( i_start, i_previous, i_frame_output, param->i_frame_total, i_file, param, 2 * last_dts - prev_dts - first_dts );
    }
fail:
    if( pts_warning_cnt >= MAX_PTS_WARNING && cli_log_level < X264_LOG_DEBUG )
        x264_cli_log( "x264", X264_LOG_WARNING, "%d suppressed nonmonotonic pts warnings\n", pts_warning_cnt-MAX_PTS_WARNING );

    /* duration algorithm fails when only 1 frame is output */
    if( i_frame_output == 1 )
        duration = (double)param->i_fps_den / param->i_fps_num;
    else if( b_ctrl_c )
        duration = (double)(2 * last_dts - prev_dts - first_dts) * param->i_timebase_num / param->i_timebase_den;
    else
        duration = (double)(2 * largest_pts - second_largest_pts) * param->i_timebase_num / param->i_timebase_den;

    i_end = x264_mdate();
    /* Erase progress indicator before printing encoding stats. */
    if( opt->b_progress )
        fprintf( stderr, "                                                                               \r" );
    if( h )
        x264_encoder_close( h );
    fprintf( stderr, "\n" );

    if( b_ctrl_c )
        fprintf( stderr, "aborted at input frame %d, output frame %d\n", opt->i_seek + i_frame, i_frame_output );

    cli_output.close_file( opt->hout, largest_pts, second_largest_pts );
    opt->hout = NULL;

    if( i_frame_output > 0 )
    {
        double fps = (double)i_frame_output * (double)1000000 /
                     (double)( i_end - i_start );

        fprintf( stderr, "encoded %d frames, %.2f fps, %.2f kb/s\n", i_frame_output, fps,
                 (double) i_file * 8 / ( 1000 * duration ) );
    }

    return retval;
}
