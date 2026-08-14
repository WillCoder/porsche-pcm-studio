/* gf_defs.h - self-contained QNX Graphics Framework (gf) defs for a
 * -nostdlib SH4 overlay against the BENCH gf library.
 *
 * GROUND TRUTH: scratchpad/libgdcApiCarmine.so (QNX 6.3.2, SH4 LE, the actual
 * gf implementation on the bench; server = gdcServerCarmine).
 *   - Constant VALUES cross-checked against QNX 6.5.0 gf.h
 *     (RunzeJustin/qnx650 target/qnx6/usr/include/gf/gf.h). Bit values are
 *     identical across 6.3.2/6.5.0 for everything we use.
 *   - STRUCT OFFSETS verified by disassembling the bench library
 *     (gf_surface_get_info -> gdcSharedMemory::cmd_surface_get_info @0x5bffc,
 *      gf_display_attach @0x879fa). See "VERIFIED" notes below.
 *
 * !!! DIVERGENCE FROM PUBLIC HEADER !!!
 * In the 6.3.2 bench library, gf_surface_info_t has paddr(u64) BEFORE
 * vaddr(ptr): vaddr is at offset 32, stride at 16. The 6.5.0 public header
 * lists vaddr@20/paddr@24 (swapped) -- do NOT use the public layout.
 */
#ifndef GF_DEFS_H
#define GF_DEFS_H

typedef unsigned char      _uint8;
typedef unsigned short     _uint16;
typedef unsigned int       _uint32;
typedef signed   int       _int32;
typedef unsigned long long _uint64;

typedef _uint32 gf_sid_t;    /* surface id */
typedef _uint32 gf_color_t;  /* native-endian B,G,R,A (A in MSB) */

/* opaque handles (all pointers) */
typedef struct _gf_dev     *gf_dev_t;
typedef struct _gf_display *gf_display_t;
typedef struct _gf_layer   *gf_layer_t;
typedef struct _gf_surface *gf_surface_t;
typedef struct _gf_context *gf_context_t;

/* ------------------------------------------------------------------ */
/* gf_dev_attach device selector. GF_DEVICE_INDEX(0) => (const char*)0 */
/* (n<64 ? (const char*)n : ""). Passing plain 0 == index 0 == works.  */
#define GF_DEVICE_INDEX(n)  ((const char *)((n) < 64 ? (long)(n) : 0))

/* ---- error codes (gf_errno.h) ------------------------------------- */
#define GF_ERR_OK            0x00000000
#define GF_ERR_MEM           0x00000001
#define GF_ERR_IODISPLAY     0x00000002
#define GF_ERR_DEVICE        0x00000003
#define GF_ERR_SHMEM         0x00000004
#define GF_ERR_DLL           0x00000005
#define GF_ERR_THREAD        0x00000006
#define GF_ERR_PARM          0x00000007
#define GF_ERR_INUSE         0x00000008
#define GF_ERR_NOSUPPORT     0x00000009
#define GF_ERR_CFG           0x0000000a

/* ---- gf_layer_attach flags ---------------------------------------- */
#define GF_LAYER_ATTACH_PASSIVE       0x00000001
#define GF_LAYER_ATTACH_NODEFAULTS    0x00000002
#define GF_LAYER_ATTACH_NOAUTODISABLE 0x00000004

/* ---- gf_layer_update flags ---------------------------------------- */
/* NOTE: gf_layer_update @0x886c0 on this build does NOT forward its `flags`
 * arg to the server (it hardcodes the internal update flag to 1). Pass 0.
 * Values below are the ABI manifests, harmless but effectively ignored here. */
#define GF_LAYER_UPDATE_NO_WAIT_VSYNC 0x00000001
#define GF_LAYER_UPDATE_NO_WAIT_IDLE  0x00000002

/* ---- gf_surface_create flags -------------------------------------- */
#define GF_SURFACE_CREATE_2D_ACCESSIBLE         0x00000001
#define GF_SURFACE_CREATE_3D_ACCESSIBLE         0x00000002
#define GF_SURFACE_CREATE_CPU_LINEAR_ACCESSIBLE 0x00000004
#define GF_SURFACE_CREATE_CPU_FAST_ACCESS       0x00000008
#define GF_SURFACE_CREATE_PHYS_CONTIG           0x00000010
#define GF_SURFACE_CREATE_PAGE_ALIGNED          0x00000020
#define GF_SURFACE_CREATE_ALPHA_MAP             0x00000040
#define GF_SURFACE_CREATE_SHAREABLE             0x00000080

