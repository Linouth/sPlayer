#include <stdio.h>
#include <assert.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <SDL2/SDL.h>

#include "logging.h"

#define SDL_AUDIO_BUFFER_SIZE 1024


void fatal(char *msg) {
    fprintf(stderr, msg);
    exit(-1);
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
        return -1;
    }

    return 0;
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

    SDL_Window          *window;
    SDL_Renderer        *renderer = NULL;
    SDL_Texture         *texture = NULL;
    SDL_Event           event;
    SDL_Rect            rect;
    int                 response = 0, data_size;


    if (argc < 2) {
        log_info("Usage: %s <video_file>", argv[0]);
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
            printf("Video Codec: resolution %d x %d", pCodecParameters->width, pCodecParameters->height);
        } else if (pCodecParameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            printf("Audio Codec: %d channels, sample rate %d", pCodecParameters->channels, pCodecParameters->sample_rate);
        }
        printf("\tCodec %s, ID %d, bit_rate %ld\n", pCodec->long_name, pCodec->id, pCodecParameters->bit_rate);

        if (video_stream_index != -1 && audio_stream_index != -1)
            break;
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
    /* SDL_AudioStream     *stream = SDL_NewAudioStream; */
    SDL_AudioSpec       wanted_spec, spec;
    SDL_AudioDeviceID   dev = 0;

    SDL_zero(wanted_spec);

    wanted_spec.freq = aCodecContext->sample_rate;
    wanted_spec.format = AUDIO_F32SYS;
    wanted_spec.channels = aCodecContext->channels;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = NULL;

    dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, 0);
    if (dev == 0) {
        SDL_Log("Failed to open audio: %s\n", SDL_GetError());
        return -1;
    }

    // Open codec
    if (avcodec_open2(aCodecContext, aCodec, NULL) < 0)
        fatal("Could not open audio codec");
    SDL_PauseAudioDevice(dev, 0);

    printf("width: %d\n", pCodecContext->width);


    // Allocate memory for frames
    AVFrame *pFrame = av_frame_alloc();
    if (!pFrame)
        fatal("Could not allocate memory for AVFrame");

    // Allocate memory for packets
    AVPacket *pPacket = av_packet_alloc();
    if (!pPacket)
        fatal("Could not allocate memory for AVPacket");

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

    int channel;
    // Loop through all frames in video stream
    while (av_read_frame(pFormatContext, pPacket) >= 0) {
        if (pPacket->stream_index == video_stream_index) {
            response = decode_packet(pPacket, pCodecContext, pFrame);

            if (response < 0) {
                printf("Something went wrong with the video stream, skipping frame\n");
                continue;
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
            response = avcodec_send_packet(aCodecContext, pPacket);

            // While there are still audio frames to unpack form the package
            while (response >= 0) {
                response = avcodec_receive_frame(aCodecContext, pFrame);
                printf("response: %d\n", response);

                if (response == AVERROR(EAGAIN) || response == AVERROR_EOF)
                    break;
                else if (response < 0) {
                    printf("Something went wrong with the audio stream, skipping frame: %s\n", av_err2str(response));
                    continue;
                }

                // Get size of single sample
                data_size = av_get_bytes_per_sample(aCodecContext->sample_fmt);

                // Loop over all samples and channels, and queue the data
                for (i = 0; i < pFrame->nb_samples; i++) {
                    for (channel = 0; channel < pFrame->channels; channel++) {
                        SDL_QueueAudio(dev, pFrame->data[channel] + data_size*i, data_size);
                    }
                }

                av_frame_unref(pFrame);
            }
        } else {
            av_packet_unref(pPacket);
        }


        SDL_PollEvent(&event);
        if (event.type == SDL_QUIT) {
            SDL_Quit();
            break;
        } else if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                SDL_Quit();
                break;
            }
        }
    }

    av_frame_free(&pFrame);

    avcodec_close(pCodecContext);
    avformat_close_input(&pFormatContext);

    return 0;
}

