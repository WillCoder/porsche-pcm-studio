/*
 * scene_aux.c — 场景 #4: AUX 输入
 *
 * AUX 是纯模拟输入 —— 没有曲目信息、没有传输控制可言, 头部单元对它只有"音量"这一个控制维度。
 * 所以这一页**故意做得克制**: 一个大的信号图形 + 音量 + 连接提示。
 * 硬要塞进度条/上下曲那些控件才是假的(原厂对 AUX 也给不出这些数据)。
 *
 * 这一页完全在我们这一侧, 不需要命令原厂 —— 输入一通就能用。
 */
#ifndef SCENE_AUX_C
#define SCENE_AUX_C

#include "gfx.c"
#include "../sys/shell_draw.c"
#include "../sys/pcm_shell.c"

#define A_BG0   11,14,20
#define A_BG1   23,29,41
#define A_TILE  33,43,61
#define A_INK   243,245,249
#define A_INK2  154,163,178
#define A_INK3  97,106,121
#define A_AMBER 233,178,74

/* AUX 图形: 3.5mm 插头轮廓(两道环 + 杆 + 头), 矢量画, 免字库 */
static void draw_aux_plug(int cx, int cy, int s){
    int i, j;
    int w = s/3;                        /* 杆半宽 */
    /* 杆 */
    for(j=-s; j<=s/2; j++)
        for(i=-w; i<=w; i++)
            gfx_blend(cx+i, cy+j, 150,190,235, 235);
    /* 两道绝缘环(挖空) */
    for(j=-s/2; j<=-s/2+4; j++) for(i=-w;i<=w;i++) gfx_blend(cx+i, cy+j, 33,43,61, 255);
    for(j=-s/6; j<=-s/6+4; j++) for(i=-w;i<=w;i++) gfx_blend(cx+i, cy+j, 33,43,61, 255);
    /* 尖端: 收成圆头 */
    for(j=-s-s/4; j<-s; j++){
        int hw = w * (j + s + s/4) / (s/4);
        for(i=-hw;i<=hw;i++) gfx_blend(cx+i, cy+j, 150,190,235, 235);
    }
    /* 线缆护套 */
    for(j=s/2; j<=s; j++)
        for(i=-w-4;i<=w+4;i++)
            gfx_blend(cx+i, cy+j, 90,110,140, 235);
}

static void aux_render(u16_ *fb, const PcmState *st, unsigned t_ms){
    gfx_target(fb);
    /* 🚨 五个场景**共用同一个背景**(同一底色 + 同一柔光锚点)。
     * 不是偷懒 —— 2026-08-06 用户在真机上看到切页时"从上往下扫描刷新":
     * 切页 = 整屏内容全变 = 480 行都要写进显存, 而写显存 130ms 且显示器同时在扫描输出,
     * 那道推进就看得见。而背景占了切页变化像素的一半以上, 且它**本来就该是同一盏环境光** ——
     * 统一之后切页只重画前景, 搬运量掉到约 1/5。
     * 视觉上也更像一套系统: 一个座舱只有一个光源。改这个数之前先想清楚这一点。 */
    gfx_backdrop(A_BG0, A_BG1, 180, 140, 330, 58,120,190, 0);

    gfx_text(56, 56, T(STR_AUX_TITLE), 2, A_INK);

    /* 左: 大图块 */
    gfx_rrect(56, 120, 280, 280, 28, A_TILE);
    draw_aux_plug(196, 250, 70);

    /* 右: 状态 */
    {
        int x = 392;
        gfx_text(x, 150, T(STR_AUX_SUB), 2, A_INK);
        gfx_text(x, 200, st->source == SRC_AUX ? T(STR_AUX_ON) : T(STR_AUX_OFF),
                 1, st->source == SRC_AUX ? A_INK2 : A_INK3);
        gfx_text(x, 236, T(STR_AUX_HINT), 1, A_INK3);

        /* 音量条 —— 这一页唯一真正的控制维度 */
        {
            int bw = 320, bh = 8, by = 300;
            int v = st->volume; if(v < 0) v = 0; if(v > 40) v = 40;
            gfx_pill(x, by, bw, bh, 45,55,72);
            if(v > 0) gfx_pill(x, by, bw*v/40, bh, A_AMBER);
            gfx_circle(x + bw*v/40, by + bh/2, 9, 250,250,252);
            gfx_text(x, by + 30, T(STR_VOLUME), 1, A_INK3);
            {   /* 数值 */
                char b[4]; int p=0, n=v;
                if(n >= 10){ b[p++]=(char)('0'+n/10); }
                b[p++]=(char)('0'+n%10); b[p]=0;
                gfx_text(x + bw - gfx_text_w(b,1), by + 30, b, 1, A_INK2);
            }
        }
    }
}

static int aux_event(const PcmEvent *ev, const PcmState *st){
    if(ev->type == EV_ROTARY && ev->which == KNOB_VOLUME){
        plat_command(CMD_SET_VOLUME, st->volume + ev->arg);
        return 1;
    }
    return 0;
}

static const PcmScene SCENE_AUX = {
    "aux", STR_AUX, 0, 0, aux_render, aux_event
};

#endif /* SCENE_AUX_C */