/* ---- gf_layer_choose_format criteria keys (terminated by GF_NONE) -- */
/* !!! VERIFIED: on this bench build gf_layer_choose_format @0x88844 is a
 * NO-OP STUB (mov #0,r0; rts -> always returns 0, writes nothing). Do NOT
 * rely on it to pick a format. Instead read gf_display_info_t.format (from
 * gf_display_attach/query) and pass it straight to gf_surface_create_layer,
 * exactly as AdvancedGraphicsTest.cc does. Keys kept for source-compat only. */
#define GF_NONE          0
#define GF_2D_RENDERABLE 1
#define GF_RED_SIZE      2
#define GF_GREEN_SIZE    3
#define GF_BLUE_SIZE     4
#define GF_ALPHA_SIZE    5   /* request a format that carries an alpha channel */

/* ---- gf_format_t --------------------------------------------------- */
/* field masks */
#define GF_FORMAT_BPP     0x0000007f
#define GF_FORMAT_PLANAR  0x00000080
#define GF_FORMAT_PKLE    0x00000100
#define GF_FORMAT_PKBE    0x00000200
#define GF_FORMAT_PACK    (GF_FORMAT_PKLE | GF_FORMAT_PKBE)  /* 0x300 */
#define GF_FORMAT_ALPHA   0x00000400
#define GF_FORMAT_PALETTE 0x00000800
#define GF_FORMAT_RGB     0x00001000
#define GF_FORMAT_YUV     0x00002000
#define GF_FORMAT_BO_BGRA 0x00000000
#define GF_FORMAT_BO_ARGB 0x00000200
typedef enum {
    GF_FORMAT_INVALID       = 0,
    GF_FORMAT_BYTE          = 8,                                   /* 0x008 alpha-map plane */
    GF_FORMAT_PAL8          = 8 | GF_FORMAT_PALETTE,               /* 0x808 */
    GF_FORMAT_PACK_ARGB1555 = 16| GF_FORMAT_PACK |GF_FORMAT_ALPHA|GF_FORMAT_RGB, /* 0x1710 (== device RGBA5551) */
    GF_FORMAT_PKLE_ARGB1555 = 16| GF_FORMAT_PKLE |GF_FORMAT_ALPHA|GF_FORMAT_RGB, /* 0x1510 */
    GF_FORMAT_PKBE_ARGB1555 = 16| GF_FORMAT_PKBE |GF_FORMAT_ALPHA|GF_FORMAT_RGB, /* 0x1610 */
    GF_FORMAT_PACK_RGB565   = 16| GF_FORMAT_PACK |GF_FORMAT_RGB,   /* 0x1310 */
    GF_FORMAT_PKLE_RGB565   = 16| GF_FORMAT_PKLE |GF_FORMAT_RGB,   /* 0x1110 */
    GF_FORMAT_PKBE_RGB565   = 16| GF_FORMAT_PKBE |GF_FORMAT_RGB,   /* 0x1210 */
    GF_FORMAT_BGR888        = 24| GF_FORMAT_RGB,                   /* 0x1018 */
    GF_FORMAT_BGRA8888      = 32| GF_FORMAT_ALPHA|GF_FORMAT_RGB|GF_FORMAT_BO_BGRA, /* 0x1420 */
    GF_FORMAT_ARGB8888      = 32| GF_FORMAT_ALPHA|GF_FORMAT_RGB|GF_FORMAT_BO_ARGB  /* 0x1620 */
} gf_format_t;

/* ---- gf_alpha_t.mode bits (blending) ------------------------------ */
/* M1 source select */
#define GF_ALPHA_M1_SRC_PIXEL_ALPHA 0x00010000
#define GF_ALPHA_M1_DST_PIXEL_ALPHA 0x00020000
#define GF_ALPHA_M1_GLOBAL          0x00040000  /* use gf_alpha_t.m1 */
#define GF_ALPHA_M1_MAP             0x00080000  /* use gf_alpha_t.map */
/* M2 source select */
#define GF_ALPHA_M2_SRC_PIXEL_ALPHA 0x01000000
#define GF_ALPHA_M2_DST_PIXEL_ALPHA 0x02000000
#define GF_ALPHA_M2_GLOBAL          0x04000000  /* use gf_alpha_t.m2 */
#define GF_ALPHA_M2_MAP             0x08000000
/* blend source factor (bits 8-11) */
#define GF_BLEND_SRC_0     0x00000000
#define GF_BLEND_SRC_M1    0x00000100
#define GF_BLEND_SRC_1mM1  0x00000200
#define GF_BLEND_SRC_M2    0x00000400
#define GF_BLEND_SRC_1     0x00000600
/* blend destination factor (bits 0-3) */
#define GF_BLEND_DST_0     0x00000000
#define GF_BLEND_DST_M1    0x00000001
#define GF_BLEND_DST_1mM1  0x00000002
#define GF_BLEND_DST_M2    0x00000004
#define GF_BLEND_DST_1     0x00000006
/* Canonical "source-over" per-pixel alpha:
 *   mode = GF_ALPHA_M1_SRC_PIXEL_ALPHA | GF_BLEND_SRC_M1 | GF_BLEND_DST_1mM1
 * Global constant alpha: add GF_ALPHA_M1_GLOBAL and set .m1.            */

