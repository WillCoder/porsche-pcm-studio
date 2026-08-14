/*
 * scene_radio.c — 场景 #3: 收音机
 *
 * 大频率数字 + 频段刻度 + 预设电台。旋钮调台, 数字键选预设。
 * 这是第二种"内容型"页面 —— 用来检验绘图库/布局体系够不够用。
 */
#ifndef SCENE_RADIO_C
#define SCENE_RADIO_C

#include "gfx.c"
#include "../sys/shell_draw.c"
#include "../sys/pcm_shell.c"

#define R_BG0   11,14,20
#define R_BG1   23,29,41
#define R_INK   243,245,249
#define R_INK2  154,163,178
#define R_INK3  97,106,121
#define R_AMBER 233,178,74
#define R_CARD  27,34,48
#define R_CARD_SEL 38,50,72

/* FM 频段 87.5 - 108.0 MHz */
#define FM_LO   87500
#define FM_HI   108000

/* 预设台(模拟数据; 真机接上后从原厂读) */
typedef struct { int khz; const char *name; } Preset;
/* 🚨 这里原来是一张**写死的电台名表**("北京文艺""交通台"…)—— 纯编的, 已删。
 * 2026-08-06 六份 dump 复核的结论:
 *   · **预设电台的频率是真的**(PCM3Root CPTunerPresCtrl +0x2A4 起, 真车实读出 6 个真实频率)
 *     ⇒ 走 PcmState.preset_freq[]/preset_band[]
 *   · **电台名(RDS PS)拿不到** —— 固件里只有请求侧, 8 份 dump 每一项都是共享空串单例,
 *     "非空时能不能正确读出中文台名"一次都没验过。
 *   ⇒ 预设卡**只显示频率**, 不显示台名, 版面里也不给台名留位置。
 *   本项目铁律: 不许假渲染。宁可少显示一个元素, 不编。 */
static int g_preset_sel = 0;

/* 刻度尺 */
#define SCALE_X 56
#define SCALE_Y 210
#define SCALE_W (SCR_W - 112)

/* 预设卡片 */
#define PC_W  108
#define PC_H  74
#define PC_GAP 12
#define PC_Y  296
#define PC_X0 ((SCR_W - (6*PC_W + 5*PC_GAP))/2)

/* 把频率画成 "101.7" 这样的大字。返回结束 x */
static int radio_draw_freq(int x, int y, int khz, int scale, int r, int g, int b){
    char buf[12];
    int mhz = khz/1000, dec = (khz%1000)/100, p = 0;
    if(mhz >= 100) buf[p++] = '0' + (mhz/100)%10;
    buf[p++] = '0' + (mhz/10)%10;
    buf[p++] = '0' + mhz%10;
    buf[p++] = '.';
    buf[p++] = '0' + dec;
    buf[p] = 0;
    return gfx_text(x, y, buf, scale, r,g,b);
}

