#include <stdio.h>
#include <assert.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <SDL2/SDL.h>

#include "logging.h"

#define FF_REFRESH_EVENT SDL_USEREVENT
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)


#define TEXTURE_QUEUE_SIZE 1
#define MAX_AUDIO_QUEUE_SIZE 10000

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_URL_SIZE 1024


typedef struct VideoState {
    AVFormatContext *pFormatContext;

    int             audio_stream_index;
    int             audioDevice;
    AVCodecContext  *audioContext;
    AVStream        *audioStream;

    int             video_stream_index;
    AVCodecContext  *videoContext;
    AVStream        *videoStream;

    SDL_Texture     *textureQueue[TEXTURE_QUEUE_SIZE];
    int             textureQueue_size;
    int             textureQueue_windex; // Write index
    int             textureQueue_rindex; // Read index
    SDL_mutex       *textureQueueMutex;
    SDL_cond        *textureQueueCond;

    SDL_Thread      *video_tid;

    SDL_Renderer    *renderer;

    char            url[MAX_URL_SIZE];
    int             quit;
} VideoState;


void fatal(char *msg) {
    LOG_ERR(msg);
    exit(-1);
}

int open_stream_component(VideoState *is, int stream_index) {
    AVFormatContext     *pFormatContext = is->pFormatContext;
    AVCodecParameters   *codecParameters;
    AVCodecContext      *codecContext;
    AVCodec             *codec;
    SDL_AudioSpec       wanted_spec, spec;
    int                 dev;

    if (stream_index < 0 || stream_index >= pFormatContext->nb_streams) {
        return -1;
    }

    codecParameters = pFormatContext->streams[stream_index]->codecpar;
    codec = avcodec_find_decoder(codecParameters->codec_id);
    if (!codec) {
        LOG_ERR("Unsupported codec");
        return -1;
    }

    codecContext = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codecContext, codecParameters) != 0) {
        LOG_ERR("Could not create codec context from parameters");
        return -1;
    }

    if (codecContext->codec_type == AVMEDIA_TYPE_AUDIO) {
        SDL_zero(wanted_spec);
        wanted_spec.freq = codecContext->sample_rate;
        wanted_spec.format = AUDIO_F32SYS;
        wanted_spec.channels = codecContext->channels;
        wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
        wanted_spec.callback = NULL;

        dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, 0);
        if (dev == 0) {
            LOG_ERR("SDL_OpenAudio: %s", SDL_GetError());
            return -1;
        }
    }
    if (avcodec_open2(codecContext, codec, NULL) < 0) {
        LOG_ERR("Unsupported codec");
        return -1;
    }

    if (codecContext->codec_type == AVMEDIA_TYPE_AUDIO) {
        // Audio Stuff
        is->audio_stream_index  = stream_index;
        is->audioStream         = pFormatContext->streams[stream_index];
        is->audioContext        = codecContext;
        is->audioDevice         = dev;
        
        SDL_PauseAudioDevice(dev, 0);
    } else if (codecContext->codec_type == AVMEDIA_TYPE_VIDEO) {
        // Video Stuff
        is->video_stream_index  = stream_index;
        is->videoStream         = pFormatContext->streams[stream_index];
        is->videoContext        = codecContext;

        /* is ->video_tid          = SDL_CreateThread(video_thread, "VideoThread", is); */
    }

    return 0;
}

int queue_audio_frame(VideoState *is, AVFrame *frame) {
    int data_size, i, channel;

    // Get size of single sample
    data_size = av_get_bytes_per_sample(is->audioContext->sample_fmt);

    // Loop over all samples and channels, and queue the data
    for (i = 0; i < frame->nb_samples; i++) {
        for (channel = 0; channel < frame->channels; channel++) {
            if (SDL_QueueAudio(is->audioDevice,
                               frame->data[channel] + data_size*i,
                               data_size) < 0) {
                LOG_ERR("SDL_QueueAudio: %s", SDL_GetError());
                return -1;
            }
        }
    }

    return 0;
}

