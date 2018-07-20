#include <stdio.h>
#include <assert.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <SDL2/SDL.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 10000

int open_codec_contexts(AVFormatContext *pFormatContext, AVCodecContext *pCodecContext, AVCodecContext *aCodecContext);
static int decode_packet(AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame);
void audio_callback(void *userdata, Uint8 *stream, int len);


struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
};


struct PacketQueue audioq;
int quit = 0;

void fatal(char *msg) {
    fprintf(stderr, msg);
    exit(-1);
}

int packet_queue_put(struct PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pkt1;
    if (av_packet_make_refcounted(pkt) < 0) {
        return -1;
    }
    pkt1 = av_malloc(sizeof(AVPacketList));
        if (!pkt1)
            return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

int packet_queue_get(struct PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (quit) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

void packet_queue_init(struct PacketQueue *q) {
    memset(q, 0, sizeof(struct PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int main(int argc, char *argv[]) {
    AVFormatContext     *pFormatContext = NULL;
    AVCodec             *pCodec = NULL;
    AVCodec             *aCodec = NULL;
    AVCodecParameters   *pCodecParameters = NULL;
    AVCodecParameters   *aCodecParameters = NULL;
    AVCodecContext      *pCodecContext = NULL;
    AVCodecContext      *aCodecContext = NULL;
    int                 video_stream_index, audio_stream_index, i;


    if (argc < 2) {
        printf("Usage: %s <video_file>\n", argv[0]);
        return -1;
    }

    // Initialize SDL
    if (SDL_Init(SDL_INIT_EVERYTHING) < 0) {
        fprintf(stderr, "Failed to initialize SDL - %s\n", SDL_GetError());
        return -1;
    }

    // Open file
    pFormatContext = avformat_alloc_context();
    if (!pFormatContext)
        fatal("Could not allocate memory for context");

    if (avformat_open_input(&pFormatContext, argv[1], NULL, NULL) < 0)
        fatal("Could not open file");

    // Retrieve stream info
    if (avformat_find_stream_info(pFormatContext, NULL) < 0)
        fatal("Could not get stream info");


    /* // Open Video and Audio Codec contexts */

    // Loop over streams
    video_stream_index = audio_stream_index = -1;
    for (i = 0; i < pFormatContext->nb_streams; i++) {
        pCodecParameters = pFormatContext->streams[i]->codecpar;
        pCodec = avcodec_find_decoder(pCodecParameters->codec_id);

        if (!pCodec) {
            fprintf(stderr, "Error: Unsupported codec");
            continue;
        }

        if (pCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
        } else if (pCodecParameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
        }
    }
    if (video_stream_index == -1 || audio_stream_index == -1)
        return -1;

    pCodecParameters = pFormatContext->streams[video_stream_index]->codecpar;
    pCodec = avcodec_find_decoder(pCodecParameters->codec_id);
    aCodecParameters = pFormatContext->streams[audio_stream_index]->codecpar;
    aCodec = avcodec_find_decoder(aCodecParameters->codec_id);

    // -- Video Codec --
    // Allocate memory for codec
    pCodecContext = avcodec_alloc_context3(pCodec);
    if (!pCodecContext)
        fatal("Could not allocate memory for AVCodecContext");

    // Load codec context from parameters
    if (avcodec_parameters_to_context(pCodecContext, pCodecParameters) < 0)
        fatal("Could not get video codec from parameters");

    // Open codec
    if (avcodec_open2(pCodecContext, pCodec, NULL) < 0)
        fatal("Could not open video codec");

    // -- Audio Codec --
    // Allocate memory for codec
    aCodecContext = avcodec_alloc_context3(aCodec);
    if (!aCodecContext)
        fatal("Could not allocate memory for AVCodecContext");

    // Load codec context from parameters
    if (avcodec_parameters_to_context(aCodecContext, aCodecParameters) < 0)
        fatal("Could not get audio codec from parameters");

    // Setting up SDL Audio
    
    SDL_AudioSpec      wanted_spec, spec;
    SDL_zero(wanted_spec);

    wanted_spec.freq = aCodecContext->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = aCodecContext->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = aCodecContext;

    if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "Failed to open audio stream: %s\n", SDL_GetError());
        return -1;
    }

    // Open codec
    if (avcodec_open2(aCodecContext, aCodec, NULL) < 0)
        fatal("Could not open audio codec");

    packet_queue_init(&audioq);
    SDL_PauseAudio(0);

    printf("width: %d\n", pCodecContext->width);


    // Allocate memory for frames
    AVFrame *pFrame = av_frame_alloc();
    if (!pFrame)
        fatal("Could not allocate memory for AVFrame");

    // Allocate memory for packets
    AVPacket *pPacket = av_packet_alloc();
    if (!pPacket)
        fatal("Could not allocate memory for AVPacket");

    SDL_Window          *window;
    SDL_Renderer        *renderer = NULL;
    SDL_Texture         *texture = NULL;
    SDL_Event           event;
    SDL_Rect            rect;
    int                 response = 0;

    // Create window
    window = SDL_CreateWindow("Player",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              pCodecContext->width,
                              pCodecContext->height,
                              SDL_WINDOW_SHOWN);
    if (!window)
        fatal("SDL: Could not create window");

    // Create renderer
    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer)
        fatal("SDL: Could not create renderer");

    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    SDL_RenderClear(renderer);

    texture = SDL_CreateTexture(renderer,
                                SDL_PIXELFORMAT_YV12,
                                SDL_TEXTUREACCESS_STATIC, 
                                pCodecContext->width,
                                pCodecContext->height);

    // Loop through all frames in video stream
    while (av_read_frame(pFormatContext, pPacket) >= 0) {
        if (pPacket->stream_index == video_stream_index) {
            response = decode_packet(pPacket, pCodecContext, pFrame);

            if (response < 0) {
                printf("Something went wrong with the video file stream");
                break;
            }

            rect.x = 0;
            rect.y = 0;
            rect.w = pCodecContext->width;
            rect.h = pCodecContext->height;

            // Update texture with YUV converted video frame
            SDL_UpdateYUVTexture(texture,
                                 &rect,
                                 pFrame->data[0],
                                 pFrame->linesize[0],
                                 pFrame->data[1],
                                 pFrame->linesize[1],
                                 pFrame->data[2],
                                 pFrame->linesize[2]);

            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);
        } else if (pPacket->stream_index == audio_stream_index) {
            packet_queue_put(&audioq, pPacket);
        } else {
            av_packet_unref(pPacket);
        }


        SDL_PollEvent(&event);
        if (event.type == SDL_QUIT) {
            SDL_Quit();
            quit = 1;
            break;
        } else if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                SDL_Quit();
                quit = 1;
                break;
            }
        }
    }

    av_frame_free(&pFrame);

    avcodec_close(pCodecContext);
    avformat_close_input(&pFormatContext);

    return 0;
}

