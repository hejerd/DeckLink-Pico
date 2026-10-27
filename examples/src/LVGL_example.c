/*****************************************************************************
* | File      	:   LCD_2in41_LVGL_test.c
* | Author      :   Waveshare team
* | Function    :   2.41inch LCD  test demo
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2024-06-27
* | Info        :
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
******************************************************************************/
#include "LVGL_example.h" 
#include "src/core/lv_obj.h"
#include "src/misc/lv_area.h"
#include "pico/multicore.h"
#include "psram_tool.h"
#include "rp_pico_alloc.h"
#include <string.h>
#include <stdlib.h>

LV_FONT_DECLARE(font_kr_20);
// LVGL
static lv_disp_drv_t disp_drv;
static lv_disp_draw_buf_t disp_buf;
static lv_color_t *buf0;
static lv_disp_t *disp;

// Touch
static uint16_t ts_x;
static uint16_t ts_y;
static volatile absolute_time_t ts_last;
static lv_indev_drv_t indev_ts;

static uint16_t *rot_buf;

// Timer 
static struct repeating_timer lvgl_timer;
static void disp_flush_cb(lv_disp_drv_t * disp, const lv_area_t * area, lv_color_t * color_p);
static void touch_callback(uint gpio, uint32_t events);
static void ts_read_cb(lv_indev_drv_t * drv, lv_indev_data_t*data);
static bool repeating_lvgl_timer_callback(struct repeating_timer *t); 

/* ===== 방향 =====
 * 0: 가로 (기본)   1: 가로 180°
 * 2: 세로          3: 세로 180°
 * 세로 두 방향이 서로 바뀌어 나오면 PORTRAIT_SWAP 를 1로 */
#define PORTRAIT_SWAP 0
static volatile int orient = 0;
static bool ui_portrait = false;
static volatile int orient_pending = -1;

/* ===== 배경/글자색 (앨범 커버 색) ===== */
static volatile int bg_r = 255, bg_g = 255, bg_b = 255;
static volatile int fg_r = 0,   fg_g = 0,   fg_b = 0;
static bool bg_valid = false;
static volatile bool bg_updated = false;

/* ===== 앨범아트 ===== */
#define ART_W 100
#define ART_H 100
static uint8_t art_buf[ART_W * ART_H * 2];
static volatile bool art_updated = false;

static lv_obj_t *mus_img;
static lv_img_dsc_t art_dsc = {
    .header.always_zero = 0,
    .header.w = ART_W,
    .header.h = ART_H,
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .data_size = ART_W * ART_H * 2,
    .data = art_buf,
};

/* ===== 화면 상태 ===== */
#define VIEW_MUSIC   0
#define VIEW_PAGE    1
#define VIEW_SYSTEM  2
#define VIEW_COUNT   3
#define PAGE_COUNT   3

static lv_obj_t *scr_music;
static lv_obj_t *scr_page;
static lv_obj_t *scr_system;

static lv_obj_t *btns[6];
static lv_obj_t *btn_img[6];

static int cur_view = VIEW_PAGE;
static int cur_page = 1;              /* 0,1,2 → 페이지 1,2,3 */
static bool swiped = false;
#define BL_LEVEL 90                         /* 켜졌을 때 밝기 (10~100) */
#define BL_DIM   50                         /* 음악 화면 일시정지 시 밝기 */
static volatile int bl_pending = -1;        /* 0=끄기, 1=켜기 */
static bool screen_off = false;

/* ===== 버튼 아이콘 (현재 페이지 6개, 정적 27KB) ===== */
#define ICON_W 48
#define ICON_H 48
#define ICON_BYTES (ICON_W * ICON_H * 2)
#define BTN_BG_HEX 0x2A2A2A
static uint8_t icon_buf[6 * ICON_BYTES];
static lv_img_dsc_t icon_dsc[6];
static volatile int icon_dirty = -1;

/* ===== 바이너리 수신 상태 ===== */
#define BIN_NONE 0
#define BIN_ART  1
#define BIN_ICON 2
static uint8_t *bin_dst = NULL;
static int bin_remain = 0, bin_pos = 0, bin_cap = 0;
static int bin_kind = BIN_NONE, bin_slot = 0;
static absolute_time_t bin_last;

/* ===== 음악 정보 ===== */
static char mus_title[96] = "";
static char mus_artist[64] = "";
static char lyr_prev[160] = "", lyr_cur[160] = "", lyr_next[160] = "";
static volatile int mus_pos = 0, mus_end = 0, mus_playing = 0;
static volatile bool mus_updated = false;
static volatile bool lyric_updated = false;

static lv_obj_t *mus_lbl_now;        /* "NOW PLAYING" (세로만) */
static lv_obj_t *mus_lbl_title;
static lv_obj_t *mus_lbl_artist;     /* 세로만 */
static lv_obj_t *mus_lbl_time;       /* 가로: "cur / total", 세로: cur */
static lv_obj_t *mus_lbl_dur;        /* 세로만: total */
static lv_obj_t *mus_bar;
static lv_obj_t *mus_lbl_lyric;      /* 현재 가사 */
static lv_obj_t *mus_lbl_lprev;      /* 세로만 */
static lv_obj_t *mus_lbl_lnext;      /* 세로만 */

/* ===== 곡 넘김 드래그 ===== */
#define DRAG_THRESH_L 80        /* 가로 모드: 이 이상 끌면 곡 넘김 */
#define DRAG_THRESH_P 50        /* 세로 모드 */
#define CARD_ANIM_MS  200
static lv_obj_t *mus_card;                    /* 아트+제목(+아티스트) 묶음 */
static lv_obj_t *mus_lbl_hint_l, *mus_lbl_hint_r;
static int  drag_x0, drag_y0, drag_dx;
static int  drag_axis = 0;                    /* 0 미정, 1 가로, 2 세로 */
static bool drag_was_h = false;
static bool card_out = false;                 /* 카드 빠져나가고 새 곡 대기 중 */
static int  card_out_dir = 0;                 /* -1 왼쪽으로 나감(NEXT), +1 오른쪽(PREV) */
static uint32_t card_out_t;
static char card_title[96];
static bool card_new_song = false;

