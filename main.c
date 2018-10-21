//#define _POSIX_SOURCE 1 /* POSIX compliant source */
//#define _POSIX_C_SOURCE 200809L /* POSIX compliant source */

#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>

#include <time.h>

// NOTE(jason): have to use termios2 and ioctl in order to set line speed to
// non-standard values
#include <sys/ioctl.h>
//#include <asm/termbits.h>

#include <sys/mman.h>
#include <linux/videodev2.h>

#include "SDL.h"
#include "asteroids_font.h"

#include "fu.h"

static void
errno_exit(char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

static int
sdl_filter(void *userdata, SDL_Event *event)
{
    (void)userdata;
    return event->type == SDL_QUIT || event->type == SDL_KEYDOWN;
}

// currently used for v4l2
struct buffer {
    void *start;
    size_t length;
};

inline
int clamp(int n, int min, int max)
{
    if (n < min) {
        return min;
    } else if (n > max) {
        return max;
    } else {
        return n;
    }
}

// clamp a value to 0 to 255
inline
int clamp255(int n)
{
    return clamp(n, 0, 255);
}

typedef struct {
    int width;
    int height;
    int channels;
    int stride;
    int n_pixels;
    u8 *data;
} image;

image * new_image(int width, int height, int channels)
{
    image *img = malloc(sizeof(*img));
    img->n_pixels = height*width;
    img->height = height;
    img->width = width;
    img->channels = channels;
    img->stride = img->width*channels; // 1 byte per channel per pixel
    // doesn't seem like this should be zero'd
    img->data = calloc(img->n_pixels*channels, sizeof(img->data[0]));

    return img;
}

// YV12 is the MS recommended video YUV420 video format.
image *
new_yv12_image(int width, int height)
{
    image *img = malloc(sizeof(*img));
    img->n_pixels = width*height;
    img->height = height;
    img->width = width;
    img->channels = 1;
    img->stride = img->width;
    img->data = calloc(img->n_pixels + img->n_pixels/2, sizeof(img->data[0]));

    memset(img->data, 0x80, img->n_pixels + img->n_pixels/2);

    return img;
}

image * new_image_like(image *img) {
    return new_image(img->width, img->height, img->channels);
}

void free_image(image *img)
{
    if (img) {
        free(img->data);
        free(img);
    }
}

void clear_image(image *img)
{
    memset(img->data, 0, img->n_pixels*img->channels);
}

void
checkerboard_yv12(image *img, int size)
{
    u8 pixel, yfg, ybg;

    u8 black = 0x00;
    u8 white = 0xff;

    u8 *data = img->data;
    for (int y = 0; y < img->height; y++) {
        if (y % size == 0) {
            if (yfg == black) {
                yfg = white;
                ybg = black;
            } else {
                yfg = black;
                ybg = white;
            }
        }

        pixel = yfg;
        for (int x = 0; x < img->width; x++) {
            if (x % size == 0) {
                pixel = (pixel == yfg) ? ybg : yfg;
            }

            //debugf("%x %x %x", yfg, ybg, pixel);
            data[y*img->width + x] = pixel;
        }
    }
}

void
checkerboard_image(image *img, int size)
{
    u32 pixel, yfg, ybg;

    u32 black = 0xff000000;
    u32 white = 0xffffffff;

    u32 *data = (u32 *)img->data;
    for (int y = 0; y < img->height; y++) {
        if (y % size == 0) {
            if (yfg == black) {
                yfg = white;
                ybg = black;
            } else {
                yfg = black;
                ybg = white;
            }
        }

        pixel = yfg;
        for (int x = 0; x < img->width; x++) {
            if (x % size == 0) {
                pixel = (pixel == yfg) ? ybg : yfg;
            }

            //debugf("%x %x %x", yfg, ybg, pixel);
            data[y*img->width + x] = pixel;
        }
    }
}

void copy_image(image *src, image *dest)
{
    assert(src->n_pixels == dest->n_pixels);
    assert(src->channels == dest->channels);

    memcpy(dest->data, src->data, src->n_pixels*src->channels);
}

void
copy_rect_image(int width, int height, image *src, int x1, int y1, image *dest, int x2, int y2)
{
    assert(src->channels == 1);
    assert(src->channels == dest->channels);

    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            dest->data[(y2 + i)*dest->width + x2 + j] = src->data[(y1 + i)*src->width + x1 + j];
        }
    }
}

int
mix_images(image *old, image *new, const int weight)
{
    assert(old->n_pixels == new->n_pixels);
    assert(old->channels == new->channels);

    int max = old->n_pixels * old->channels;

    const int scale = 1000;
    const int new_weight = weight;
    const int old_weight = scale - new_weight;
    for (int i = 0; i < max; i++) {
        old->data[i] = (old_weight * (int)old->data[i] + new_weight * (int)new->data[i])/scale;
    }

    return 0;
}

typedef struct {
    u8 red;
    u8 green;
    u8 blue;
    u8 alpha;
} color;

const color WHITE = {
    .red = 255,
    .green = 255,
    .blue = 255,
    .alpha = 255
};

const color BLACK = {
    .red = 0,
    .green = 0,
    .blue = 0,
    .alpha = 255
};

const color RED = {
    .red = 255,
    .green = 0,
    .blue = 0,
    .alpha = 255
};

