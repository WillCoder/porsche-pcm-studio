/* main_pcm.c — PCM Studio · 真机入口(台架/车)
 *
 * 编: bash studio/tools/build_pcm.sh
 * 推: dev/bench/push.py studio/pcm_studio.stripped /HBpersistence/dev/bin/studio --slow
 * 跑: on -d /HBpersistence/dev/bin/studio     (串口; 别用 & )
 *
 * 跟 main_mac.c 唯一的区别就是换了个平台后端 —— 场景/外壳是同一份源码。
 */
/* 字库外置: 见 gfx.c 的说明。字库文件 /tmp/studio.fnt 由 bake_font.py --bin 生成, 单独推一次。 */
#define STUDIO_FONT_EXTERNAL
#include "platform/plat_pcm.c"
#include "scenes/scene_btplay.c"
#include "scenes/scene_home.c"
#include "scenes/scene_radio.c"
#include "scenes/scene_settings.c"
#include "scenes/scene_aux.c"

extern int usleep(unsigned us);

/* 加载外置字库。放这里而不是 plat_pcm.c —— 包含顺序上 gfx.c 在它之后, 那里才有字库接口。
 * 字库单独推一次(/tmp/studio.fnt), 之后改代码只推 ~52KB 而不是 263KB, 串口 12 分钟 -> 2.5 分钟。
 * 加载失败不致命: 文字会画成占位框, 系统照常跑, 日志说清楚原因。 */
/* 🚨 2026-08-12 真截图逮到: 原来只烤 681 字形(扫源码里出现的汉字), 对固定文案够用,
 *   但**歌名是运行时从手机来的任意中文** —— 真机上 "印地安老斑鸠" 只渲染出 "地安",
 *   其余全是占位框。Mac 预览看不出来(那边用的 mock 文案刚好都在库里)。
 *   ⇒ 先改烤 GB2312 全集(6763), 结果**又被真截图打脸**: 手机发的是**繁体**
 *   (《髮如雪》的"髮"), GB2312 是纯简体, 照样出框。
 *   ⇒ 最终烤 **GB2312 全集 ∪ Big5 全部汉字 = 15318 字**(简体全 + 繁体全 = 中文完整覆盖)。
 *   代价: 字库 6.47MB。/HBpersistence 可用 8.55MB(现有字库会被替换), RAM 余 260MB。
 *   查找是二分, 15429 个字形只多两三次比较, 渲染成本可忽略。
 *   ⚠️ 别把字库放 /mnt/nav(导航盘, 40GB 但**默认只读**、重挂不持久, 而且真车未必同样可写)
 *      —— 那会违反 sim==car。字库必须留在 /HBpersistence。 */
/* ============ 引擎级: 读满一个文件 ============
 * 🚨 存在的理由(2026-08-14 花了两轮才定位): 手写的
 *     while(n < size && (got = read(...)) > 0) n += got;
 *   在 read 返回 **-1**(被信号打断)时会直接退出, 把"出错"当成"读完",
 *   于是数据被截断在**每次都不一样**的位置 —— 这种不确定性极难归因。
 *   **read 返回 0 才是 EOF, <0 是错误。** 谁都别再手写这个循环。
 * 返回真正读到的字节数; *interrupted 回填被打断重试了几次(想记日志就传, 不要传 0)。 */
static int read_all(int fd, unsigned char *buf, int cap, int *interrupted){
    int n = 0, got, retry = 0, tot_retry = 0;
    for(;;){
        int room = cap - n;
        if(room <= 0) break;
        got = read(fd, buf + n, (unsigned)room);
        if(got > 0){ n += got; retry = 0; continue; }
        if(got == 0) break;                       /* 真 EOF */
        if(++retry > 64) break;                   /* 连续 64 次失败才放弃 */
        tot_retry++;
    }
    if(interrupted) *interrupted = tot_retry;
    return n;
}

