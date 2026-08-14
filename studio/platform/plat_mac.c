/*
 * plat_mac.c — PCM Studio · Mac 平台后端
 *
 * 让整套系统在 Mac 上真的跑起来: 800×480 画面输出 + 输入 + 假状态源。
 *
 * 【为什么不用 SDL】零依赖优先(装 SDL/编译头文件都是摩擦)。这里走一个极简方案:
 *   - 画面: 每帧把 RGBA5551 转 RGB888 写成 PPM 到 /tmp/pcm_studio/frame.ppm(原子替换),
 *           studio/tools/studio_server.py 起个网页把它显示成实时画面。
 *   - 输入: 网页把点击/按键/旋钮 POST 给 server, server 写进 /tmp/pcm_studio/events,
 *           这里读它 -> 变成 PcmEvent。
 *   - 状态: /tmp/pcm_studio/state.json 由网页面板控制(音量/歌名/播放状态...), 这里读它。
 * ⇒ 你在浏览器里看着画面、点按钮、拧旋钮, 系统真的在跑, 全程不碰台架。
 */
#ifndef PLAT_MAC_C
#define PLAT_MAC_C

#include "../sys/pcm_sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define RUNDIR "/tmp/pcm_studio"

static u16_ g_fb[SCR_W * SCR_H];
static unsigned g_t0_ms = 0;
static PcmState g_state;          /* 由 state.json 喂 */
static long g_ev_pos = 0;         /* events 文件读到哪了 */

static unsigned now_ms_raw(void){
    struct timeval tv; gettimeofday(&tv, 0);
    return (unsigned)(tv.tv_sec*1000u + tv.tv_usec/1000u);
}

const char *plat_name(void){ return "mac"; }
unsigned plat_now_ms(void){ return now_ms_raw() - g_t0_ms; }
void plat_log(const char *s){ fputs(s, stderr); }
u16_ *plat_framebuf(void){ return g_fb; }

int plat_init(void){
    char cmd[256];
    g_t0_ms = now_ms_raw();
    snprintf(cmd, sizeof cmd, "mkdir -p %s", RUNDIR);
    if(system(cmd)){}
    /* 默认状态(网页面板可改) */
    memset(&g_state, 0, sizeof g_state);
    g_state.volume = 18; g_state.source = SRC_BT;
    g_state.play_state = PLAY_PLAYING;
    g_state.pos_ms = 67000; g_state.dur_ms = 96000;
    /* 用**台架上真读到的那首歌**做默认值 —— 假数据容易把版式撑得刚刚好,
     * 真数据才会暴露"专辑名太长会不会顶到边"这类问题。
     * 出处: 2026-08-13 台架 peek2 读 S+0x6c/0x70/0x74/0x78 */
    strcpy(g_state.title,  "还在流浪");
    strcpy(g_state.artist, "Jay Chou");
    strcpy(g_state.album,  "最伟大的作品");
    strcpy(g_state.genre,  "Mandopop");
    strcpy(g_state.device, "Demo Phone");
    g_state.u_trk_cur = 3; g_state.u_trk_total = 12;
    g_state.u_battery = BATT_NORMAL;
    g_state.u_shuffle = 0; g_state.u_repeat = 2;
    g_state.freq_khz = 87500; strcpy(g_state.station, "FM 87.5");
    g_state.u_hour = 3; g_state.u_minute = 0; g_state.ignition = 1;
    /* 清空事件文件 */
    { char p[256]; snprintf(p,sizeof p,"%s/events",RUNDIR); FILE*f=fopen(p,"w"); if(f) fclose(f); }
    g_ev_pos = 0;
    fprintf(stderr, "[studio] mac platform up, rundir=%s\n", RUNDIR);
    return 0;
}
void plat_shutdown(void){}

