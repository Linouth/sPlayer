#include <stdio.h>
#include <assert.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>

#include <SDL2/SDL.h>

#define DEBUG
#include "logging.h"

#define FF_REFRESH_EVENT SDL_USEREVENT
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define QUIT -42

#define TEXTURE_QUEUE_SIZE 1
#define MAX_AUDIO_QUEUE_SIZE 10000
#define PACKET_QUEUE_SIZE 1000

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_URL_SIZE 1024

typedef struct PacketQueue {
    AVPacketList  *first_pkt, *last_pkt;
    int             nb_packets;
    int             quit;

    SDL_mutex       *mutex;
    SDL_cond        *cond;
} PacketQueue;

typedef struct Decoder {
    PacketQueue     *queue;
    AVCodecContext  *codecContext;
    SDL_cond        *empty_queue_cond;
    SDL_Thread      *decoder_tid;
} Decoder;

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

    SDL_Thread      *parse_tid;

    SDL_Renderer    *renderer;

    char            url[MAX_URL_SIZE];
    int             quit;


    SDL_Thread      *decode_tid;
    AVPacket        *packetQueue[PACKET_QUEUE_SIZE];
    int             packetQueue_size;
    int             packetQueue_windex;
    int             packetQueue_rindex;
    SDL_mutex       *packetQueueMutex;
    SDL_cond        *packetQueueCond;
    PacketQueue     audioq;
    PacketQueue     videoq;

    Decoder         auddec;
    SDL_cond        *continue_thread_read;
} VideoState;

int audio_thread(void *arg);

void fatal(char *msg) {
    LOG_ERR(msg);
    exit(-1);
}

/**
 * Add a packet to the packet queue
 * @param q the queue to add to
 * @param pkt pointer to the packet to add
 */
static int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pkt_ind;
    int ret = 0;

    if (q->quit)
        ret = QUIT;

    SDL_LockMutex(q->mutex);

        pkt_ind = av_malloc(sizeof(AVPacketList));
        if (!pkt_ind)
            ret = -1;
        pkt_ind->pkt = *pkt;
        pkt_ind->next = NULL;

        if (!q->last_pkt)
            q->first_pkt = pkt_ind;
        else
            q->last_pkt->next = pkt_ind;

        q->last_pkt = pkt_ind;
        q->nb_packets++;
        SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);

    if (ret < 0)
        av_packet_unref(pkt);

    return ret;

}

/**
 * Prepare PacketQueue, clear memory and create mutex/cond
 * @param q pointer to PacketQueue to initialize
 */
static int packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        LOG_ERR("Could not create mutex: %s", SDL_GetError());
        return -1;
    }

    q->cond = SDL_CreateCond();
    if (!q->cond) {
        LOG_ERR("Could not create cond: %s", SDL_GetError());
        return -1;
    }
    q->quit = 1;
    
    return 0;
}

/**
 * Flush PacketQueue and free all memory
 * @param q pointer to PacketQueue to flush
 */
static void packet_queue_flush(PacketQueue *q) {
    AVPacketList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
        for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
            pkt1 = pkt->next;
            av_packet_unref(&pkt->pkt);
            av_freep(&pkt);
        }
        q->first_pkt = NULL;
        q->last_pkt = NULL;
        q->nb_packets = 0;
    SDL_UnlockMutex(q->mutex);
}

/**
 * Destroy PacketQueue by flushing, then destroying mutex/cond
 * @param q pointer to PacketQueue
 */
static void packet_queue_destroy(PacketQueue *q) {
    packet_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

/**
 * Set quit flag to 0, enabeling the use of the PacketQueue
 * @param q pointer to PacketQueue
 */
static void packet_queue_start(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    q->quit = 0;
    SDL_UnlockMutex(q->mutex);
}

/**
 * Get a packet from the PacketQueue
 * @param q pointer to PacketQueue
 * @param pkt pointer to AVPacket to be set
 */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

        for (;;) {
            if (q->quit) {
                ret = QUIT;
                break;
            }

            pkt1 = q->first_pkt;
            if (pkt1) {
                q->first_pkt = pkt1->next;
                if (!q->first_pkt)
                    q->last_pkt = NULL;
                q->nb_packets--;
                *pkt = pkt1->pkt;

                av_free(pkt1);
                ret = 1;
                break;
            } else {
                // Wait for new packets
                SDL_CondWait(q->cond, q->mutex);
            }
        }

    SDL_UnlockMutex(q->mutex);
    return ret;
}

static void decoder_init(Decoder *d, AVCodecContext *codecContext, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->codecContext = codecContext;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
}

