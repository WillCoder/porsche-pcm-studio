/*
 * scene_source.c — 场景: 音源切换页(原 scene_home.c)
 *
 * 【为什么它不叫"首页"】PCM 根本没有首页这个概念 —— 原厂按 SOURCE 是**弹一层浮窗**,
 * 一会儿自己消失。所以这一页对应的是原厂的"音源选择", 不是什么主菜单。
 * 用户 2026-08-14 定的形态: **装了 Studio, SOURCE 键就永远归我们**, 这不是一个可关的选项。
 *
 * 【点卡片是真的切源】走 CMD_SET_SOURCE -> 傀儡 cave -> 原厂自己的音源切换函数,
 * 原厂会自己走完后续(切页/切音频通路)。我们**不模拟坐标、不假装**。
 *
 * 【切完谁显示】不在这里决定。原厂切页 -> 页 id 变 -> main_pcm 的路由按**设置**决定
 * 接管还是让开。把"谁显示"集中在一处, 否则两边各判一次迟早打架。
 */
#ifndef SCENE_SOURCE_C
#define SCENE_SOURCE_C

#include "gfx.c"
#include "../sys/shell_draw.c"
#include "../sys/pcm_shell.c"

#define S_BG0   11,14,20
#define S_BG1   23,29,41
#define S_CARD  27,34,48
#define S_CARD_SEL 38,50,72
#define S_INK   243,245,249
#define S_INK2  154,163,178
#define S_INK3  97,106,121
#define S_AMBER 233,178,74

/* 三张卡片横排。比原来四张大一圈 —— 少一张就该给剩下的更多地方,
 * 而且这是块**电阻屏**, 用户明确要求命中区大、padding 多。 */
#define SC_N     3
#define CARD_W   216
#define CARD_H   224
#define CARD_GAP 28
#define CARD_Y   150
#define CARD_X0  ((SCR_W - (SC_N*CARD_W + (SC_N-1)*CARD_GAP))/2)

/* 齿轮入口(右上角)。绘制中心 + 命中区分开写, 但命中区**由绘制中心推出来** ——
 * 2026-08-14 在蓝牙页栽过一次: 画在一处、命中判在另一处, 版式一改就错位。 */
#define GEAR_CX  (SCR_W - 62)
#define GEAR_CY  62
#define GEAR_HIT 46            /* 命中区半边长: 92×92, 电阻屏够按 */

static int g_src_sel = 0;      /* 旋钮选中 0..SC_N-1 */

/* 🚨 `label` 存的是**文案 id 不是字符串**。表是 `static const`, 拿 T() 初始化既不是
 *   常量表达式(编不过), 更要命的是**会把语言冻在启动那一刻** —— 用户在设置里换了语言,
 *   这三张卡还是老语言。凡是"表里存文案"的地方都要按 id 存, 画的时候再查。 */
typedef struct {
    int label;     /* STR_* */
    int icon;      /* 0=蓝牙 1=收音机 2=AUX */
    int src;       /* SRC_* —— 传给 CMD_SET_SOURCE 的就是它, 不是原厂槽号 */
    int cfg;       /* CFG_TAKEOVER_* —— 这个源的页面归不归我们 */
} SrcCard;

static const SrcCard SRC_CARDS[SC_N] = {
    { STR_BLUETOOTH, 0, SRC_BT,  CFG_TAKEOVER_BT  },
    { STR_RADIO,     1, SRC_FM,  CFG_TAKEOVER_FM  },
    { STR_AUX,       2, SRC_AUX, CFG_TAKEOVER_AUX },
};

/* ---- 图标(矢量, 免字库) ---- */
static void src_icon(int cx, int cy, int kind, int r, int g, int b){
    int i, k;
    switch(kind){
    case 0:  /* 蓝牙符文 */
        {
            int s = 24;
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
            int bx = cx-22, by = cy-7, bw = 44, bh = 26;
            for(i=0;i<20;i++){
                gfx_blend(cx+7+i*2/3, cy-9-i, r,g,b, 235);
                gfx_blend(cx+8+i*2/3, cy-9-i, r,g,b, 180);
            }
            gfx_rrect_ring(bx, by, bw, bh, 4, r,g,b, 235);
            for(k=0;k<3;k++)
                for(i=0;i<14;i++) gfx_blend(bx+6+i, by+8+k*5, r,g,b, 190);
            gfx_circle(bx+33, by+13, 5, r,g,b);
            gfx_circle(bx+33, by+13, 2, S_CARD);
        }
        break;
    case 2:  /* AUX: 3.5mm 插头 */
        for(i=0;i<28;i++) for(k=-3;k<=3;k++) gfx_blend(cx-15+i, cy+k, r,g,b, 235);
        for(i=0;i<9;i++)  for(k=-8;k<=8;k++) gfx_blend(cx+13+i, cy+k, r,g,b, 235);
        for(k=-3;k<=3;k++){ gfx_blend(cx-19, cy+k, r,g,b, 150); gfx_blend(cx-21, cy+k, r,g,b, 150); }
        break;
    }
}