const color GREEN = {
    .red = 0,
    .green = 255,
    .blue = 0,
    .alpha = 255
};

const color BLUE = {
    .red = 0,
    .green = 0,
    .blue = 255,
    .alpha = 128
};

const color YELLOW = {
    .red = 255,
    .green = 255,
    .blue = 0,
    .alpha = 255
};

int
percent_diff_images(const image *a, const image *b, image *c, int threshold)
{
    assert(a->n_pixels == b->n_pixels);
    assert(a->channels == b->channels);
    assert(a->channels == 1);

    u32 total = 0;
    u32 diff = 0;

    int max = a->n_pixels * a->channels;
    for (int i = 0; i < max; i++) {
        total++;
        //if (abs(a->data[i] - b->data[i]) > 0) {
            //debugf("diff: a: %d, b: %d", a->data[i], b->data[i]);
        //}

        if (abs(a->data[i] - b->data[i]) > threshold) {
            c->data[i] = a->data[i];
            diff += 100;
        } else {
            c->data[i] = 0;
        }
    }

    return diff/total;
}

// https://en.wikipedia.org/wiki/Insertion_sort
void
insertion_sort_u8(u8 *a, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        u8 x = a[i];
        int j = i - 1;
        for (; j >=0 && a[j] > x; j--) {
            a[j + 1] = a[j];
        }
        a[j + 1] = x;
    }
}

void
test_insertion_sort_u8()
{
    u8 a[] = { 33, 12, 9, 3, 42, 88 };
    int n = sizeof(a)/sizeof(a[0]);

    insertion_sort_u8(a, n);

    printf("XXXXXXXX testing insertion sort\n");
    for (int i = 0; i < n; i++) {
        printf("%u, ", a[i]);
    }
    printf("\n");
}

void
median_channel(image *a, image *b, const int r, const int channel)
{
    assert(a->n_pixels == b->n_pixels);
    assert(a->channels = b->channels);

    //u64 start = SDL_GetPerformanceCounter();

    //const int r = 1;
    const int n = (2*r + 1)*(2*r + 1);
    //const int pad = n/2;
    u8 win[n];
    const int median = n/2;
    const int pad = r*a->stride + r*a->channels + channel;
    //const int imax = (a->n_pixels - r) * a->channels - r*a->stride;
    const int imax = a->n_pixels * a->channels - pad;

    memcpy(b->data, a->data, pad);
    memcpy(b->data + imax, a->data + imax, pad);

    // TODO(jason): treating this as one long 1D array causes "problems" on the
    // edge when left and right are significantly different
    for (int i = pad; i < imax; i += a->channels) {
        for (int j = 0, k = -r; k <= r; k++) {
            int x = i + k*a->channels;
            for (int y = -r; y <= r; y++) {
                win[j++] = a->data[x + y*a->stride];
            }
        }

        insertion_sort_u8(win, n);

        b->data[i] = win[median];
    }
    //debugf("median_channel: %ld", (SDL_GetPerformanceCounter() - start)/1000);
}

inline
void set_pixel(image *img, int x, int y, const color *fg)
{
    assert(x < img->width);
    assert(x >= 0);
    assert(y < img->height);
    assert(y >= 0);

    int i = y*img->width*img->channels + x*img->channels;
    u8 *data = img->data;

    if (img->channels == 4) {
        data[i] = fg->blue;
        data[i + 1] = fg->green;
        data[i + 2] = fg->red;
        data[i + 3] = fg->alpha;
    } else {
        data[i] = fg->blue;
    }
}

void fill_rect(image *img, int x, int y, int width, int height, const color *fg)
{
    x = clamp(x, 0, img->width - 1);
    y = clamp(y, 0, img->height - 1);

    int max_col = clamp(x + width, 0, img->width - 1);
    int max_row = clamp(y + height, 0, img->height - 1);

    for (int row = y; row < max_row; row++) {
        for (int col = x; col < max_col; col++) {
            set_pixel(img, col, row, fg);
        }
    }
}

void fill_square_center(image *img, int x, int y, int size, const color *fg)
{
    fill_rect(img, x - size/2, y - size/2, size, size, fg);
}

void draw_line_low(image *img, int x0, int y0, int x1, int y1, const color *fg)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int yi = 1;
    if (dy < 0) {
        yi = -1;
        dy = -dy;
    }
    int D = 2*dy - dx;

    int y = y0;
    for (int x = x0; x < x1; x++) {
        set_pixel(img, x, y, fg);
        if (D > 0) {
            y = y + yi;
            D = D - 2*dx;
        }
        D = D + 2*dy;
    }
}

void draw_line_high(image *img, int x0, int y0, int x1, int y1, const color *fg)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int xi = 1;
    if (dx < 0) {
        xi = -1;
        dx = -dx;
    }
    int D = 2*dx - dy;

    int x = x0;
    for (int y = y0; y < y1; y++) {
        set_pixel(img, x, y, fg);
        if (D > 0) {
            x = x + xi;
            D = D - 2*dy;
        }
        D = D + 2*dx;
    }
}