/* ---- gf_chroma_t.mode bits (color-key) ---------------------------- */
#define GF_CHROMA_OP_SRC_MATCH 0x00000001
#define GF_CHROMA_OP_DST_MATCH 0x00000002
#define GF_CHROMA_OP_DRAW      0x00000004
#define GF_CHROMA_OP_NO_DRAW   0x00000008

/* ================================================================== */
/* STRUCTS. Layouts below are the BENCH (6.3.2) layouts, verified by   */
/* disassembly where noted. gf uses natural alignment (NOT packed).    */
/* ================================================================== */

typedef struct { _int32 x, y; } gf_point_t;
typedef struct { _int32 ncolors; gf_color_t *colors; } gf_palette_t;

/* gf_dev_info_t: ndisplays@0 confirmed (app reads only this). Rest per header. */
typedef struct {
    _uint32     ndisplays;   /* +0  */
    _uint32     flags;       /* +4  */
    const char *description; /* +8  */
    _uint32     vendor_id;   /* +12 */
    _uint32     device_id;   /* +16 */
    _int32      index;       /* +20 */
    _int32      reserved[3]; /* +24 */
} gf_dev_info_t;             /* size 36 */

/* gf_display_info_t: VERIFIED - gf_display_attach memcpy's exactly 20 bytes
 * (0x87a52: n=20) into this struct, matching field order below. */
typedef struct {
    _uint32     nlayers;          /* +0  */
    _uint32     main_layer_index; /* +4  */
    _uint16     xres;             /* +8  */
    _uint16     yres;             /* +10 */
    gf_format_t format;           /* +12 */
    _int32      refresh;          /* +16 (Hz) */
    _int32      reserved[6];      /* +20 (NOT filled by attach) */
} gf_display_info_t;              /* first 20 bytes are the live payload */

/* gf_surface_info_t: VERIFIED via gdcSharedMemory::cmd_surface_get_info @0x5bffc.
 * Store offsets observed: @0 sid, @4 w, @8 h, @12 format, @16 stride,
 * @24 paddr.lo, @28 paddr.hi(=0), @32 vaddr, @36/@40 palette, @44 flags.
 * NOTE: vaddr@32 and stride@16 match the project's coexist code. This is the
 * paddr/vaddr order that DIFFERS from the 6.5.0 public header. */
typedef struct {
    gf_sid_t     sid;        /* +0  */
    _int32       w;          /* +4  */
    _int32       h;          /* +8  */
    gf_format_t  format;     /* +12 */
    _uint32      stride;     /* +16  <-- bytes per row */
    _uint32      _pad20;     /* +20  alignment before u64 paddr */
    _uint64      paddr;      /* +24 */
    _uint8      *vaddr;      /* +32  <-- CPU pointer to pixels */
    gf_palette_t palette;    /* +36 (ncolors@36, colors@40) */
    _uint32      flags;      /* +44 */
    _int32       reserved[6];/* +48 */
} gf_surface_info_t;

/* gf_layer_info_t: filled by gf_layer_query (per-format-index). Matches
 * public header; we mainly read .format and .caps. */
typedef struct {
    gf_format_t format;             /* +0  */
    _uint32     caps;               /* +4  */
    _uint32     alpha_valid_flags;  /* +8  */
    _uint32     alpha_combinations; /* +12 */
    _uint64     order_caps;         /* +16 */
    _uint32     chromakey_caps;     /* +24 */
    _int32      src_max_height;     /* +28 */
    _int32      src_max_width;      /* +32 */
    _int32      src_max_viewport_height;
    _int32      src_max_viewport_width;
    _int32      dst_max_height;
    _int32      dst_max_width;
    _int32      dst_min_height;
    _int32      dst_min_width;
    _int32      max_scaleup_x;
    _int32      max_scaleup_y;
    _int32      max_scaledown_x;
    _int32      max_scaledown_y;
    _uint32     output_mask;
    _uint32     vcap_mask;
    _int32      reserved[6];
} gf_layer_info_t;