/* ---- 画面: RGBA5551 -> RGB888 PPM, 原子替换 ---- */
void plat_present(void){
    static unsigned char rgb[SCR_W*SCR_H*3];
    char tmp[256], dst[256];
    FILE *f; int i;
    for(i=0;i<SCR_W*SCR_H;i++){
        u16_ p = g_fb[i];
        int r=(p>>11)&0x1f, g=(p>>6)&0x1f, b=(p>>1)&0x1f;
        rgb[i*3+0]=(unsigned char)((r<<3)|(r>>2));
        rgb[i*3+1]=(unsigned char)((g<<3)|(g>>2));
        rgb[i*3+2]=(unsigned char)((b<<3)|(b>>2));
    }
    snprintf(tmp,sizeof tmp,"%s/.frame.tmp",RUNDIR);
    snprintf(dst,sizeof dst,"%s/frame.ppm",RUNDIR);
    f = fopen(tmp,"wb");
    if(!f) return;
    fprintf(f,"P6\n%d %d\n255\n",SCR_W,SCR_H);
    fwrite(rgb,1,sizeof rgb,f);
    fclose(f);
    rename(tmp,dst);            /* 原子, 网页永远读到完整帧 */
}

/* ---- 输入: 读 events 文件的新增行 ---- */
/* 行格式: "type which arg x y\n" (纯数字, 好解析) */
int plat_poll_event(PcmEvent *ev){
    char path[256]; FILE *f; char line[128];
    snprintf(path,sizeof path,"%s/events",RUNDIR);
    f = fopen(path,"r");
    if(!f) return 0;
    if(fseek(f, g_ev_pos, SEEK_SET)!=0){ fclose(f); return 0; }
    if(!fgets(line,sizeof line,f)){ fclose(f); return 0; }
    g_ev_pos = ftell(f);
    fclose(f);
    memset(ev,0,sizeof *ev);
    if(sscanf(line,"%d %d %d %d %d",&ev->type,&ev->which,&ev->arg,&ev->x,&ev->y) < 1) return 0;
    return ev->type != EV_NONE;
}

/* ---- 状态: 读 state.json(极简解析, 只认我们自己写的格式) ---- */
static void read_str(const char *js, const char *key, char *out, int cap){
    const char *p = strstr(js, key);
    int i=0;
    if(!p) return;
    p = strchr(p, ':'); if(!p) return;
    p = strchr(p, '"'); if(!p) return;
    p++;
    while(*p && *p!='"' && i<cap-1) out[i++]=*p++;
    out[i]=0;
}
static int read_int(const char *js, const char *key, int dflt){
    const char *p = strstr(js, key);
    if(!p) return dflt;
    p = strchr(p, ':'); if(!p) return dflt;
    return atoi(p+1);
}
void plat_read_state(PcmState *st){
    char path[256]; FILE *f; static char js[4096];
    long n;
    snprintf(path,sizeof path,"%s/state.json",RUNDIR);
    f = fopen(path,"rb");
    if(f){
        n = fread(js,1,sizeof js - 1,f); js[n>0?n:0]=0; fclose(f);
        g_state.volume     = read_int(js,"\"volume\"",     g_state.volume);
        g_state.muted      = read_int(js,"\"muted\"",      g_state.muted);
        g_state.source     = read_int(js,"\"source\"",     g_state.source);
        g_state.play_state = read_int(js,"\"play_state\"", g_state.play_state);
        g_state.pos_ms     = read_int(js,"\"pos_ms\"",     g_state.pos_ms);
        g_state.dur_ms     = read_int(js,"\"dur_ms\"",     g_state.dur_ms);
        g_state.freq_khz   = read_int(js,"\"freq_khz\"",   g_state.freq_khz);
        g_state.u_hour       = read_int(js,"\"hour\"",       g_state.u_hour);
        g_state.u_minute     = read_int(js,"\"minute\"",     g_state.u_minute);
        read_str(js,"\"title\"",  g_state.title,  sizeof g_state.title);
        read_str(js,"\"artist\"", g_state.artist, sizeof g_state.artist);
        read_str(js,"\"album\"",  g_state.album,  sizeof g_state.album);
        read_str(js,"\"genre\"",  g_state.genre,  sizeof g_state.genre);
        g_state.u_trk_cur    = read_int(js,"\"trk_cur\"",   g_state.u_trk_cur);
        g_state.u_trk_total  = read_int(js,"\"trk_total\"", g_state.u_trk_total);
        g_state.u_battery    = read_int(js,"\"battery\"",   g_state.u_battery);
        g_state.u_shuffle    = read_int(js,"\"shuffle\"",   g_state.u_shuffle);
        g_state.u_repeat     = read_int(js,"\"repeat\"",    g_state.u_repeat);
        read_str(js,"\"device\"", g_state.device, sizeof g_state.device);
        read_str(js,"\"station\"",g_state.station,sizeof g_state.station);
    }
    /* 播放中时间自己走(模拟真实播放) */
    if(g_state.play_state == PLAY_PLAYING && g_state.dur_ms > 0){
        static unsigned last = 0;
        unsigned now = plat_now_ms();
        if(last && now > last) g_state.pos_ms += (int)(now - last);
        last = now;
        if(g_state.pos_ms > g_state.dur_ms) g_state.pos_ms = 0;
    }
    *st = g_state;
}

