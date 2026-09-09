/*
 * image.cc
 *
 * Handling images
 *
 * by WangLu
 * 2012.08.14
 */

#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

using std::cerr;
using std::endl;

#include "HTMLRenderer.h"
#include "Base64Stream.h"
#include "util/namespace.h"
#include "util/image_split.h"

namespace pdf2htmlEX {

using std::string;
using std::ifstream;

void HTMLRenderer::drawImage(GfxState * state, Object * ref, Stream * str,
                 int width, int height, GfxImageColorMap * colorMap,
                 bool interpolate, const int *maskColors, bool inlineImg)
{
    tracer.draw_image(state);
    if(getenv("SPLIT_IMAGE_DEBUG"))
        cerr << "[split-image] drawImage page=" << pageNum << " img=" << width << "x" << height
             << " mask=" << (maskColors != nullptr) << " inline=" << inlineImg << endl;
    // --split-images: 大面积/轴对齐/未裁剪的图片拆为独立 <img>, 便于前端移动编辑;
    // 色键掩码(maskColors)与内联图片不参与拆分
    if(param.split_images && !inlineImg && maskColors == nullptr
            && should_split_image(state, width, height, colorMap,
                                  text_zoom_factor() * DEFAULT_DPI, param.split_image_min_size)
            && split_image_to_html(state, ref, str, width, height, colorMap,
                                   nullptr, 0, 0, nullptr))
    {
        return;
    }

    return OutputDev::drawImage(state,ref,str,width,height,colorMap,interpolate,maskColors,inlineImg);
}

void HTMLRenderer::drawSoftMaskedImage(GfxState *state, Object *ref, Stream *str,
                   int width, int height,
                   GfxImageColorMap *colorMap,
                   bool interpolate,
                   Stream *maskStr,
                   int maskWidth, int maskHeight,
                   GfxImageColorMap *maskColorMap,
                   bool maskInterpolate)
{
    tracer.draw_image(state);
    if(param.split_images
            && should_split_image(state, width, height, colorMap,
                                  text_zoom_factor() * DEFAULT_DPI, param.split_image_min_size)
            && split_image_to_html(state, ref, str, width, height, colorMap,
                                   maskStr, maskWidth, maskHeight, maskColorMap))
    {
        return;
    }

    return OutputDev::drawSoftMaskedImage(state,ref,str, // TODO really required?
            width,height,colorMap,interpolate,
            maskStr, maskWidth, maskHeight, maskColorMap, maskInterpolate);
}

void HTMLRenderer::drawMaskedImage(GfxState *state, Object *ref, Stream *str,
                   int width, int height,
                   GfxImageColorMap *colorMap,
                   bool interpolate,
                   Stream *maskStr,
                   int maskWidth, int maskHeight,
                   bool maskInvert, bool maskInterpolate)
{
    tracer.draw_image(state);
    // 显式掩码图片不拆分(拆分会丢掩码), 直接走 OutputDev 默认(文字趟不绘制),
    // 背景趟由 Splash/CairoOutputDev 带掩码正常绘制
    OutputDev::drawImage(state, ref, str, width, height, colorMap, interpolate, nullptr, false);
}

/*
 * --split-images 的核心: 把图片导出为文件并输出独立定位的 <img class="si ...">。
 * 返回 false 时调用方回退到背景合并(不丢图)。
 */
bool HTMLRenderer::split_image_to_html(GfxState * state, Object * ref, Stream * str,
                   int width, int height, GfxImageColorMap * colorMap,
                   Stream * maskStr, int maskWidth, int maskHeight,
                   GfxImageColorMap * maskColorMap)
{
    const double * ctm = state->getCTM();
    bool flip_h = ctm[0] < 0;
    bool flip_v = ctm[3] < 0;

    if(getenv("SPLIT_IMAGE_DEBUG") && f_curpage)
        cerr << "[split-image] tellp=" << f_curpage->tellp() << " "
             << "ctm=[" << ctm[0] << "," << ctm[1] << "," << ctm[2] << ","
             << ctm[3] << "," << ctm[4] << "," << ctm[5] << "] page=" << pageNum
             << " imgsize=" << width << "x" << height << endl;

    // 设备空间包围盒: 图像空间单位方格四角经 CTM 映射
    double xs[4] = {ctm[4], ctm[0] + ctm[4], ctm[2] + ctm[4], ctm[0] + ctm[2] + ctm[4]};
    double ys[4] = {ctm[5], ctm[1] + ctm[5], ctm[3] + ctm[5], ctm[1] + ctm[3] + ctm[5]};
    double x_min = *std::min_element(xs, xs + 4), x_max = *std::max_element(xs, xs + 4);
    double y_min = *std::min_element(ys, ys + 4), y_max = *std::max_element(ys, ys + 4);

    // 同一 PDF 图像对象只导出一次(页眉 logo 等重复引用场景)
    string src;
    bool have_ref = (ref != nullptr) && ref->isRef();
    auto key = have_ref ? std::make_pair(ref->getRefNum(), ref->getRefGen())
                        : std::make_pair(-1, -1);
    auto iter = have_ref ? split_image_src_map.find(key) : split_image_src_map.end();
    if (iter != split_image_src_map.end())
    {
        src = iter->second;
    }
    else
    {
        ++split_image_count;
        string fn_base = (char*)str_fmt("si%llx", split_image_count);
        string path_prefix = (param.embed_image ? param.tmp_dir : param.dest_dir) + "/" + fn_base;

        string ext = maskStr
            ? (dump_soft_masked_image(str, width, height, colorMap,
                                      maskStr, maskWidth, maskHeight, maskColorMap,
                                      flip_h, flip_v, path_prefix) ? "png" : "")
            : dump_image(str, width, height, colorMap, flip_h, flip_v, path_prefix);
        if (ext.empty())
        {
            cerr << "Warning: failed to split image " << split_image_count
                 << ", keep it in the page background" << endl;
            return false;
        }

        if(param.embed_image)
        {
            string path = path_prefix + "." + ext;
            tmp_files.add(path);
            ifstream fin(path, ifstream::binary);
            if(!fin)
                return false;
            auto mime_iter = FORMAT_MIME_TYPE_MAP.find(ext);
            if(mime_iter == FORMAT_MIME_TYPE_MAP.end())
                return false;
            std::ostringstream oss;
            oss << "data:" << mime_iter->second << ";base64," << Base64Stream(fin);
            src = oss.str();
        }
        else
        {
            src = fn_base + "." + ext;
        }

        if (have_ref)
            split_image_src_map[key] = src;
    }

    // 文本是缓冲到 HTMLTextPage 在 endPage 才落盘的, 这里同样先入缓冲,
    // 由 endPage 在背景图之后、文本之前输出(版式: 压在背景上、垫在文本下)。
    // 位置用内联 style 而非 StateManager 类(id 与文本共享管理器, 曾出现 id/值错位),
    // 图片数量少, 不值得为它走类去重。
    std::ostringstream oss;
    oss << "<img class=\"" << CSS::SPLIT_IMAGE_CN
        << "\" style=\"left:" << x_min << "px;bottom:" << y_min
        << "px;width:" << (x_max - x_min) << "px;height:" << (y_max - y_min) << "px;"
        << "\" alt=\"\" src=\"" << src << "\"/>";
    split_image_elements.push_back(oss.str());

    return true;
}

} // namespace pdf2htmlEX