// https://en.wikipedia.org/wiki/Bresenham%27s_line_algorithm
void draw_line(image *img, int x0, int y0, int x1, int y1, const color *fg)
{
    if (abs(y1 - y0) < abs(x1 - x0)) {
        if (x0 > x1) {
            draw_line_low(img, x1, y1, x0, y0, fg);
        } else {
            draw_line_low(img, x0, y0, x1, y1, fg);
        }
    } else {
        if (y0 > y1) {
            draw_line_high(img, x1, y1, x0, y0, fg);
        } else {
            draw_line_high(img, x0, y0, x1, y1, fg);
        }
    }
}

void draw_rect(image *img, int x, int y, int width, int height, const color *fg)
{
    draw_line(img, x, y, x + width, y, fg);
    draw_line(img, x, y, x, y + height, fg);
    draw_line(img, x + width, y, x + width, y + height, fg);
    draw_line(img, x, y + height, x + width, y + height, fg);
}

void draw_square_center(image *img, int x, int y, int size, const color *fg)
{
    x = x - size/2;
    y = y - size/2;

    int diff = x + size - img->width;
    int width = (diff > 0) ? size - diff : size;
    diff = y + size - img->height;
    int height = (diff > 0) ? size - diff : size;

    x = clamp(x, 0, img->width - 1);
    y = clamp(y, 0, img->height - 1);

    for (int row = 0; row < height; row++) {
        if (row == 0 || row == height - 1) {
            for (int col = 0; col < width; col++) {
                if (x + col >= img->width) {
                    break;
                }
                set_pixel(img, x + col, y + row, fg);
            }
        } else {
            set_pixel(img, x, y + row, fg);
            set_pixel(img, x + width - 1, y + row, fg);
        }
    }
}

/*
 * draw a line of text.  8x12 character max of 8 points times the size.
 * returns the height of the line plus padding where the next line should be
 * drawn.
 */
int draw_text(image *img, int x, int y, int size, const color *fg, const char *text)
{
    const int max_points = 8;
    const int max_height = 12;
    int char_width = 10*size;

    const char *p = text;
    while (*p) {
        // convert lowercase to uppercase
        char c = (*p >= 'a' && *p <= 'z') ? *p & 0xDF : *p;
        const u8 * const pts = asteroids_font[c - ' '].points;

        int x0 = x, y0 = y;
        int x1, y1;

        int next_draw = 0;
        for (int i = 0; i < max_points; i++) {
            u8 delta = pts[i];
            if (delta == FONT_LAST) {
                break;
            }

            if (delta == FONT_UP) {
                // pickup pen, discontinuity
                next_draw = 0;
                continue;
            }

            u8 dx = ((delta >> 4) & 0xF) * size;
            u8 dy = (max_height - ((delta >> 0) & 0xF)) * size;

            x1 = x + dx;
            y1 = y + dy;

            if (next_draw) {
                draw_line(img, x0, y0, x1, y1, fg);
            }

            x0 = x1;
            y0 = y1;
            next_draw = 1;
        }

        p++;
        x += char_width;
    }

    return (max_height + 4) * size;
}

/*
 * draw a line of text with a black shadow (offset x and y by 1)
 */
int // y offset for next line
draw_shadow_text(image *img, int x, int y, int size, const color *fg, const char *text)
{
    draw_text(img, x + 1, y + 1, size, &BLACK, text);
    return draw_text(img, x, y, size, fg, text);
}

int draw_debugf(image *img, int y, const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, 1024, fmt, args);
    va_end(args);

    return draw_text(img, 2, y, 1, &GREEN, buf);
}

int draw_int(image *img, int x, int y, int size, const color *fg, int n)
{
    char text[16];
    snprintf(text, 16, "%d", n);
    return draw_text(img, x, y, size, fg, text);
}

// Integer operation of ITU-R standard for YCbCr(8 bits per channel) to RGB888
// https://en.wikipedia.org/wiki/YUV#Converting_between_Y%E2%80%B2UV_and_RGB
// The Y - 16 was necessary to make it look similar to yv12 texture
void yuyv2rgba(const u8 *yuyv, u8 *rgba, const int n_pixels)
{
    //u32 start_ms = SDL_GetTicks();

    int yuyv_max = n_pixels*2;
    int rgb_max = n_pixels*4;

    for (int i = 0, j = 0; i < yuyv_max && j < rgb_max; i += 4, j += 8) {
        // YUYV: Y0 U0 Y1 V0  Y2 U1 Y3 V1
        int Y0 = yuyv[i] - 16;
        int U = yuyv[i + 1] - 128;
        int Y1 = yuyv[i + 2] - 16;
        int V = yuyv[i + 3] - 128;

        int vp = V + (V >> 2) + (V >> 3) + (V >> 5);
        int uvp = -((U >> 2) + (U >> 4) + (U >> 5)) - ((V >> 1) + (V >> 3) + (V >> 4) + (V >> 5));
        int up = U + (U >> 1) + (U >> 2) + (U >> 6);

        // bgra
        rgba[j] = clamp255(Y0 + up);
        rgba[j + 1] = clamp255(Y0 + uvp);
        rgba[j + 2] = clamp255(Y0 + vp);
        rgba[j + 3] = 255;

        // bgra
        rgba[j + 4] = clamp255(Y1 + up);
        rgba[j + 5] = clamp255(Y1 + uvp);
        rgba[j + 6] = clamp255(Y1 + vp);
        rgba[j + 7] = 255;
    }

    //debugf("yuyv2rgba2: %d", SDL_GetTicks() - start_ms);
}

