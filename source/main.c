#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <SDL.h>
#include <SDL_image.h>

#define PHONE_IP "10.178.61.104"
#define PHONE_PORT 8080
#define STREAM_PATH "/stream.mjpeg?clientId=ScreenMirrorWiiU"

#define FRAME_MAX (2 * 1024 * 1024)
#define SCREEN_W 854
#define SCREEN_H 480

static int connect_to_phone(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PHONE_PORT);

    if (inet_pton(AF_INET, PHONE_IP, &addr.sin_addr) != 1) {
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    char request[512];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n"
        "Accept: multipart/x-mixed-replace\r\n\r\n",
        STREAM_PATH, PHONE_IP, PHONE_PORT);

    size_t sent = 0;
    size_t length = strlen(request);

    while (sent < length) {
        ssize_t n = send(sock, request + sent, length - sent, 0);
        if (n <= 0) {
            close(sock);
            return -1;
        }
        sent += (size_t)n;
    }

    return sock;
}

int main(int argc, char **argv)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        return 1;

    IMG_Init(IMG_INIT_JPG);

    SDL_Window *window = SDL_CreateWindow(
        "Screen Mirror",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_W,
        SCREEN_H,
        SDL_WINDOW_FULLSCREEN
    );

    if (!window) {
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Texture *texture = NULL;
    uint8_t *frame = malloc(FRAME_MAX);

    if (!frame) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    int sock = -1;
    size_t frame_size = 0;
    bool in_frame = false;
    bool running = true;
    bool connected = false;
    uint8_t previous = 0;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                running = false;
        }

        if (sock < 0) {
            sock = connect_to_phone();
            connected = (sock >= 0);
            SDL_Delay(1000);
        }

        if (sock >= 0) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);

            struct timeval timeout = {0, 20000};
            int ready = select(sock + 1, &readfds, NULL, NULL, &timeout);

            if (ready > 0 && FD_ISSET(sock, &readfds)) {
                uint8_t data[8192];
                ssize_t count = recv(sock, data, sizeof(data), 0);

                if (count <= 0) {
                    close(sock);
                    sock = -1;
                    connected = false;
                    in_frame = false;
                    frame_size = 0;
                } else {
                    for (ssize_t i = 0; i < count; i++) {
                        uint8_t byte = data[i];

                        if (!in_frame) {
                            if (previous == 0xFF && byte == 0xD8) {
                                in_frame = true;
                                frame_size = 0;
                                frame[frame_size++] = 0xFF;
                                frame[frame_size++] = 0xD8;
                            }
                        } else if (frame_size < FRAME_MAX) {
                            frame[frame_size++] = byte;

                            if (previous == 0xFF && byte == 0xD9) {
                                SDL_RWops *rw = SDL_RWFromConstMem(
                                    frame, (int)frame_size);

                                if (rw) {
                                    SDL_Surface *image =
                                        IMG_Load_RW(rw, 1);

                                    if (image) {
                                        SDL_Texture *new_texture =
                                            SDL_CreateTextureFromSurface(
                                                renderer, image);

                                        if (new_texture) {
                                            if (texture)
                                                SDL_DestroyTexture(texture);
                                            texture = new_texture;
                                        }

                                        SDL_FreeSurface(image);
                                    }
                                }

                                in_frame = false;
                                frame_size = 0;
                            }
                        } else {
                            in_frame = false;
                            frame_size = 0;
                        }

                        previous = byte;
                    }
                }
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        if (texture) {
            SDL_Rect destination = {0, 0, SCREEN_W, SCREEN_H};
            SDL_RenderCopy(renderer, texture, NULL, &destination);
        } else {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderClear(renderer);
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(1);
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