static unsigned char g_fontbuf[7168*1024];
static void load_font(void){
    /* 🚨 依次试候选路径, 而且**读到的必须被 gfx 认**, 不认就换下一个。
     *   2026-08-14 踩过: /HBpersistence 上躺着一个写坏的残骸(2.8MB),
     *   加载器"先试 /HBpersistence 再回退 /tmp"—— 它 open **成功**了,
     *   于是一直读那个残骸, **从没碰过 /tmp 里那份好的**, 而且每次读出的长度还不一样
     *   (闪存在后台 GC, 坏文件内容本身在变)。那个"每次不一样"把我误导到 EINTR 上。
     *   ⇒ 光靠 open 成功不够, 必须**用内容判**, 不合格就继续往下试。 */
    static const char *cands[] = {
        "/HBpersistence/dev/share/studio.fnt",
        "/tmp/studio.fnt",
        "/fs/usb0/studio.fnt",          /* U 盘插着就能直接用, 省一次拷贝 */
        0
    };
    int k;
    for(k = 0; cands[k]; k++){
        int fd = open(cands[k], O_RDONLY, 0), n, itr = 0, flen;
        if(fd < 0) continue;
        flen = (int)lseek(fd, 0, 2); lseek(fd, 0, 0);     /* 文件实际多大 */
        n = read_all(fd, g_fontbuf, (int)sizeof(g_fontbuf), &itr);
        close(fd);
        if(itr){ plat_log("  (读被打断 "); p_logd(itr); plat_log(" 次, 已重试)\n"); }
        /* 🚨 f3s(NOR 闪存)写大文件后, **元数据立刻更新但数据块还在慢慢落盘** ——
         *   `ls` 已经显示全长, 而读到已提交的边界就返回 EOF。台架没有 sync 命令。
         *   2026-08-14 实测: 刚 cp 完的 3.9MB 字库只读出 2.29MB, 几分钟后同一个文件读满。
         *   ⇒ **短读不等于文件坏**。这里把"读到的"和"文件实际有多大"分开报,
         *     否则会像今天一样被误判成内存不足 / EINTR(我先后猜错两次)。 */
        if(n > 0 && n < flen)
            plat_log("  (只读到 "), p_logd(n), plat_log("/"), p_logd(flen),
            plat_log(" —— 文件是全的, 多半是刚写完还没落盘, 过会儿再试)\n");
        if(gfx_font_use_blob(g_fontbuf, n)){
            plat_log("字库已加载 "); p_logd(n); plat_log(" 字节, ");
            p_logd(SF_NG); plat_log(" 字形  <- "); plat_log(cands[k]); plat_log("\n");
            return;
        }
        plat_log("⚠ 字库不合格("); p_logd(n); plat_log(" 字节) <- ");
        plat_log(cands[k]); plat_log("   试下一个\n");
    }
    plat_log("⚠ 所有候选字库都不可用 -> 文字会是占位框\n");
}

/* 自校验: 开机把自己哈一遍打进日志(串口推二进制要防传坏, 台架没 cksum) */
static void self_ck(void){
    static unsigned char b[8192];
    int fd = open("/HBpersistence/dev/bin/studio", O_RDONLY, 0);
    unsigned h = 2166136261u; int n, total = 0;
    if(fd < 0){ fd = open("/tmp/studio", O_RDONLY, 0); }
    if(fd < 0){ plat_log("自校验: 打不开自己\n"); return; }
    { int itr = 0, n2;
      for(;;){ n2 = read_all(fd, b, (int)sizeof b, &itr);
               if(n2 <= 0) break;
               { int i; for(i=0;i<n2;i++){ h ^= b[i]; h *= 16777619u; } }
               total += n2;
               if(n2 < (int)sizeof b) break; } }
    close(fd);
    plat_log("自校验 ck="); p_logh(h); plat_log(" size="); p_logd(total); plat_log("\n");
}