/* 齿轮 —— 圆盘 + 8 个方齿 + 中心孔 */
static void src_gear(int cx, int cy, int rad, int r, int g, int b){
    static const int ux[8]={0,45,64,45,0,-45,-64,-45};
    static const int uy[8]={-64,-45,0,45,64,45,0,-45};
    int i, d, s;
    gfx_circle(cx, cy, rad, r,g,b);
    for(i=0;i<8;i++){
        for(d = rad-2; d <= rad+7; d++){
            int px = cx + ux[i]*d/64, py = cy + uy[i]*d/64;
            int nx = -uy[i], ny = ux[i];
            for(s=-3;s<=3;s++) gfx_blend(px + nx*s/64, py + ny*s/64, r,g,b, 235);
        }
    }
    gfx_circle(cx, cy, rad/3, S_BG1);
}

static void source_render(u16_ *fb, const PcmState *st, unsigned t_ms){
    int i;
    gfx_target(fb);
    /* 五个场景共用同一个背景 —— 切页只重画前景, 搬运量掉到约 1/5。
     * 视觉上也更像一套系统: 一个座舱只有一个光源。改这个数之前先想清楚。 */
    gfx_backdrop(S_BG0, S_BG1, 180, 140, 330, 58,120,190, 0);

    gfx_text(56, 56, T(STR_SOURCE), 2, S_INK);

    /* 时钟。🚨 走 pcm_clock(), "拿不到"这个分支必须处理 —— 台架 RTC 给 -1,
     * 直接打印会渲出乱码(2026-08-13 真截图抓到过)。 */
    {
        char b[8]; int p=0, h, m;
        if(pcm_clock(st, &h, &m)){
            b[p++]='0'+(h/10); b[p++]='0'+(h%10); b[p++]=':';
            b[p++]='0'+(m/10); b[p++]='0'+(m%10); b[p]=0;
            gfx_text(GEAR_CX - 34 - gfx_text_w(b,1), 64, b, 1, S_INK2);
        }
    }

    /* 设置入口(齿轮) */
    src_gear(GEAR_CX, GEAR_CY, 15, S_INK2);

    for(i=0;i<SC_N;i++){
        int x = CARD_X0 + i*(CARD_W+CARD_GAP);
        int sel = (i == g_src_sel);
        int active = (SRC_CARDS[i].src == st->source);
        int mine = plat_cfg_get(SRC_CARDS[i].cfg);
        int y = CARD_Y;
        int ir, ig, ib, lr, lg, lb;

        if(sel) y -= 6;
        if(sel){
            gfx_rrect(x, y, CARD_W, CARD_H, 22, S_CARD_SEL);
            gfx_rrect_ring(x, y, CARD_W, CARD_H, 22, S_AMBER, 200);
        } else {
            gfx_rrect(x, y, CARD_W, CARD_H, 22, S_CARD);
        }

        /* ⚠️ 颜色宏是三个逗号分隔的分量, **绝不能放进三元** —— 会被解析成
         *   `cond?(a,b,c):d` 再把剩下两个当别的实参, 编译零错误但画错色
         *   (2026-08-14 进度条本该琥珀画成了青色)。构建期 lint 也拦这个写法。 */
        if(sel){ ir=233; ig=178; ib=74;  lr=243; lg=245; lb=249; }
        else   { ir=154; ig=163; ib=178; lr=154; lg=163; lb=178; }

        src_icon(x+CARD_W/2, y+80, SRC_CARDS[i].icon, ir,ig,ib);
        {
            const char *lb2 = T(SRC_CARDS[i].label);   /* 每帧现查 -> 换语言当场生效 */
            gfx_text(x + (CARD_W-gfx_text_w(lb2,1))/2, y+142, lb2, 1, lr,lg,lb);
        }
        /* 正在使用的音源: 标签正下方一颗暖点 */
        if(active) gfx_circle(x+CARD_W/2, y+186, 5, S_AMBER);
        /* 没开接管 ⇒ 明说"原厂"。不说的话用户点完看到原厂页会以为坏了。
         * ⚠️ 放**卡片左上角**当角标, 不放底部 —— 底部是"正在使用"那颗点的地方,
         *   一张卡完全可能既在用又没开接管(比如现在正听 FM 而 FM 交给原厂),
         *   两个元素抢同一行迟早叠上。位置分开就不用去想它们会不会同时出现。 */
        if(!mine) gfx_text(x+18, y+16, T(STR_STOCK), 1, S_INK3);
    }

    gfx_text(56, SCR_H-46, T(STR_SRC_HINT), 1, S_INK3);
}

