#!/usr/bin/env python3
"""studio/tools/bake_font.py — 给 PCM Studio 烤字库(抗锯齿字模图集)

嵌入式标准做法: 离线把真字体光栅化成灰度覆盖字模, 运行时只查表+alpha混合 ⇒ SH4 上也跑得动,
不需要运行时 TTF 光栅器。Mac 和台架用同一份字模 ⇒ 所见即所得。

输出 studio/scenes/studio_font.h(Studio 专属, 不覆盖 coexist-app/mvp/ui_font.h)。

用法:
  python3 studio/tools/bake_font.py > studio/scenes/studio_font.h
  python3 studio/tools/bake_font.py --extra "额外要用的汉字" > ...
"""
import sys, argparse
from PIL import Image, ImageDraw, ImageFont

# 场景里会出现的字符集。汉字按需加(CJK 全集太大, 只烤用得到的)。
ASCII = ''.join(chr(c) for c in range(0x20, 0x7f))
CJK = (
    # 标点/符号(· 是分隔符, 到处在用)
    "·—…、。，：；？！《》（）℃°"
    # 通用 UI
    "蓝牙音频播放暂停上下曲目排序媒体选项设置返回确定取消关闭开启连接断开择机旋钮进入按压确认更多空"
    "首页音源收音电话导航信息车辆系统时间日期"
    # 状态/提示 + 系统会说的话(toast/对话框用字, 缺一个就露空框)
    "正在中已无未搜索列表当前无线设备名称电量信号强度"
    "这那面还做完成失败错误请稍候重试成功保存加载启动关机"
    "支持可用不可用第次个页项条最新旧同步等待准备就绪"
    "静音最大小声调节亮度屏幕"
    # 电台
    "频道存储自动预设波段立体声路况"
    # 常见歌名字(示例内容用)
    "扒伪黄帝新装的了是我你他她们不在有和就人都一个啊哦呀吧吗呢么什怎样好可以能会要想说看听走来去"
)