static int decoder_start(Decoder *d, int (*fn)(void *), void *arg) {
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThread(fn, "decoder", arg);
    if (!d->decoder_tid) {
        LOG_ERR("Could not create decoding thread: %s", SDL_GetError());
        return -1;
    }
    return 0;
}

static void decoder_destroy(Decoder *d) {
    /* av_packet_unref(&d->pkt); */
    avcodec_free_context(&d->codecContext);
}

static int decoder_decode_frame(Decoder *d, AVFrame *frame) {
    AVCodecContext *context = d->codecContext;
    AVPacket packet;
    int response;

    if (d->queue->nb_packets == 0)
        SDL_CondSignal(d->empty_queue_cond);
    else {
        if (packet_queue_get(d->queue, &packet) < 0)
            return -1;
    }

    response = avcodec_send_packet(context, &packet);
    if (response < 0) {
        LOG_ERR("Error while sending packet to the decoder: %s - %s", context->codec->name,
                av_err2str(response));
        LOG_DEBUG("Codec %s, ID, %d, bit_rate %ld", context->codec->long_name,
                  context->codec->id, context->bit_rate);
        return -1;
    }

    // While there are still frames to unpack from the packet
    while (response >= 0) {
        response = avcodec_receive_frame(context, frame);

        // If no more data to be had from the packet
        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF)
            break;
        else if (response < 0) {
            printf("Something went wrong with the stream, skipping frame: %s - %s\n",
                   context->codec->name, av_err2str(response));
            continue;
        }

    }
    av_packet_unref(&packet);

    return 0;
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
        wanted_spec.freq        = codecContext->sample_rate;
        wanted_spec.format      = AUDIO_F32SYS;
        wanted_spec.channels    = codecContext->channels;
        wanted_spec.samples     = SDL_AUDIO_BUFFER_SIZE;
        wanted_spec.callback    = NULL;

        dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, 0);
        if (dev == 0) {
            LOG_ERR("SDL_OpenAudio: %s", SDL_GetError());
            return -1;
        }

        decoder_init(&is->auddec, codecContext, &is->audioq, is->continue_thread_read);
        if (decoder_start(&is->auddec, audio_thread, is) < 0)
            return -1;
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

int decode_packet_and_queue(VideoState *is, AVPacket *packet) {
    int (*queue_frame_func)(VideoState*, AVFrame*);
    AVCodecContext *context;
    AVFrame *frame;
    int response;


    if (packet->stream_index == is->audio_stream_index) {
        context = is->audioContext;
        queue_frame_func = queue_audio_frame;
    } else if (packet->stream_index == is->video_stream_index) {
        context = is->videoContext;
        queue_frame_func = queue_video_frame;
    } else {
        // Wut?
        LOG_ERR("Unknown stream index: %d", packet->stream_index);
        return -1;
    }

    response = avcodec_send_packet(context, packet);
    if (response < 0) {
        LOG_ERR("Error while sending packet to the decoder: %s - %s", context->codec->name,
                av_err2str(response));
        LOG_DEBUG("Codec %s, ID, %d, bit_rate %ld", context->codec->long_name,
                  context->codec->id, context->bit_rate);
        return -1;
    }

    frame = av_frame_alloc();
    if (!frame) {
        LOG_ERR("Could not allocate memory for frame");
        return -1;
    }

    // While there are still frames to unpack from the packet
    while (response >= 0) {
        response = avcodec_receive_frame(is->audioContext, frame);

        // If no more data to be had from the packet
        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF)
            break;
        else if (response < 0) {
            printf("Something went wrong with the stream, skipping frame: %s - %s\n",
                   context->codec->name, av_err2str(response));
            continue;
        }

        // Add data to the designated queue
        if ((*queue_frame_func)(is, frame) < 0) {
            LOG_ERR("Could not queue data");
            return -1;
        }

    }
    av_frame_unref(frame);

    return 0;
}

/*
int decode_thread(void *arg) {
    VideoState  *is = (VideoState *)arg;
    PacketQueue *q;
    AVPacket packet;

    q = &is->audioq;
    for (;;) {
        if (is->quit)
            break;

        if (q->nb_packets <= 0) {
            SDL_CondWaitTimeout(q->cond, q->mutex, 10);
            continue;
        }

        packet_queue_get(q, &packet);
        if (packet.stream_index == 0)
            continue;

        decode_packet_and_queue(is, &packet);
    }

    return 0;
}
*/

