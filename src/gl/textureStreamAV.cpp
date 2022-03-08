#include "ada/gl/textureStreamAV.h"

#include <iostream>
#include <fstream>

#ifdef SUPPORT_LIBAV

#include <ctime>

extern "C" {
#include <libavformat/version.h>
#include <libavutil/log.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#include "ada/window.h"
#include "ada/tools/text.h"
#include "ada/tools/pixels.h"

#define EPS 0.000025

#ifdef _WIN32
// https://stackoverflow.com/questions/33696092/whats-the-correct-replacement-for-posix-memalign-in-windows#comment55165270_33696092
#define posix_memalign(p, a, s) (((*(p)) = _aligned_malloc((s), (a))), *(p) ? 0 : errno)
#endif

namespace ada {

TextureStreamAV::TextureStreamAV() : 
    av_format_ctx(NULL),
    av_codec_ctx(NULL),
    av_decoder(NULL),
    av_frame(NULL),
    av_packet(NULL),
    conv_ctx(NULL),
    frame_data(NULL),
    m_fps(0.0),
    m_startSecond(0.0),
    m_totalSeconds(-1.0),
    m_currentSecond(-1.0),
    m_waitFromSecond(0.0),
    m_waitUntilSecond(0.0),
    m_speed(1.0),
    m_totalFrames(-1),
    m_currentFrame(-1),
    m_streamId(-1),
    m_device(false)
{

    // initialize libav
    avformat_network_init();
    
    // https://gist.github.com/shakkhar/619fd90ccbd17734089b
    avdevice_register_all();
}

TextureStreamAV::TextureStreamAV( bool _isDevice ) : 
    av_format_ctx(NULL),
    av_codec_ctx(NULL),
    av_decoder(NULL),
    av_frame(NULL),
    av_packet(NULL),
    conv_ctx(NULL),
    frame_data(NULL),
    m_fps(0.0),
    m_startSecond(0.0),
    m_totalSeconds(-1.0),
    m_currentSecond(-1.0),
    m_waitFromSecond(0.0),
    m_waitUntilSecond(0.0),
    m_speed(1.0),
    m_totalFrames(-1),
    m_currentFrame(-1),
    m_streamId(-1),
    m_device(_isDevice)
{

    // initialize libav
    avformat_network_init();
    
    // https://gist.github.com/shakkhar/619fd90ccbd17734089b
    avdevice_register_all();
}

TextureStreamAV::~TextureStreamAV() {
    clear();
}

bool TextureStreamAV::load(const std::string& _path, bool _vFlip, TextureFilter _filter, TextureWrap _wrap) {
    m_vFlip = _vFlip;
    m_filter = _filter;
    m_wrap = _wrap;

    // https://github.com/bartjoyce/video-app/blob/master/src/video_reader.cpp#L35-L40
    // Open the file using libavformat
    av_format_ctx = avformat_alloc_context();
    if (!av_format_ctx) {
        std::cout << "Couldn't created AVFormatContext" << std::endl;
        clear();
        return false;
    }

    av_log_set_level(AV_LOG_QUIET);

    int input_lodaded = -1;
    if (m_device) {
        std::string driver = "video4linux2";

        #ifdef PLATFORM_OSX 
        driver = "avfoundation";
        #elif defined(_WIN32)
        driver = "vfwcap";
        #endif
        AVDictionary *options = NULL;
        av_dict_set(&options, "framerate", "30", 0);
        // av_dict_set(&options, "framerate", toString(getFps()).c_str(), 0);
        // std::cout << "Opening " << driver << " at " << _path << std::endl;

        input_lodaded = avformat_open_input(&av_format_ctx, _path.c_str(), av_find_input_format(driver.c_str()) , &options);
    }
    else 
        input_lodaded = avformat_open_input(&av_format_ctx, _path.c_str(), NULL, NULL);

    // https://gist.github.com/rcolinray/7552384#file-gl_ffmpeg-cpp-L229
    if (input_lodaded < 0) {
        std::cout << "failed to open input" << std::endl;
        clear();
        return false;
    }
        
    // find stream info
    if (avformat_find_stream_info(av_format_ctx, NULL) < 0) {
        std::cout << "failed to get stream info" << std::endl;
        clear();
        return false;
    }

    // dump debug info
    // av_dump_format(av_format_ctx, 0, _path.c_str(), 0);
        
    m_streamId = -1;
    AVCodecParameters* av_codec_params;
    const AVCodec* av_codec;

    // find the video stream
    for (unsigned int i = 0; i < av_format_ctx->nb_streams; ++i) {
        av_codec_params = av_format_ctx->streams[i]->codecpar;
        av_codec = avcodec_find_decoder(av_codec_params->codec_id);
        if (!av_codec) {
            continue;
        }
        if (av_codec_params->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_width = av_codec_params->width;
            m_height = av_codec_params->height;
            time_base = av_format_ctx->streams[i]->time_base;
            m_streamId = i;
            break;
        }
    }

    // std::cout << "time base: " << time_base << std::endl;

    if (m_streamId == -1) {
        std::cout << "failed to find video stream" << std::endl;
        clear();
        return false;
    }

    // Set up a codec context for the decoder
    av_codec_ctx = avcodec_alloc_context3(av_codec);
    if (!av_codec_ctx) {
        printf("Couldn't create AVCodecContext\n");
        return false;
    }
    if (avcodec_parameters_to_context(av_codec_ctx, av_codec_params) < 0) {
        printf("Couldn't initialize AVCodecContext\n");
        return false;
    }

    // open the decoder
    if (avcodec_open2(av_codec_ctx, av_decoder, NULL) < 0) {
        std::cout << "failed to open codec" << std::endl;
        clear();
        return false;
    }

    // allocate the video frames
    av_frame = av_frame_alloc();
    if (!av_frame) {
        std::cout << "Couldn't allocate AVFrame" << std::endl;
        return false;
    }

    av_packet = av_packet_alloc();
    if (!av_packet) {
        std::cout << "Couldn't allocate AVPacket" << std::endl;
        return false;
    }

    constexpr int ALIGNMENT = 128;
    if (posix_memalign((void**)&frame_data, ALIGNMENT, m_width * m_height * 4) != 0) {
        printf("Couldn't allocate frame buffer\n");
        return 1;
    }

    // Generate an OpenGL texture ID for this texturez
    glEnable(GL_TEXTURE_2D);
    if (m_id == 0)
        glGenTextures(1, &m_id);
    glBindTexture(GL_TEXTURE_2D, m_id);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, getMinificationFilter(m_filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, getMagnificationFilter(m_filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, getWrap(m_wrap));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, getWrap(m_wrap));

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    m_path = _path;

    m_totalFrames = getTotalFrames();
    m_currentFrame = 0;

    m_totalSeconds = getTotalSeconds();
    m_currentSecond = 0;

    m_startSecond = ada::getTimeSec();

    return true;
}

// av_err2str returns a temporary array. This doesn't work in gcc.
// This function can be used as a replacement for av_err2str.
static const char* av_make_error(int errnum) {
    static char str[AV_ERROR_MAX_STRING_SIZE];
    memset(str, 0, sizeof(str));
    return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}

static AVPixelFormat correct_for_deprecated_pixel_format(AVPixelFormat pix_fmt) {
    // Fix swscaler deprecated pixel format warning
    // (YUVJ has been deprecated, change pixel format to regular YUV)
    switch (pix_fmt) {
        case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
        default:                  return pix_fmt;
    }
}

// helper function as taken from OpenCV ffmpeg reader
double r2d(AVRational r) {
    return r.num == 0 || r.den == 0 ? 0. : (double)r.num / (double)r.den;
}

void TextureStreamAV::setSpeed( float _speed ) {
    m_speed = _speed;
    m_waitFromSecond = 0.0;
    m_waitUntilSecond = 0.0;
}

double TextureStreamAV::getFPS() {
    if (m_fps <= 0.0) {
        double fps = r2d(av_format_ctx->streams[m_streamId]->r_frame_rate);

        if (fps < EPS)
            fps = 1.0 / r2d(av_format_ctx->streams[m_streamId]->time_base);

        m_fps = fps;
    }

    return m_fps;
}

float TextureStreamAV::getTotalSeconds() {
    if (m_totalSeconds < 0.0) {       
        double sec = (double)av_format_ctx->duration / (double)AV_TIME_BASE;
        
        if (sec < EPS)
            sec = (double)av_format_ctx->streams[m_streamId]->duration * r2d(av_format_ctx->streams[m_streamId]->time_base);
        
        if (sec < EPS)
            sec = (double)av_format_ctx->streams[m_streamId]->duration * r2d(av_format_ctx->streams[m_streamId]->time_base);

        m_totalSeconds = sec;
    }

    return m_totalSeconds;
}

float TextureStreamAV::getTotalFrames() {
    if (m_totalFrames < 0) {

        if (av_format_ctx == NULL)
            return -1;

        if (m_device)
            return 1;
        
        int64_t nbf = av_format_ctx->streams[m_streamId]->nb_frames;
        
        if (nbf == 0)
            nbf = (int64_t)floor(getTotalSeconds() * getFPS() + 0.5);

        m_totalFrames = nbf;
    }

    return m_totalFrames;
}

float TextureStreamAV::getCurrentFrame() const { 
    double delta = m_waitUntilSecond - m_waitFromSecond;
    double pct = (m_waitUntilSecond - m_currentSecond)/delta;
    pct = glm::fract(1.0-glm::clamp(pct, 0.0, 1.0));
    
    return (m_device)? 1 : m_currentFrame + pct; 
}

double TextureStreamAV::dts_to_sec(int64_t dts) {
    return (double)(dts - av_format_ctx->streams[m_streamId]->start_time) * r2d(av_format_ctx->streams[m_streamId]->time_base);
}

int64_t TextureStreamAV::dts_to_frame_number(int64_t dts) {
    double sec = dts_to_sec(dts);
    return (int64_t)( getFPS() * sec + 0.5 );
}

bool TextureStreamAV::update() {
    m_currentSecond = (ada::getTimeSec() - m_startSecond) * m_speed;

    if ( newFrame() )
        return decodeFrame();

    return false;
}

void TextureStreamAV::restart() {
    avio_seek(av_format_ctx->pb, 0, SEEK_SET);
    avformat_seek_file(av_format_ctx, m_streamId, 0, 0, av_format_ctx->streams[m_streamId]->duration, 0);
    m_currentFrame = 0;
    m_currentSecond = 0.0;
    m_startSecond = ada::getTimeSec();
    m_waitFromSecond = 0.0;
    m_waitUntilSecond = 0.0;
}

double TextureStreamAV::currentFramePts() {
    return dts_to_sec( av_frame->pts );
}

bool TextureStreamAV::newFrame() {

    if (m_currentFrame > 0 && m_waitUntilSecond > 0.0) {
        double pts = currentFramePts();
        if ( m_currentSecond >= pts ) {
            m_waitFromSecond = pts;
            m_waitUntilSecond = 0.0;
            return true;
        }
        else 
            return false;
    }

    // Decode next frame
    int response = 0;
    bool running = true; 
    while (running) {
        // reads in a packet and stores it in the AVPacket struct. 
        // Note that we've only allocated the packet structure - ffmpeg allocates the 
        // internal data for us, which is pointed to by packet.data 
        response = av_read_frame(av_format_ctx, av_packet);

        // if the response is negative nothing is loaded
        if (response < 0)
            running = false;

        // LOOP IT: if it's the end of the file, start it back again
        if (response == AVERROR_EOF) {
            restart();
            return false;
        }

        // Ignore other streams 
        if (av_packet->stream_index != m_streamId) {
            av_packet_unref(av_packet);
            continue;
        }

        // If the stream is from a DEVICE
        if (m_device) {
            
            bool got_picture = false;
            if (av_codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO ||
                av_codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
                response = avcodec_send_packet(av_codec_ctx, av_packet);
                if (response < 0 && response != AVERROR(EAGAIN) && response != AVERROR_EOF) {
                    // TODO
                } else {
                    if (response >= 0)
                        av_packet->size = 0;
                    response = avcodec_receive_frame(av_codec_ctx, av_frame);

                    if (response >= 0)
                        got_picture = true;
                }
            }

            if (response < 0) {
                printf("Failed to decode packet: %s\n", av_make_error(response));
                return false;
            }

            if (!got_picture)
                return false;
        }

        // If the stream is from a FILE
        else {
            response = avcodec_send_packet(av_codec_ctx, av_packet);
            
            if (response < 0) {
                printf("Failed to decode packet: %s\n", av_make_error(response));
                return false;
            }

            response = avcodec_receive_frame(av_codec_ctx, av_frame);
            if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
                av_packet_unref(av_packet);
                continue;
            } 
            else if (response < 0) {
                printf("Failed to decode packet: %s\n", av_make_error(response));
                return false;
            }
        }

        av_packet_unref(av_packet);
        break;
    }
    
    double pts = currentFramePts();
    if ( m_currentSecond >= pts ) {
        m_waitFromSecond = pts;
        return true;
    }
    
    m_waitUntilSecond = pts;
    return  false; 
}

bool TextureStreamAV::decodeFrame() {
    // Set up sws scaler
    if (!conv_ctx) {
        AVPixelFormat source_pix_fmt = correct_for_deprecated_pixel_format(av_codec_ctx->pix_fmt);
        conv_ctx = sws_getContext(  av_codec_ctx->width, av_codec_ctx->height, source_pix_fmt, 
                                    av_codec_ctx->width, av_codec_ctx->height, AV_PIX_FMT_RGB0,
                                    SWS_BILINEAR, NULL, NULL, NULL);
        
    }
    if (!conv_ctx) {
        printf("Couldn't initialize sw scaler\n");
        return false;
    }

    uint8_t* dest[4] = { frame_data, NULL, NULL, NULL };
    int dest_linesize[4] = { av_frame->width * 4, 0, 0, 0 };
    sws_scale(conv_ctx, av_frame->data, av_frame->linesize, 0, av_frame->height, dest, dest_linesize);

    if (m_vFlip)
        flipPixelsVertically(frame_data, av_codec_ctx->width, av_codec_ctx->height, 4);
    
    m_currentFrame++;
    return Texture::load(av_codec_ctx->width, av_codec_ctx->height, 4, 8, frame_data, m_filter, m_wrap);
}

void  TextureStreamAV::clear() {
    if (conv_ctx)
        sws_freeContext(conv_ctx);

    if (av_frame) 
        av_free(av_frame);

    if (av_packet)
        av_free(av_packet);
        
    if (av_codec_ctx)
        avcodec_close(av_codec_ctx);
        
    if (av_format_ctx)
        avformat_free_context(av_format_ctx);
        // avformat_close_input(&av_format_ctx);

    if (frame_data)
        free(frame_data);

    if (m_id != 0)
        glDeleteTextures(1, &m_id);

    m_id = 0;
}

}

#endif
