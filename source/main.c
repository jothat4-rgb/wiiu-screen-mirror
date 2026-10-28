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
#include <vpad/input.h>

#define PHONE_IP "10.178.61.104"
#define PHONE_PORT 8080
#define STREAM_PATH "/stream.mjpeg?clientId=ScreenMirrorWiiU"

#define FRAME_MAX (2 * 1024 * 1024)
#define FALLBACK_W 854
#define FALLBACK_H 480
#define RESIZE_STEP 16
#define MIN_IMAGE_SIZE 80
#define RESIZE_INTERVAL 100

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

    if (connect(sock, (struct sockaddr *)&addr,
                sizeof(addr)) < 0) {
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
        ssize_t n = send(sock, request + sent,
                         length - sent, 0);
        if (n <= 0) {
            close(sock);
            return -1;
        }
        sent += (size_t)n;
    }

    return sock;
}

static int clamp_size(int value, int maximum)
{
    if (value < MIN_IMAGE_SIZE)
        return MIN_IMAGE_SIZE;

    if (value > maximum)
        return maximum;

    return value;
}

int main(int argc, char **argv)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        return 1;

    IMG_Init(IMG_INIT_JPG);

    SDL_Window *window = SDL_CreateWindow(
        "Screen Mirror",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        0,
        0,
        SDL_WINDOW_FULLSCREEN_DESKTOP
    );

    if (!window) {
        IMG_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer =
        SDL_CreateRenderer(
            window, -1, SDL_RENDERER_ACCELERATED);

    if (!renderer) {
        SDL_DestroyWindow(window);
        IMG_Quit();
        SDL_Quit();
        return 1;
    }

    int screen_w = FALLBACK_W;
    int screen_h = FALLBACK_H;

    int output_w = 0;
    int output_h = 0;

    if (SDL_GetRendererOutputSize(
            renderer, &output_w, &output_h) == 0 &&
        output_w >= 320 && output_h >= 200) {
        screen_w = output_w;
        screen_h = output_h;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    int image_w = screen_w;
    int image_h = screen_h;

    SDL_Texture *texture = NULL;
    uint8_t *frame = malloc(FRAME_MAX);

    if (!frame) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        IMG_Quit();
        SDL_Quit();
        return 1;
    }

    int sock = -1;
    size_t frame_size = 0;
    bool in_frame = false;
    bool running = true;
    uint8_t previous = 0;

    uint32_t last_resize_time = 0;

    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                running = false;
        }

        /*
         * Resize only while BOTH L and R are held.
         * Releasing either button immediately disables
         * the resizing controls.
         */
        VPADStatus vpad;
        int32_t vpad_error = 0;

        VPADRead(VPAD_CHAN_0, &vpad, 1, &vpad_error);

        bool l_held = (vpad.hold & VPAD_BUTTON_L) != 0;
        bool r_held = (vpad.hold & VPAD_BUTTON_R) != 0;
        bool resize_mode = l_held && r_held;

        uint32_t now = SDL_GetTicks();

        if (resize_mode &&
            (now - last_resize_time >= RESIZE_INTERVAL)) {

            bool changed = false;

            if (vpad.hold & VPAD_BUTTON_LEFT) {
                image_w -= RESIZE_STEP;
                changed = true;
            }

            if (vpad.hold & VPAD_BUTTON_RIGHT) {
                image_w += RESIZE_STEP;
                changed = true;
            }

            if (vpad.hold & VPAD_BUTTON_UP) {
                image_h += RESIZE_STEP;
                changed = true;
            }

            if (vpad.hold & VPAD_BUTTON_DOWN) {
                image_h -= RESIZE_STEP;
                changed = true;
            }

            if (vpad.hold & VPAD_BUTTON_A) {
                image_w = screen_w;
                image_h = screen_h;
                changed = true;
            }

            image_w = clamp_size(image_w, screen_w);
            image_h = clamp_size(image_h, screen_h);

            if (changed)
                last_resize_time = now;
        }

        if (sock < 0) {
            sock = connect_to_phone();
            SDL_Delay(1000);
        }

        if (sock >= 0) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);

            struct timeval timeout = {0, 20000};

            int ready = select(
                sock + 1, &readfds, NULL, NULL, &timeout);

            if (ready > 0 && FD_ISSET(sock, &readfds)) {
                uint8_t data[8192];

                ssize_t count = recv(
                    sock, data, sizeof(data), 0);

                if (count <= 0) {
                    close(sock);
                    sock = -1;
                    in_frame = false;
                    frame_size = 0;
                    previous = 0;
                } else {
                    for (ssize_t i = 0; i < count; i++) {
                        uint8_t byte = data[i];

                        if (!in_frame) {
                            if (previous == 0xFF &&
                                byte == 0xD8) {
                                in_frame = true;
                                frame_size = 0;
                                frame[frame_size++] = 0xFF;
                                frame[frame_size++] = 0xD8;
                            }
                        } else if (frame_size < FRAME_MAX) {
                            frame[frame_size++] = byte;

                            if (previous == 0xFF &&
                                byte == 0xD9) {
                                SDL_RWops *rw =
                                    SDL_RWFromConstMem(
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
                                                SDL_DestroyTexture(
                                                    texture);

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
            SDL_Rect destination = {
                (screen_w - image_w) / 2,
                (screen_h - image_h) / 2,
                image_w,
                image_h
            };

            SDL_RenderCopy(
                renderer, texture, NULL, &destination);
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
hi
