/* quirc_preprocess -- QR 识别前的图像预处理辅助模块
 *
 * 该模块与 quirc 主库解耦：所有函数都直接操作原始灰度/RGB 像素缓冲，
 * 不依赖 struct quirc 内部状态。可以在调用 quirc_begin() 填充图像之前
 * 按需串联调用。
 *
 * 典型流程：
 *   1. quirc_pp_to_gray()         将 RGB/RGBA 转成灰度（可选，已是灰度则跳过）
 *   2. quirc_pp_median3()         消除椒盐 / 斑点噪声（可选）
 *   3. quirc_pp_gaussian3()       轻微平滑，抑制纹理干扰（可选）
 *   4. quirc_pp_histeq()          直方图均衡，提升低光照对比度（可选）
 *   5. quirc_pp_warp_rect()       根据四个角点做透视矫正（可选）
 *   6. 把结果拷贝到 quirc_begin() 返回的缓冲区，调用 quirc_end()
 */

#ifndef QUIRC_PREPROCESS_H_
#define QUIRC_PREPROCESS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 把 RGB / RGBA 缓冲转换为灰度。
 *
 * src       : 输入像素缓冲
 * dst       : 输出灰度缓冲（w*h 字节），可以与 src 相同（就地处理）
 * w, h      : 图像尺寸
 * channels  : 源像素通道数，支持 1（灰度，直接拷贝）、3（RGB）、4（RGBA）
 *
 * 返回 0 成功，-1 参数非法。
 */
int quirc_pp_to_gray(const uint8_t *src, uint8_t *dst,
		     int w, int h, int channels);

/* 直方图均衡化（全局）。
 * 提升低光照、低对比度场景下二维码与背景的差异。
 * 输入输出均为 w*h 单通道灰度缓冲，支持就地处理。
 */
int quirc_pp_histeq(uint8_t *img, int w, int h);

/* 3x3 高斯滤波（核 [1 2 1; 2 4 2; 1 2 1] / 16）。
 * 平滑纹理干扰，抑制高频噪声，对二维码模块边缘影响较小。
 * scratch 至少 w*h 字节；不允许就地处理。
 */
int quirc_pp_gaussian3(const uint8_t *src, uint8_t *dst,
		       int w, int h, uint8_t *scratch);

/* 3x3 中值滤波。
 * 对椒盐噪声（白点 / 黑点杂斑）非常有效，能保护二维码模块的直线边缘。
 * scratch 至少 w*h 字节；不允许就地处理。
 */
int quirc_pp_median3(const uint8_t *src, uint8_t *dst,
		     int w, int h, uint8_t *scratch);

/* 透视畸变矫正：把由 src_quad 指定的任意四边形区域
 * 拉成 dst_w * dst_h 的矩形灰度图。
 *
 * src       : 输入灰度图（w*h）
 * src_quad  : 4 个角点的 (x,y) 坐标，顺序为左上、右上、右下、左下
 * dst       : 输出缓冲，dst_w * dst_h 字节
 *
 * 返回 0 成功，-1 参数非法。
 *
 * 实现使用逆向映射 + 双线性插值，保证 dst 没有空洞。
 */
struct quirc_pp_point {
	float x;
	float y;
};

int quirc_pp_warp_rect(const uint8_t *src, int w, int h,
		       const struct quirc_pp_point src_quad[4],
		       uint8_t *dst, int dst_w, int dst_h);

#ifdef __cplusplus
}
#endif

#endif /* QUIRC_PREPROCESS_H_ */
