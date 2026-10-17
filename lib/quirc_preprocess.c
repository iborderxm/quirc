/* quirc_preprocess -- QR 识别前的图像预处理辅助模块
 *
 * 仅依赖 C 标准库，方便在 ESP32 等嵌入式平台上直接编译。
 */

#include <stdlib.h>
#include <string.h>

#include "quirc_preprocess.h"

int quirc_pp_to_gray(const uint8_t *src, uint8_t *dst,
		     int w, int h, int channels)
{
	int i;
	int num_pixels;

	if (!src || !dst || w <= 0 || h <= 0)
		return -1;

	num_pixels = w * h;

	switch (channels) {
	case 1:
		if (dst != src)
			memcpy(dst, src, (size_t)num_pixels);
		return 0;
	case 3:
		/* ITU-R BT.601 亮度：Y = 0.299 R + 0.587 G + 0.114 B
		 * 用整数运算近似：(77 R + 150 G + 29 B) >> 8
		 */
		for (i = 0; i < num_pixels; i++) {
			unsigned int r = src[i * 3 + 0];
			unsigned int g = src[i * 3 + 1];
			unsigned int b = src[i * 3 + 2];

			dst[i] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
		}
		return 0;
	case 4:
		for (i = 0; i < num_pixels; i++) {
			unsigned int r = src[i * 4 + 0];
			unsigned int g = src[i * 4 + 1];
			unsigned int b = src[i * 4 + 2];

			dst[i] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
		}
		return 0;
	default:
		return -1;
	}
}

int quirc_pp_histeq(uint8_t *img, int w, int h)
{
	uint32_t hist[256];
	uint32_t cdf[256];
	size_t num_pixels;
	size_t i;
	uint32_t cdf_min = 0;
	int v;

	if (!img || w <= 0 || h <= 0)
		return -1;

	num_pixels = (size_t)w * (size_t)h;

	memset(hist, 0, sizeof(hist));
	for (i = 0; i < num_pixels; i++)
		hist[img[i]]++;

	/* 计算累积分布函数（CDF）以及最小的非零 CDF 值 */
	cdf[0] = hist[0];
	for (v = 1; v < 256; v++)
		cdf[v] = cdf[v - 1] + hist[v];

	for (v = 0; v < 256; v++) {
		if (cdf[v] > 0) {
			cdf_min = cdf[v];
			break;
		}
	}

	/* 图像只有一种灰度，无需均衡 */
	if (cdf_min == num_pixels)
		return 0;

	/* 标准均衡公式：h(v) = round((cdf(v) - cdf_min) * 255 /
	 *                          (num_pixels - cdf_min))
	 */
	{
		uint32_t denom = (uint32_t)num_pixels - cdf_min;
		uint8_t lut[256];

		for (v = 0; v < 256; v++) {
			uint32_t num = (cdf[v] > cdf_min)
				? (cdf[v] - cdf_min) * 255u
				: 0;
			lut[v] = (uint8_t)(num / denom);
		}

		for (i = 0; i < num_pixels; i++)
			img[i] = lut[img[i]];
	}

	return 0;
}

int quirc_pp_gaussian3(const uint8_t *src, uint8_t *dst,
		       int w, int h, uint8_t *scratch)
{
	int x, y;

	if (!src || !dst || !scratch || w <= 0 || h <= 0)
		return -1;

	/* 先按行做水平方向 [1 2 1]/4，再按列做垂直方向 [1 2 1]/4，
	 * 等价于完整的 3x3 [1 2 1; 2 4 2; 1 2 1]/16，但避免 16 位溢出
	 * 并把邻域访存次数从 9 次降到 6 次。
	 */
	for (y = 0; y < h; y++) {
		const uint8_t *row = src + (size_t)y * w;
		uint8_t *out = scratch + (size_t)y * w;

		out[0] = row[0];
		for (x = 1; x < w - 1; x++)
			out[x] = (uint8_t)((row[x - 1] + 2 * row[x] +
					    row[x + 1]) >> 2);
		out[w - 1] = row[w - 1];
	}

	for (y = 0; y < h; y++) {
		uint8_t *out = dst + (size_t)y * w;

		if (y == 0) {
			memcpy(out, scratch, (size_t)w);
			continue;
		}
		if (y == h - 1) {
			memcpy(out, scratch + (size_t)y * w, (size_t)w);
			continue;
		}

		{
			const uint8_t *r0 = scratch + (size_t)(y - 1) * w;
			const uint8_t *r1 = scratch + (size_t)y * w;
			const uint8_t *r2 = scratch + (size_t)(y + 1) * w;

			for (x = 0; x < w; x++)
				out[x] = (uint8_t)((r0[x] + 2 * r1[x] +
						    r2[x]) >> 2);
		}
	}

	return 0;
}