/* 命中判定和上面的绘制**共用同一组常量** —— 版式一改两边一起动。 */
static int src_card_hit(int x, int y){
    int i;
    if(y < CARD_Y-6 || y >= CARD_Y+CARD_H) return -1;
    for(i=0;i<SC_N;i++){
        int cx = CARD_X0 + i*(CARD_W+CARD_GAP);
        if(x >= cx && x < cx+CARD_W) return i;
    }
    return -1;
}

/* 选中一张卡: 真切源。切完谁显示由 main_pcm 的路由按设置决定, 这里不管。
 * 🚨 **必须看返回值**。切源要穿过傀儡 cave 一路到原厂函数, 中间任何一环出问题
 *   (cave 没武装 / 队列还占着 / 音源号不在表里)都只是返回 -1 ——
 *   不提示的话用户看到的就是"点了没反应", 而这是最难描述也最难归因的故障。
 *   宁可弹一句难看的提示, 也不要装作什么都没发生。 */
static void src_pick(int i){
    g_src_sel = i;
    if(plat_command(CMD_SET_SOURCE, SRC_CARDS[i].src) != 0)
        shell_toast(T(STR_SRC_FAIL));
}

static int source_event(const PcmEvent *ev, const PcmState *st){
    if(ev->type == EV_ROTARY && ev->which == KNOB_TUNE){
        g_src_sel = (g_src_sel + ev->arg + SC_N) % SC_N;
        return 1;
    }
    if(ev->type == EV_KEY_DOWN){
        if(ev->arg == K_LEFT)  { g_src_sel = (g_src_sel+SC_N-1)%SC_N; return 1; }
        if(ev->arg == K_RIGHT) { g_src_sel = (g_src_sel+1)%SC_N; return 1; }
        if(ev->arg == K_OK)    { src_pick(g_src_sel); return 1; }
    }
    if(ev->type == EV_TOUCH_DOWN){
        int i;
        /* 齿轮先判 —— 它在右上角, 不和卡片重叠, 顺序其实无所谓, 但先判小目标更稳。 */
        if(ev->x >= GEAR_CX-GEAR_HIT && ev->x < GEAR_CX+GEAR_HIT &&
           ev->y >= GEAR_CY-GEAR_HIT && ev->y < GEAR_CY+GEAR_HIT){
            shell_goto("settings"); return 1;
        }
        i = src_card_hit(ev->x, ev->y);
        if(i >= 0){ src_pick(i); return 1; }
    }
    return 0;
}

/* 开机自检: 每张卡画出来的中心必须落在它自己的命中区里。
 * 🚨 2026-08-14 蓝牙页真栽过 —— 按钮画在 408/486/…, 命中判的还是上一版的 88/292/…,
 *   用户点了没反应, 而代码注释里当时就写着"两边别不一致"。注释拦不住, 断言能。 */
static void source_enter(void){
    int i, bad = 0;
    for(i=0;i<SC_N;i++){
        int cx = CARD_X0 + i*(CARD_W+CARD_GAP) + CARD_W/2;
        if(src_card_hit(cx, CARD_Y + CARD_H/2) != i) bad = 1;
    }
    /* 齿轮的命中区不许压到卡片上 */
    if(src_card_hit(GEAR_CX, GEAR_CY) >= 0) bad = 1;
    if(bad) plat_log("‼️ [音源页] 自检失败: 画的位置和命中区对不上, 点了会没反应\n");
}

static const PcmScene SCENE_SOURCE = {
    "source", STR_SOURCE, source_enter, 0, source_render, source_event
};

#endif /* SCENE_SOURCE_C */