int open_codec_contexts(AVFormatContext *pFormatContext, AVCodecContext *pCodecContext, AVCodecContext *aCodecContext) {
    // TODO
    int         video_stream_index, audio_stream_index, i;
    AVCodec     *pCodec = NULL;
    AVCodec     *aCodec = NULL;
    AVCodecParameters   *pCodecParameters = NULL;


    // Loop over streams
    video_stream_index = audio_stream_index = -1;
    for (i = 0; i < pFormatContext->nb_streams; i++) {
        pCodecParameters = pFormatContext->streams[i]->codecpar;
        pCodec = avcodec_find_decoder(pCodecParameters->codec_id);

        if (!pCodec) {
            fprintf(stderr, "Error: Unsupported codec");
            continue;
        }

        if (pCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
        } else if (pCodecParameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
        }
    }
    if (video_stream_index == -1 || audio_stream_index == -1)
        return -1;

    pCodec = avcodec_find_decoder(pFormatContext->streams[video_stream_index]->codecpar->codec_id);
    aCodec = avcodec_find_decoder(pFormatContext->streams[audio_stream_index]->codecpar->codec_id);

    // -- Video Codec --
    /* if (pCodecContext) { */
        // Allocate memory for codec
        pCodecContext = avcodec_alloc_context3(pCodec);
        if (!pCodecContext)
            return -2;

        // Load codec context from parameters
        if (avcodec_parameters_to_context(pCodecContext, pCodecParameters) < 0)
            return -3;

        // Open codec
        if (avcodec_open2(pCodecContext, pCodec, NULL) < 0)
            return -4;
    /* } */

    /* // -- Audio Codec -- */
    /* /1* if (aCodecContext) { *1/ */
    /*     // Allocate memory for codec */
    /*     aCodecContext = avcodec_alloc_context3(aCodec); */
    /*     if (!aCodecContext) */
    /*         return -2; */

    /*     // Load codec context from parameters */
    /*     if (avcodec_parameters_to_context(aCodecContext, pCodecParameters) < 0) */
    /*         return -3; */

    /*     // Open codec */
    /*     if (avcodec_open2(aCodecContext, aCodec, NULL) < 0) */
    /*         return -4; */
    /* /1* } *1/ */

    return 0;
}


static int decode_packet(AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame) {
    int response = avcodec_send_packet(pCodecContext, pPacket);

    if (response < 0) {
        fprintf(stderr, "Could not send packet to the decoder: %s\n", av_err2str(response));
        return -1;
    }

    response = avcodec_receive_frame(pCodecContext, pFrame);
    if (response < 0) {
        fprintf(stderr, "Could not receive packet from the decoder: %s\n", av_err2str(response));
    /*     return -1; */
    }

    return 0;
}

int audio_decode_frame(AVCodecContext *aCodecContext, uint8_t *audio_buf, int buf_size) {
    static AVPacket pkt;
    static uint8_t *audio_pkt_data = NULL;
    static int audio_pkt_size = 0;
    static AVFrame frame;

    int len1 = 0, data_size = 0;

    for (;;) {
        /* while (audio_pkt_size > 0) { */
            int got_frame = 0;
            got_frame = decode_packet(&pkt, aCodecContext, &frame);
            /* if (len1 < 0) { */
            /*     audio_pkt_size = 0; */
            /*     break; */
            /* } */
            audio_pkt_data += pkt.size;
            audio_pkt_size -= pkt.size;
            data_size = 0;
            if (got_frame == 0) {
                data_size = av_samples_get_buffer_size(NULL,
                                                       aCodecContext->channels,
                                                       frame.nb_samples,
                                                       aCodecContext->sample_fmt,
                                                       1);
                assert(data_size <= buf_size);
                memcpy(audio_buf, frame.data[0], data_size);
            }
            if (data_size <= 0) {
                continue;
            }

            return data_size;
        /* } */
        if (pkt.data)
            av_packet_unref(&pkt);

        if (quit)
            return -1;

        if (packet_queue_get(&audioq, &pkt, 1) < 0)
            return -1;

        audio_pkt_data = pkt.data;
        audio_pkt_size = pkt.size;
    }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
    AVCodecContext *aCodecContext = (AVCodecContext *)userdata;
    int len1, audio_size;

    static uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    while (len > 0) {
        if (audio_buf_index >= audio_buf_size) {
            // Fetch more data
            audio_size = audio_decode_frame(aCodecContext, audio_buf, sizeof(audio_buf));

            if (audio_size < 0) {
                audio_buf_size = 1024;
                memset(audio_buf, 0, audio_buf_size);
            } else {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }

        len1 = audio_buf_size - audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}

