#!/bin/bash
# PCM Studio 构建 —— 一份源码, 两个平台
#   ./studio/tools/build.sh mac   -> /tmp/pcm_studio_run  (Mac 上跑, 配 studio_server.py)
#   ./studio/tools/build.sh pcm   -> studio/pcm_studio.stripped (SH4, 推台架)
#   ./studio/tools/build.sh both
set -e
cd "$(dirname "$0")/../.."

# ============ 构建期硬拦截(每条都对应一次真踩过的坑) ============
lint_guards(){
  local bad=0
  # ① `cond ? C_AAA : C_BBB` —— 颜色宏是三个逗号分隔的分量, 逗号优先级最低,
  #    会被解析成 `cond?(a,b,c):d` 再把剩下两个当成另外两个实参。**编译无错, 画出来是错色**。
  #    2026-08-14 实际发生过: 进度条本该琥珀, 画成了青色 (74,173,189)。
  #    ⚠️ 同样只看代码行 —— 注释里写反面示例是**好事**, 别把文档也拦下来。
  if grep -rn '? *C_[A-Z_]* *:' studio/scenes/ 2>/dev/null | grep -vE ':[[:space:]]*[*/]'; then
    echo "‼️ 上面这些是 \`cond ? C_X : C_Y\` —— 颜色宏不能这样用(会静默画错色), 拆成 if/else"; bad=1
  fi
  # ② 手写 read 循环 —— read 返回 -1(被信号打断)会被当成读完, 数据截断在随机位置。
  #    ⚠️ 只看代码行 —— 注释里写着这个坏模式的**反面示例**, 别把自己的文档拦下来(实际误报过)。
  if grep -rn 'read(.*) *> *0' studio/main_pcm.c 2>/dev/null \
       | grep -v read_all | grep -vE ':[[:space:]]*[*/]'; then
    echo "‼️ 上面是手写的 read 循环 —— 用 read_all(), 它区分 EOF(0) 和 错误(-1)"; bad=1
  fi
  # ③ 场景层引用引擎内部接口 —— plat_internal.h 里写着"构建期 lint 会检查", 
  #    而 2026-08-14 文档审查发现**那条 lint 根本不存在**(我把打算做的写成了已经做的)。
  #    触摸门是"盖着⟺装着"这条不变式的执行机构, 一旦某个场景自己去 arm/disarm 就没人说得清了。
  if grep -rn -e 'plat_ts_' -e 'plat_peek2' -e 'plat_touchgate' studio/scenes/ 2>/dev/null \
       | grep -vE ':[[:space:]]*[*/]'; then
    echo "‼️ 上面是场景层在碰引擎内部接口(plat_internal.h) —— 场景不该管触摸门/内存探针"; bad=1
  fi
  # ④ 会画到屏上的文字里出现**中文字面量** —— 说明没走 pcm_i18n.h 那张表。
  #    多语言最常见的坏死法不是翻错, 是**漏翻**: 加个新按钮顺手写了中文, 换成英文界面时
  #    那个按钮还是中文, 而且没人知道它在哪。这条 lint 让"忘了走表"当场红。
  #    ⚠️ 只看 gfx_text*/shell_toast 这几个真的会上屏的函数 —— plat_log 和自检消息
  #      是给开发者看的、永远中文, 拦它们只会让这条守卫变吵, 吵了就会被忽略。
  #    ⚠️ 引号后面那段**也必须排除 `;`**(写成 `[^";]*`)。第一版写的是 `[^"]*`,
  #      于是它能跨过语句末尾的分号一直吃到行尾注释里 —— `gfx_text_w_s("…", sc16); /* … 的宽度 */`
  #      被报成漏翻。一个天天误报的守卫, 三天后就会被人习惯性忽略, 等于没有。
  if grep -rnE '(gfx_text[a-z_]*|shell_toast)\([^;]*"[^";]*[一-龥]' \
       studio/scenes/ studio/sys/ 2>/dev/null | grep -vE ':[[:space:]]*[*/]'; then
    echo "‼️ 上面是**会画到屏上**的中文字面量 —— 加进 studio/sys/pcm_i18n.h 的 I18N_LIST, 用 T(STR_xxx)"; bad=1
  fi
  # ⑤ gf_layer_set_blending 只许收那个 const 全零块。
  #    2026-08-11 真车事故: gdcServerCarmine 的 alpha 平面池**发出去永远收不回**
  #    (分配写 8..11, 释放守卫要求 ≤3, 值域不相交 ⇒ 释放代码不可达), 池被占满
  #    倒车雷达就纯黑。只有**非零** gf_alpha_t 才会走到分配器。
  #    所以"传进去的永远是零"是**安全件**, 不能只靠 const 和注释 —— 谁新写一处
  #    传自己拼的结构体, 编得过也刷得上, 而代价是车主的雷达。
  #    ⚠️ 只扫**产品路径**(platform/sys/scenes)。studio/tools/probe.c 是台架诊断工具, 有自己的
  #      缓冲, 不随 studio 上车; 把它算进来只会让这条守卫变吵。
  if grep -rn 'gf_layer_set_blending(' studio/platform/ studio/sys/ studio/scenes/ 2>/dev/null \
       | grep -v 'g_alpha_zero' | grep -vE ':[[:space:]]*[*/]'; then
    echo "‼️ 上面的 gf_layer_set_blending 没传 g_alpha_zero —— 非零 gf_alpha_t 会占走一块永不归还的 alpha 平面(真车 PDC 变黑的机制)"; bad=1
  fi
  # ⑥ gf_layer_disable 只许出现在 layer_release() 里。
  #    2026-08-15 审计: 三处 disable(让开/进镜像/退出)一个都没查 g_yield ——
  #    让出态下关层就是把**原厂正在显示的东西**关掉。和防绿屏闸门同一条道理:
  #    放在唯一出口上, 谁都绕不过去; 靠每个调用方各记一次, 已经漏过一次了。
  #    合法站点只有三个, 每个都必须挂 /*DISABLE-OK*/ 标记:
  #      layer_release()  —— 唯一出口, 带 g_yield 守卫
  #      plat_init 清残留 —— 跑在占用闸门之后、任何覆盖状态之前
  #      ts_panic_restore —— 崩溃兜底, 走的是 alarm(2) 限时那条路
  #    没标记 = 新加的 = 红。标记是**显式且可 grep** 的, 比"记得查一下 g_yield"可靠。
  if grep -rn 'gf_layer_disable(' studio/platform/ studio/sys/ studio/scenes/ 2>/dev/null \
       | grep -v 'DISABLE-OK' | grep -vE ':[[:space:]]*[*/]'; then
    echo "‼️ 上面的 gf_layer_disable 不在白名单里 —— 让出态下关层会抹掉原厂正在显示的画面。走 layer_release()"; bad=1
  fi
  # ⑦ alarm() 不许出现裸数字。
  #    2026-08-17 台架实测栽过一次: 看门狗**装在** plat_init(无条件), **续在** plat_ts_watch
  #    (只在触摸门装上时), 两者条件不一致 ⇒ 门没装上就 3 秒后被自己打死, 而处理器里
  #    exit(3) 不打日志 ⇒ 日志停在半句话上, 查了一轮才想到看门狗。
  #    同一个常量 + 强制具名, 是让"装"和"续"不可能写成两个数的最省事办法。
  if grep -rnE 'alarm\([0-9]' studio/platform/ studio/sys/ studio/scenes/ 2>/dev/null \
       | grep -vE 'alarm\(0\)' | grep -vE ':[[:space:]]*[*/]'; then
    echo "‼️ 上面的 alarm() 用了裸数字 —— 用 WATCHDOG_SEC / PANIC_EXIT_SEC(alarm(0) 除外)"; bad=1
  fi
  [ $bad -eq 0 ] || { echo "构建被 lint 拦下"; return 1; }
}

