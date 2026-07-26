/*
 * redact.c — minimal PNG redaction tool.
 *
 * Usage: redact input.png output.png
 *
 * Controls:
 *   drag (left mouse)   redact: black out a rectangle
 *   ctrl+z              undo last rectangle
 *   s                   save to output.png
 *   h / l               pan left / right
 *   j / k               pan down / up
 *   + / -               zoom in / out
 *   0                   reset zoom/pan
 *   q / Esc             quit
 *
 * The saved PNG contains only IHDR/IDAT/IEND chunks — no text, time, or
 * color-profile metadata is ever written.
 *
 * Build:
 *   gcc redact.c -o redact $(pkg-config --cflags --libs libpng x11) -lm
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TEXT_MARGIN 22
#define HELP_TEXT \
    "drag=redact  ctrl+z=undo  s=save  hjkl=pan  +/-=zoom  0=reset  q/esc=quit"

typedef struct {
    int width, height;
    unsigned char *rgb; /* 3 bytes/pixel, row-major, no padding */
} Image;

typedef struct {
    int x0, y0, x1, y1;
} Rect;

typedef struct {
    double zoom;         /* screen px per image px */
    double cam_x, cam_y; /* image-space point mapped to view center */
    int view_w, view_h;  /* canvas area, excludes the help-text strip */
} View;

/* ---------- PNG I/O (no metadata is ever read back out or written) ---------- */

static int load_png(const char *path, Image *img) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", path); return -1; }

    unsigned char sig[8];
    if (fread(sig, 1, 8, fp) != 8 || png_sig_cmp(sig, 0, 8)) {
        fprintf(stderr, "%s is not a PNG file\n", path);
        fclose(fp);
        return -1;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) {
        fprintf(stderr, "libpng error while reading %s\n", path);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    png_uint_32 w, h;
    int bit_depth, color_type;
    png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, NULL, NULL, NULL);

    /* Normalize to 8-bit RGB (drop alpha, expand palette/gray). Any
     * ancillary chunks (tEXt, tIME, iCCP, eXIf, ...) are simply never
     * requested, so they're dropped on load. */
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (bit_depth == 16)
        png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_RGB_ALPHA || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_strip_alpha(png);

    png_read_update_info(png, info);

    img->width = (int)w;
    img->height = (int)h;
    img->rgb = malloc((size_t)w * h * 3);
    if (!img->rgb) {
        fprintf(stderr, "Out of memory\n");
        fclose(fp);
        return -1;
    }

    png_bytep *rows = malloc(sizeof(png_bytep) * h);
    for (png_uint_32 y = 0; y < h; y++)
        rows[y] = img->rgb + (size_t)y * w * 3;

    png_read_image(png, rows);
    png_read_end(png, NULL);

    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return 0;
}

static int save_png(const char *path, Image *img) {
    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "Cannot open %s for writing\n", path); return -1; }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) {
        fprintf(stderr, "libpng error while writing %s\n", path);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);
    /* Only IHDR/IDAT/IEND are written below — no tEXt/tIME/iCCP/etc, so
     * the output file carries no metadata beyond raw pixels. */
    png_set_IHDR(png, info, img->width, img->height, 8,
                 PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    png_bytep *rows = malloc(sizeof(png_bytep) * img->height);
    for (int y = 0; y < img->height; y++)
        rows[y] = img->rgb + (size_t)y * img->width * 3;

    png_write_image(png, rows);
    png_write_end(png, NULL);

    free(rows);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}

/* ---------- Redaction ---------- */

static void fill_black_rect(Image *img, int x0, int y0, int x1, int y1) {
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= img->width) x1 = img->width - 1;
    if (y1 >= img->height) y1 = img->height - 1;

    for (int y = y0; y <= y1; y++) {
        unsigned char *row = img->rgb + (size_t)y * img->width * 3;
        for (int x = x0; x <= x1; x++) {
            row[x * 3 + 0] = 0;
            row[x * 3 + 1] = 0;
            row[x * 3 + 2] = 0;
        }
    }
}

static void image_copy_from(Image *dst, Image *src) {
    dst->width = src->width;
    dst->height = src->height;
    memcpy(dst->rgb, src->rgb, (size_t)src->width * src->height * 3);
}

/* Rebuild img from the pristine original plus every rect in history
 * (used for undo, so an undone rectangle leaves no trace). */
static void reapply_rects(Image *img, Image *orig, Rect *rects, int count) {
    int i;
    image_copy_from(img, orig);
    for (i = 0; i < count; i++)
        fill_black_rect(img, rects[i].x0, rects[i].y0, rects[i].x1, rects[i].y1);
}

/* ---------- Viewer (pan/zoom, centered, black background) ---------- */