/* 简单的 9 元素插入排序取中值；3x3 窗口下比快速选择更稳定。 */
static uint8_t median9(uint8_t *v)
{
	int i, j;

	for (i = 1; i < 9; i++) {
		uint8_t key = v[i];
		j = i - 1;

		while (j >= 0 && v[j] > key) {
			v[j + 1] = v[j];
			j--;
		}
		v[j + 1] = key;
	}

	return v[4];
}

int quirc_pp_median3(const uint8_t *src, uint8_t *dst,
		     int w, int h, uint8_t *scratch)
{
	int x, y;

	(void)scratch; /* 中值滤波逐像素写 dst，不需要中间缓冲 */

	if (!src || !dst || w <= 0 || h <= 0)
		return -1;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			uint8_t window[9];
			int k = 0;
			int dy, dx;

			/* 边界处用最近像素填充，避免引入黑边 */
			for (dy = -1; dy <= 1; dy++) {
				int yy = y + dy;

				if (yy < 0)
					yy = 0;
				else if (yy >= h)
					yy = h - 1;

				for (dx = -1; dx <= 1; dx++) {
					int xx = x + dx;

					if (xx < 0)
						xx = 0;
					else if (xx >= w)
						xx = w - 1;

					window[k++] = src[(size_t)yy * w + xx];
				}
			}

			dst[(size_t)y * w + x] = median9(window);
		}
	}

	return 0;
}

/* 由 src_quad 与目标矩形求 3x3 单应矩阵（仅 8 个自由度）。
 * 这里采用逆向求解：先求 dst -> src 的映射，
 * 这样在对 dst 每个像素扫描时可以直接得到 src 上的坐标。
 */
static int compute_homography(const struct quirc_pp_point src_quad[4],
			      int dst_w, int dst_h, float H[9])
{
	/* 目标矩形的 4 个角点（顺序与 src_quad 一致） */
	static const float corner_x[4] = {0.f, 1.f, 1.f, 0.f};
	static const float corner_y[4] = {0.f, 0.f, 1.f, 1.f};

	/* 构造 8x8 线性方程组 A * h = b。
	 * 对每对对应点 (X, Y) -> (x, y)：
	 *   x = (h0 X + h1 Y + h2) / (h6 X + h7 Y + 1)
	 *   y = (h3 X + h4 Y + h5) / (h6 X + h7 Y + 1)
	 */
	float A[8][9]; /* 最后一列是 b */
	int i, j, k;

	/* 目标点坐标先归一化到 [0,1]，再乘以实际尺寸 */
	for (i = 0; i < 4; i++) {
		float X = corner_x[i] * (float)(dst_w - 1);
		float Y = corner_y[i] * (float)(dst_h - 1);
		float x = src_quad[i].x;
		float y = src_quad[i].y;

		A[i * 2][0] = X;
		A[i * 2][1] = Y;
		A[i * 2][2] = 1.f;
		A[i * 2][3] = 0.f;
		A[i * 2][4] = 0.f;
		A[i * 2][5] = 0.f;
		A[i * 2][6] = -x * X;
		A[i * 2][7] = -x * Y;
		A[i * 2][8] = x;

		A[i * 2 + 1][0] = 0.f;
		A[i * 2 + 1][1] = 0.f;
		A[i * 2 + 1][2] = 0.f;
		A[i * 2 + 1][3] = X;
		A[i * 2 + 1][4] = Y;
		A[i * 2 + 1][5] = 1.f;
		A[i * 2 + 1][6] = -y * X;
		A[i * 2 + 1][7] = -y * Y;
		A[i * 2 + 1][8] = y;
	}

	/* 高斯消元（部分主元）求解 8 元一次方程组 */
	for (i = 0; i < 8; i++) {
		int pivot = i;
		float max = A[i][i] < 0.f ? -A[i][i] : A[i][i];

		for (j = i + 1; j < 8; j++) {
			float v = A[j][i] < 0.f ? -A[j][i] : A[j][i];

			if (v > max) {
				max = v;
				pivot = j;
			}
		}

		if (max < 1e-6f)
			return -1;

		if (pivot != i) {
			for (k = i; k < 9; k++) {
				float tmp = A[i][k];

				A[i][k] = A[pivot][k];
				A[pivot][k] = tmp;
			}
		}

		for (j = i + 1; j < 8; j++) {
			float f = A[j][i] / A[i][i];

			for (k = i; k < 9; k++)
				A[j][k] -= f * A[i][k];
		}
	}

	for (i = 7; i >= 0; i--) {
		float s = A[i][8];

		for (j = i + 1; j < 8; j++)
			s -= A[i][j] * A[j][8];

		A[i][8] = s / A[i][i];
	}

	for (i = 0; i < 8; i++)
		H[i] = A[i][8];
	H[8] = 1.f;

	return 0;
}

