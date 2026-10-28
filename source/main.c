#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <SDL.h>
#include <SDL_image.h>

#define PHONE_IP "REPLACE_WITH_YOUR_PHONE_IP"
#define PHONE_PORT 8080
#define STREAM_PATH "/stream.mjpeg?clientId=ScreenMirrorWiiU"

#define SCREEN_W 854
#define SCREEN_H 480
#define FRAME_MAX (2 * 1024 * 1024)
#define NET_BUFFER_SIZE 16384
#define LINE_SIZE 1024

typedef struct {
    int socket_fd;
    uint8_t data[NET_BUFFER_SIZE];
    size_t position;
    size_t length;
} StreamReader;

/* Read buffered network data without losing bytes between reads. */
static ssize_t stream_read(StreamReader *reader,
                           uint8_t *output, size_t wanted)
{
    size_t copied = 0;

    while (copied < wanted) {
        if (reader->position >= reader->length) {
            ssize_t received = recv(
                reader->socket_fd,
                reader->data,
                sizeof(reader->data),
                0
            );

            if (received <= 0)
                return copied ? (ssize_t)copied : -1;

            reader->position = 0;
            reader->length = (size_t)received;
        }

        size_t available = reader->length - reader->position;
        size_t amount = wanted - copied;

        if (amount > available)
            amount = available;

        memcpy(output + copied,
               reader->data + reader->position,
               amount);

        reader->position += amount;
        copied += amount;
    }

    return (ssize_t)copied;
}

/* Read one HTTP/MIME header line and remove CR/LF. */
static bool stream_read_line(StreamReader *reader,
                             char *line, size_t capacity)
{
    size_t used = 0;

    while (used + 1 < capacity) {
        uint8_t ch;

        if (stream_read(reader, &ch, 1) != 1)
            return false;

        if (ch == '\n') {
            line[used] = '\0';

            if (used > 0 && line[used - 1] == '\r')
                line[used - 1] = '\0';

            return true;
        }

        line[used++] = (char)ch;
    }

    line[used] = '\0';
    return false;
}

static bool stream_read_exact(StreamReader *reader,
                              uint8_t *data, size_t size)
{
    return stream_read(reader, data, size) == (ssize_t)size;
}

/* Connect to ScreenStream and request its continuous MJPEG stream. */
static int connect_to_phone(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0)
        return -1;

    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               &timeout, sizeof(timeout));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons(PHONE_PORT);

    if (inet_pton(AF_INET, PHONE_IP, &address.sin_addr) != 1) {
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&address,
                sizeof(address)) < 0) {
        close(sock);
        return -1;
    }

    char request[512];

    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: keep-alive\r\n"
        "Accept: multipart/x-mixed-replace\r\n\r\n",
        STREAM_PATH, PHONE_IP, PHONE_PORT);

    size_t length = strlen(request);
    size_t sent = 0;

    while (sent < length) {
        ssize_t amount = send(
            sock, request + sent, length - sent, 0);

        if (amount <= 0) {
            close(sock);
            return -1;
        }

        sent += (size_t)amount;
    }

    return sock;
}

/* Read the initial HTTP response headers. */
static bool read_http_headers(StreamReader *reader)
{
    char line[LINE_SIZE];

    if (!stream_read_line(reader, line, sizeof(line)))
        return false;

    if (strncmp(line, "HTTP/", 5) != 0 ||
        strstr(line, " 200 ") == NULL) {
        return false;
    }

    while (stream_read_line(reader, line, sizeof(line))) {
        if (line[0] == '\0')
            return true;
    }

    return false;
}

/*
 * Read one complete multipart JPEG frame.
 * Frame boundaries come from MIME headers and Content-Length,
 * not guesses based on JPEG marker bytes.
 */
