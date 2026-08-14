/*
 * scene_settings.c — 场景: 设置
 *
 * 【这一页只有一件事】每个音源的页面**归 Studio 还是归原厂**。用户 2026-08-14 定的:
 *   "设置里暂时不加其他的东西, 后续再说"。
 *
 * 🚨 这一版删掉了原来那三个开关(跟随原厂换页 / 音量条 / 顶栏时钟)。
 *   原因不是嫌多, 是**它们全树没有任何地方读**(只有本文件自己用来画那个胶囊) ——
 *   用户拨得动、也看得见状态变化, 但什么都不会发生。那就是假 UI,
 *   而"不许假渲染"是这个项目的铁律。要么接上真行为, 要么别画。
 *
 * 【开关立刻落盘 + 立刻生效】
 *   · 落盘: plat_cfg_set 当场写文件 —— "改了没保存"比"没改"更糟。
 *   · 生效: 谁显示由 main_pcm 的页面路由每次**现查** plat_cfg_get 决定, 没有缓存,
 *     所以不存在"要等下次切页才生效"。
 *   📌 关掉某一项**不会把你从这一页踢走** —— 接管开关只管媒体页, 而你人在设置页。
 *     音源页和设置页永远是我们的, 不可关: 装了 Studio 就是要替换 SOURCE, 那是前提不是选项。
 */
#ifndef SCENE_SETTINGS_C
#define SCENE_SETTINGS_C

#include "gfx.c"
#include "../sys/shell_draw.c"
#include "../sys/pcm_shell.c"

#define T_BG0   11,14,20
#define T_BG1   23,29,41
#define T_ROW   27,34,48
#define T_ROW_SEL 38,50,72
#define T_INK   243,245,249
#define T_INK2  154,163,178
#define T_INK3  97,106,121
#define T_AMBER 233,178,74

/* 行高给大 —— 电阻屏, 用户明确要求命中区大、padding 多。 */
#define ROW_X   56
#define ROW_W   380
/* 一行制: 名字在左, 状态右对齐在开关之前。
 * 🚨 原来是两行(名字 + 状态各一行), 两个 24px 行塞进 64px 行高**怎么调都局促**,
 *   英文下"Studio"直接顶到框底。改成一行之后行高反而能降, 视觉也干净。 */
#define ROW_H   60
#define ROW_Y0  128
#define ROW_GAP 12
#define ROW_SW_X (ROW_X + ROW_W - 80)      /* 开关左边 */
#define ROW_ST_R (ROW_X + ROW_W - 96)      /* 状态文字的右边界 */
#define HINT_Y  (ROW_Y0 + NROW*(ROW_H+ROW_GAP) + 12)
#define NROW    4              /* 三个接管开关 + 语言 */

/* 返回入口(左上角)。命中区 88×88。 */
#define BACK_CX 44
#define BACK_CY 56
#define BACK_HIT 44

static int g_set_sel = 0;

/* 🚨 存**文案 id 不是字符串**: 表是 static const, 用 T() 初始化编不过,
 *   而且会把语言冻在启动那一刻(换了语言这几行还是老语言)。画的时候再查。 */
enum { RK_TOGGLE = 0, RK_LANG };      /* 行的种类: 开关 / 选语言 */
typedef struct { int name; int hint; int cfg; int kind; } SetRow;
static const SetRow SET_ROWS[NROW] = {
    { STR_BLUETOOTH, STR_HINT_BT,   CFG_TAKEOVER_BT,  RK_TOGGLE },
    { STR_RADIO,     STR_HINT_FM,   CFG_TAKEOVER_FM,  RK_TOGGLE },
    { STR_AUX,       STR_HINT_AUX,  CFG_TAKEOVER_AUX, RK_TOGGLE },
    { STR_LANGUAGE,  STR_HINT_LANG, CFG_LANG,         RK_LANG   },
};

/* 开关图形: 胶囊 + 圆钮 */
static void set_switch(int x, int y, int on){
    int w = 56, h = 30, r = h/2;
    if(on) gfx_rrect(x, y, w, h, r, T_AMBER);
    else   gfx_rrect(x, y, w, h, r, 58,66,82);
    if(on) gfx_circle(x + w - r, y + r, r - 4, 250,250,252);
    else   gfx_circle(x + r,     y + r, r - 4, 250,250,252);
}

