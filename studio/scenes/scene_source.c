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

/* 齿轮入口 —— **坐进状态栏那一行的最右端**, 和时钟/音量同一条中线。
 * 🎨 上一版把它单独摆在右上角(62,62), 结果直接压在状态栏的时钟上。
 *   它是导航控件不是状态, 但既然共用这一行, 就得排进同一个序列里, 而不是各占各的角。
 * 命中区仍然**由绘制中心推出来**, 别分开写(蓝牙页为此栽过一次)。 */
#define GEAR_CX  (SB_R - 14)
#define GEAR_CY  SB_ICY
#define GEAR_HIT 34            /* 68×68, 电阻屏够按, 又不至于压到时钟 */
/* 图标外缘半径。**别直接调它, 调完必须过 source_enter 里的高度守卫** ——
 * 状态栏里图标和文字要**差不多高**(iPhone/Android 都是这样), 大一圈就跳出来了。 */
#define GEAR_EXT (SB_ICON * 2 / 3)
/* 实测标定: 画出来的墨迹总高 ≈ 2.08·ext + 3(抗锯齿外扩)。ext=9 量到 22px, ext=8 量到 19px。 */
#define GEAR_PX  ((208 * GEAR_EXT) / 100 + 3)
/* ⚠️ 本想做成编译期断言, 但 SB_CAP 顺着 SFONT_ASC 走到 gfx.c 里的**变量** SF_ASC,
 *   不是常量表达式 ⇒ 只能放在 source_enter 的开机自检里。 */

/* 🚫 **没有"选中项"这个概念**(用户 2026-08-14 定): 不做旋钮选中, 只做触摸。
 *   以前每张卡有黄色 focus 框 + 上浮 6px, 那是给旋钮用的 ——
 *   留着不只是多余, 它会让人以为存在"当前选中项", 而点哪张就是哪张。 */

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

/* 设置图标 —— **三条滑杆**, 不是齿轮。
 * 🎨 2026-08-14 用户判齿轮丑, 而且原因是结构性的:
 *   齿轮在 ~21px 下要塞 8 个齿, 每个齿才 4px 宽, 必然糊成一团;
 *   而且它是**硬边逐像素**画的(gfx_circle + 方齿), 不像蓝牙标/播放键走**距离场**(gfx_shape),
 *   那条路才有抗锯齿。小尺寸下"细节多 + 硬边"是最差的组合。
 * ⇒ 换成全是直线和圆点的滑杆图标: 同样是公认的"设置"语义, 但小尺寸下反而更清楚,
 *   而且能整条走 gfx_shape 拿到抗锯齿。
 * ext = 外缘半径(和状态栏其它图标一个量纲)。 */
static void src_gear(int cx, int cy, int ext, int r, int g, int b){
    /* 🚨 比例是放大看出来的, 别随手改:
     *   第一版 杆粗 0.22·ext(2px)、行距 0.62·ext(3.6px)、钮半径 0.30·ext(直径 5.4px)
     *   ⇒ 钮直接压到邻杆, 三条糊成一坨。**钮的直径必须小于行距**, 而且要留得出白。 */
    float w  = (float)ext * 0.16f; if(w < 1.2f) w = 1.2f;   /* 杆: 细 */
    float kr = (float)ext * 0.26f; if(kr < 2.0f) kr = 2.0f; /* 钮: 直径 ≈ 0.52·ext < 行距 0.78·ext */
    float x0 = (float)cx - (float)ext * 0.92f, x1 = (float)cx + (float)ext * 0.92f;
    /* 三条杆的 y 和各自钮的 x —— 钮**必须错开**, 排成一列就成了汉堡菜单 */
    const float ry[3] = { -0.78f, 0.0f, 0.78f };
    const float kx[3] = { -0.34f, 0.38f, -0.06f };
    int i;
    for(i = 0; i < 3; i++){
        float y = (float)cy + ry[i] * (float)ext;
        float ln[4] = { x0, y, x1, y };
        float kc[4];
        gfx_shape(ln, 2, w, 0, 0, r, g, b, 200);
        kc[0] = kc[2] = (float)cx + kx[i] * (float)ext; kc[1] = kc[3] = y;
        /* 钮 = **两点重合的线段**(不是 n=1 —— 单点形不成线段, 什么都不画,
         *   实测那样画出来是个汉堡菜单)。距离场对退化线段天然给出抗锯齿的圆点。 */
        gfx_shape(kc, 2, kr, 0, 0, r, g, b, 255);
    }
}

