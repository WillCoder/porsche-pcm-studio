/*
 * scene_settings.c — 场景 #3: 设置
 *
 * 【为什么这一页现在就该做】用户 2026-08-05 说得对: "不可能一次性把全部东西都替换了,
 *   触摸或旋钮的选择要先打通才行"。而**目标页在我们这一侧的**导航, 输入一通就能立刻用 ——
 *   完全不需要命令原厂。设置页就是典型: 选它 -> 进我们自己的设置页, 原厂不用配合。
 *   (只有"让声音真的从某个音源出来"才需要命令原厂, 那是另一条线。)
 *
 * 【这一页不是摆设】左列是**我们真正能生效**的开关(改了立刻影响我们自己的渲染);
 *   右列是**只读镜像的原厂真实状态**(页 id / 音源编码 / 音量)——
 *   它同时是这套状态镜像的活体验证窗口: 你在原厂那边动一下, 这里的数字就跟着变。
 */
#ifndef SCENE_SETTINGS_C
#define SCENE_SETTINGS_C

#include "gfx.c"
#include "../sys/shell_draw.c"
#include "../sys/pcm_shell.c"

#define S_BG0   11,14,20
#define S_BG1   23,29,41
#define S_ROW   27,34,48
#define S_ROW_SEL 38,50,72
#define S_INK   243,245,249
#define S_INK2  154,163,178
#define S_INK3  97,106,121
#define S_AMBER 233,178,74

#define ROW_X   56
#define ROW_W   360
#define ROW_H   56
#define ROW_Y0  126
#define ROW_GAP 10
#define NROW    4

/* 我们自己的设置项 —— 改了立刻在渲染上生效, 不依赖原厂 */
int g_set_clock   = 1;      /* 顶栏显示时钟 */
int g_set_follow  = 1;      /* 跟随原厂换页(状态镜像驱动场景切换) */
int g_set_gauge   = 1;      /* 音量变化时弹音量条 */
static int g_set_sel = 0;

typedef struct { const char *name; const char *hint; int *val; } SetRow;
static const SetRow SET_ROWS[NROW] = {
    { "跟随原厂换页", "原厂切到蓝牙/收音机时, 我们自动跟着切", &g_set_follow },
    { "音量条",       "拧旋钮时弹出音量提示",                   &g_set_gauge  },
    { "顶栏时钟",     "在标题右侧显示时间",                     &g_set_clock  },
    { "关于",         "版本与状态", 0 },
};

/* 开关图形: 胶囊 + 圆钮 */
static void set_switch(int x, int y, int on){
    int w = 52, h = 28, r = h/2;
    if(on) gfx_rrect(x, y, w, h, r, S_AMBER);
    else   gfx_rrect(x, y, w, h, r, 58,66,82);
    gfx_circle(on ? x + w - r : x + r, y + r, r - 4, 250,250,252);
}

/* 一行"名字: 值"的只读信息 */
static void info_line(int x, int y, const char *k, const char *v){
    gfx_text(x, y, k, 1, S_INK3);
    gfx_text(x + 132, y, v, 1, S_INK2);
}
static void info_num(int x, int y, const char *k, int n){
    char b[16]; int p = 0, i, j;
    if(n < 0){ b[p++]='-'; n = -n; }
    if(!n) b[p++]='0';
    else { char t[12]; j=0; while(n){ t[j++]=(char)('0'+n%10); n/=10; } while(j) b[p++]=t[--j]; }
    b[p]=0;
    info_line(x, y, k, b);
}