static double fit_zoom(Image *img, View *v) {
    double zx = (double)v->view_w / img->width;
    double zy = (double)v->view_h / img->height;
    return zx < zy ? zx : zy;
}

static void view_reset(Image *img, View *v) {
    v->zoom = fit_zoom(img, v);
    v->cam_x = img->width / 2.0;
    v->cam_y = img->height / 2.0;
}

static void screen_to_image(View *v, int sx, int sy, int *ix, int *iy) {
    *ix = (int)floor((sx - v->view_w / 2.0) / v->zoom + v->cam_x);
    *iy = (int)floor((sy - v->view_h / 2.0) / v->zoom + v->cam_y);
}

/* Render the current view (image resampled with pan/zoom, black outside
 * image bounds) into a freshly allocated 32-bit BGRA buffer. */
static unsigned char *render_canvas(Image *img, View *v) {
    unsigned char *buf = calloc((size_t)v->view_w * v->view_h, 4); /* black */
    int sy, sx;
    for (sy = 0; sy < v->view_h; sy++) {
        int iy = (int)floor((sy - v->view_h / 2.0) / v->zoom + v->cam_y);
        unsigned char *row, *dst;
        if (iy < 0 || iy >= img->height) continue;
        row = img->rgb + (size_t)iy * img->width * 3;
        dst = buf + (size_t)sy * v->view_w * 4;
        for (sx = 0; sx < v->view_w; sx++) {
            int ix = (int)floor((sx - v->view_w / 2.0) / v->zoom + v->cam_x);
            unsigned char *px;
            if (ix < 0 || ix >= img->width) continue;
            px = row + ix * 3;
            dst[sx * 4 + 0] = px[2]; /* B */
            dst[sx * 4 + 1] = px[1]; /* G */
            dst[sx * 4 + 2] = px[0]; /* R */
        }
    }
    return buf;
}

