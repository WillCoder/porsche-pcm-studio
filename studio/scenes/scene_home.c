/*
 * scene_home.c — 场景 #2: 首页(音源卡片)
 *
 * 系统入口。四张大卡片: 蓝牙 / 收音机 / AUX / 设置。旋钮选择, OK 或点击进入。
 * 这是第一个**有选中态**的场景, 也是导航结构的根。
 */
#ifndef SCENE_HOME_C
#define SCENE_HOME_C

#include "gfx.c"
#include "../sys/shell_draw.c"
#include "../sys/pcm_shell.c"

#define H_BG0   11,14,20
#define H_BG1   23,29,41
#define H_CARD  27,34,48
#define H_CARD_SEL 38,50,72
#define H_INK   243,245,249
#define H_INK2  154,163,178
#define H_AMBER 233,178,74
#define H_BT    58,160,255

/* 卡片布局: 4 张横排 */
#define CARD_W  164
#define CARD_H  200
#define CARD_GAP 22
#define CARD_Y  158
#define CARD_X0 ((SCR_W - (4*CARD_W + 3*CARD_GAP))/2)

static int g_home_sel = 0;      /* 当前选中 0..3 */

typedef struct {
    const char *label;
    const char *scene;          /* 进入哪个场景, 0 = 还没做 */
    int icon;                   /* 0=蓝牙 1=收音机 2=AUX 3=设置 */
    int src;                    /* 对应的 SRC_*, 0 = 不是音源 */
} HomeCard;

static const HomeCard HOME_CARDS[4] = {
    { "蓝牙",   "btplay",   0, SRC_BT  },
    { "收音机", "radio",    1, SRC_FM  },
    { "AUX",    "aux",      2, SRC_AUX },
    { "设置",   "settings", 3, 0       },
};

/* ---- 卡片图标(矢量, 免字库) ---- */
static void home_icon(int cx, int cy, int kind, int r, int g, int b){
    int i, k;
    switch(kind){
    case 0:  /* 蓝牙符文 */
        {
            int s = 22;
            #define LN(x1,y1,x2,y2) do{ \
                int _a=((x2)-(x1))>0?((x2)-(x1)):-((x2)-(x1)); \
                int _b=((y2)-(y1))>0?((y2)-(y1)):-((y2)-(y1)); \
                int _n=_a>_b?_a:_b, _i; if(_n<1)_n=1; \
                for(_i=0;_i<=_n;_i++){ int _x=(x1)+((x2)-(x1))*_i/_n, _y=(y1)+((y2)-(y1))*_i/_n, dx,dy; \
                    for(dy=-1;dy<=1;dy++) for(dx=-1;dx<=1;dx++) gfx_blend(_x+dx,_y+dy,r,g,b,235); } }while(0)
            LN(cx, cy-s, cx, cy+s);
            LN(cx, cy-s, cx+s*3/4, cy-s/2);
            LN(cx+s*3/4, cy-s/2, cx-s/2, cy+s/4);
            LN(cx, cy+s, cx+s*3/4, cy+s/2);
            LN(cx+s*3/4, cy+s/2, cx-s/2, cy-s/4);
            #undef LN
        }
        break;
    case 1:  /* 收音机: 机身 + 喇叭格栅 + 调谐旋钮 + 天线 */
        {
            int bx = cx-20, by = cy-6, bw = 40, bh = 24;
            /* 天线(斜) */
            for(i=0;i<18;i++){
                gfx_blend(cx+6+i*2/3, cy-8-i, r,g,b, 235);
                gfx_blend(cx+7+i*2/3, cy-8-i, r,g,b, 180);
            }
            /* 机身描边 */
            gfx_rrect_ring(bx, by, bw, bh, 4, r,g,b, 235);
            /* 喇叭格栅(左半, 三条) */
            for(k=0;k<3;k++)
                for(i=0;i<13;i++) gfx_blend(bx+5+i, by+7+k*5, r,g,b, 190);
            /* 调谐旋钮(右半, 小圆) */
            gfx_circle(bx+30, by+12, 5, r,g,b);
            gfx_circle(bx+30, by+12, 2, 27,34,48);
        }
        break;
    case 2:  /* AUX: 3.5mm 插头 */
        for(i=0;i<26;i++) for(k=-3;k<=3;k++) gfx_blend(cx-14+i, cy+k, r,g,b, 235);
        for(i=0;i<8;i++) for(k=-7;k<=7;k++) gfx_blend(cx+12+i, cy+k, r,g,b, 235);
        for(k=-3;k<=3;k++){ gfx_blend(cx-18, cy+k, r,g,b, 150); gfx_blend(cx-20, cy+k, r,g,b, 150); }
        break;
    case 3:  /* 设置: 齿轮 —— 圆盘 + 8 个方齿 + 中心孔 */
        {
            /* 8 个齿: 单位方向 ×64 定点(上/右上/右/... 顺时针) */
            static const int ux[8]={0,45,64,45,0,-45,-64,-45};
            static const int uy[8]={-64,-45,0,45,64,45,0,-45};
            gfx_circle(cx, cy, 14, r,g,b);
            for(i=0;i<8;i++){
                /* 沿方向从 r=12 铺到 r=21, 每步画一小段横截面 = 方齿 */
                int d;
                for(d=12; d<=21; d++){
                    int px = cx + ux[i]*d/64, py = cy + uy[i]*d/64;
                    /* 垂直方向(法向)展开 ±3 = 齿宽 */
                    int nx = -uy[i], ny = ux[i], s;
                    for(s=-3;s<=3;s++)
                        gfx_blend(px + nx*s/64, py + ny*s/64, r,g,b, 235);
                }
            }
            gfx_circle(cx, cy, 5, 27,34,48);
        }
        break;
    }
}