/* ===== 토스트 ===== */
static char toast_app[64] = "", toast_title[128] = "", toast_body[256] = "";
static volatile bool toast_updated = false;
static lv_obj_t *toast_panel, *toast_lbl1, *toast_lbl2;
static lv_timer_t *toast_timer;

/* ===== 시스템 상태 ===== */
static volatile int sys_val[7] = {0,0,0,0,0,0,0};   /* cpu, cpuT, gpu, gpuT, ram%, ramUsed*10, ramTotal*10 */
static volatile bool sys_updated = false;

static lv_obj_t *sys_big[3];
static lv_obj_t *sys_pct[3];
static lv_obj_t *sys_sub[3];
static lv_obj_t *sys_bar[3];
static lv_timer_t *sys_timer;

static const char *sys_name[3] = { "CPU", "GPU", "RAM" };
static const uint32_t sys_color[3] = { 0x2EE6E6, 0x8B5CF6, 0xF5A623 };

/* ===== 스타일 ===== */
static lv_style_t style_kr, style_small;


/* ===== 아이콘 버퍼 비우기 ===== */
static void icons_clear(void)
{
    int r = (BTN_BG_HEX >> 16) & 0xFF, g = (BTN_BG_HEX >> 8) & 0xFF, b = BTN_BG_HEX & 0xFF;
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    for (int i = 0; i < 6 * ICON_W * ICON_H; i++) {
        icon_buf[i * 2]     = (v >> 8) & 0xFF;
        icon_buf[i * 2 + 1] = v & 0xFF;
    }
}

/* ===== 페이지 적용: 버퍼 비우고 PC에 아이콘 요청 ===== */
static void apply_page(void)
{
    icons_clear();
    for (int i = 0; i < 6; i++)
        lv_obj_invalidate(btn_img[i]);
    printf("P:%d\n", cur_page + 1);
}

static void bl_apply(void)
{
    static int last = -1;
    int lv;
    if (screen_off)                                   lv = 0;
    else if (cur_view == VIEW_MUSIC && !mus_playing)  lv = BL_DIM;
    else                                              lv = BL_LEVEL;
    if (lv != last) { DEV_SET_PWM(lv); last = lv; }
}

/* ===== 화면 전환 ===== */
static void show_view(int v)
{
    cur_view = v;
    if (v == VIEW_MUSIC)       lv_scr_load(scr_music);
    else if (v == VIEW_SYSTEM) lv_scr_load(scr_system);
    else                       lv_scr_load(scr_page);
    bl_apply();
}