do_mac(){
  lint_guards || return 1
  # 🚨 2026-08-13: 这里以前**编译失败也不删旧产物** —— 我改完 main_mac.c 编译红了,
  #    结果起来的还是上一版二进制, 日志照常打, 差点拿旧版的渲染当新版的结果判版式。
  #    (pcm 路径 08-07/08-12 已经因为同一个形状栽过两次, mac 路径一直没补。)
  rm -f /tmp/pcm_studio_run
  # -DSTUDIO_FONT_EXTERNAL: 让 Mac 侧也走**外挂字库**(和台架同一份 studio_cjk.fnt)。
  #   不加这个宏 gfx 用的是内建小字库, **中文全渲成方块**, 版式根本没法判。
  cc -O2 -DSTUDIO_FONT_EXTERNAL -o /tmp/pcm_studio_run studio/main_mac.c -lm || {
      rm -f /tmp/pcm_studio_run
      echo "‼️ Mac 编译失败 -> 产物已删除, 别拿旧版当新版看"; return 1; }
  [ -x /tmp/pcm_studio_run ] || { echo "‼️ 没生成可执行文件"; return 1; }
  echo "✅ Mac:  /tmp/pcm_studio_run"
}
do_pcm(){
  lint_guards || return 1
  # 🚨 2026-08-07 实测过的陷阱: 以前这里没有 pipefail, 而且过滤器把
  #    build_coexist_vol.sh 那句 "!!! 编译失败 —— 旧二进制未被替换 !!!" 整条吃掉
  #    (它不含 error/Built/size:)。结果是**编译失败照样打印上一次的 ck 并 exit 0**。
  #    我拿一个故意的语法错验过: 报了 error, 但 ck 还是旧值, 退出码 0。
  #    而清单里 ck 正是"studio 与 flash 同一版"的死门 —— 假绿会直接害到上机流程。
  #    三道保险: ①先删旧产物 ②pipefail ③过滤器只滤 warning, 别的一律放行。
  #
  # 🚨 2026-08-12 再证伪一次, 又抓到一条(上一版没堵干净):
  #    ①只删了 .stripped, **没删未剥离的 studio/pcm_studio**;
  #    ②build_coexist_vol.sh 里 `strip $OUT -o $OUT.stripped` 跑在 `grep error:` **之前**。
  #    ⇒ 编译失败时 strip 会拿**上一次的** studio/pcm_studio 重新生成一个 .stripped,
  #      于是失败之后目录里躺着一个"旧字节 + 新时间戳"的产物, 而脚本还打印"产物已删除"。
  #      退出码确实是 1, 但谁要是看时间戳就推台架, 推的是上一版 —— 而 ck 正是
  #      "studio 与 flash 同一版"的死门。修法: 两个产物都删, 失败路径上再删一次。
  set -o pipefail
  rm -f studio/pcm_studio studio/pcm_studio.stripped
  bash dev/build_coexist_vol.sh studio/main_pcm.c studio/pcm_studio 2>&1 \
    | grep -vE 'warning:|^\s*[0-9]+ \|' || {
        rm -f studio/pcm_studio studio/pcm_studio.stripped
        echo "‼️ 编译失败 -> 两个产物都已删除, 别推台架"; return 1; }
  [ -f studio/pcm_studio.stripped ] || { echo "‼️ 没生成 .stripped -> 编译没成功"; return 1; }
  python3 - <<'PY'
d=open('studio/pcm_studio.stripped','rb').read()
h=2166136261
for b in d: h=((h^b)*16777619)&0xffffffff
print(f"   ck=0x{h:08x}  <- 推上台架后日志里的'自校验'必须等于它")
PY
}
case "${1:-both}" in
  mac) do_mac;;
  pcm) do_pcm;;
  *)   do_mac; do_pcm;;
esac