/* 返回箭头 '‹' —— 画成两段线, 免字库 */
static void back_arrow(int cx, int cy, int r, int g, int b){
    int i;
    for(i=0;i<12;i++){
        int dx;
        for(dx=-1;dx<=1;dx++){
            gfx_blend(cx+6+dx-i/2, cy-11+i, r,g,b, 235);
            gfx_blend(cx+6+dx-i/2, cy+11-i, r,g,b, 235);
        }
    }
}

/* 右栏信息行: 标签左对齐, 值**右对齐到栏右边**。
 * 🚨 原来值写死在 x+132 —— "音源编码 app" 这种长标签直接压上去了(Mac 预览一眼看见)。
 *   写死的列宽只要标签一改就错位, 所以改成按实际文字宽度算。 */
#define INFO_X  (ROW_X + ROW_W + 48)
#define INFO_R  (SCR_W - 56)                 /* 值的右边界 */
static void info_line(int x, int y, const char *k, const char *v){
    gfx_text(x, y, k, 1, T_INK3);
    gfx_text(INFO_R - gfx_text_w(v,1), y, v, 1, T_INK2);
}
static void info_num(int x, int y, const char *k, int n){
    char b[16]; int p = 0, j;
    if(n < 0){ b[p++]='-'; n = -n; }
    if(!n) b[p++]='0';
    else { char t[12]; j=0; while(n){ t[j++]=(char)('0'+n%10); n/=10; } while(j) b[p++]=t[--j]; }
    b[p]=0;
    info_line(x, y, k, b);
}

static void settings_render(u16_ *fb, const PcmState *st, unsigned t_ms){
    int i;
    gfx_target(fb);
    /* 五个场景共用同一个背景 —— 切页只重画前景, 搬运量掉到约 1/5。 */
    gfx_backdrop(T_BG0, T_BG1, 180, 140, 330, 58,120,190, 0);

    back_arrow(BACK_CX, BACK_CY, T_INK2);
    /* ⚠️ 行距按**英文**留, 不是按中文。拉丁字母有升部降部(Settings 的 g),
     *   实际占的高度比同号中文大一截 —— 中文下看着刚好的间距, 英文下就是压在一起。
     *   这一条是加英文那天在 Mac 预览上当场看见的, 别再按中文调回去。 */
    gfx_text(ROW_X + 36, 26, T(STR_SETTINGS), 2, T_INK);
    gfx_text(ROW_X + 36, 88, T(STR_SET_SUB), 1, T_INK3);

    for(i=0;i<NROW;i++){
        int y = ROW_Y0 + i*(ROW_H+ROW_GAP);
        int sel = (i == g_set_sel);
        int on  = plat_cfg_get(SET_ROWS[i].cfg);      /* 现查, 不缓存 */
        if(sel){
            gfx_rrect(ROW_X, y, ROW_W, ROW_H, 16, T_ROW_SEL);
            gfx_rrect_ring(ROW_X, y, ROW_W, ROW_H, 16, T_AMBER, 200);
        } else {
            gfx_rrect(ROW_X, y, ROW_W, ROW_H, 16, T_ROW);
        }
        /* ⚠️ 颜色宏是三个分量, 绝不能进三元(构建期 lint 也拦) */
        if(sel) gfx_text(ROW_X+22, y+18, T(SET_ROWS[i].name), 1, T_INK);
        else    gfx_text(ROW_X+22, y+18, T(SET_ROWS[i].name), 1, T_INK2);
        if(SET_ROWS[i].kind == RK_LANG){
            /* 语言行不画开关, 画"现在是哪种语言"。名字用**该语言自己的写法** ——
             * 界面一旦变成看不懂的语言, 用户就是靠这一行认回来的。 */
            const char *ln = lang_name(on);
            gfx_text(ROW_ST_R + 40 - gfx_text_w(ln,1), y+18, ln, 1, T_AMBER);
            gfx_text(ROW_X+ROW_W-32, y+18, ">", 1, T_INK3);
        } else {
            const char *stt;
            if(on) stt = T(STR_STUDIO); else stt = T(STR_STOCK_UI);
            if(on) gfx_text(ROW_ST_R - gfx_text_w(stt,1), y+18, stt, 1, T_AMBER);
            else   gfx_text(ROW_ST_R - gfx_text_w(stt,1), y+18, stt, 1, T_INK3);
            set_switch(ROW_SW_X, y+(ROW_H-30)/2, on);
        }
    }
    gfx_text(ROW_X, HINT_Y, T(SET_ROWS[g_set_sel].hint), 1, T_INK3);

    /* 右列: 只读的原厂真实状态 —— 既是"关于", 也是状态镜像的活体验证窗口。
     * 你在原厂那边切一下源/换一下页, 这里的数字会立刻跟着变。 */
    {
        int x = INFO_X, y = ROW_Y0 + 4;
        const char *srcname = T(STR_UNKNOWN);
        switch(st->source){
            case SRC_BT:  srcname = T(STR_BLUETOOTH); break;
            case SRC_FM:  srcname = T(STR_RADIO); break;
            case SRC_AUX: srcname = T(STR_AUX); break;
            default: break;
        }
        gfx_text(x, y, T(STR_STOCK_STATE), 1, T_INK2);
        y += 34;
        info_line(x, y, T(STR_CUR_SOURCE),  srcname);              y += 28;
        info_num (x, y, T(STR_SRC_APP),  st->stock_src_app);    y += 28;
        info_num (x, y, T(STR_SRC_SLOT), st->stock_src_slot);   y += 28;
        info_num (x, y, T(STR_PAGE_ID),     st->stock_page);       y += 28;
        info_num (x, y, T(STR_VOLUME),      st->volume);           y += 38;
        gfx_text(x, y, T(STR_ABOUT), 1, T_INK3);
    }
}