/* ===== 버튼 ===== */
static void btn_event_cb(lv_event_t *e)
{    
    lv_event_code_t code = lv_event_get_code(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    if (code == LV_EVENT_PRESSED) {
        swiped = false;
        return;
    }
    if (code == LV_EVENT_CLICKED) {
        if (swiped) return;
        printf("B:%d:%d\n", cur_page + 1, idx + 1);
    }
}

static void card_set_x(void *obj, int32_t x) { lv_obj_set_x(obj, x); }

static void card_anim(int from, int to, int ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, mus_card);
    lv_anim_set_exec_cb(&a, card_set_x);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void card_slide_in(int from_dir)        /* +1: 오른쪽에서, -1: 왼쪽에서 */
{
    int w = lv_obj_get_width(mus_card);
    lv_anim_del(mus_card, card_set_x);
    card_anim(from_dir * w, 0, CARD_ANIM_MS + 50);
    card_out = false;
    card_new_song = false;
}

static void card_drag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int thresh = ui_portrait ? DRAG_THRESH_P : DRAG_THRESH_L;

    if (code == LV_EVENT_PRESSED) {
        drag_x0 = p.x; drag_y0 = p.y;
        drag_axis = 0; drag_dx = 0; drag_was_h = false;
        if (!card_out) lv_anim_del(mus_card, card_set_x);
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (card_out) return;
        int dx = p.x - drag_x0, dy = p.y - drag_y0;
        if (drag_axis == 0 && (abs(dx) > 12 || abs(dy) > 12))
            drag_axis = (abs(dx) > abs(dy)) ? 1 : 2;
        if (drag_axis != 1) return;
        drag_dx = dx;
        lv_obj_set_x(mus_card, dx);
        lv_obj_add_flag(mus_lbl_hint_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mus_lbl_hint_r, LV_OBJ_FLAG_HIDDEN);
        if (dx >  thresh / 2) lv_obj_clear_flag(mus_lbl_hint_l, LV_OBJ_FLAG_HIDDEN);
        if (dx < -thresh / 2) lv_obj_clear_flag(mus_lbl_hint_r, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_add_flag(mus_lbl_hint_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mus_lbl_hint_r, LV_OBJ_FLAG_HIDDEN);
        drag_was_h = (drag_axis == 1);
        drag_axis = 0;
        if (card_out || !drag_was_h) return;
        int w = lv_obj_get_width(mus_card);
        if (drag_dx <= -thresh) {                 /* 왼쪽으로 밀기 → 다음 곡 */
            card_out = true; card_out_dir = -1;
            card_out_t = lv_tick_get();
            strncpy(card_title, mus_title, sizeof(card_title));
            card_anim(drag_dx, -w, CARD_ANIM_MS);
            printf("C:NEXT\n");
        } else if (drag_dx >= thresh) {           /* 오른쪽으로 밀기 → 이전 곡 */
            card_out = true; card_out_dir = 1;
            card_out_t = lv_tick_get();
            strncpy(card_title, mus_title, sizeof(card_title));
            card_anim(drag_dx, w, CARD_ANIM_MS);
            printf("C:PREV\n");
        } else {
            card_anim(drag_dx, 0, CARD_ANIM_MS);  /* 제자리 복귀 */
        }
    }
}

static void music_click_cb(lv_event_t *e)
{
    if (drag_was_h) { drag_was_h = false; return; }   /* 끈 건 클릭 아님 */
    printf("C:PLAY\n");
}
/* ===== 제스처 ===== */
static void gesture_cb(lv_event_t *e)
{
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (cur_view == VIEW_MUSIC && (d == LV_DIR_LEFT || d == LV_DIR_RIGHT || drag_axis == 1))
        return;                                    /* 좌우는 card_drag_cb 가 처리 */
    swiped = true;
    lv_indev_wait_release(lv_indev_get_act());

    if (d == LV_DIR_TOP) {
        show_view((cur_view + VIEW_COUNT - 1) % VIEW_COUNT);
    } else if (d == LV_DIR_BOTTOM) {
        show_view((cur_view + 1) % VIEW_COUNT);
    } else if (cur_view == VIEW_PAGE) {
        if (d == LV_DIR_LEFT) {
            cur_page = (cur_page + 1) % PAGE_COUNT;
            apply_page();
        } else if (d == LV_DIR_RIGHT) {
            cur_page = (cur_page + PAGE_COUNT - 1) % PAGE_COUNT;
            apply_page();
        }
    }
}


/* ===== 바이너리 수신 ===== */
static void bin_start(uint8_t *dst, int cap, int n, int kind, int slot)
{
    bin_dst = dst; bin_cap = cap; bin_remain = n; bin_pos = 0;
    bin_kind = kind; bin_slot = slot;
    bin_last = get_absolute_time();
}

static void copy_field(char *dst, size_t cap, const char *src)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static void serial_poll(void)
{
    static char buf[512];
    static int len = 0;

    if (bin_remain > 0 && absolute_time_diff_us(bin_last, get_absolute_time()) > 2000000) {
        bin_remain = 0; bin_kind = BIN_NONE;
        len = 0;
    }

    absolute_time_t deadline = make_timeout_time_ms(30);

    while (!time_reached(deadline)) {
        int ch = getchar_timeout_us(bin_remain > 0 ? 1500 : 0);
        if (ch == PICO_ERROR_TIMEOUT) break;

        if (bin_remain > 0) {
            if (bin_pos < bin_cap)
                bin_dst[bin_pos++] = (uint8_t)ch;
            bin_remain--;
            bin_last = get_absolute_time();
            if (bin_remain == 0) {
                if (bin_kind == BIN_ART)  art_updated = true;
                if (bin_kind == BIN_ICON) icon_dirty = bin_slot;
                bin_kind = BIN_NONE;
            }
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            if (len > 0) {
                buf[len] = 0;

                if (buf[0] == 'S' && buf[1] == ':') {
                    int v[7];
                    if (sscanf(buf+2, "%d,%d,%d,%d,%d,%d,%d",
                               &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6]) == 7) {
                        for (int i = 0; i < 7; i++) sys_val[i] = v[i];
                        sys_updated = true;
                    }
                }
                else if (buf[0] == 'M' && buf[1] == ':') {
                    char *p = buf + 2;
                    char *b1 = strchr(p, '|');
                    if (b1) {
                        *b1 = 0;
                        copy_field(mus_title, sizeof(mus_title), p);
                        char *p2 = b1 + 1;
                        char *b2 = strchr(p2, '|');
                        if (b2) {
                            *b2 = 0;
                            copy_field(mus_artist, sizeof(mus_artist), p2);
                            int a2,b3,c4;
                            if (sscanf(b2+1, "%d|%d|%d", &a2,&b3,&c4) == 3) {
                                mus_pos = a2; mus_end = b3; mus_playing = c4;
                                mus_updated = true;
                            }
                        }
                    }
                }
                else if (buf[0] == 'A' && buf[1] == ':') {
                    int n = atoi(buf + 2);
                    if (n > 0 && n <= (int)sizeof(art_buf))
                        bin_start(art_buf, sizeof(art_buf), n, BIN_ART, 0);
                }
                else if (buf[0] == 'I' && buf[1] == ':') {
                    int p, sl, n;
                    if (sscanf(buf + 2, "%d:%d:%d", &p, &sl, &n) == 3 &&
                        p == cur_page + 1 && sl >= 1 && sl <= 6 &&
                        n > 0 && n <= ICON_BYTES) {
                        bin_start(icon_buf + (sl - 1) * ICON_BYTES, ICON_BYTES, n, BIN_ICON, sl - 1);
                    }
                }
                else if (buf[0] == 'L' && buf[1] == ':') {
                    /* L:이전|현재|다음   (구형 L:현재 도 허용) */
                    char *p = buf + 2;
                    char *b1 = strchr(p, '|');
                    char *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
                    if (b1 && b2) {
                        *b1 = 0; *b2 = 0;
                        copy_field(lyr_prev, sizeof(lyr_prev), p);
                        copy_field(lyr_cur,  sizeof(lyr_cur),  b1 + 1);
                        copy_field(lyr_next, sizeof(lyr_next), b2 + 1);
                    } else {
                        lyr_prev[0] = 0; lyr_next[0] = 0;
                        copy_field(lyr_cur, sizeof(lyr_cur), p);
                    }
                    lyric_updated = true;
                }
                else if (buf[0] == 'G' && buf[1] == ':') {
                    int r, g, b, r2, g2, b2;
                    if (sscanf(buf + 2, "%d,%d,%d,%d,%d,%d", &r, &g, &b, &r2, &g2, &b2) == 6) {
                        bg_r = r;  bg_g = g;  bg_b = b;
                        fg_r = r2; fg_g = g2; fg_b = b2;
                        bg_valid = true;
                        bg_updated = true;
                    }
                }
                else if (buf[0] == 'D' && buf[1] == ':') {
                    bl_pending = (buf[2] == '0') ? 0 : 1;
                }
                else if (buf[0] == 'T' && buf[1] == ':') {
                    char *p = buf + 2;
                    char *b1 = strchr(p, '|');
                    char *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
                    if (b1 && b2) {
                        *b1 = 0; *b2 = 0;
                        copy_field(toast_app,   sizeof(toast_app),   p);
                        copy_field(toast_title, sizeof(toast_title), b1 + 1);
                        copy_field(toast_body,  sizeof(toast_body),  b2 + 1);
                        toast_updated = true;
                    }
                }
                len = 0;
            }
        } else if (len < 510) {
            buf[len++] = (char)ch;
        }
    }
}