int decode_packet(VideoState *is, AVPacket *packet,
                  int (*queue_frame_func)(VideoState*, AVFrame*)) {
    AVFrame *frame;
    int response;

    response = avcodec_send_packet(is->audioContext, packet);

    if (response < 0) {
        LOG_ERR("Error while sending packet to the decoder: %s", av_err2str(response));
        return -1;
    }

    frame = av_frame_alloc();
    if (!frame) {
        LOG_ERR("Could not allocate memory for frame");
        return -1;
    }

    // While there are still audio frames to unpack from the package
    while (response >= 0) {
        response = avcodec_receive_frame(is->audioContext, frame);

        // If no more data to be had from the packet
        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF)
            break;
        else if (response < 0) {
            printf("Something went wrong with the stream, skipping frame: %s\n",
                   av_err2str(response));
            continue;
        }

        //// Add audio data to the audio queue
        if ((*queue_frame_func)(is, frame) < 0) {
            LOG_ERR("Could not queue data");
            return -1;
        }

        av_frame_unref(frame);
    }

    return 0;
}

/*
int audio_decode_packet(VideoState *is, AVPacket *packet) {
    AVFrame *frame;
    int response;

    response = avcodec_send_packet(is->audioContext, packet);

    if (response < 0) {
        LOG_ERR("Error while sending packet to the audio decoder: %s", av_err2str(response));
        return -1;
    }

    // While there are still audio frames to unpack from the package
    while (response >= 0) {
        response = avcodec_receive_frame(is->audioContext, frame);

        // If no more data to be had from the packet
        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF)
            break;
        else if (response < 0) {
            printf("Something went wrong with the audio stream, skipping frame: %s\n",
                   av_err2str(response));
            continue;
        }

        //// Add audio data to the audio queue
        if (queue_audio_frame(is, frame) < 0) {
            LOG_ERR("Could not queue audio data");
            return -1;
        }

        av_frame_unref(frame);
    }

    return 0;
}
*/

int queue_video_frame(VideoState *is, AVFrame *frame) {
    SDL_Texture *texture;
    SDL_Rect    rect;

    SDL_LockMutex(is->textureQueueMutex);
    while (is->textureQueue_size >= TEXTURE_QUEUE_SIZE && !is->quit) {
        SDL_CondWait(is->textureQueueCond, is->textureQueueMutex);
    }
    SDL_UnlockMutex(is->textureQueueMutex);

    if (is->quit)
        return -1;

    texture = is->textureQueue[is->textureQueue_windex];

    // Create and allocate memory for texture
    texture = SDL_CreateTexture(is->renderer,
                                SDL_PIXELFORMAT_YV12,
                                SDL_TEXTUREACCESS_STATIC, 
                                is->videoContext->width,
                                is->videoContext->height);

    // Select area to update
    rect.x = 0;
    rect.y = 0;
    rect.w = is->videoContext->width;
    rect.h = is->videoContext->height;

    // Update texture with YUV converted video frame
    SDL_UpdateYUVTexture(texture,
                         &rect,
                         frame->data[0],
                         frame->linesize[0],
                         frame->data[1],
                         frame->linesize[1],
                         frame->data[2],
                         frame->linesize[2]);

    if (++is->textureQueue_windex == TEXTURE_QUEUE_SIZE)
        is->textureQueue_windex = 0;

    SDL_LockMutex(is->textureQueueMutex);
    is->textureQueue_size++;
    SDL_UnlockMutex(is->textureQueueMutex);

    return 0;
}

/*
int video_decode_packet(VideoState *is, AVPacket *packet) {
    AVFrame     *frame;
    int         response;

    response = avcodec_send_packet(is->videoContext, packet);

    if (response < 0) {
        LOG_ERR("Error while sending packet to the video decoder: %s", av_err2str(response));
        return -1;
    }

    while (response >= 0) {
        response = avcodec_receive_frame(is->videoContext, frame);

        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF)
            break;
        else if (response < 0) {
            printf("Something went wrong with the video stream, skipping frame: %s\n",
                   av_err2str(response));
            continue;
        }

        //// Add video data to the video queue
        if (queue_video_frame(is, frame) < 0) {
            LOG_ERR("Could not queue video frame");
            return -1;
        }

        av_frame_unref(frame);
    }

    return 0;
}
*/