int quirc_pp_warp_rect(const uint8_t *src, int w, int h,
		       const struct quirc_pp_point src_quad[4],
		       uint8_t *dst, int dst_w, int dst_h)
{
	float H[9];
	int x, y;

	if (!src || !dst || !src_quad || w <= 0 || h <= 0 ||
	    dst_w <= 0 || dst_h <= 0)
		return -1;

	if (compute_homography(src_quad, dst_w, dst_h, H) < 0)
		return -1;

	for (y = 0; y < dst_h; y++) {
		for (x = 0; x < dst_w; x++) {
			float X = (float)x;
			float Y = (float)y;
			float den = H[6] * X + H[7] * Y + H[8];
			float sx_f, sy_f;
			int sx, sy;
			int x0, y0, x1, y1;
			float fx, fy;
			int p00, p01, p10, p11;
			int top, bot;

			if (den < 1e-6f && den > -1e-6f) {
				dst[(size_t)y * dst_w + x] = 0;
				continue;
			}

			sx_f = (H[0] * X + H[1] * Y + H[2]) / den;
			sy_f = (H[3] * X + H[4] * Y + H[5]) / den;

			/* 双线性插值 */
			sx = (int)sx_f;
			sy = (int)sy_f;

			if (sx_f < 0.f) sx--;
			if (sy_f < 0.f) sy--;

			x0 = sx;
			y0 = sy;
			x1 = sx + 1;
			y1 = sy + 1;

			/* 越界时取最近像素 */
			if (x0 < 0) x0 = 0;
			if (x1 < 0) x1 = 0;
			if (y0 < 0) y0 = 0;
			if (y1 < 0) y1 = 0;
			if (x0 >= w) x0 = w - 1;
			if (x1 >= w) x1 = w - 1;
			if (y0 >= h) y0 = h - 1;
			if (y1 >= h) y1 = h - 1;

			fx = sx_f - (float)sx;
			fy = sy_f - (float)sy;
			if (fx < 0.f) fx = 0.f;
			if (fx > 1.f) fx = 1.f;
			if (fy < 0.f) fy = 0.f;
			if (fy > 1.f) fy = 1.f;

			p00 = src[(size_t)y0 * w + x0];
			p01 = src[(size_t)y0 * w + x1];
			p10 = src[(size_t)y1 * w + x0];
			p11 = src[(size_t)y1 * w + x1];

			top = (int)(p00 + (p01 - p00) * fx);
			bot = (int)(p10 + (p11 - p10) * fx);

			dst[(size_t)y * dst_w + x] =
				(uint8_t)(top + (int)((bot - top) * fy));
		}
	}

	return 0;
}