/* gf_alpha_t: passed to gf_layer_set_blending / gf_context_set_alpha.
 * Matches public header (unchanged across versions). map=surface handle. */
typedef struct {
    _uint32      mode;          /* +0  GF_ALPHA_* | GF_BLEND_* */
    _uint32      map_x_offset;  /* +4  */
    _uint32      map_y_offset;  /* +8  */
    gf_surface_t map;           /* +12 alpha-map surface (or 0) */
    _uint8       m1;            /* +16 global M1 (0..255) */
    _uint8       m2;            /* +17 global M2 */
    _uint16      padding;       /* +18 */
    _int32       reserved[4];   /* +20 */
} gf_alpha_t;                   /* size 36 */

typedef struct {
    _uint32     mode;           /* +0  GF_CHROMA_OP_* */
    gf_color_t  color0;         /* +4  */
    gf_color_t  color1;         /* +8  */
    gf_color_t  mask;           /* +12 */
    _int32      reserved[4];    /* +16 */
} gf_chroma_t;

/* ================================================================== */
/* FUNCTION PROTOTYPES (arg order/count verified vs bench dynsym+usage) */
/* ================================================================== */
extern int  gf_dev_attach(gf_dev_t *pdev, const char *fname, gf_dev_info_t *info);
extern void gf_dev_detach(gf_dev_t gdev);
extern int  gf_display_attach(gf_display_t *pdisplay, gf_dev_t gdev, unsigned display_index, gf_display_info_t *info);
extern void gf_display_detach(gf_display_t display);
/* 真·屏幕截图: 把合成后的显示内容抓进 surface。RE 确认 7 参(r4-r7 + 3 栈参),
 * 空 display 返回 GF_ERR_PARM=7。vaddr 0x87c06。*/
extern int  gf_display_snapshot(gf_display_t display, int output,
                                int x1, int y1, int x2, int y2, gf_surface_t surface);
extern int  gf_display_set_layer_order(gf_display_t display, const unsigned order[], unsigned flags);
extern int  gf_layer_attach(gf_layer_t *player, gf_display_t display, unsigned layer_index, unsigned flags);
extern void gf_layer_detach(gf_layer_t layer);
extern int  gf_layer_query(gf_layer_t layer, int format_index, gf_layer_info_t *info);
extern unsigned gf_layer_choose_format(gf_layer_t layer, gf_format_t format[], unsigned nformat, const unsigned *criteria);
extern void gf_layer_enable(gf_layer_t layer);
extern void gf_layer_disable(gf_layer_t layer);
extern void gf_layer_set_surfaces(gf_layer_t layer, gf_surface_t surfs[], unsigned nsurfs);
extern void gf_layer_set_src_viewport(gf_layer_t layer, int x1, int y1, int x2, int y2);
extern void gf_layer_set_dst_viewport(gf_layer_t layer, int x1, int y1, int x2, int y2);
extern void gf_layer_set_blending(gf_layer_t layer, const gf_alpha_t *alpha);
extern void gf_layer_set_chroma(gf_layer_t layer, const gf_chroma_t *chroma);
extern int  gf_layer_update(gf_layer_t layer, unsigned flags);
extern int  gf_layer_update_multi(gf_layer_t layer[], unsigned nlayers, unsigned flags);
extern int  gf_surface_create(gf_surface_t *psurface, gf_dev_t gfx, int w, int h, gf_format_t format, const gf_palette_t *palette, unsigned flags);
extern int  gf_surface_create_layer(gf_surface_t *psurface, gf_layer_t *layers, int nlayers, int surface_index, int w, int h, gf_format_t format, const gf_palette_t *palette, unsigned flags);
extern int  gf_surface_attach(gf_surface_t *psurface, gf_dev_t gfx, int w, int h, int stride, gf_format_t format, const gf_palette_t *palette, _uint8 *ptr, unsigned flags);
extern int  gf_surface_attach_by_sid(gf_surface_t *psurface, gf_dev_t gfx, gf_sid_t sid);
extern gf_surface_info_t *gf_surface_get_info(gf_surface_t surface, gf_surface_info_t *info);
extern void gf_surface_free(gf_surface_t surface);

/* NOTE: gf_layer_flip_bank is NOT exported by libgdcApiCarmine.so (bench).
 * Use gf_layer_set_surfaces + gf_layer_update to swap banks instead. */

#endif /* GF_DEFS_H */