/* 把当前模拟状态写回 state.json —— 否则命令改的值会被面板值覆盖(下一拍 read_state 读回旧值)。
 * 网页面板 1s 轮询不到冲突: 它只在用户操作时 POST。 */
static void write_state_back(void){
    char path[256]; FILE *f;
    snprintf(path,sizeof path,"%s/state.json",RUNDIR);
    f = fopen(path,"w");
    if(!f) return;
    fprintf(f,
      "{\"volume\":%d,\"muted\":%d,\"source\":%d,\"play_state\":%d,"
      "\"pos_ms\":%d,\"dur_ms\":%d,\"freq_khz\":%d,\"hour\":%d,\"minute\":%d,"
      "\"title\":\"%s\",\"artist\":\"%s\",\"device\":\"%s\",\"station\":\"%s\"}",
      g_state.volume, g_state.muted, g_state.source, g_state.play_state,
      g_state.pos_ms, g_state.dur_ms, g_state.freq_khz, g_state.u_hour, g_state.u_minute,
      g_state.title, g_state.artist, g_state.device, g_state.station);
    fclose(f);
}

/* ---- 命令: 改模拟状态 + 打日志 ---- */
int plat_command(int cmd, int arg){
    switch(cmd){
        case CMD_PLAY:  g_state.play_state = PLAY_PLAYING; break;
        case CMD_PAUSE: g_state.play_state = PLAY_PAUSED;  break;
        case CMD_NEXT:  g_state.pos_ms = 0; strcpy(g_state.title,"下一首 · 模拟"); break;
        case CMD_PREV:  g_state.pos_ms = 0; strcpy(g_state.title,"上一首 · 模拟"); break;
        case CMD_SET_VOLUME: g_state.volume = arg<0?0:(arg>40?40:arg); break;
        case CMD_SET_SOURCE: g_state.source = arg; break;
        case CMD_SET_SHUFFLE: if(arg>=0&&arg<=4) g_state.u_shuffle = arg; break;
        case CMD_SET_REPEAT:  if(arg>=0&&arg<=4) g_state.u_repeat  = arg; break;
        case CMD_TUNE:  g_state.freq_khz = arg; g_state.station[0] = 0; break;
        default: return -1;
    }
    fprintf(stderr,"[studio] cmd=%d arg=%d\n",cmd,arg);
    write_state_back();
    return 0;
}

int plat_can_animate(void){ return 1; }   /* Mac 上一拍 33ms, 动画随便做 */

#endif /* PLAT_MAC_C */
