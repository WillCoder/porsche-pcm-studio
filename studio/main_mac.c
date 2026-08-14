/* main_mac.c — PCM Studio · Mac 宿主入口
 *
 * 编: cc -O2 -o /tmp/pcm_studio_run studio/main_mac.c -lm
 * 跑: /tmp/pcm_studio_run   (配合 studio/tools/studio_server.py 在浏览器里看+点)
 */
#include <stdlib.h>
#include "platform/plat_mac.c"
#include "scenes/scene_btplay.c"
#include "scenes/scene_source.c"
#include "scenes/scene_radio.c"
#include "scenes/scene_settings.c"
#include "scenes/scene_aux.c"
#include <unistd.h>
#include <stdio.h>

/* 🚨 2026-08-13 补: Mac 预览**以前根本不加载字库**, 中文全渲成方块。
 * 这让"改 UI 先在 Mac 上渲、别推台架"这条工作方式形同虚设 —— 我们的内容全是中文,
 * 看不到真字就判不了版式(专辑名会不会顶到边、两行会不会挤)。
 * 台架侧读 /HBpersistence/dev/share/studio.fnt, 这里读仓库里的同一份源文件。 */
static unsigned char g_fontbuf[8 * 1024 * 1024];
static void mac_load_font(void){
    const char *cands[] = { "studio/studio_notosc.fnt", "studio/studio_cjk.fnt", "../studio/studio_cjk.fnt",
                            "studio/studio_gb2312.fnt", 0 };
    int i;
    for(i = 0; cands[i]; i++){
        FILE *f = fopen(cands[i], "rb");
        size_t n;
        if(!f) continue;
        n = fread(g_fontbuf, 1, sizeof g_fontbuf, f);
        fclose(f);
        if(n > 0 && gfx_font_use_blob(g_fontbuf, (int)n)){
            printf("[studio] 字库 %s  %zu 字节\n", cands[i], n); fflush(stdout);
            return;
        }
    }
    printf("[studio] ⚠ 字库没加载上 -> 中文会是方块, **版式判断不作数**\n"); fflush(stdout);
}

int main(void){
    if(plat_init() != 0) return 1;
    mac_load_font();
    shell_register(&SCENE_SOURCE);
    shell_register(&SCENE_BTPLAY);
    shell_register(&SCENE_RADIO);
    shell_register(&SCENE_SETTINGS);
    shell_register(&SCENE_AUX);
    { const char *sc = getenv("STUDIO_SCENE");   /* 预览用: STUDIO_SCENE=btplay 直接停在那一页 */
      shell_goto(sc && *sc ? sc : "source"); }
    for(;;){
        shell_tick();
        usleep(33000);          /* ~30fps 上限; 只有 dirty 才真画 */
    }
    plat_shutdown();
    return 0;
}