/********************************************************************************
function : Initializes LVGL and enbable timers IRQ and DMA IRQ
********************************************************************************/
void LVGL_Init(void)
{
    add_repeating_timer_ms(5, repeating_lvgl_timer_callback, NULL, &lvgl_timer);
    lv_init();

    rot_buf = (uint16_t *)malloc(172 * 640 * 2);
    printf("rot_buf=%p\n", (void*)rot_buf);

    buf0 = (lv_color_t *)malloc(640 * 172 * sizeof(lv_color_t));
    printf("buf0=%p\n", (void*)buf0);

    lv_disp_draw_buf_init(&disp_buf, buf0, NULL, 640 * 172);
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = disp_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.hor_res = 640;
    disp_drv.ver_res = 172;
    disp_drv.full_refresh = 1;
    disp_drv.direct_mode = 0;
    disp = lv_disp_drv_register(&disp_drv);

#if INPUTDEV_TS
    lv_indev_drv_init(&indev_ts); 
    indev_ts.type = LV_INDEV_TYPE_POINTER;    
    indev_ts.read_cb = ts_read_cb;            
    lv_indev_t * ts_indev = lv_indev_drv_register(&indev_ts);
    (void)ts_indev;
    DEV_KEY_Config(TOUCH_INT_PIN);
    DEV_IRQ_SET(TOUCH_INT_PIN, GPIO_IRQ_EDGE_FALL, &touch_callback);
#endif
}

