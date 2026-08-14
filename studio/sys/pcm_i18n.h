/*
 * pcm_i18n.h — PCM Studio · **界面文案的唯一出处**
 *
 * 【为什么是一张表而不是"哪里要就写哪里"】
 * 多语言最常见的坏死法不是翻错, 是**漏翻**: 加了个新按钮, 顺手写了中文,
 * 三个月后换成英文界面时那个按钮还是中文, 而且没人知道它在哪。
 * ⇒ 所以这里做了两件事, 缺一不可:
 *   ① 文案只能从这张表来(下面的 X 宏), 表里**一行就是一条**, 中英并排写, 漏一列编不过;
 *   ② `tools/build.sh` 有条 lint: **场景里的 gfx_text/shell_toast 不许出现中文字面量**。
 *      忘了走表的话构建当场红, 不用等到有人换语言才发现。
 *
 * 【enum 和表为什么不会对不齐】两者都由同一个 `I18N_LIST` 展开 ——
 * 结构上不存在"加了 enum 忘了加表项"这种事, 那是这类表最经典的段错误来源。
 *
 * 【默认英文】用户 2026-08-14 定的。中文是可选项, 不是基线。
 *
 * 📌 **日志和自检消息不进这张表**, 它们是给开发者看的、永远中文 ——
 *    翻译它们没有收益, 只会让表变长、让 lint 变吵。lint 也只看会画到屏上的那几个函数。
 */
#ifndef PCM_I18N_H
#define PCM_I18N_H

#include "pcm_sys.h"

/* 语言。**顺序就是配置文件里存的数字**, 别重排 —— 重排会把老用户的设置解释成另一种语言。 */
enum { LANG_EN = 0, LANG_ZH = 1, LANG_N };

/*  X(标识,                英文,                                   简体中文)  */
#define I18N_LIST(X) \
    /* ---- 通用: 音源名(三处共用, 别各写各的) ---- */ \
    X(STR_BLUETOOTH,   "Bluetooth",                       "蓝牙") \
    X(STR_RADIO,       "Radio",                           "收音机") \
    X(STR_AUX,         "AUX",                             "AUX") \
    X(STR_UNKNOWN,     "Unknown",                         "未知") \
    X(STR_VOLUME,      "Volume",                          "音量") \
    /* ---- 音源页 ---- */ \
    X(STR_SOURCE,      "Source",                          "音源") \
    X(STR_STOCK,       "Stock",                           "原厂") \
    X(STR_SRC_HINT,    "Tap a card to switch source",     "点卡片切换音源") \
    X(STR_SRC_FAIL,    "Switch failed, source unchanged", "切换失败, 音源没变") \
    /* ---- 设置页 ---- */ \
    X(STR_SETTINGS,    "Settings",                        "设置") \
    X(STR_SET_SUB,     "Pages Studio takes over",         "哪些页面由 Studio 接管") \
    X(STR_STUDIO,      "Studio",                          "Studio") \
    X(STR_STOCK_UI,    "Stock UI",                        "原厂界面") \
    X(STR_HINT_BT,     "Studio's UI for Bluetooth",       "蓝牙播放页用 Studio 的界面") \
    X(STR_HINT_FM,     "Studio's UI for the radio",       "收音机页用 Studio 的界面") \
    X(STR_HINT_AUX,    "Studio's UI for AUX",             "AUX 页用 Studio 的界面") \
    X(STR_LANGUAGE,    "Language",                        "语言") \
    X(STR_HINT_LANG,   "Interface language",              "界面语言") \
    X(STR_STOCK_STATE, "Stock state (read-only)",         "原厂状态(只读)") \
    X(STR_CUR_SOURCE,  "Source",                          "当前音源") \
    X(STR_SRC_APP,     "src app",                         "音源 app") \
    X(STR_SRC_SLOT,    "src slot",                        "音源 slot") \
    X(STR_PAGE_ID,     "page id",                         "页 id") \
    X(STR_ABOUT,       "PCM Studio",                      "PCM Studio") \
    /* ---- 蓝牙播放页 ---- */ \
    X(STR_BT_TITLE,    "Bluetooth",                       "蓝牙播放") \
    X(STR_BT_CONNECT,  "Waiting for a phone",             "等待手机连接") \
    X(STR_BT_CONNECT2, "Pair this unit from your phone",  "在手机的蓝牙设置里选择本机") \
    X(STR_BT_IDLE,     "Start playing on your phone",     "在手机上开始播放") \
    X(STR_BT_WRONG,    "Source is not Bluetooth",         "音源不是蓝牙") \
    X(STR_BT_WRONG2,   "Pick Bluetooth on the source page","在音源页选择蓝牙") \
    X(STR_CONNECTED,   "Connected",                       "已连接") \
    /* ---- 收音机页 ---- */ \
    X(STR_FM_HINT,     "Turn to tune, press for presets", "旋钮调台 · 确定选择预设") \
    X(STR_EMPTY,       "empty",                           "空") \
    X(STR_READING,     "reading",                         "读取中") \
    /* ---- AUX 页 ---- */ \
    X(STR_AUX_TITLE,   "AUX input",                       "AUX 输入") \
    X(STR_AUX_SUB,     "External source",                 "外部音源") \
    X(STR_AUX_ON,      "AUX selected",                    "已选中 AUX") \
    X(STR_AUX_OFF,     "Not on AUX",                      "当前不是 AUX") \
    X(STR_AUX_HINT,    "No track info on AUX, volume only","AUX 没有曲目信息, 只能调音量") \
    /* ---- 外壳 ---- */ \
    X(STR_NOT_BUILT,   "This page isn't built yet",       "这个页面还没做")

#define I18N_ENUM(id, en, zh) id,
enum { I18N_LIST(I18N_ENUM) STR_N };
#undef I18N_ENUM

#define I18N_ROW(id, en, zh) { en, zh },
static const char *const I18N_TAB[STR_N][LANG_N] = { I18N_LIST(I18N_ROW) };
#undef I18N_ROW

/* 取一条文案。
 * 🚨 越界返回 "?" 而不是崩 —— 屏上一个问号比一次空指针解引用好查一百倍,
 *   而且这里是**每帧都在跑**的路径, 一个坏 id 会让整台车黑屏。
 * 语言每次现查(plat_cfg_get 首次之后就是内存读), 所以设置一改当场生效, 不用重启。 */
static const char *T(int id){
    int lang;
    if(id < 0 || id >= STR_N) return "?";
    lang = plat_cfg_get(CFG_LANG);
    if(lang < 0 || lang >= LANG_N) lang = LANG_EN;
    return I18N_TAB[id][lang];
}

/* 语言自己的名字**永远用它自己的语言写**(英文行就写 English, 中文行就写 简体中文) ——
 * 这是国际惯例, 而且用户在看不懂当前界面语言时正是靠它找回来的。 */
static const char *lang_name(int lang){
    return (lang == LANG_ZH) ? "简体中文" : "English";
}

#endif /* PCM_I18N_H */