int decode_thread(void *args) {
    VideoState      *is = (VideoState *)args;
    AVFormatContext *pFormatContext;
    AVPacket        *packet;

    int audio_index = -1;
    int video_index = -1;
    int i;

    is->audio_stream_index = -1;
    is->video_stream_index = -1;

    if (avformat_open_input(&pFormatContext, is->url, NULL, NULL) < 0) {
        LOG_ERR("Could not open the file");
        return -1;
    }
    is->pFormatContext = pFormatContext;

    packet = av_packet_alloc();
    if (!packet) {
        LOG_ERR("Could not allocate memory for packet");
        return -1;
    }

    if (avformat_find_stream_info(pFormatContext, NULL) < 0)
        return -1;

    // Find the first video and audio streams
    // TODO: System to choose specific stream (Different audio stream)
    for (i = 0; i < pFormatContext->nb_streams; i++) {
        if (pFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
                && audio_index < 0)
            audio_index = i;
        else if (pFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
                && video_index < 0)
            video_index = i;
    }
    if (audio_index >= 0)
        open_stream_component(is, audio_index);
    if (video_index >= 0)
        open_stream_component(is, video_index);

    // Check if both video and audio stream index are set (meaning they are found and opened)
    // TODO: Make it so either are optional (Just an audio or video stream)
    if (is->audio_stream_index < 0 || is->video_stream_index < 0) {
        LOG_ERR("Could not open codecs");
        goto fail;
    }

    for (;;) {
        if (is->quit)
            break;

        // TODO: Max Video queue
        if (SDL_GetQueuedAudioSize(is->audioDevice) > MAX_AUDIO_QUEUE_SIZE) {
            SDL_Delay(10);
            continue;
        }

        if (av_read_frame(is->pFormatContext, packet) < 0) {
            if (is->pFormatContext->pb->error == 0) {
                SDL_Delay(100);
                continue;
            } else {
                break;
            }
        }

        if (packet->stream_index == is->video_stream_index) {
            // Video packet
            decode_packet(is, packet, queue_video_frame);
        } else if (packet->stream_index == is->audio_stream_index) {
            // Audio packet
            /* audio_decode_packet(is, packet); */
            decode_packet(is, packet, queue_audio_frame);
        }
    }


fail:
    if(1) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    
    av_packet_free(&packet);
    return 0;
}


int main(int argc, char *argv[]) {
    VideoState          *is = NULL;
    SDL_Window          *window;


    is = av_mallocz(sizeof(VideoState));
    if (!is) {
        LOG_ERR("Could not allocate memory for VideoState");
        return -1;
    }

    if (argc < 2) {
        log_info("Usage: %s <video_file>", argv[0]);
        return -1;
    }

    // Initialize SDL
    if (SDL_Init(SDL_INIT_EVERYTHING) < 0) {
        fprintf(stderr, "Failed to initialize SDL - %s\n", SDL_GetError());
        return -1;
    }


    // Create window
    window = SDL_CreateWindow("Player",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              is->videoContext->width,
                              is->videoContext->height,
                              SDL_WINDOW_SHOWN);
    if (!window) {
        LOG_ERR("SDL: Could not create window");
        return -1;
    }

    // Create renderer
    is->renderer = SDL_CreateRenderer(window, -1, 0);
    if (!is->renderer) {
        LOG_ERR("SDL: Could not create renderer");
        return -1;
    }

    SDL_SetRenderDrawColor(is->renderer, 255, 0, 0, 255);
    SDL_RenderClear(is->renderer);

    av_free(is);

    return 0;
}