/* ===== 토스트 ===== */
static void toast_hide_cb(lv_timer_t *t)
{
    if (toast_panel) lv_obj_add_flag(toast_panel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(toast_timer);
}

static void toast_click_cb(lv_event_t *e)
{
    toast_hide_cb(NULL);
}

static void toast_show(void)
{
    char line1[200];
    if (toast_app[0]) snprintf(line1, sizeof(line1), "%s  ·  %s", toast_app, toast_title);
    else              snprintf(line1, sizeof(line1), "%s", toast_title);
    lv_label_set_text(toast_lbl1, line1);
    lv_label_set_text(toast_lbl2, toast_body);
    lv_obj_clear_flag(toast_panel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(toast_timer);
    lv_timer_resume(toast_timer);
}

/* ===== 현재 데이터로 화면 채우기 (UI 재생성 후에도 사용) ===== */
static void refresh_lyrics(void)
{
    lv_label_set_text(mus_lbl_lyric, lyr_cur[0] ? lyr_cur : " ");
    if (mus_lbl_lprev) lv_label_set_text(mus_lbl_lprev, lyr_prev);
    if (mus_lbl_lnext) lv_label_set_text(mus_lbl_lnext, lyr_next);

    if (ui_portrait && mus_lbl_lprev && mus_lbl_lnext) {
        lv_obj_update_layout(mus_lbl_lyric);
        lv_obj_align_to(mus_lbl_lprev, mus_lbl_lyric, LV_ALIGN_OUT_TOP_MID,    0, -3);
        lv_obj_align_to(mus_lbl_lnext, mus_lbl_lyric, LV_ALIGN_OUT_BOTTOM_MID, 0,  24);
    }
}
static void refresh_music(void)
{
    char s[32];
    if (!ui_portrait) {
        char tt[170];
        if (mus_artist[0]) snprintf(tt, sizeof(tt), "%s - %s", mus_artist, mus_title);
        else               snprintf(tt, sizeof(tt), "%s", mus_title[0] ? mus_title : "-");
        lv_label_set_text(mus_lbl_title, tt);
        snprintf(s, sizeof(s), "%d:%02d / %d:%02d", mus_pos/60, mus_pos%60, mus_end/60, mus_end%60);
        lv_label_set_text(mus_lbl_time, s);
    } else {
        lv_label_set_text(mus_lbl_title, mus_title[0] ? mus_title : "-");
        lv_label_set_text(mus_lbl_artist, mus_artist);
        snprintf(s, sizeof(s), "%d:%02d", mus_pos/60, mus_pos%60);
        lv_label_set_text(mus_lbl_time, s);
        snprintf(s, sizeof(s), "%d:%02d", mus_end/60, mus_end%60);
        lv_label_set_text(mus_lbl_dur, s);
    }
    int p = (mus_end > 0) ? (mus_pos * 1000 / mus_end) : 0;
    lv_bar_set_value(mus_bar, p, LV_ANIM_OFF);
    refresh_lyrics();
}

static void refresh_colors(void)
{
    if (!bg_valid) return;
    lv_color_t bg = lv_color_make(bg_r, bg_g, bg_b);
    lv_color_t fg = lv_color_make(fg_r, fg_g, fg_b);
    lv_color_t dim = lv_color_mix(fg, bg, LV_OPA_50);
    lv_obj_set_style_bg_color(scr_music, bg, 0);
    lv_obj_set_style_text_color(mus_lbl_title, fg, 0);
    lv_obj_set_style_text_color(mus_lbl_lyric, fg, 0);
    lv_obj_set_style_text_color(mus_lbl_time,  fg, 0);
    lv_obj_set_style_bg_color(mus_bar, fg, LV_PART_INDICATOR);
    if (mus_lbl_now)    lv_obj_set_style_text_color(mus_lbl_now,    dim, 0);
    if (mus_lbl_artist) lv_obj_set_style_text_color(mus_lbl_artist, dim, 0);
    if (mus_lbl_dur)    lv_obj_set_style_text_color(mus_lbl_dur,    fg,  0);
    if (mus_lbl_lprev)  lv_obj_set_style_text_color(mus_lbl_lprev,  dim, 0);
    if (mus_lbl_lnext)  lv_obj_set_style_text_color(mus_lbl_lnext,  dim, 0);
}

static void refresh_system(void)
{
    int load[3] = { sys_val[0], sys_val[2], sys_val[4] };
    char s[24];
    for (int i = 0; i < 3; i++) {
        int pct = load[i] > 100 ? 100 : load[i];
        snprintf(s, sizeof(s), "%d", pct);
        lv_label_set_text(sys_big[i], s);
        lv_obj_align_to(sys_pct[i], sys_big[i], LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -8);
        lv_bar_set_value(sys_bar[i], pct, LV_ANIM_OFF);
    }
    snprintf(s, sizeof(s), "%d°C", sys_val[1]);
    lv_label_set_text(sys_sub[0], s);
    snprintf(s, sizeof(s), "%d°C", sys_val[3]);
    lv_label_set_text(sys_sub[1], s);
    snprintf(s, sizeof(s), "%d.%d / %d GB", sys_val[5]/10, sys_val[5]%10, sys_val[6]/10);
    lv_label_set_text(sys_sub[2], s);
}

/* ===== UI 생성 (가로 / 세로) ===== */
static void build_ui(bool portrait)
{
    /* --- 페이지 화면 --- */
    scr_page = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr_page, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_set_style_bg_color(scr_page, lv_color_hex(0x101010), 0);

    for (int i = 0; i < 6; i++) {
        btns[i] = lv_btn_create(scr_page);
        if (!portrait) {
            lv_obj_set_size(btns[i], 98, 156);
            lv_obj_align(btns[i], LV_ALIGN_BOTTOM_LEFT, 8 + i * 104, -6);
        } else {
            int col = i % 2, row = i / 2;
            lv_obj_set_size(btns[i], 76, 196);
            lv_obj_set_pos(btns[i], 8 + col * 80, 8 + row * 208);
        }
        lv_obj_set_style_radius(btns[i], 12, 0);
        lv_obj_set_style_bg_color(btns[i], lv_color_hex(BTN_BG_HEX), 0);
        lv_obj_set_style_bg_color(btns[i], lv_color_hex(0x444444), LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(btns[i], 0, 0);
        lv_obj_add_flag(btns[i], LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(btns[i], btn_event_cb, LV_EVENT_ALL, (void *)(intptr_t)i);

        btn_img[i] = lv_img_create(btns[i]);
        lv_img_set_src(btn_img[i], &icon_dsc[i]);
        lv_obj_center(btn_img[i]);
    }

    /* --- 음악 화면 --- */
    scr_music = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr_music, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(scr_music, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_music, music_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr_music, card_drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr_music, card_drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr_music, card_drag_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(scr_music, card_drag_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_set_style_pad_all(scr_music, 0, 0);
    lv_obj_clear_flag(scr_music, LV_OBJ_FLAG_SCROLLABLE);

    mus_lbl_now = NULL; mus_lbl_artist = NULL; mus_lbl_dur = NULL;
    mus_lbl_lprev = NULL; mus_lbl_lnext = NULL;

    /* 드래그로 움직이는 카드 (투명, 화면 크기) */
    mus_card = lv_obj_create(scr_music);
    lv_obj_remove_style_all(mus_card);
    lv_obj_set_size(mus_card, portrait ? 172 : 640, portrait ? 640 : 172);
    lv_obj_set_pos(mus_card, 0, 0);
    lv_obj_clear_flag(mus_card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    card_out = false; drag_axis = 0; drag_was_h = false;

    /* 넘김 힌트 */
    mus_lbl_hint_l = lv_label_create(scr_music);
    lv_label_set_text(mus_lbl_hint_l, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(mus_lbl_hint_l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(mus_lbl_hint_l, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(mus_lbl_hint_l, LV_ALIGN_LEFT_MID, 12, portrait ? -214 : -20);
    lv_obj_add_flag(mus_lbl_hint_l, LV_OBJ_FLAG_HIDDEN);
    mus_lbl_hint_r = lv_label_create(scr_music);
    lv_label_set_text(mus_lbl_hint_r, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(mus_lbl_hint_r, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(mus_lbl_hint_r, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(mus_lbl_hint_r, LV_ALIGN_RIGHT_MID, -12, portrait ? -214 : -20);
    lv_obj_add_flag(mus_lbl_hint_r, LV_OBJ_FLAG_HIDDEN);

    mus_img = lv_img_create(mus_card);
    lv_img_set_src(mus_img, &art_dsc);

    mus_lbl_title = lv_label_create(mus_card);
    lv_obj_add_style(mus_lbl_title, &style_kr, 0);

    mus_bar = lv_bar_create(mus_card);
    lv_bar_set_range(mus_bar, 0, 1000);

    mus_lbl_time = lv_label_create(mus_card);
    lv_obj_add_style(mus_lbl_time, &style_small, 0);

    mus_lbl_lyric = lv_label_create(mus_card);
    lv_label_set_long_mode(mus_lbl_lyric, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(mus_lbl_lyric, &style_kr, 0);

    if (!portrait) {
        mus_lbl_now = lv_label_create(scr_music);
        lv_label_set_text(mus_lbl_now, "NOW PLAYING");
        lv_obj_add_style(mus_lbl_now, &style_small, 0);
        lv_obj_set_style_text_color(mus_lbl_now, lv_color_hex(0x888888), 0);
        lv_obj_align(mus_lbl_now, LV_ALIGN_TOP_LEFT, 20, 10);

        lv_obj_set_pos(mus_img, 20, 36);
        lv_label_set_long_mode(mus_lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(mus_lbl_title, 470);
        lv_obj_set_pos(mus_lbl_title, 140, 40);
        lv_obj_set_size(mus_bar, 370, 8);
        lv_obj_set_pos(mus_bar, 140, 82);
        lv_obj_set_pos(mus_lbl_time, 525, 76);
        lv_obj_set_width(mus_lbl_lyric, 480);
        lv_obj_set_pos(mus_lbl_lyric, 140, 108);
    } else {
        /* NOW PLAYING */
        mus_lbl_now = lv_label_create(scr_music);
        lv_label_set_text(mus_lbl_now, "NOW PLAYING");
        lv_obj_add_style(mus_lbl_now, &style_small, 0);
        lv_obj_set_style_text_color(mus_lbl_now, lv_color_hex(0x888888), 0);
        lv_obj_align(mus_lbl_now, LV_ALIGN_TOP_MID, 0, 12);

        /* 앨범아트 100 → 1.4배 = 140 */
        lv_img_set_zoom(mus_img, 358);
        lv_obj_align(mus_img, LV_ALIGN_TOP_MID, 0, 56);   /* 그려지는 영역 ≈ y 36~176 */

        /* 제목 / 아티스트 */
        lv_label_set_long_mode(mus_lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(mus_lbl_title, 156);
        lv_obj_set_style_text_align(mus_lbl_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(mus_lbl_title, 8, 196);

        mus_lbl_artist = lv_label_create(mus_card);
        lv_label_set_long_mode(mus_lbl_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(mus_lbl_artist, 156);
        lv_obj_add_style(mus_lbl_artist, &style_kr, 0);
        lv_obj_set_style_text_align(mus_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(mus_lbl_artist, lv_color_hex(0x888888), 0);
        lv_obj_set_pos(mus_lbl_artist, 8, 226);

        /* 재생바 + 시간 */
        lv_obj_set_size(mus_bar, 152, 6);
        lv_obj_set_pos(mus_bar, 10, 300);
        lv_obj_set_pos(mus_lbl_time, 10, 312);
        mus_lbl_dur = lv_label_create(mus_card);
        lv_obj_add_style(mus_lbl_dur, &style_small, 0);
        lv_obj_set_width(mus_lbl_dur, 60);
        lv_obj_set_style_text_align(mus_lbl_dur, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(mus_lbl_dur, 102, 312);

        /* 가사 3줄: 이전(1줄) / 현재(최대 3줄 고정 높이) / 다음(1줄) */
        mus_lbl_lprev = lv_label_create(mus_card);
        lv_label_set_long_mode(mus_lbl_lprev, LV_LABEL_LONG_DOT);
        lv_obj_set_width(mus_lbl_lprev, 156);
        lv_obj_add_style(mus_lbl_lprev, &style_kr, 0);
        lv_obj_set_style_text_align(mus_lbl_lprev, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(mus_lbl_lprev, lv_color_hex(0x777777), 0);
        lv_obj_set_pos(mus_lbl_lprev, 8, 380);

        lv_label_set_long_mode(mus_lbl_lyric, LV_LABEL_LONG_WRAP);
        lv_obj_set_size(mus_lbl_lyric, 156, 78);           /* 3줄 고정 */
        lv_obj_set_style_text_align(mus_lbl_lyric, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(mus_lbl_lyric, 8, 396);

        mus_lbl_lnext = lv_label_create(mus_card);
        lv_label_set_long_mode(mus_lbl_lnext, LV_LABEL_LONG_DOT);
        lv_obj_set_width(mus_lbl_lnext, 156);
        lv_obj_add_style(mus_lbl_lnext, &style_kr, 0);
        lv_obj_set_style_text_align(mus_lbl_lnext, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(mus_lbl_lnext, lv_color_hex(0x777777), 0);
        lv_obj_set_pos(mus_lbl_lnext, 8, 500);
    }

    /* --- 시스템 화면 --- */
    scr_system = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr_system, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_set_style_pad_all(scr_system, 0, 0);
    lv_obj_set_style_bg_color(scr_system, lv_color_hex(0x101010), 0);
    lv_obj_clear_flag(scr_system, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++) {
        int x, y0, barw;
        if (!portrait) { x = 40 + i * 213; y0 = 0;           barw = 150; }
        else           { x = 16;           y0 = 4 + i * 208; barw = 140; }
        lv_color_t col = lv_color_hex(sys_color[i]);

        lv_obj_t *name = lv_label_create(scr_system);
        lv_label_set_text(name, sys_name[i]);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(name, col, 0);
        lv_obj_set_pos(name, x, y0 + 16);

        sys_big[i] = lv_label_create(scr_system);
        lv_label_set_text(sys_big[i], "0");
        lv_obj_set_style_text_font(sys_big[i], &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(sys_big[i], lv_color_white(), 0);
        lv_obj_set_pos(sys_big[i], x, y0 + 44);

        sys_pct[i] = lv_label_create(scr_system);
        lv_label_set_text(sys_pct[i], "%");
        lv_obj_set_style_text_font(sys_pct[i], &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(sys_pct[i], lv_color_white(), 0);
        lv_obj_align_to(sys_pct[i], sys_big[i], LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -8);

        sys_sub[i] = lv_label_create(scr_system);
        lv_label_set_text(sys_sub[i], "-");
        lv_obj_set_style_text_font(sys_sub[i], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(sys_sub[i], lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_pos(sys_sub[i], x, y0 + 104);

        sys_bar[i] = lv_bar_create(scr_system);
        lv_obj_set_size(sys_bar[i], barw, 8);
        lv_obj_set_pos(sys_bar[i], x, y0 + 136);
        lv_bar_set_range(sys_bar[i], 0, 100);
        lv_obj_set_style_bg_color(sys_bar[i], lv_color_hex(0x3A3A3A), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sys_bar[i], col, LV_PART_INDICATOR);
        lv_obj_set_style_radius(sys_bar[i], 4, LV_PART_MAIN);
        lv_obj_set_style_radius(sys_bar[i], 4, LV_PART_INDICATOR);

        if (i > 0) {
            lv_obj_t *ln = lv_obj_create(scr_system);
            if (!portrait) { lv_obj_set_size(ln, 1, 130); lv_obj_set_pos(ln, i * 213, 20); }
            else           { lv_obj_set_size(ln, 140, 1); lv_obj_set_pos(ln, 16, y0 - 8); }
            lv_obj_set_style_bg_color(ln, lv_color_hex(0x3A3A3A), 0);
            lv_obj_set_style_border_width(ln, 0, 0);
            lv_obj_set_style_radius(ln, 0, 0);
        }
    }

    /* --- 토스트 배너 (모든 화면 위) --- */
    toast_panel = lv_obj_create(lv_layer_top());
    if (!portrait) lv_obj_set_size(toast_panel, 620, 70);
    else           lv_obj_set_size(toast_panel, 160, 110);
    lv_obj_align(toast_panel, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_color(toast_panel, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(toast_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_color(toast_panel, lv_color_hex(0x4A90D9), 0);
    lv_obj_set_style_border_width(toast_panel, 2, 0);
    lv_obj_set_style_radius(toast_panel, 10, 0);
    lv_obj_set_style_pad_all(toast_panel, 8, 0);
    lv_obj_clear_flag(toast_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(toast_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(toast_panel, toast_click_cb, LV_EVENT_CLICKED, NULL);

    toast_lbl1 = lv_label_create(toast_panel);
    lv_obj_add_style(toast_lbl1, &style_kr, 0);
    lv_obj_set_style_text_color(toast_lbl1, lv_color_hex(0x8EC5FF), 0);
    toast_lbl2 = lv_label_create(toast_panel);
    lv_obj_add_style(toast_lbl2, &style_kr, 0);
    lv_obj_set_style_text_color(toast_lbl2, lv_color_white(), 0);
    if (!portrait) {
        lv_label_set_long_mode(toast_lbl1, LV_LABEL_LONG_DOT);
        lv_label_set_long_mode(toast_lbl2, LV_LABEL_LONG_DOT);
        lv_obj_set_width(toast_lbl1, 600); lv_obj_set_pos(toast_lbl1, 0, 0);
        lv_obj_set_width(toast_lbl2, 600); lv_obj_set_pos(toast_lbl2, 0, 28);
    } else {
        lv_label_set_long_mode(toast_lbl1, LV_LABEL_LONG_DOT);
        lv_label_set_long_mode(toast_lbl2, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(toast_lbl1, 140); lv_obj_set_pos(toast_lbl1, 0, 0);
        lv_obj_set_size(toast_lbl2, 140, 60); lv_obj_set_pos(toast_lbl2, 0, 28);
    }
    lv_obj_add_flag(toast_panel, LV_OBJ_FLAG_HIDDEN);
}

/* ===== 방향 적용: 필요하면 해상도 바꾸고 UI 재생성 ===== */
/* ===== 방향 적용 ===== */
static void apply_orientation(int new_orient)
{
    bool portrait = (new_orient >= 2);
    printf("O:%d\n", new_orient);              /* deck.py 콘솔에서 확인용 */

    if (portrait == ui_portrait) {             /* 180° 만 바뀜 */
        orient = new_orient;
        lv_obj_invalidate(lv_scr_act());
        return;
    }

    /* 1) 임시 빈 화면으로 넘기고 옛 위젯 전부 삭제 (메모리 확보) */
    lv_obj_t *tmp = lv_obj_create(NULL);
    lv_scr_load(tmp);
    lv_obj_del(scr_page);
    lv_obj_del(scr_music);
    lv_obj_del(scr_system);
    lv_obj_del(toast_panel);
    toast_panel = NULL;
    lv_timer_pause(toast_timer);

    /* 2) 해상도 교체 */
    ui_portrait = portrait;
    orient = new_orient;
    disp_drv.hor_res = portrait ? 172 : 640;
    disp_drv.ver_res = portrait ? 640 : 172;
    lv_disp_drv_update(disp, &disp_drv);

    /* 3) 새 방향으로 생성하고 데이터 다시 채움 */
    build_ui(portrait);
    refresh_music();
    refresh_colors();
    refresh_system();
    show_view(cur_view);
    lv_obj_del(tmp);
}

/* ===== IMU: 중력 방향으로 4방향 판정 (mg 단위) ===== */
static void imu_cb(lv_timer_t *t)
{
    float acc[3], gyro[3];
    unsigned int tc = 0;
    QMI8658_read_xyz(acc, gyro, &tc);

    int o = orient;
    if      (acc[1] < -600.0f) o = 0;
    else if (acc[1] >  600.0f) o = 1;
    else if (acc[0] >  600.0f) o = PORTRAIT_SWAP ? 3 : 2;
    else if (acc[0] < -600.0f) o = PORTRAIT_SWAP ? 2 : 3;

    if (o != orient && orient_pending < 0)
        orient_pending = o;
}

static void serial_cb(lv_timer_t *t)
{
    serial_poll();
}


static void sys_update_cb(lv_timer_t *t)
{
    if (orient_pending >= 0) {
        int o = orient_pending;
        orient_pending = -1;
        apply_orientation(o);
    }

    if (bl_pending >= 0) {
        int on = bl_pending;
        bl_pending = -1;
        screen_off = !on;
        bl_apply();
    }

    if (mus_updated) {
        mus_updated = false;
        refresh_music();
        bl_apply();
        if (card_out && (strcmp(mus_title, card_title) != 0 ||
                         (card_out_dir > 0 && mus_pos < 3))) {
            lv_obj_add_flag(mus_img, LV_OBJ_FLAG_HIDDEN);   /* 옛 커버 감추고 */
            printf("X:new song, in from %s\n", card_out_dir < 0 ? "right" : "left");
            card_slide_in(-card_out_dir);                    /* 바로 들어옴 */
        }
    }
    if (card_out && lv_tick_elaps(card_out_t) > 5000) {
        printf("X:timeout, back from %s\n", card_out_dir < 0 ? "left" : "right");
        card_slide_in(card_out_dir);
    }

    if (lyric_updated) {
        lyric_updated = false;
        refresh_lyrics();
    }

    if (art_updated) {
        art_updated = false;
        lv_obj_clear_flag(mus_img, LV_OBJ_FLAG_HIDDEN);     /* 새 커버 도착 → 표시 */
        lv_obj_invalidate(mus_img);
    }

    if (icon_dirty >= 0) {
        int sl = icon_dirty;
        icon_dirty = -1;
        lv_obj_invalidate(btn_img[sl]);
    }

    if (toast_updated) {
        toast_updated = false;
        toast_show();
    }

    if (bg_updated) {
        bg_updated = false;
        refresh_colors();
    }

    if (sys_updated) {
        sys_updated = false;
        refresh_system();
    }

}


/* ===== 초기화 ===== */
void Widgets_Init(void)
{
    lv_style_init(&style_kr);
    lv_style_set_text_font(&style_kr, &font_kr_20);

    lv_style_init(&style_small);
    lv_style_set_text_font(&style_small, &lv_font_montserrat_16);

    icons_clear();
    for (int i = 0; i < 6; i++) {
        icon_dsc[i].header.always_zero = 0;
        icon_dsc[i].header.w = ICON_W;
        icon_dsc[i].header.h = ICON_H;
        icon_dsc[i].header.cf = LV_IMG_CF_TRUE_COLOR;
        icon_dsc[i].data_size = ICON_BYTES;
        icon_dsc[i].data = icon_buf + i * ICON_BYTES;
    }
    memset(art_buf, 0x20, sizeof(art_buf));

    toast_timer = lv_timer_create(toast_hide_cb, 10000, NULL);
    lv_timer_pause(toast_timer);

    build_ui(false);
    refresh_music();
    refresh_system();

    sys_timer = lv_timer_create(sys_update_cb, 300, NULL);
    lv_timer_create(serial_cb, 10, NULL);
    lv_timer_create(imu_cb, 500, NULL);

    apply_page();
    lv_scr_load(scr_page);
}

/********************************************************************************
function : Refresh image — 4방향 회전 복사 후 QSPI DMA
********************************************************************************/
static void disp_flush_cb(lv_disp_drv_t * d, const lv_area_t * area, lv_color_t * color_p)
{
    uint16_t *src = (uint16_t *)color_p;

    switch (orient) {
    case 0:   /* 가로: LVGL 640x172 → 물리 172x640 */
        for (int py = 0; py < 640; py++)
            for (int y = 0; y < 172; y++)
                rot_buf[py * 172 + y] = src[(171 - y) * 640 + py];
        break;
    case 1:   /* 가로 180° */
        for (int py = 0; py < 640; py++)
            for (int y = 0; y < 172; y++)
                rot_buf[py * 172 + y] = src[y * 640 + (639 - py)];
        break;
    case 2:   /* 세로: LVGL 172x640 → 물리 그대로 */
        memcpy(rot_buf, src, 172 * 640 * 2);
        break;
    default:  /* 세로 180° */
        for (int py = 0; py < 640; py++)
            for (int y = 0; y < 172; y++)
                rot_buf[py * 172 + y] = src[(639 - py) * 172 + (171 - y)];
        break;
    }

    LCD_3IN49_SetWindows(0, 0, 172, 640);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2c);
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));

    dma_channel_configure(dma_tx, &c,
                          &qspi.pio->txf[qspi.sm],
                          rot_buf,
                          172 * 640 * 2,
                          true);
    while (dma_channel_is_busy(dma_tx));
    busy_wait_us(200);
    QSPI_Deselect(qspi);

    lv_disp_flush_ready(d);
}

/********************************************************************************
function : Touch interrupt handler
********************************************************************************/
static void touch_callback(uint gpio, uint32_t events)
{
    if (gpio == TOUCH_INT_PIN)
    {
        Touch_Read_State();
        if (screen_off) {               /* 꺼져 있으면 깨우기만 */
            bl_pending = 1;
            printf("W:\n");             /* PC에 알림 → 유휴 타이머 리셋 */
            return;
        }
        ts_x = TOUCH.Point1_x;
        ts_y = TOUCH.Point1_y;
        ts_last = get_absolute_time();
    }
}

/********************************************************************************
function : Touch read — 4방향 좌표 변환
********************************************************************************/
static void ts_read_cb(lv_indev_drv_t * drv, lv_indev_data_t*data)
{
    switch (orient) {
    case 0:  data->point.x = ts_y;        data->point.y = 171 - ts_x; break;
    case 1:  data->point.x = 639 - ts_y;  data->point.y = ts_x;       break;
    case 2:  data->point.x = ts_x;        data->point.y = ts_y;       break;
    default: data->point.x = 171 - ts_x;  data->point.y = 639 - ts_y; break;
    }
    bool down = absolute_time_diff_us(ts_last, get_absolute_time()) < 80000;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/********************************************************************************
function : Report the elapsed time to LVGL each 5ms
********************************************************************************/
static bool repeating_lvgl_timer_callback(struct repeating_timer *t) 
{
    lv_tick_inc(5);
    return true;
}