// copy only the luminance values
void
yuyv2y(const u8 *yuyv, u8 *out, const int n_pixels)
{
    const int yuyv_max = n_pixels*2;
    for (int i = 0, j = 0; i < yuyv_max; i += 2, j++) {
        out[j] = yuyv[i];
    }
}

int print_display_info()
{
    int display_in_use = 0; /* Only using first display */

    int i, display_mode_count;
    SDL_DisplayMode mode;
    Uint32 f;

    SDL_Log("SDL_GetNumVideoDisplays(): %i", SDL_GetNumVideoDisplays());

    display_mode_count = SDL_GetNumDisplayModes(display_in_use);
    if (display_mode_count < 1) {
        SDL_Log("SDL_GetNumDisplayModes failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Log("SDL_GetNumDisplayModes: %i", display_mode_count);

    for (i = 0; i < display_mode_count; ++i) {
        if (SDL_GetDisplayMode(display_in_use, i, &mode) != 0) {
            SDL_Log("SDL_GetDisplayMode failed: %s", SDL_GetError());
            return 1;
        }
        f = mode.format;

        SDL_Log("Mode %i\tbpp %i\t%s\t%i x %i", i,
                SDL_BITSPERPIXEL(f), SDL_GetPixelFormatName(f), mode.w, mode.h);
    }

    SDL_GetCurrentDisplayMode(display_in_use, &mode);
    SDL_Log("Current Mode\tbpp %i\t%s\t%i x %i",
            SDL_BITSPERPIXEL(f), SDL_GetPixelFormatName(f), mode.w, mode.h);

    return 0;
}

int
get_refresh_rate(SDL_Window *window)
{
    SDL_DisplayMode mode;
    int index = SDL_GetWindowDisplayIndex(window);
    // If we can't find the refresh rate, we'll return this:
    int default_rate = 60;
    if (SDL_GetDesktopDisplayMode(index, &mode) != 0) {
        return default_rate;
    }

    if (mode.refresh_rate == 0) {
        return default_rate;
    }

    return mode.refresh_rate;
}

struct capture_data {
    bool running;
    int fd;
    bool reset_race;

    u32 width;
    u32 height;

    // when v4l2 setup is in the thread this won't need to be here
    struct buffer *buffers;
    bool valid_image;
    image *image1[2];
    image *image2[2];
    int rindex;
    SDL_mutex *mutex;

    u64 laps[4];
    int n_laps;
};

int
run_capture(void *data)
{
    struct capture_data *cd = data;

    struct v4l2_streamparm vparm = {};
    vparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cd->fd, VIDIOC_G_PARM, &vparm) == -1) {
        debugf("VIDIOC_G_PARM: %s", strerror(errno));
    }

    if (vparm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) {
        vparm.parm.capture.timeperframe.numerator = 1;
        vparm.parm.capture.timeperframe.denominator = 20;
        if (ioctl(cd->fd, VIDIOC_S_PARM, &vparm) == -1) {
            debugf("VIDIOC_S_PARM: %s", strerror(errno));
        }

        if (ioctl(cd->fd, VIDIOC_G_PARM, &vparm) == -1) {
            debugf("VIDIOC_G_PARM: %s", strerror(errno));
        }
    }

    debugf("capture timeperframe: %u/%u", vparm.parm.capture.timeperframe.numerator, vparm.parm.capture.timeperframe.denominator);

    enum v4l2_buf_type vtype;
    vtype = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cd->fd, VIDIOC_STREAMON, &vtype) == -1) {
        errno_exit("VIDIOC_STREAMON");
    }

    struct v4l2_buffer vbuf;
    vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vbuf.memory = V4L2_MEMORY_MMAP;

    // TODO(jason): maybe read these from cam fd with VIDIOC_G_FMT
    // probably should be setting camera settings
    u32 cam_width = cd->width;
    u32 cam_height = cd->height;

    const int n_laps = 3;
    //const u64 min_lap_count = 2*SDL_GetPerformanceFrequency();
    const double min_lap_time = 2.0;
    double lap_times[n_laps];
    u64 race_start = 0;
    u64 lap_start = 0;
    int fastest_lap = 0;
    int lap = 0;
    bool race_logged = false;

    cd->image1[0] = new_yv12_image(cd->width, cd->height);
    cd->image1[1] = new_yv12_image(cd->width, cd->height);
    cd->image2[0] = new_image(cd->width, cd->height, 4);
    cd->image2[1] = new_image(cd->width, cd->height, 4);
    cd->rindex = 0;

    image *finish_line = new_image(64, 256, 1);
    image *bg_finish_line = new_image_like(finish_line);
    image *initial_bg_finish_line = new_image_like(finish_line);
    image *tmp_finish_line = new_image_like(finish_line);
    int finish_line_x = cam_width/2 - finish_line->width/2;
    int finish_line_y = cam_height - finish_line->height - 32;
    bool finish_line_active = false;
    // if the background has been mixed enough and the finish line should be
    // checked
    bool finish_line_valid = false;
    int motion_threshold = 8;

    cd->running = true;

    const int require_bg_frames = 60;
    const int n_usable_frames  = require_bg_frames - require_bg_frames/3;
    int need_bg_frames = require_bg_frames;

    u64 last_bg_mix = 0;
    u64 bg_mix_count = 60 * SDL_GetPerformanceFrequency();

    while (cd->running) {
        //s64 start = SDL_GetPerformanceCounter();
        //debugf("got frame: %d, elapsed: %ld", vbuf.index, (start - last_frame_count)/1000);
        //last_frame_count = start;

        if (cd->reset_race) {
            lap = 0;
            race_logged = false;
            race_start = 0;
            lap_start = 0;
            fastest_lap = 0;
            cd->reset_race = false;
            //need_bg_frames = require_bg_frames;
        }

        if (ioctl(cd->fd, VIDIOC_DQBUF, &vbuf) == -1) {
            // TODO(jason): just close fd and get EBADF force exit?
            debugf("thread no camera image %d: %s", errno, strerror(errno));
            break;
        }
        //debugf("VIDIOC_DQBUF: %ld", (SDL_GetPerformanceCounter() - start)/1000);

        image *write1_image = cd->image1[!cd->rindex];
        image *write2_image = cd->image2[!cd->rindex];

        yuyv2y(cd->buffers[vbuf.index].start, write1_image->data, write1_image->n_pixels);
        yuyv2rgba(cd->buffers[vbuf.index].start, write2_image->data, write2_image->n_pixels);
        if (ioctl(cd->fd, VIDIOC_QBUF, &vbuf) == -1) {
            debugf("VIDIOC_QBUF: %s", strerror(errno));
        }


        copy_rect_image(finish_line->width, finish_line->height, write1_image, finish_line_x, finish_line_y, tmp_finish_line, 0, 0);
        median_channel(tmp_finish_line, finish_line, 2, 0);
        //copy_image(tmp_finish_line, finish_line);

        u64 now = SDL_GetPerformanceCounter();

        if (now - last_bg_mix > bg_mix_count) {
            need_bg_frames++;
            debugf("need more bg mix (%d)", need_bg_frames);
        }

        // TODO(jason): this shouldn't be either or.  It should ALSO add to background if necessary
        if (need_bg_frames) {
            last_bg_mix = now;

            if (need_bg_frames == n_usable_frames) {
                // initialize mix
                copy_image(finish_line, bg_finish_line);
                cd->valid_image = true;
            } else if (need_bg_frames < n_usable_frames) {
                mix_images(bg_finish_line, finish_line, 10);
                draw_shadow_text(write1_image, 2, 2, 1, &WHITE, "mixing background");
            } else {
                draw_shadow_text(write1_image, 2, 2, 1, &WHITE, "skipping frame");
                // ignore some frames at the beginning
            }
            need_bg_frames--;

            if (need_bg_frames == 0 && !finish_line_valid) {
                finish_line_valid = true;
                copy_image(bg_finish_line, initial_bg_finish_line);
            }

            //debugf("background frame.  remaining: %d", need_bg_frames);
        }

        // as long as a race is running update the current lap time
        if (lap_start > 0 && lap < n_laps) {
            lap_times[lap] = (double)(now - lap_start)/SDL_GetPerformanceFrequency();
        }

        if (finish_line_valid) {
            int percent = percent_diff_images(finish_line, bg_finish_line, tmp_finish_line, motion_threshold);
            draw_int(write1_image, 2, finish_line->height + 2, 1, &WHITE, percent);
            const color *highlight;
            if (percent > 20) { // motion threshold
                if (!finish_line_active) {
                    finish_line_active = true;

                    if (lap < n_laps) {
                        if (lap_start > 0) {
                            if (lap_times[lap] > min_lap_time) {
                                if (lap == 0 || lap_times[lap] < lap_times[fastest_lap]) {
                                    fastest_lap = lap;
                                }

                                lap_start = now;
                                lap++;
                            } else {
                                debug("ignoring too fast lap!!!");
                            }
                        } else {
                            lap_start = now;
                            race_start = now;
                        }
                    }
                }

                highlight = &GREEN;
            } else {
                highlight = &WHITE;
                finish_line_active = false;
            }

            int x = 0;
            int y = 0;
            copy_rect_image(finish_line->width, finish_line->height, finish_line, 0, 0, write1_image, x, y);
            x += bg_finish_line->width + 2;
            copy_rect_image(finish_line->width, finish_line->height, bg_finish_line, 0, 0, write1_image, x, y);
            x += bg_finish_line->width + 2;
            copy_rect_image(finish_line->width, finish_line->height, initial_bg_finish_line, 0, 0, write1_image, x, y);
            //draw_rect(write1_image, 0, 0, finish_line->width, finish_line->height, highlight);
            draw_rect(write1_image, finish_line_x, finish_line_y, finish_line->width, finish_line->height, highlight);
            draw_rect(write2_image, finish_line_x, finish_line_y, finish_line->width, finish_line->height, highlight);
        }

        if (race_start > 0) {
            int x = 2;
            int y = 2;

            char text[256];
            double total = 0.0;
            for (int i = 0; i <= lap && i < n_laps; i++) {
                snprintf(text, 256, "lap %d: %.3f %s", i + 1, lap_times[i], fastest_lap == i ? "fastest" : "");
                total += lap_times[i];
                y += draw_shadow_text(write2_image, x, y, 1, &GREEN, text);
            }

            if (lap > 0) {
                snprintf(text, 256, "total: %.3f", total);
                draw_shadow_text(write2_image, x, y, 1, &GREEN, text);
            }
        }

        if (lap == n_laps && !race_logged) {
            race_logged = true;
            debug("XXX race is over XXXX");
            FILE *f = fopen("race.log", "a");
            if (f) {
                double total = 0.0;
                for (int i = 0; i < n_laps; i++) {
                    total += lap_times[i];
                    fprintf(f, "%.3f,", lap_times[i]);
                }
                fprintf(f, "%.3f\n", total);
                fclose(f);
            }
        }


        // update current image frame for display
        if (SDL_LockMutex(cd->mutex) == 0) {
            cd->rindex = !cd->rindex;
            SDL_UnlockMutex(cd->mutex);
        }
    }

    if (ioctl(cd->fd, VIDIOC_STREAMOFF, &vtype) == -1) {
        errno_exit("VIDIOC_STREAMOFF");
    }

    return 0;
}