static void home_render(u16_ *fb, const PcmState *st, unsigned t_ms){
    int i;
    gfx_target(fb);
    /* 🚨 五个场景**共用同一个背景**(同一底色 + 同一柔光锚点)。
     * 不是偷懒 —— 2026-08-06 用户在真机上看到切页时"从上往下扫描刷新":
     * 切页 = 整屏内容全变 = 480 行都要写进显存, 而写显存 130ms 且显示器同时在扫描输出,
     * 那道推进就看得见。而背景占了切页变化像素的一半以上, 且它**本来就该是同一盏环境光** ——
     * 统一之后切页只重画前景, 搬运量掉到约 1/5。
     * 视觉上也更像一套系统: 一个座舱只有一个光源。改这个数之前先想清楚这一点。 */
    gfx_backdrop(H_BG0, H_BG1, 180, 140, 330, 58,120,190, 0);

    /* 顶部: 标题 + 时钟 */
    gfx_text(56, 56, "选择音源", 2, H_INK);
    {
        char b[8]; int p=0, h, m;
        /* 🚨 原来这里**没有任何判断** —— 台架 RTC 给 -1 时会打出 '0'+(-1/10) 的乱码。
         *   走 pcm_clock() 之后, "拿不到"这个分支**必须**被处理, 忘不了。 */
        if(pcm_clock(st, &h, &m)){
            b[p++]='0'+(h/10); b[p++]='0'+(h%10); b[p++]=':';
            b[p++]='0'+(m/10); b[p++]='0'+(m%10); b[p]=0;
            gfx_text(SCR_W-56-gfx_text_w(b,1), 64, b, 1, H_INK2);
        }
    }

    /* 卡片 */
    for(i=0;i<4;i++){
        int x = CARD_X0 + i*(CARD_W+CARD_GAP);
        int sel = (i == g_home_sel);
        int active = (HOME_CARDS[i].src && HOME_CARDS[i].src == st->source);

        if(sel){
            /* 选中: 亮底 + 暖色描边 + 轻微上浮 */
            gfx_rrect(x, CARD_Y-6, CARD_W, CARD_H, 22, H_CARD_SEL);
            gfx_rrect_ring(x, CARD_Y-6, CARD_W, CARD_H, 22, 233,178,74, 200);
        } else {
            gfx_rrect(x, CARD_Y, CARD_W, CARD_H, 22, H_CARD);
        }
        {
            int cy = CARD_Y + (sel?-6:0) + 78;
            home_icon(x+CARD_W/2, cy, HOME_CARDS[i].icon,
                      sel?233:154, sel?178:163, sel?74:178);
            /* 标签 */
            {
                const char *lb = HOME_CARDS[i].label;
                int tw = gfx_text_w(lb,1);
                gfx_text(x + (CARD_W-tw)/2, CARD_Y+(sel?-6:0)+140, lb, 1,
                         sel?243:154, sel?245:163, sel?249:178);
            }
            /* 正在使用的音源: 底部一颗暖点 */
            if(active) gfx_circle(x+CARD_W/2, CARD_Y+(sel?-6:0)+172, 4, H_AMBER);
        }
    }

    /* 底部提示 */
    gfx_text(56, SCR_H-46, "旋钮选择 · 确定进入", 1, 97,106,121);
}

static int home_event(const PcmEvent *ev, const PcmState *st){
    if(ev->type == EV_ROTARY && ev->which == KNOB_TUNE){
        g_home_sel = (g_home_sel + ev->arg + 4) % 4;
        return 1;
    }
    if(ev->type == EV_KEY_DOWN){
        if(ev->arg == K_LEFT)  { g_home_sel = (g_home_sel+3)%4; return 1; }
        if(ev->arg == K_RIGHT) { g_home_sel = (g_home_sel+1)%4; return 1; }
        if(ev->arg == K_OK){
            const HomeCard *c = &HOME_CARDS[g_home_sel];
            if(c->src) plat_command(CMD_SET_SOURCE, c->src);
            if(c->scene) shell_goto(c->scene);
            else shell_toast("这个还没做");
            return 1;
        }
    }
    if(ev->type == EV_TOUCH_DOWN){
        int i;
        for(i=0;i<4;i++){
            int x = CARD_X0 + i*(CARD_W+CARD_GAP);
            if(ev->x>=x && ev->x<x+CARD_W && ev->y>=CARD_Y-6 && ev->y<CARD_Y+CARD_H){
                const HomeCard *c = &HOME_CARDS[i];
                g_home_sel = i;
                if(c->src) plat_command(CMD_SET_SOURCE, c->src);
                if(c->scene) shell_goto(c->scene);
                else shell_toast("这个还没做");
                return 1;
            }
        }
    }
    return 0;
}

static const PcmScene SCENE_HOME = {
    "home", "首页", 0, 0, home_render, home_event
};

#endif /* SCENE_HOME_C */
