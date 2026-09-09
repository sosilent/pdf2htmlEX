/*
 * image_split.h
 *
 * --split-images: 把大面积、轴对齐、未被裁剪的图片从整页背景中拆出来,
 * 作为独立 <img> 元素输出, 供前端编辑(移动/替换)使用。
 *
 * should_split_image 的判定在文字趟(HTMLRenderer)与背景渲染趟
 * (Splash/CairoBackgroundRenderer)中都会执行, 必须确定性一致,
 * 否则图片要么重影要么丢失。
 */

#ifndef IMAGE_SPLIT_H__
#define IMAGE_SPLIT_H__

#include <string>

#include <GfxState.h>
#include <Stream.h>

namespace pdf2htmlEX {

/*
 * 判定是否拆分:
 * - 轴对齐(无旋转/斜切; 水平/垂直镜像允许, 导出时翻转像素兜底)
 * - 显示尺寸两边均 >= min_edge_pt(pt 单位, 与渲染 DPI 无关)
 * - 颜色可导出: 8bpc, 1/3/4 通道
 * - 未被裁剪: 图片设备包围盒 ⊂ 当前裁剪包围盒
 */
bool should_split_image(GfxState * state, int img_width, int img_height,
                        GfxImageColorMap * colorMap, double dpi, double min_edge_pt);

/*
 * 导出图片到文件: DCT(DeviceGray/RGB, 未翻转)直传 JPEG, 其余解码转 PNG。
 * 返回格式后缀("jpg"/"png"), 失败返回空串。
 */
std::string dump_image(Stream * str, int width, int height, GfxImageColorMap * colorMap,
                       bool flip_h, bool flip_v, const std::string & path_prefix);

/* 软掩码(SMask)合成 RGBA PNG; 失败返回 false */
bool dump_soft_masked_image(Stream * str, int width, int height, GfxImageColorMap * colorMap,
                            Stream * mask_str, int mask_width, int mask_height, GfxImageColorMap * mask_color_map,
                            bool flip_h, bool flip_v, const std::string & path_prefix);

} // namespace pdf2htmlEX

#endif // IMAGE_SPLIT_H__