static bool read_mjpeg_frame(StreamReader *reader,
                             uint8_t *frame,
                             size_t frame_capacity,
                             size_t *frame_size)
{
    char line[LINE_SIZE];

    for (;;) {
        if (!stream_read_line(reader, line, sizeof(line)))
            return false;

        /* Skip blank lines and seek the next MIME boundary. */
        if (line[0] == '\0')
            continue;

        if (line[0] != '-' || line[1] != '-')
            continue;

        /* A final MIME boundary means the stream has ended. */
        if (line[2] == '-' && line[3] == '-')
            return false;

        break;
    }

    size_t content_length = 0;

    /* Read this frame's MIME headers. */
    for (;;) {
        if (!stream_read_line(reader, line, sizeof(line)))
            return false;

        if (line[0] == '\0')
            break;

        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *value = line + 15;

            while (*value == ' ' || *value == '\t')
                value++;

            char *end = NULL;
            unsigned long parsed = strtoul(value, &end, 10);

            if (end == value || parsed == 0 ||
                parsed > frame_capacity) {
                return false;
            }

            content_length = (size_t)parsed;
        }
    }

    /* Do not guess frame boundaries if the length is missing. */
    if (content_length == 0)
        return false;

    if (!stream_read_exact(reader, frame, content_length))
        return false;

    *frame_size = content_length;
    return true;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        return 1;

    if ((IMG_Init(IMG_INIT_JPG) & IMG_INIT_JPG) == 0) {
        SDL_Quit();
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Screen Mirror",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_W,
        SCREEN_H,
        SDL_WINDOW_FULLSCREEN
    );

    if (!window) {
        IMG_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED);

    if (!renderer) {
        SDL_DestroyWindow(window);
        IMG_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    SDL_Texture *texture = NULL;
    int texture_w = 0;
    int texture_h = 0;

    uint8_t *frame = malloc(FRAME_MAX);

    if (!frame) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        IMG_Quit();
        SDL_Quit();
        return 1;
    }

    int sock = -1;
    bool running = true;

    StreamReader reader;
    memset(&reader, 0, sizeof(reader));
    reader.socket_fd = -1;

    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                running = false;
        }

        if (!running)
            break;

        if (sock < 0) {
            sock = connect_to_phone();

            if (sock >= 0) {
                reader.socket_fd = sock;
                reader.position = 0;
                reader.length = 0;

                if (!read_http_headers(&reader)) {
                    close(sock);
                    sock = -1;
                    reader.socket_fd = -1;
                }
            }

            if (sock < 0) {
                SDL_Delay(500);
                continue;
            }
        }

        size_t frame_size = 0;

        if (!read_mjpeg_frame(
                &reader, frame, FRAME_MAX, &frame_size)) {
            close(sock);
            sock = -1;
            reader.socket_fd = -1;
            reader.position = 0;
            reader.length = 0;
            SDL_Delay(250);
            continue;
        }

        SDL_RWops *rw = SDL_RWFromConstMem(
            frame, (int)frame_size);

        if (rw) {
            SDL_Surface *image = IMG_Load_RW(rw, 1);

            if (image) {
                SDL_Surface *converted = SDL_ConvertSurfaceFormat(
                    image, SDL_PIXELFORMAT_ARGB8888, 0);

                SDL_FreeSurface(image);

                if (converted) {
                    if (!texture ||
                        texture_w != converted->w ||
                        texture_h != converted->h) {

                        if (texture)
                            SDL_DestroyTexture(texture);

                        texture = SDL_CreateTexture(
                            renderer,
                            SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING,
                            converted->w,
                            converted->h
                        );

                        if (texture) {
                            texture_w = converted->w;
                            texture_h = converted->h;

                            SDL_SetTextureBlendMode(
                                texture, SDL_BLENDMODE_NONE);
                        } else {
                            texture_w = 0;
                            texture_h = 0;
                        }
                    }

                    if (texture) {
                        SDL_UpdateTexture(
                            texture,
                            NULL,
                            converted->pixels,
                            converted->pitch
                        );
                    }

                    SDL_FreeSurface(converted);
                }
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        if (texture) {
            SDL_Rect destination = {
                0, 0, SCREEN_W, SCREEN_H
            };

            SDL_RenderCopy(
                renderer, texture, NULL, &destination);
        }

        SDL_RenderPresent(renderer);
    }

    if (sock >= 0)
        close(sock);

    if (texture)
        SDL_DestroyTexture(texture);

    free(frame);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();

    return 0;
}