static void radio_render(u16_ *fb, const PcmState *st, unsigned t_ms){
    int i, freq = st->freq_khz;
    gfx_target(fb);
    /* 🚨 五个场景**共用同一个背景**(同一底色 + 同一柔光锚点)。
     * 不是偷懒 —— 2026-08-06 用户在真机上看到切页时"从上往下扫描刷新":
     * 切页 = 整屏内容全变 = 480 行都要写进显存, 而写显存 130ms 且显示器同时在扫描输出,
     * 那道推进就看得见。而背景占了切页变化像素的一半以上, 且它**本来就该是同一盏环境光** ——
     * 统一之后切页只重画前景, 搬运量掉到约 1/5。
     * 视觉上也更像一套系统: 一个座舱只有一个光源。改这个数之前先想清楚这一点。 */
    gfx_backdrop(R_BG0, R_BG1, 180, 140, 330, 58,120,190, 0);

    /* 顶部: 波段 + 时钟 */
    gfx_text(56, 46, "FM", 1, R_AMBER);
    {
        char b[8]; int p=0, h, m;
        if(!pcm_clock(st, &h, &m)){ b[0]='-';b[1]='-';b[2]=':';b[3]='-';b[4]='-';b[5]=0; }
        else { b[p++]=(char)('0'+h/10); b[p++]=(char)('0'+h%10); b[p++]=':';
               b[p++]=(char)('0'+m/10); b[p++]=(char)('0'+m%10); b[p]=0; }
        gfx_text(SCR_W-56-gfx_text_w(b,1), 46, b, 1, R_INK3);
    }

    /* 大频率 + 单位。⚠️ freq<0 = 还没读到 -> **什么都不画**, 不画 0.0 */
    if(freq > 0){
        int x = radio_draw_freq(56, 84, freq, 3, R_INK);
        gfx_text(x + 14, 84 + 34, "MHz", 1, R_INK2);
    } else {
        gfx_text(56, 84 + 20, T(STR_READING), 2, R_INK3);
    }
    /* 台名: **故意没有**。见文件头说明 —— RDS 台名拿不到, 不留位置也不编。 */

    /* 频段刻度尺 */
    {
        int pos = freq > 0 ? (freq - FM_LO) * SCALE_W / (FM_HI - FM_LO) : -1;
        if(pos > SCALE_W) pos = SCALE_W;
        /* 底线 */
        gfx_pill(SCALE_X, SCALE_Y, SCALE_W, 3, 44,52,66);
        /* 刻度: 每 1MHz 一小格, 每 5MHz 一长格 */
        for(i = 88000; i <= 108000; i += 1000){
            int x = SCALE_X + (i - FM_LO)*SCALE_W/(FM_HI - FM_LO);
            int big = ((i/1000) % 5 == 0);
            int hgt = big ? 12 : 6, k;
            for(k=0;k<hgt;k++) gfx_blend(x, SCALE_Y - 4 - k, 78,88,105, big?220:140);
            if(big){
                char b[6]; int p=0, mh=i/1000;
                if(mh>=100) b[p++]='0'+(mh/100)%10;
                b[p++]='0'+(mh/10)%10; b[p++]='0'+mh%10; b[p]=0;
                gfx_text(x - gfx_text_w(b,1)/2, SCALE_Y + 10, b, 1, 78,88,105);
            }
        }
        /* 预设台标记(刻度上的小暖点)—— 只画真实存在的 FM 预设 */
        for(i=0;i<6;i++){
            int k = st->preset_freq[i];
            if(k <= 0 || st->preset_band[i] != 1) continue;
            gfx_circle(SCALE_X + (k - FM_LO)*SCALE_W/(FM_HI - FM_LO), SCALE_Y + 1, 3, R_AMBER);
        }
        /* 当前指针(频率未知就不画) */
        if(pos >= 0){
            int x = SCALE_X + pos, k;
            for(k=-16;k<=8;k++) gfx_blend(x, SCALE_Y + k, 233,178,74, 255);
            for(k=-16;k<=8;k++) gfx_blend(x+1, SCALE_Y + k, 233,178,74, 160);
            gfx_circle(x, SCALE_Y - 18, 5, R_AMBER);
        }
    }

    /* 预设卡片 */
    for(i=0;i<6;i++){
        int x = PC_X0 + i*(PC_W+PC_GAP);
        int sel = (i == g_preset_sel);
        int khz = st->preset_freq[i];
        int on  = (khz > 0 && khz == freq);
        /* 两种独立状态, 分开表达(别叠色):
         *   on  = 正在播这个台 -> 亮底 + 左侧暖色条
         *   sel = 光标停在这张 -> 暖色描边 */
        /* ⚠️ 别写 `on ? R_CARD_SEL : R_CARD` —— 这两个宏各是**三个逗号分隔的值**,
         * 三元只吃到第一个, 展开成 `on?38:27, 50,72, 34,48` 参数全错位
         * (实测症状: 选中卡片只有红分量变了 -> 一片红棕色, 查了半天)。分支写。 */
        if(on) gfx_rrect(x, PC_Y, PC_W, PC_H, 14, R_CARD_SEL);
        else   gfx_rrect(x, PC_Y, PC_W, PC_H, 14, R_CARD);
        if(on){
            int k;   /* 左侧 3px 暖色指示条 */
            for(k=0;k<3;k++) gfx_pill(x+7+k, PC_Y+12, 1, PC_H-24, 233,178,74);
        }
        if(sel) gfx_rrect_ring(x, PC_Y, PC_W, PC_H, 14, 233,178,74, 220);
        /* 序号 */
        {
            char n[3]; n[0]='0'+i+1; n[1]=0;
            gfx_text(x+18, PC_Y+8, n, 1, on?233:97, on?178:106, on?74:121);
        }
        /* 频率(真数据)。空槽就明说"空", 不编一个台名。
         * ⚠️ 真车的预设表是 **AM/FM 混着的**(实读: AM1530 + 5 个 FM)。
         *   AM 是 kHz 整数, 不能套 FM 那个"除1000保留一位小数"的格式 ——
         *   否则 1530 会显示成 "01.5"(2026-08-06 离线渲染当场抓到)。 */
        if(khz > 0 && st->preset_band[i] == 2){
            char b[8]; int p2=0, v=khz;
            if(v>=1000) b[p2++]=(char)('0'+v/1000);
            b[p2++]=(char)('0'+(v/100)%10); b[p2++]=(char)('0'+(v/10)%10); b[p2++]=(char)('0'+v%10);
            b[p2]=0;
            gfx_text(x+18, PC_Y+30, b, 1, on?243:154, on?245:163, on?249:178);
            gfx_text(x+18+gfx_text_w(b,1)+4, PC_Y+32, "AM", 1, 97,106,121);
        }
        else if(khz > 0) radio_draw_freq(x+18, PC_Y+30, khz, 1, on?243:154, on?245:163, on?249:178);
        else             gfx_text(x+18, PC_Y+34, T(STR_EMPTY), 1, 78,88,105);
    }

    gfx_text(56, SCR_H-38, T(STR_FM_HINT), 1, R_INK3);
}