int
main(int argc, char *argv[])
{
    // target size for the whole window/frame
    // c920 only supports YUYV 720p @ 10fps, MJPEG or H264 @ 30fps
    // MacBook Only YUYV @ 5fps, no other image formats
    int frame_width = 1280;
    int frame_height = 720;
    //int frame_width = 640;
    //int frame_height = 480;

    // 60 for UI and 20 for camera is probably reasonable
    // 60 causes lockups, not sure where
    const u64 target_fps = 30;
    //const u64 camera_fps = 30;

    // target size for camera capture frame (v4l2)
    // TODO(jason): for these to be different from frame_width/height requires a
    // separate texture for drawing overlays instead of frame_rgba
    u32 cam_width = 640;
    u32 cam_height = 480;

    char *vdev_name = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i - 1], "-d") == 0) {
            vdev_name = strdup(argv[i]);
        }
    }

    // setup v4l
    struct buffer *buffers;
    unsigned int n_buffers;
    struct v4l2_format fmt = {};

    if (!vdev_name) {
        char *names[] = {
            "/dev/video2",
            "/dev/video1",
            "/dev/video0"
        };

        struct stat st;

        for (int i = 0; i < 3; i++) {
            if (stat(names[i], &st) != -1) {
                vdev_name = names[i];
                break;
            }
        }

        if (!vdev_name) {
            errno_exit("video device");
        }
    }

    debugf("vdev_name: %s", vdev_name);

    int cam_fd = open(vdev_name, O_RDWR, 0);
    if (cam_fd == -1) {
        errno_exit(vdev_name);
    }

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_G_FMT, &fmt) == -1) {
        errno_exit("VIDIOC_G_FMT");
    }

    uint32_t fourcc = fmt.fmt.pix.pixelformat;
    debugf("device: %s, width: %d, height: %d, fourcc: %c%c%c%c",
            vdev_name, fmt.fmt.pix.width, fmt.fmt.pix.height,
            fourcc & 0xff, fourcc >> 8 & 0xff, fourcc >> 16 & 0xff, fourcc >> 24 & 0xff);

    // larger sizes are too slow, but may be due to VM
    fmt.fmt.pix.width = cam_width;
    fmt.fmt.pix.height = cam_height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    if (ioctl(cam_fd, VIDIOC_S_FMT, &fmt) == -1) {
        errno_exit("VIDIOC_S_FMT");
    }

    if (ioctl(cam_fd, VIDIOC_G_FMT, &fmt) == -1) {
        errno_exit("VIDIOC_G_FMT");
    }

    fourcc = fmt.fmt.pix.pixelformat;
    debugf("device: %s, width: %d, height: %d, fourcc: %c%c%c%c",
            vdev_name, fmt.fmt.pix.width, fmt.fmt.pix.height,
            fourcc & 0xff, fourcc >> 8 & 0xff, fourcc >> 16 & 0xff, fourcc >> 24 & 0xff);

    if (fmt.fmt.pix.width != cam_width || fmt.fmt.pix.height != cam_height) {
        SDL_Log("camera width x height (%d x %d) not supported", cam_width, cam_height);
        errno_exit("VIDIOC_G_FMT");
    }

    //int width = fmt.fmt.pix.width;
    //int height = fmt.fmt.pix.height;

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(cam_fd, VIDIOC_REQBUFS, &req) == -1) {
        errno_exit("VIDIOC_REQBUFS");
    }

    //debugf("allocated %d buffers", req.count);

    buffers = calloc(req.count, sizeof(*buffers));
    if (!buffers) {
        errno_exit("calloc");
    }

    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer vbuf;
        memset(&vbuf, 0, sizeof(vbuf));

        vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vbuf.memory = V4L2_MEMORY_MMAP;
        vbuf.index = i;

        if (ioctl(cam_fd, VIDIOC_QUERYBUF, &vbuf) == -1) {
            errno_exit("VIDIOC_QUERYBUF");
        }

        buffers[i].length = vbuf.length;
        buffers[i].start = mmap(NULL, vbuf.length,
                PROT_READ | PROT_WRITE, MAP_SHARED,
                cam_fd, vbuf.m.offset);

        //debugf("buffers[%u]: %p", i, buffers[i].start);

        if (buffers[i].start == MAP_FAILED) {
            errno_exit("mmap");
        }
    }

    n_buffers = req.count;

    for (unsigned int i = 0; i < n_buffers; i++) {
        struct v4l2_buffer vbuf;
        memset(&vbuf, 0, sizeof(vbuf));

        vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vbuf.memory = V4L2_MEMORY_MMAP;
        vbuf.index = i;

        if (ioctl(cam_fd, VIDIOC_QBUF, &vbuf) == -1) {
            errno_exit("VIDIOC_QBUF");
        }
    }


    // Setup SDL2

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;

    if (SDL_Init(SDL_INIT_VIDEO)) {
        debug("Unable to initialize SDL");
        errno_exit("SDL_Init");
    }

    print_display_info();

    u32 win_flags = SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_OPENGL;
    window = SDL_CreateWindow("ABE",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            frame_width, frame_height, win_flags);
    if (!window) {
        debugf("Could not create window: %s", SDL_GetError());
        errno_exit("SDL_CreateWindow");
    }

    // NOTE(jason): in Parallels, this is SDL_PIXELFORMAT_RGB888
    debugf("window pixel format: %s", SDL_GetPixelFormatName(SDL_GetWindowPixelFormat(window)));

    // NOTE(jason): VSYNC doesn't work within Parallels.  The SDL Renderer
    // info doesn't include it either when requested.
    renderer = SDL_CreateRenderer(window, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        debugf("Could not create renderer: %s", SDL_GetError());
        errno_exit("SDL_CreateRenderer");
    }

    // make the scaled rendering look smoother.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    if (SDL_RenderSetLogicalSize(renderer, frame_width, frame_height)) {
        debugf("SDL_RenderSetLogicalSize failed: %s", SDL_GetError());
    }

    SDL_RendererInfo info; SDL_GetRendererInfo(renderer, &info);
    for (u32 i = 0; i < info.num_texture_formats; i++) {
        debugf("renderer texture pixel format[%d]: %s", i, SDL_GetPixelFormatName(info.texture_formats[i]));
    }

    SDL_Texture *texture = NULL;
    texture = SDL_CreateTexture(renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            frame_width, frame_height);
    if (!texture) {
        debugf("SDL_CreateTexture failed: %s", SDL_GetError());
        errno_exit("SDL_CreateTexture");
    }
    // setting blend mode may no longer be necessary
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

    u32 pixel_format;
    SDL_QueryTexture(texture, &pixel_format, NULL, NULL, NULL);
    debugf("texture pixel format: %s", SDL_GetPixelFormatName(pixel_format));

    SDL_SetEventFilter(sdl_filter, NULL);

    SDL_Surface *sshot = NULL;

    SDL_Texture *texture1 = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, cam_width, cam_height);
    if (texture1) {
        SDL_QueryTexture(texture1, &pixel_format, NULL, NULL, NULL);
        debugf("texture1 format: %s", SDL_GetPixelFormatName(pixel_format));
    } else {
        debugf("SDL_CreateTexture failed: %s", SDL_GetError());
        errno_exit("SDL_CreateTexture: texture1");
    }

    SDL_Texture *texture2 = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, cam_width, cam_height);
    if (texture2) {
        SDL_QueryTexture(texture2, &pixel_format, NULL, NULL, NULL);
        debugf("texture2 format: %s", SDL_GetPixelFormatName(pixel_format));
    } else {
        debugf("SDL_CreateTexture failed: %s", SDL_GetError());
        errno_exit("SDL_CreateTexture: texture2");
    }

    image *frame_rgba = new_image(frame_width, frame_height, 4);

    image *checkerboard1 = new_yv12_image(cam_width, cam_height);
    image *checkerboard2 = new_image(cam_width, cam_height, 4);
    checkerboard_yv12(checkerboard1, 32);
    checkerboard_image(checkerboard2, 32);

    bool fullscreen = true;
    bool capture = false;
    bool record = false;
    int record_frame = 0;

    // NOTE(jason): run main loop at 30fps
    const u64 count_per_s = SDL_GetPerformanceFrequency();
    assert(count_per_s > 1000);
    const u64 count_per_ms = count_per_s/1000;
    assert(count_per_ms > 0);
    const u64 count_per_frame = (count_per_s/target_fps);

    bool running = true;
    SDL_Event event;

    u64 start_count = 0;
    u64 end_count;
    u64 elapsed_count;


    struct capture_data capture_data = {};
    capture_data.mutex = SDL_CreateMutex();
    capture_data.fd = cam_fd;
    capture_data.buffers = buffers;
    capture_data.width = cam_width;
    capture_data.height = cam_height;

    SDL_Thread *capture_thread = SDL_CreateThread(run_capture, "capture", &capture_data);


    // XXXXXXXXXXXXXXXXXXX Begin Main Loop XXXXXXXXXXXXXXXXXXX

    while (running) {
        // seems like this should switch to using clock_nanosleep for frame timing
        end_count = SDL_GetPerformanceCounter();
        elapsed_count = end_count - start_count;
        if (elapsed_count < count_per_frame) {
            //debugf("elapsed: %lu, start: %lu, end: %lu", elapsed_count, start_count, end_count);
            u32 delay = (count_per_frame - elapsed_count)/count_per_ms; 
            //debugf("delay: %u", delay);
            if (delay > 1) {
                // nanosleep?
                SDL_Delay(delay - 1);
            }

            //while loop for trimming off the rest of the delay
            while ((SDL_GetPerformanceCounter() - start_count) < count_per_frame) {
                // wasting time
            }
        } else {
            debugf("XXX over frame time: %lu", elapsed_count);
        }

        end_count = SDL_GetPerformanceCounter();

        // actual start of "frame", seems like this should be end_count above
        start_count = SDL_GetPerformanceCounter();

        // start frame

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                debug("quit");
                running = false;
            } else if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                switch (event.key.keysym.sym) {
                    case SDLK_f:
                        // TODO(jason): this doesn't really work great.  Doesn't show the window chrome properly
                        if (fullscreen) {
                            fullscreen = false;
                            SDL_SetWindowFullscreen(window, 0);
                        } else {
                            fullscreen = true;
                            if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP)) {
                                SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Fullscreen failed: %s", SDL_GetError());
                            }
                        }
                        break;
                    case SDLK_c: // capture raw YUYV still
                        capture = true;
                        break;
                    case SDLK_n:
                        if (event.key.keysym.mod & KMOD_CTRL) {
                            debug("reset race");
                            capture_data.reset_race = true;
                        }
                        break;
                    case SDLK_r:
                        record = !record;
                        record_frame = 1;
                        break;
                    case SDLK_q:
                        debug("q quit");
                        running = false;
                        break;
                    //default:
                    //    debugf("key down: %s", SDL_GetKeyName(event.key.keysym.sym));
                }
            }
        }

        if (!running) break;

        clear_image(frame_rgba);

        fill_rect(frame_rgba, 0, 480, frame_width, 240, &WHITE);

        if (record) {
            fill_square_center(frame_rgba, 4, 4, 4, &RED);
        }

        if (SDL_LockMutex(capture_data.mutex) == 0) {
            if (capture_data.valid_image) {
                image *img = capture_data.image1[capture_data.rindex];
                SDL_UpdateTexture(texture1, NULL, img->data, img->stride);

                img = capture_data.image2[capture_data.rindex];
                SDL_UpdateTexture(texture2, NULL, img->data, img->stride);
            } else {
                SDL_UpdateTexture(texture1, NULL, checkerboard1->data, checkerboard1->stride);
                SDL_UpdateTexture(texture2, NULL, checkerboard2->data, checkerboard2->stride);
            }

            SDL_UnlockMutex(capture_data.mutex);
        }

        // this is like 5-6ms if the texture pixel format doesn't
        // match what is supported by the SDL_Renderer?
        SDL_UpdateTexture(texture, NULL, frame_rgba->data, frame_rgba->stride);

        //draw some text stats on screen

        // make the background white so when testing at night there's some
        // light in the camera view
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        const SDL_Rect cam_dst = {
            .x = 0,
            .y = 0,
            .w = 640,
            .h = 480 
        };
        const SDL_Rect bg_dst = {
            .x = 640,
            .y = 0,
            .w = 640,
            .h = 480 
        };
        SDL_RenderCopy(renderer, texture1, NULL, &cam_dst);
        SDL_RenderCopy(renderer, texture2, NULL, &bg_dst);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        // TODO(jason): move all this image writing to a separate thread
        if (record || capture) {
            capture = false;

            char filename[256];
            if (record) {
                snprintf(filename, 256, "record-%07d.bmp", record_frame); 
                record_frame++;
            } else {
                //snprintf(filename, 256, "capture-%ld.%ld.bmp", (uint64_t)vbuf.timestamp.tv_sec, vbuf.timestamp.tv_usec); 
                snprintf(filename, 256, "capture-%ld.bmp", SDL_GetPerformanceCounter()); 
            }

            // this takes 3-5ms, mostly in SDL_RenderReadPixels
            //u32 sshot_ms = SDL_GetTicks();
            if (!sshot) {
                int w, h;
                SDL_GetRendererOutputSize(renderer, &w, &h);
                sshot = SDL_CreateRGBSurface(0, w, h, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
                if (!sshot) {
                    debugf("Unable to allocate surface from screenshot: %s", SDL_GetError());
                    errno_exit("SDL_CreateRGBSurface: sshot");
                }
            }
            SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, sshot->pixels, sshot->pitch);
            //debugf("read screen: %d", SDL_GetTicks() - sshot_ms);
            SDL_SaveBMP(sshot, filename);
            //debugf("save screenshot: %d", SDL_GetTicks() - sshot_ms);
        }
    }

    // XXXXXXXXXXXXXXXXXXX End Main Loop XXXXXXXXXXXXXXXXXXX

    // probably not thread safe?
    capture_data.running = false;

    if (capture_thread) {
        int tr;
        SDL_WaitThread(capture_thread, &tr);
        debugf("capture thread returned: %d", tr);
    }

    for (unsigned int i = 0; i < n_buffers; i++) {
        if (munmap(buffers[i].start, buffers[i].length) == -1) {
            perror("munmap");
        }
    }
    free(buffers);

    if (close(cam_fd) == -1) {
        perror("cam close");
    }

    free_image(frame_rgba);

    if (sshot) SDL_FreeSurface(sshot);
    // NOTE(jason): SDL_DestroyRenderer frees all textures
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();

    exit(EXIT_SUCCESS);
}