int parse_thread(void *arg) {
    VideoState      *is = (VideoState *)arg;
    AVFormatContext *pFormatContext;
    AVPacket        *packet;
    PacketQueue     *q;

    int audio_index = -1;
    int video_index = -1;
    int i;

    is->audio_stream_index = -1;
    is->video_stream_index = -1;

    packet_queue_start(&is->audioq);

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
        /* printf("AudStream..."); */
    if (video_index >= 0)
        /* open_stream_component(is, video_index); */
        printf("VidStream...");

    // Check if both video and audio stream index are set (meaning they are found and opened)
    // TODO: Make it so either are optional (Just an audio or video stream)
    /* if (is->audio_stream_index < 0 || is->video_stream_index < 0) { */
    /*     LOG_ERR("Could not open codecs"); */
    /*     goto fail; */
    /* } */

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

        if (packet->stream_index == audio_index)
            q = &is->audioq;
        else
            continue;

        packet_queue_put(q, packet);
        /*
        // Add packet to packet queue
        SDL_LockMutex(is->packetQueueMutex);
        while (is->packetQueue_size >= PACKET_QUEUE_SIZE && !is->quit) {
            SDL_CondWait(is->packetQueueCond, is->packetQueueMutex);
        }
        SDL_UnlockMutex(is->packetQueueMutex);

        is->packetQueue[is->packetQueue_windex] = packet;

        if (++is->packetQueue_windex == PACKET_QUEUE_SIZE)
            is->packetQueue_windex = 0;

        SDL_LockMutex(is->packetQueueMutex);
        is->packetQueue_size++;
        SDL_UnlockMutex(is->packetQueueMutex);
        */
        
        /* decode_packet_and_queue(is, packet); */
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

int audio_thread(void *arg) {
    VideoState *is = (VideoState *)arg;
    Decoder d = is->auddec;
    AVFrame *frame;

    frame = av_frame_alloc();

    for (;;) {
        if (is->quit)
            break;

        decoder_decode_frame(&d, frame);
        queue_audio_frame(is, frame);
    }

    return 0;
}

// TODO: Redo whole video rendering part.
void video_display(VideoState *is) {
    SDL_Rect    rect;
    SDL_Texture *texture;

    texture = is->textureQueue[is->textureQueue_rindex];

    // TODO: Stuff for aspect ratio and scaling
    rect.x = 0;
    rect.y = 0;
    rect.w = is->videoContext->width;
    rect.h = is->videoContext->height;

    SDL_RenderCopy(is->renderer, texture, NULL, &rect);
    SDL_RenderPresent(is->renderer);
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *arg) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = arg;
    SDL_PushEvent(&event);
    return 0;
}

static void schedule_refresh (VideoState *is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_refresh_timer(void *userdata) {
    VideoState  *is = (VideoState *)userdata;

    if (is->videoStream) {
        if (is->textureQueue_size == 0) {
            schedule_refresh(is, 1);
        } else {
            schedule_refresh(is, 80);

            video_display(is);

            if (++is->textureQueue_rindex == TEXTURE_QUEUE_SIZE) {
                is->textureQueue_rindex = 0;
            }

            SDL_LockMutex(is->textureQueueMutex);
            is->textureQueue_size--;
            SDL_CondSignal(is->textureQueueCond);
            SDL_LockMutex(is->textureQueueMutex);
        }
    } else {
        schedule_refresh(is, 100);
    }
}

int main(int argc, char *argv[]) {
    VideoState  *is = NULL;
    SDL_Window  *window;
    SDL_Event   event;


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
                              /* is->videoContext->width, */
                              /* is->videoContext->height, */
                              1920,
                              1080,
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

    av_strlcpy(is->url, argv[1], sizeof(is->url));

    is->textureQueueMutex = SDL_CreateMutex();
    is->textureQueueCond = SDL_CreateCond();

    if (packet_queue_init(&is->audioq) < 0) {
        LOG_ERR("Could not initialize packet queue");
        return -1;
    }

    schedule_refresh(is, 40);

    is->parse_tid = SDL_CreateThread(parse_thread, "ParseThread", is);
    if (!is->parse_tid) {
        LOG_ERR("Could not start Parse Thread");
        av_free(is);
        return -1;
    }

    /* is->decode_tid = SDL_CreateThread(decode_thread, "DecodeThread", is); */
    /* if (!is->decode_tid) { */
    /*     LOG_ERR("Could not start Decode Thread"); */
    /*     av_free(is); */
    /*     return -1; */
    /* } */

    for (;;) {
        SDL_WaitEvent(&event);
        switch(event.type) {
            case FF_QUIT_EVENT:
            case SDL_QUIT:
                is->quit = 1;
                SDL_Quit();
                return 0;
                break;
            case FF_REFRESH_EVENT:
                video_refresh_timer(event.user.data1);
            default:
                break;
        }
    }

    av_free(is);

    return 0;
}