/* 命中判定与绘制共用同一组常量 */
static int set_row_hit(int x, int y){
    int i;
    if(x < ROW_X || x >= ROW_X+ROW_W) return -1;
    for(i=0;i<NROW;i++){
        int ry = ROW_Y0 + i*(ROW_H+ROW_GAP);
        if(y >= ry && y < ry+ROW_H) return i;
    }
    return -1;
}

/* 点一行。开关行是取反; 语言行是**在语言之间轮转**(现在只有两种, 所以也是取反,
 * 但写成 (v+1)%LANG_N —— 加第三种语言时这里不用再改一次)。两种都当场落盘。 */
static void set_toggle(int i){
    int v = plat_cfg_get(SET_ROWS[i].cfg);
    g_set_sel = i;
    if(SET_ROWS[i].kind == RK_LANG) plat_cfg_set(SET_ROWS[i].cfg, (v + 1) % LANG_N);
    else                            plat_cfg_set(SET_ROWS[i].cfg, !v);
}

static int settings_event(const PcmEvent *ev, const PcmState *st){
    if(ev->type == EV_ROTARY && ev->which == KNOB_TUNE){
        g_set_sel = (g_set_sel + ev->arg + NROW) % NROW;
        return 1;
    }
    if(ev->type == EV_KEY_DOWN){
        if(ev->arg == K_UP)   { g_set_sel = (g_set_sel+NROW-1)%NROW; return 1; }
        if(ev->arg == K_DOWN) { g_set_sel = (g_set_sel+1)%NROW; return 1; }
        if(ev->arg == K_OK)   { set_toggle(g_set_sel); return 1; }
    }
    if(ev->type == EV_TOUCH_DOWN){
        int i;
        if(ev->x >= BACK_CX-BACK_HIT && ev->x < BACK_CX+BACK_HIT &&
           ev->y >= BACK_CY-BACK_HIT && ev->y < BACK_CY+BACK_HIT){
            shell_goto("source"); return 1;
        }
        i = set_row_hit(ev->x, ev->y);
        if(i >= 0){ set_toggle(i); return 1; }
    }
    return 0;
}

/* 开机自检: 画出来的行中心必须落在它自己的命中区里; 返回键的命中区不许压到行上。 */
static void settings_enter(void){
    int i, bad = 0;
    for(i=0;i<NROW;i++){
        int cy = ROW_Y0 + i*(ROW_H+ROW_GAP) + ROW_H/2;
        if(set_row_hit(ROW_X + ROW_W/2, cy) != i) bad = 1;
    }
    if(set_row_hit(BACK_CX, BACK_CY) >= 0) bad = 1;
    /* 🚨 加一行就可能掉出屏幕 —— 加语言那行的时候真掉了(最后一行到 456, 提示行到 484 > 480),
     *   而屏外的内容**在预览图上也看不见**, 只会表现成"那一项点不到"。所以由自检兜住。 */
    if(ROW_Y0 + (NROW-1)*(ROW_H+ROW_GAP) + ROW_H > SCR_H) bad = 1;
    if(HINT_Y + 24 > SCR_H) bad = 1;
    if(bad) plat_log("‼️ [设置页] 自检失败: 位置/命中区对不上, 或者行数放不下\n");
}

static const PcmScene SCENE_SETTINGS = {
    "settings", STR_SETTINGS, settings_enter, 0, settings_render, settings_event
};

#endif /* SCENE_SETTINGS_C */
