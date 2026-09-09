/*
 * image_split.cc
 *
 * 实现见 image_split.h。PNG 用 libpng 直接写, 不依赖 poppler utils 的 PNGWriter
 * (后者在 utils/ 下, 不随库安装导出)。
 */

#include "image_split.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <algorithm>

#include <png.h>
#include <Stream.h>
#include <Object.h>

namespace pdf2htmlEX {

using std::string;
using std::vector;

bool should_split_image(GfxState * state, int img_width, int img_height,
                        GfxImageColorMap * colorMap, double dpi, double min_edge_pt)
{
    const double * ctm = state->getCTM();
    const bool dbg = getenv("SPLIT_IMAGE_DEBUG") != nullptr;
#define SPLIT_DBG_FAIL(reason) do { if(dbg) fprintf(stderr, "[split-image] skip: %s (ctm=[%g,%g,%g,%g,%g,%g] img=%dx%d)\n", reason, ctm[0],ctm[1],ctm[2],ctm[3],ctm[4],ctm[5], img_width, img_height); return false; } while(0)

    // 轴对齐(无旋转/斜切)
    if (std::fabs(ctm[1]) > 1e-4 || std::fabs(ctm[2]) > 1e-4)
        SPLIT_DBG_FAIL("rotated");

    double w_dev = std::fabs(ctm[0]);
    double h_dev = std::fabs(ctm[3]);
    if (w_dev < 1e-6 || h_dev < 1e-6)
        SPLIT_DBG_FAIL("degenerate");

    // 显示尺寸阈值(换算成 pt, 与渲染 DPI 无关, 保证文字趟/背景趟判定一致)
    if (w_dev * 72.0 / dpi < min_edge_pt || h_dev * 72.0 / dpi < min_edge_pt)
        SPLIT_DBG_FAIL("too small");

    // 颜色可导出
    if (colorMap->getBits() != 8)
        SPLIT_DBG_FAIL("bits!=8");
    int comps = colorMap->getNumPixelComps();
    if (comps != 1 && comps != 3 && comps != 4)
        SPLIT_DBG_FAIL("comps");

    // 未被裁剪: 图片设备包围盒 ⊂ 当前裁剪包围盒(1px 容差)
    double xs[4] = {ctm[4], ctm[0] + ctm[4], ctm[2] + ctm[4], ctm[0] + ctm[2] + ctm[4]};
    double ys[4] = {ctm[5], ctm[1] + ctm[5], ctm[3] + ctm[5], ctm[1] + ctm[3] + ctm[5]};
    double x_min = *std::min_element(xs, xs + 4), x_max = *std::max_element(xs, xs + 4);
    double y_min = *std::min_element(ys, ys + 4), y_max = *std::max_element(ys, ys + 4);

    double cx0, cy0, cx1, cy1;
    state->getClipBBox(&cx0, &cy0, &cx1, &cy1);
    const double clip_eps = 1.0;
    if (x_min < cx0 - clip_eps || y_min < cy0 - clip_eps
            || x_max > cx1 + clip_eps || y_max > cy1 + clip_eps)
        SPLIT_DBG_FAIL("clipped");

    return true;
#undef SPLIT_DBG_FAIL
}

namespace {

/* 把 ImageStream 解码为 RGB(A) 缓冲, 供翻转/合成后写 PNG */
bool decode_rgb(Stream * str, int width, int height, GfxImageColorMap * colorMap,
                vector<unsigned char> & out)
{
    ImageStream img_str(str, width, colorMap->getNumPixelComps(), colorMap->getBits());
    img_str.reset();

    out.resize((size_t)width * height * 3);
    int comps = colorMap->getNumPixelComps();
    for (int y = 0; y < height; ++y)
    {
        unsigned char * p = img_str.getLine();
        if (!p)
            return false;
        for (int x = 0; x < width; ++x)
        {
            GfxRGB rgb;
            colorMap->getRGB(p, &rgb);
            size_t base = ((size_t)y * width + x) * 3;
            out[base]     = colToByte(rgb.r);
            out[base + 1] = colToByte(rgb.g);
            out[base + 2] = colToByte(rgb.b);
            p += comps;
        }
    }
    img_str.close();
    return true;
}

bool write_png_pixels(const vector<unsigned char> & buf, int width, int height, bool rgba,
                      bool flip_h, bool flip_v, const string & path)
{
    FILE * fp = fopen(path.c_str(), "wb");
    if (!fp)
        return false;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png)))
    {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return false;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, width, height, 8,
                 rgba ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    int bpp = rgba ? 4 : 3;
    vector<unsigned char> row((size_t)width * bpp);
    for (int y = 0; y < height; ++y)
    {
        int sy = flip_v ? (height - 1 - y) : y;
        for (int x = 0; x < width; ++x)
        {
            int sx = flip_h ? (width - 1 - x) : x;
            memcpy(row.data() + (size_t)x * bpp,
                   buf.data() + ((size_t)sy * width + sx) * bpp, bpp);
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return true;
}

} // anonymous namespace

string dump_image(Stream * str, int width, int height, GfxImageColorMap * colorMap,
                  bool flip_h, bool flip_v, const string & path_prefix)
{
    int comps = colorMap->getNumPixelComps();

    // DCT 原样直传(翻转会导致像素序错误, 有翻转时退化解码转 PNG)
    if (!flip_h && !flip_v && str->getKind() == strDCT
            && (comps == 1 || comps == 3) && colorMap->getBits() == 8)
    {
        string path = path_prefix + ".jpg";
        std::ofstream out(path, std::ofstream::binary);
        if (!out)
            return "";
        Stream * raw = str->getUndecodedStream();
        raw->reset();
        int c;
        while ((c = raw->getChar()) != EOF)
            out.put((char)c);
        raw->close();
        return out ? "jpg" : "";
    }

    vector<unsigned char> buf;
    if (!decode_rgb(str, width, height, colorMap, buf))
        return "";
    if (!write_png_pixels(buf, width, height, false, flip_h, flip_v, path_prefix + ".png"))
        return "";
    return "png";
}

bool dump_soft_masked_image(Stream * str, int width, int height, GfxImageColorMap * colorMap,
                            Stream * mask_str, int mask_width, int mask_height, GfxImageColorMap * mask_color_map,
                            bool flip_h, bool flip_v, const string & path_prefix)
{
    vector<unsigned char> rgb_buf;
    if (!decode_rgb(str, width, height, colorMap, rgb_buf))
        return false;

    // 掩码解码为灰度(掩码与主图尺寸可能不同, 最近邻缩放)
    vector<unsigned char> mask_buf;
    {
        ImageStream mask_img(mask_str, mask_width,
                             mask_color_map->getNumPixelComps(), mask_color_map->getBits());
        mask_img.reset();
        mask_buf.resize((size_t)mask_width * mask_height);
        int mcomps = mask_color_map->getNumPixelComps();
        for (int y = 0; y < mask_height; ++y)
        {
            unsigned char * p = mask_img.getLine();
            if (!p)
                return false;
            for (int x = 0; x < mask_width; ++x)
            {
                GfxGray gray;
                mask_color_map->getGray(p, &gray);
                mask_buf[(size_t)y * mask_width + x] = colToByte(gray);
                p += mcomps;
            }
        }
        mask_img.close();
    }

    vector<unsigned char> rgba((size_t)width * height * 4);
    for (int y = 0; y < height; ++y)
    {
        int my = std::min((int)((long long)y * mask_height / height), mask_height - 1);
        for (int x = 0; x < width; ++x)
        {
            int mx = std::min((int)((long long)x * mask_width / width), mask_width - 1);
            size_t src = ((size_t)y * width + x) * 3;
            size_t dst = ((size_t)y * width + x) * 4;
            rgba[dst]     = rgb_buf[src];
            rgba[dst + 1] = rgb_buf[src + 1];
            rgba[dst + 2] = rgb_buf[src + 2];
            rgba[dst + 3] = mask_buf[(size_t)my * mask_width + mx];
        }
    }

    return write_png_pixels(rgba, width, height, true, flip_h, flip_v, path_prefix + ".png");
}

} // namespace pdf2htmlEX