static int radio_event(const PcmEvent *ev, const PcmState *st){
    if(ev->type == EV_ROTARY && ev->which == KNOB_TUNE){
        int f;
        if(st->freq_khz <= 0) return 1;          /* 还没读到当前频率, 别瞎调 */
        f = st->freq_khz + ev->arg*100;          /* 每格 0.1MHz */
        if(f < FM_LO) f = FM_HI;
        if(f > FM_HI) f = FM_LO;
        plat_command(CMD_TUNE, f);
        return 1;
    }
    if(ev->type == EV_KEY_DOWN){
        if(ev->arg == K_LEFT)  { g_preset_sel = (g_preset_sel+5)%6; return 1; }
        if(ev->arg == K_RIGHT) { g_preset_sel = (g_preset_sel+1)%6; return 1; }
        if(ev->arg == K_OK){
            int k = st->preset_freq[g_preset_sel];
            if(k > 0 && st->preset_band[g_preset_sel] == 1) plat_command(CMD_TUNE, k);
            return 1; }
    }
    if(ev->type == EV_TOUCH_DOWN){
        int i;
        for(i=0;i<6;i++){
            int x = PC_X0 + i*(PC_W+PC_GAP);
            if(ev->x>=x && ev->x<x+PC_W && ev->y>=PC_Y && ev->y<PC_Y+PC_H){
                g_preset_sel = i;
                /* AM 预设先不动作 —— 我们现在只做 FM 页, 切波段是另一条命令 */
                if(st->preset_freq[i] > 0 && st->preset_band[i] == 1)
                    plat_command(CMD_TUNE, st->preset_freq[i]);
                return 1;
            }
        }
        /* 点刻度尺直接调到那个频率 */
        if(ev->y >= SCALE_Y-24 && ev->y <= SCALE_Y+24 &&
           ev->x >= SCALE_X && ev->x <= SCALE_X+SCALE_W){
            int f = FM_LO + (ev->x - SCALE_X)*(FM_HI-FM_LO)/SCALE_W;
            plat_command(CMD_TUNE, (f/100)*100);
            return 1;
        }
    }
    return 0;
}

static const PcmScene SCENE_RADIO = {
    "radio", STR_RADIO, 0, 0, radio_render, radio_event
};

#endif /* SCENE_RADIO_C */