def scan_source_chars():
    """从 studio/ 的源码里扫出所有出现在字符串字面量里的汉字。
    ⚠️ 这条是必须的: 靠手写字符集猜"要哪些字"必然漏(2026-07-17 亲历:
    toast "这个页面还没做" 只有"个页"在库里, 屏幕上就剩两个字, 查了半天)。
    现在改成 —— 代码里写了什么字, 就烤什么字。"""
    import glob, re, os
    chars = set()
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    for f in glob.glob(os.path.join(root, '**', '*.c'), recursive=True) + \
             glob.glob(os.path.join(root, '**', '*.h'), recursive=True):
        if 'studio_font.h' in f:      # 别扫自己(几万个数字)
            continue
        try:
            src = open(f, encoding='utf-8', errors='ignore').read()
        except Exception:
            continue
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
            for ch in lit:
                if '\u4e00' <= ch <= '\u9fff' or ch in '·—…、。，：；？！《》（）℃°':
                    chars.add(ch)
    return ''.join(sorted(chars))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--body-font', default='/System/Library/Fonts/Supplemental/Arial Unicode.ttf')
    ap.add_argument('--body-px', type=int, default=22)
    ap.add_argument('--face-index', type=int, default=0,
                    help='.ttc 里第几个字面(PingFang.ttc: 3=SC Regular, 7=SC Medium)')
    ap.add_argument('--weight', default='',
                    help='可变字体的命名字重, 例 Medium。**思源黑体可变版默认是 Thin(100),\n                          不指定就会烘出发丝一样细的字**')
    ap.add_argument('--extra', default='')
    ap.add_argument('--charset', default='',
                    help='额外字符集: gb2312-1 = GB2312 一级字(3755, 覆盖现代中文约 99.7%% 用字), '
                         'gb2312 = 一级+二级(6763), cjk = 全部 U+4E00..U+9FFF(20992, **别用**)')
    ap.add_argument('--bin', help='输出二进制字库(运行时加载), 不走 stdout')

    a = ap.parse_args()

    def charset_chars(name):
        """🚨 为什么需要它: 曲名/歌手是**任意中文**, 手挑字符集必漏 —— 但烤全字符集
        (U+4E00..9FFF, 12792 字形)出来是 **7.3MB**, 在台架上放不下:
          · /HBpersistence 只有约 3.4MB 可用(还被断电残留的块吃掉一截)
          · /tmp 是内存盘, 每次重启要重拷
          · 2026-08-14 那次"字库残骸顶掉好文件"的事故, 土壤就是它太大
        GB2312 一级字 3755 个覆盖现代中文约 99.7% 的用字, 烤出来约 2.1MB, 两个地方都放得下。
        漏掉的是生僻字, 会渲成占位框 —— 诚实, 而且极少出现。"""
        out = []
        if name in ('gb2312-1', 'gb2312'):
            lo, hi = 0xB0, (0xD7 if name == 'gb2312-1' else 0xF7)
            for b1 in range(lo, hi + 1):
                for b2 in range(0xA1, 0xFF):
                    try:
                        ch = bytes([b1, b2]).decode('gb2312')
                    except Exception:
                        continue
                    if '\u4e00' <= ch <= '\u9fff':
                        out.append(ch)
        elif name == 'cjk':
            out = [chr(c) for c in range(0x4e00, 0xa000)]
        elif name:
            sys.stderr.write('未知 --charset %s\n' % name)
        return ''.join(out)

    src_chars = scan_source_chars()
    cs_chars = charset_chars(a.charset)
    if cs_chars:
        sys.stderr.write('字符集 %s: %d 个汉字\n' % (a.charset, len(cs_chars)))
    chars = []
    seen = set()
    for ch in ASCII + CJK + src_chars + cs_chars + a.extra:
        if ch not in seen:
            seen.add(ch); chars.append(ch)

    font = ImageFont.truetype(a.body_font, a.body_px, index=a.face_index)
    if a.weight:
        font.set_variation_by_name(a.weight)      # 可变字体: 选命名实例
        sys.stderr.write('可变字重 -> %s\n' % a.weight)
    asc, desc = font.getmetrics()

    chars.sort(key=ord)          # 按 codepoint 升序 -> 运行时可二分查找
    glyphs = []   # (cp, w, h, xo, yo, adv, off)
    blob = bytearray()
    for ch in chars:
        try:
            mask = font.getmask(ch, mode='L')
        except Exception:
            continue
        w, h = mask.size
        bbox = font.getbbox(ch)
        adv = int(font.getlength(ch))
        xo = bbox[0] if bbox else 0
        yo = bbox[1] if bbox else 0
        data = bytes(mask) if w and h else b''
        glyphs.append((ord(ch), w, h, xo, yo, adv, len(blob)))
        blob += data

    # ---- --bin: 输出运行时可加载的二进制字库 ----
    # 为什么要它: 字库占了 SH4 二进制的 211KB/263KB, 而它很少变。拆出去之后
    # 改代码只推 ~52KB(串口 2.5 分钟), 字库只推一次 —— 迭代成本降 5 倍。
    # 布局(全部小端, 与 SFontGlyph 的 C 结构逐字节一致):
    #   magic "SFNT" | u32 ng | u32 asc | u32 px | u32 datalen
    #   ng × 20 字节: i32 cp, i16 w,h,xo,yo,adv, i16 pad, i32 off
    #   datalen 字节覆盖数据
    if a.bin:
        import struct as _st
        hdr = b'SFNT' + _st.pack('<4I', len(glyphs), asc, a.body_px, len(blob))
        tbl = b''.join(_st.pack('<i6hi', cp, w, h, xo, yo, adv, 0, off)
                       for cp,w,h,xo,yo,adv,off in glyphs)
        open(a.bin,'wb').write(hdr + tbl + bytes(blob))
        sys.stderr.write("wrote %s: %d 字形, 表 %d B, 数据 %d B, 合计 %d B\n"
                         % (a.bin, len(glyphs), len(tbl), len(blob), len(hdr)+len(tbl)+len(blob)))
        return

    out = sys.stdout
    out.write("/* 自动生成: studio/tools/bake_font.py | 正文=%s@%dpx | %d 字形 */\n"
              % (a.body_font.split('/')[-1], a.body_px, len(glyphs)))
    out.write("#ifndef STUDIO_FONT_H\n#define STUDIO_FONT_H\n\n")
    out.write("#define SFONT_ASC %d\n#define SFONT_PX %d\n" % (asc, a.body_px))
    out.write("#define SFONT_NG %d\n\n" % len(glyphs))
    out.write("typedef struct { int cp; short w,h,xo,yo,adv; int off; } SFontGlyph;\n")
    out.write("static const SFontGlyph SFONT_G[SFONT_NG]={\n")
    for cp,w,h,xo,yo,adv,off in glyphs:
        out.write(" {%d,%d,%d,%d,%d,%d,%d},\n" % (cp,w,h,xo,yo,adv,off))
    out.write("};\n\n")
    out.write("static const unsigned char SFONT_DATA[%d]={" % max(len(blob),1))
    out.write(','.join(str(b) for b in blob) if blob else '0')
    out.write("};\n\n#endif\n")
    sys.stderr.write("baked %d glyphs (%d 个来自源码扫描), %d bytes\n" % (len(glyphs), len(src_chars), len(blob)))

if __name__ == '__main__':
    main()

# ============ 定稿配方(2026-08-14) ============
# 部署用的这份就是这么烤的:
#   python3 studio/tools/bake_font.py --body-font references/fonts/NotoSansSC-var.ttf \
#           --weight Medium --body-px 24 --charset gb2312 --bin studio/studio_notosc.fnt
# 为什么是 gb2312(一级+二级 6763 字)而不是全字符集:
#   · 全字符集 12792 字形 = **7.3MB**, 台架上放不下 —— /HBpersistence 空间紧,
#     /tmp 是内存盘每次重启要重拷; 2026-08-14 那次"字库残骸顶掉好文件"的事故土壤就是它。
#   · 只烤一级(3755)= 2.1MB, 但实测**曲名里会缺字**(「陈奕迅」的"奕"就在二级)。
#     曲名出现方框比字库大 1.6MB 更糟。
#   · 一级+二级 = 6875 字形 / **3.76MB**, 39 个真实曲名歌手 110 个汉字**零缺失**,
#     且文字区渲染与全字符集版**逐像素完全相同**(同 TTF 同字号同光栅化, 实测 0 差异)。
# ⚠️ --weight Medium 不能省: 思源黑体可变版默认是 Thin(100), 不指定会烘出发丝一样细的字。