int main(void){
    self_ck();
    load_font();
    if(plat_init() != 0){ plat_log("平台起不来, 空转\n"); for(;;) usleep(5000000); }
    shell_register(&SCENE_HOME);
    shell_register(&SCENE_BTPLAY);
    shell_register(&SCENE_RADIO);
    shell_register(&SCENE_SETTINGS);
    shell_register(&SCENE_AUX);
    shell_goto("home");
    plat_log("外壳启动, 进主循环  (停止: touch /tmp/studio.stop, **别 slay**)\n");
    for(;;){
        /* 🚨 必须有优雅退出: 被 kill 的话我们最后一帧会**永久冻在屏上**(gdc 不归还层),
         *   原厂再也盖不回来。所以停 studio 一律走 /tmp/studio.stop。 */
        if(plat_should_stop()){
            plat_log("收到 /tmp/studio.stop -> 归还层并退出\n");
            plat_shutdown();
            return 0;
        }
        /* 🚨 模式开关必须**每拍**读, 而且读到变化要强制重画一帧 ——
         *   外壳是 dirty 驱动的, 首页画完就不脏了, plat_present 从此不再被调用。
         *   把 read_mode 放在 present 里等于永远读不到(08-05 台架实测过, 屏幕纹丝不动)。
         *   自检模式有走动方块, 每拍都得重画。 */
        plat_tick_watch();
        plat_peek();
        plat_ts_watch();    /* 触摸门: armed 期间盯住那个字 */
        plat_peek2();       /* 读 PCM3Reload: echo <hex> > /tmp/studio_peek2 */
        plat_post_cmd();  /* echo "<序号> <参数>" > /tmp/studio_post 投一条命令进信箱 */     /* 只读内存查看: echo <hex地址> > /tmp/studio_peek */      /* 🚨 让出协议: 必须每拍跑, 不能挂在 dirty 门控后的 present 里 */
        /* 原厂页 id -> 我们的场景。这就是"输入"的落地方式:
         * 用户按原厂的键, 原厂切它的页, 我们跟着切我们的页。全程只读, 不碰事件流。
         * 页 id 是台架逐条实证的(855=FM页 375=蓝牙播放页); 认不出的页就按音源兜底。 */
        /* ============ 页面归属: 谁显示由原厂当前页决定 ============
         * 2026-08-06 台架实地普查(用户走了一圈原厂菜单)得到的分界:
         *   媒体类(三位数): 855=FM主页 873=FM电台列表 375=蓝牙播放页 387=音源页
         *   设置类(四位数): 3394 3475 3611 4813  -> 用户明确要**沿用原厂**
         * 策略: **只接管认得的页, 其余一律让开。**
         *   保守是故意的 —— 用户的硬要求是"不能有页面错乱/功能不可用",
         *   遇到没见过的页就把屏幕还给原厂, 绝不拿一个空壳页面糊弄。 */
        /* 🔧 调试开关: `echo btplay > /tmp/studio_scene` 强制停在某一页。
         * 为什么需要它: 正常路径是**跟着原厂页 id 走**(375 -> 蓝牙页), 但
         *   ① PCM3Reload 的 /proc/as 打不开时页 id 恒为 -1, 永远不切;
         *   ② 判 UI 时常常没连手机、进不去原厂蓝牙页。
         * 于是"看一眼这页在真屏上什么样"这件最基本的事反而做不了。
         * ⚠️ 只影响调试: 文件不存在时行为与以前完全一致。写空文件即恢复自动跟随。 */
        {
            static char forced[24] = {0};
            int fd = open("/tmp/studio_scene", O_RDONLY, 0);
            if(fd >= 0){
                char nb[24]; int n = read(fd, nb, sizeof nb - 1); close(fd);
                if(n > 0){
                    int i;
                    for(i = 0; i < n; i++) if(nb[i]=='\n' || nb[i]=='\r' || nb[i]==' ') break;
                    nb[i] = 0;
                    if(i > 0){
                        int same = 1, j;
                        for(j = 0; j <= i; j++) if(nb[j] != forced[j]){ same = 0; break; }
                        if(!same){
                            for(j = 0; j <= i && j < 23; j++) forced[j] = nb[j];
                            forced[23] = 0;
                            plat_log("[强制场景] "); plat_log(forced); plat_log("\n");
                            shell_goto(forced);
                            set_cover(1);
                        }
                        goto scene_done;      /* 强制期间不理原厂页 id */
                    }
                }
            }
            forced[0] = 0;
        }
        {
            static int last_page = -1;
            PcmState ps; plat_read_state(&ps);
            if(ps.stock_page != last_page && ps.stock_page >= 0){
                int owned = 1;
                last_page = ps.stock_page;
                switch(ps.stock_page){
                    case 855: case 873: shell_goto("radio");  break;   /* FM 主页 / 电台列表 */
                    case 375:           shell_goto("btplay"); break;   /* 蓝牙播放页 */
                    case 387:           shell_goto("home");   break;   /* 音源页 -> 我们的首页 */
                    default:            owned = 0;            break;   /* 没见过 -> 交还原厂 */
                }
                set_cover(owned);
            }
        }
scene_done:
        if(read_mode() || g_mode == 0) shell_invalidate();
        shell_tick();
        /* 25ms = 40Hz 轮询上限。以前是 100ms, 因为那时上屏要 258ms, 转快了也没用。
         * 现在局部更新把常规一帧压到 **8ms**(2026-08-05 台架实测, 搬 40/480 行),
         * 瓶颈从显存搬到了这个 sleep 上 —— 所以把它放开, 让输入跟手。
         * 静止时外壳根本不重画(dirty 驱动), 所以这不会白烧 CPU。 */
        usleep(25000);
    }
}