int main(int argc, char **argv) {
    Image img, orig;
    Rect *rects = NULL;
    int rect_count = 0, rect_cap = 0;
    Display *dpy;
    int screen, depth, max_w, max_h, win_w, win_h;
    Visual *visual;
    View v;
    Window win;
    XSizeHints hints;
    GC gc;
    XFontStruct *font;
    Atom wm_delete;
    XImage *ximage;
    int dragging = 0, sx = 0, sy = 0, cx = 0, cy = 0, saved_since_edit = 1, running = 1;
    double pan_step = 40; /* screen px per key press */

    if (argc != 3) {
        fprintf(stderr, "Usage: %s input.png output.png\n", argv[0]);
        return 1;
    }

    if (load_png(argv[1], &img) != 0)
        return 1;
    orig.rgb = malloc((size_t)img.width * img.height * 3);
    if (!orig.rgb) { fprintf(stderr, "Out of memory\n"); free(img.rgb); return 1; }
    image_copy_from(&orig, &img);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open X display\n");
        free(img.rgb);
        return 1;
    }

    screen = DefaultScreen(dpy);
    visual = DefaultVisual(dpy, screen);
    depth = DefaultDepth(dpy, screen);
    if (depth < 24 || visual->class != TrueColor) {
        fprintf(stderr, "This tool needs a 24/32-bit TrueColor display.\n");
        XCloseDisplay(dpy);
        free(img.rgb);
        return 1;
    }

    max_w = DisplayWidth(dpy, screen) - 100;
    max_h = DisplayHeight(dpy, screen) - 100;
    win_w = img.width + 40;
    win_h = img.height + 40 + TEXT_MARGIN;
    if (win_w > max_w) win_w = max_w;
    if (win_h > max_h) win_h = max_h;
    if (win_w < 300) win_w = 300;
    if (win_h < 200) win_h = 200;

    v.view_w = win_w;
    v.view_h = win_h - TEXT_MARGIN;
    view_reset(&img, &v);

    /* Black background: window bg and border both black. */
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                               win_w, win_h, 0,
                               BlackPixel(dpy, screen),
                               BlackPixel(dpy, screen));
    XStoreName(dpy, win, "redact");
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask | KeyPressMask);

    /* Non-resizable: keeps the pan/zoom math simple. */
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = win_w;
    hints.min_height = hints.max_height = win_h;
    XSetWMNormalHints(dpy, win, &hints);

    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, WhitePixel(dpy, screen));
    font = XLoadQueryFont(dpy, "fixed");
    if (font) XSetFont(dpy, gc, font->fid);

    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    ximage = XCreateImage(dpy, visual, depth, ZPixmap, 0,
                           (char *)render_canvas(&img, &v), v.view_w, v.view_h, 32, 0);

    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            XPutImage(dpy, win, gc, ximage, 0, 0, 0, 0, v.view_w, v.view_h);
            if (dragging)
                XDrawRectangle(dpy, win, gc,
                                sx < cx ? sx : cx, sy < cy ? sy : cy,
                                abs(cx - sx), abs(cy - sy));
            XDrawString(dpy, win, gc, 8, win_h - 7, HELP_TEXT, (int)strlen(HELP_TEXT));
            break;

        case ButtonPress:
            if (ev.xbutton.button == Button1 && ev.xbutton.y < v.view_h) {
                dragging = 1;
                sx = cx = ev.xbutton.x;
                sy = cy = ev.xbutton.y;
            }
            break;

        case MotionNotify:
            if (dragging) {
                cx = ev.xmotion.x;
                cy = ev.xmotion.y;
                if (cy > v.view_h) cy = v.view_h;
                XPutImage(dpy, win, gc, ximage, 0, 0, 0, 0, v.view_w, v.view_h);
                XDrawRectangle(dpy, win, gc,
                                sx < cx ? sx : cx, sy < cy ? sy : cy,
                                abs(cx - sx), abs(cy - sy));
            }
            break;

        case ButtonRelease:
            if (ev.xbutton.button == Button1 && dragging) {
                int ix0, iy0, ix1, iy1;
                dragging = 0;
                cx = ev.xbutton.x;
                cy = ev.xbutton.y;
                if (cy > v.view_h) cy = v.view_h;

                screen_to_image(&v, sx, sy, &ix0, &iy0);
                screen_to_image(&v, cx, cy, &ix1, &iy1);

                if (rect_count == rect_cap) {
                    rect_cap = rect_cap ? rect_cap * 2 : 16;
                    rects = realloc(rects, sizeof(Rect) * rect_cap);
                }
                rects[rect_count].x0 = ix0; rects[rect_count].y0 = iy0;
                rects[rect_count].x1 = ix1; rects[rect_count].y1 = iy1;
                rect_count++;

                fill_black_rect(&img, ix0, iy0, ix1, iy1);
                saved_since_edit = 0;

                XDestroyImage(ximage);
                ximage = XCreateImage(dpy, visual, depth, ZPixmap, 0,
                                       (char *)render_canvas(&img, &v), v.view_w, v.view_h, 32, 0);
                XPutImage(dpy, win, gc, ximage, 0, 0, 0, 0, v.view_w, v.view_h);
                XDrawString(dpy, win, gc, 8, win_h - 7, HELP_TEXT, (int)strlen(HELP_TEXT));
            }
            break;

        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int need_redraw = 0;

            if (ks == XK_z && (ev.xkey.state & ControlMask)) {
                if (rect_count > 0) {
                    rect_count--;
                    reapply_rects(&img, &orig, rects, rect_count);
                    saved_since_edit = 0;
                    need_redraw = 1;
                }
            } else if (ks == XK_s || ks == XK_S) {
                if (save_png(argv[2], &img) == 0) {
                    printf("Saved %s\n", argv[2]);
                    saved_since_edit = 1;
                }
            } else if (ks == XK_q || ks == XK_Q || ks == XK_Escape) {
                running = 0;
            } else if (ks == XK_h) {
                v.cam_x -= pan_step / v.zoom; need_redraw = 1;
            } else if (ks == XK_l) {
                v.cam_x += pan_step / v.zoom; need_redraw = 1;
            } else if (ks == XK_j) {
                v.cam_y += pan_step / v.zoom; need_redraw = 1;
            } else if (ks == XK_k) {
                v.cam_y -= pan_step / v.zoom; need_redraw = 1;
            } else if (ks == XK_plus || ks == XK_equal || ks == XK_KP_Add) {
                v.zoom *= 1.25; if (v.zoom > 20.0) v.zoom = 20.0; need_redraw = 1;
            } else if (ks == XK_minus || ks == XK_KP_Subtract) {
                v.zoom *= 0.8; if (v.zoom < 0.02) v.zoom = 0.02; need_redraw = 1;
            } else if (ks == XK_0) {
                view_reset(&img, &v); need_redraw = 1;
            }

            if (need_redraw) {
                XDestroyImage(ximage);
                ximage = XCreateImage(dpy, visual, depth, ZPixmap, 0,
                                       (char *)render_canvas(&img, &v), v.view_w, v.view_h, 32, 0);
                XPutImage(dpy, win, gc, ximage, 0, 0, 0, 0, v.view_w, v.view_h);
                XDrawString(dpy, win, gc, 8, win_h - 7, HELP_TEXT, (int)strlen(HELP_TEXT));
            }
            break;
        }

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == wm_delete)
                running = 0;
            break;
        }
    }

    if (!saved_since_edit)
        fprintf(stderr, "Note: quit with unsaved changes.\n");

    XDestroyImage(ximage);
    if (font) XFreeFont(dpy, font);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    free(img.rgb);
    free(orig.rgb);
    free(rects);
    return 0;
}