static void source_render(u16_ *fb, const PcmState *st, unsigned t_ms){
    int i;
    gfx_target(fb);
    /* 五个场景共用同一个背景 —— 切页只重画前景, 搬运量掉到约 1/5。
     * 视觉上也更像一套系统: 一个座舱只有一个光源。改这个数之前先想清楚。 */
    gfx_backdrop(S_BG0, S_BG1, 180, 140, 330, 58,120,190, 0);

    /* 状态栏一整行: 左边页名, 右半(音量+时钟)由 shell_statusbar 画。 */
    shell_statusbar(st, GEAR_CX - GEAR_HIT + 4, S_INK2);   /* 给齿轮让位 */
    sb_text(SB_L, T(STR_SOURCE), S_INK);

    /* 设置入口 —— 画小了不影响好按: 命中区是 GEAR_HIT 单独给的(68×68)。 */
    src_gear(GEAR_CX, GEAR_CY, GEAR_EXT, S_INK2);

    for(i=0;i<SC_N;i++){
        int x = CARD_X0 + i*(CARD_W+CARD_GAP);
        int active = (SRC_CARDS[i].src == st->source);
        int mine = plat_cfg_get(SRC_CARDS[i].cfg);
        int y = CARD_Y;
        int ir, ig, ib, lr, lg, lb;

        gfx_rrect(x, y, CARD_W, CARD_H, 22, S_CARD);

        /* 唯一的高亮理由是"**这个音源正在用**", 不是"光标停在这儿"。 */
        if(active){ ir=233; ig=178; ib=74;  lr=243; lg=245; lb=249; }
        else      { ir=154; ig=163; ib=178; lr=154; lg=163; lb=178; }

        src_icon(x+CARD_W/2, y+80, SRC_CARDS[i].icon, ir,ig,ib);
        {
            const char *lb2 = T(SRC_CARDS[i].label);   /* 每帧现查 -> 换语言当场生效 */
            gfx_text(x + (CARD_W-gfx_text_w(lb2,1))/2, y+142, lb2, 1, lr,lg,lb);
        }
        /* 正在使用的音源: 标签正下方一颗暖点(配合上面的暖色图标/文字) */
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
    if(plat_command(CMD_SET_SOURCE, SRC_CARDS[i].src) != 0)
        shell_toast(T(STR_SRC_FAIL));
}

static int source_event(const PcmEvent *ev, const PcmState *st){
    /* 🚫 不处理旋钮/方向键/OK —— 没有"选中项"就没有它们的语义。只认触摸。 */
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
    /* 图标不许比同一行的文字大一圈(2026-08-14 栽过: 22px 图标配 13px 数字, 一眼就突兀)。
     * 这条只拦回归, **不证明好看** —— 好不好看还是得放大了自己看。 */
    if(GEAR_PX < SB_CAP || GEAR_PX > SB_CAP * 8 / 5){
        plat_log("‼️ [音源页] 自检失败: 设置图标和状态栏文字不成比例(改 GEAR_EXT)\n");
        bad = 1;
    }
    if(bad) plat_log("‼️ [音源页] 自检失败: 画的位置和命中区对不上, 点了会没反应\n");
}

static const PcmScene SCENE_SOURCE = {
    "source", STR_SOURCE, source_enter, 0, source_render, source_event
};

#endif /* SCENE_SOURCE_C */