static void settings_render(u16_ *fb, const PcmState *st, unsigned t_ms){
    int i;
    gfx_target(fb);
    /* 🚨 五个场景**共用同一个背景**(同一底色 + 同一柔光锚点)。
     * 不是偷懒 —— 2026-08-06 用户在真机上看到切页时"从上往下扫描刷新":
     * 切页 = 整屏内容全变 = 480 行都要写进显存, 而写显存 130ms 且显示器同时在扫描输出,
     * 那道推进就看得见。而背景占了切页变化像素的一半以上, 且它**本来就该是同一盏环境光** ——
     * 统一之后切页只重画前景, 搬运量掉到约 1/5。
     * 视觉上也更像一套系统: 一个座舱只有一个光源。改这个数之前先想清楚这一点。 */
    gfx_backdrop(S_BG0, S_BG1, 180, 140, 330, 58,120,190, 0);

    gfx_text(ROW_X, 56, "设置", 2, S_INK);
    if(g_set_clock){
        char b[8]; int p=0, h, m;
        if(pcm_clock(st, &h, &m)){        /* 同 home: 原来没判断, 台架上是乱码 */
            b[p++]=(char)('0'+h/10); b[p++]=(char)('0'+h%10); b[p++]=':';
            b[p++]=(char)('0'+m/10); b[p++]=(char)('0'+m%10); b[p]=0;
            gfx_text(SCR_W-56-gfx_text_w(b,1), 64, b, 1, S_INK2);
        }
    }

    /* 左列: 可操作的设置行 */
    for(i=0;i<NROW;i++){
        int y = ROW_Y0 + i*(ROW_H+ROW_GAP);
        int sel = (i == g_set_sel);
        if(sel){
            gfx_rrect(ROW_X, y, ROW_W, ROW_H, 14, S_ROW_SEL);
            gfx_rrect_ring(ROW_X, y, ROW_W, ROW_H, 14, 233,178,74, 200);
        } else {
            gfx_rrect(ROW_X, y, ROW_W, ROW_H, 14, S_ROW);
        }
        gfx_text(ROW_X+20, y+18, SET_ROWS[i].name, 1, sel ? S_INK : S_INK2);
        if(SET_ROWS[i].val) set_switch(ROW_X+ROW_W-72, y+14, *SET_ROWS[i].val);
        else                gfx_text(ROW_X+ROW_W-40, y+18, ">", 1, S_INK3);
    }
    /* 选中项的说明 */
    if(SET_ROWS[g_set_sel].hint)
        gfx_text(ROW_X, ROW_Y0 + NROW*(ROW_H+ROW_GAP) + 14, SET_ROWS[g_set_sel].hint, 1, S_INK3);

    /* 右列: 只读的原厂真实状态 —— 这既是"关于", 也是状态镜像的活体验证窗口。
     * 你在原厂那边切一下源/换一下页, 这里的数字会立刻跟着变。 */
    {
        int x = ROW_X + ROW_W + 48, y = ROW_Y0 + 4;
        const char *srcname = "未知";
        switch(st->source){
            case SRC_BT:  srcname = "蓝牙"; break;
            case SRC_FM:  srcname = "收音机"; break;
            case SRC_AUX: srcname = "AUX"; break;
            default: break;
        }
        gfx_text(x, y, "原厂状态(只读)", 1, S_INK2);
        y += 34;
        info_line(x, y, "当前音源", srcname);            y += 26;
        info_num (x, y, "音源编码 app", st->stock_src_app);  y += 26;
        info_num (x, y, "音源编码 slot", st->stock_src_slot); y += 26;
        info_num (x, y, "原厂页 id", st->stock_page);     y += 26;
        info_num (x, y, "音量", st->volume);              y += 34;
        gfx_text(x, y, "PCM Studio · 独立硬件层", 1, S_INK3);
    }
}

static int settings_event(const PcmEvent *ev, const PcmState *st){
    if(ev->type == EV_ROTARY && ev->which == KNOB_TUNE){
        g_set_sel = (g_set_sel + ev->arg + NROW) % NROW;
        return 1;
    }
    if(ev->type == EV_KEY_DOWN){
        if(ev->arg == K_UP)   { g_set_sel = (g_set_sel+NROW-1)%NROW; return 1; }
        if(ev->arg == K_DOWN) { g_set_sel = (g_set_sel+1)%NROW; return 1; }
        if(ev->arg == K_OK){
            if(SET_ROWS[g_set_sel].val){ *SET_ROWS[g_set_sel].val = !*SET_ROWS[g_set_sel].val; return 1; }
            shell_toast("PCM Studio · 独立 gf 硬件层");
            return 1;
        }
    }
    if(ev->type == EV_TOUCH_DOWN){
        int i;
        for(i=0;i<NROW;i++){
            int y = ROW_Y0 + i*(ROW_H+ROW_GAP);
            if(ev->x>=ROW_X && ev->x<ROW_X+ROW_W && ev->y>=y && ev->y<y+ROW_H){
                g_set_sel = i;
                if(SET_ROWS[i].val) *SET_ROWS[i].val = !*SET_ROWS[i].val;
                else shell_toast("PCM Studio · 独立 gf 硬件层");
                return 1;
            }
        }
    }
    return 0;
}

static const PcmScene SCENE_SETTINGS = {
    "settings", "设置", 0, 0, settings_render, settings_event
};

#endif /* SCENE_SETTINGS_C */
