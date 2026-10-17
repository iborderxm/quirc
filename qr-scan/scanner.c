#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quirc.h"
#include "quirc_preprocess.h"

/* 定义 STB_IMAGE_IMPLEMENTATION 以实现 stb_image.h 的函数 */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/*
 * 识别流水线（按代价从低到高依次尝试，任一阶段成功即返回）：
 *
 *   1) 原图直接识别（库内部已做局部自适应阈值，不再做全图高斯/中值/直方图
 *      均衡——实测这三步对失焦翻拍图反而会把 5px 大小的模块糊在一起）；
 *   2) 双线性放大 2x、3x：提高每模块像素数，让网格采样落在模块中心；
 *   3) 前序阶段只要检出过二维码，就按角点裁剪 ROI（留 1/8 边长余量，
 *      相当于静音区），再放大 2x / 4x 识别，排除大面积暗背景干扰；
 *   4) 每个阶段内部：解码失败先翻转重解，仍失败再按四角点做透视矫正重解。
 */

/* 单阶段尝试结果 */
enum stage_result {
	STAGE_DECODED = 0,	/* 成功解出内容 */
	STAGE_NO_CODE,		/* 未检出任何二维码 */
	STAGE_MULTI_CODE,	/* 检出多个二维码（按需求不处理） */
	STAGE_DECODE_FAIL	/* 检出但解码失败 */
};

struct stage_report {
	const char		*tag;
	enum stage_result	result;
	quirc_decode_error_t	err;
};

/*
 * 根据 quirc 给出的四个角点做透视矫正，把倾斜/畸变的二维码
 * 拉成 dst_size * dst_size 的正方形灰度图。
 * 返回 0 成功，-1 失败。
 */
static int rectify_qr(const uint8_t *img, int w, int h,
                      const struct quirc_point corners[4],
                      uint8_t *dst, int dst_size)
{
	struct quirc_pp_point quad[4];
	int i;

	for (i = 0; i < 4; i++) {
		quad[i].x = (float)corners[i].x;
		quad[i].y = (float)corners[i].y;
	}

	return quirc_pp_warp_rect(img, w, h, quad, dst, dst_size, dst_size);
}

/*
 * 在单幅灰度图上执行一次完整的 quirc 检测 + 解码尝试。
 * 成功时 *out_data 写入解码结果，*out_corners（可选）写入首个码的四角点。
 */
static enum stage_result scan_one(const uint8_t *img, int w, int h,
				  struct quirc_data *out_data,
				  struct quirc_point *out_corners,
				  quirc_decode_error_t *out_err)
{
	struct quirc *qr;
	struct quirc_code code;
	enum stage_result ret = STAGE_NO_CODE;
	int iw = w, ih = h;
	int num_codes;

	qr = quirc_new();
	if (!qr || quirc_resize(qr, w, h) < 0) {
		if (qr)
			quirc_destroy(qr);
		if (out_err)
			*out_err = QUIRC_SUCCESS;
		return STAGE_NO_CODE;
	}

	memcpy(quirc_begin(qr, &iw, &ih), img, (size_t)w * (size_t)h);
	quirc_end(qr);

	num_codes = quirc_count(qr);
	if (num_codes == 0) {
		quirc_destroy(qr);
		if (out_err)
			*out_err = QUIRC_SUCCESS;
		return STAGE_NO_CODE;
	}

	if (num_codes > 1) {
		printf("More than one QR code detected, stopping processing.\n");
		quirc_destroy(qr);
		if (out_err)
			*out_err = QUIRC_SUCCESS;
		return STAGE_MULTI_CODE;
	}

	quirc_extract(qr, 0, &code);
	if (out_corners)
		memcpy(out_corners, code.corners, sizeof(code.corners));

	{
		quirc_decode_error_t err = quirc_decode(&code, out_data);

		if (err == QUIRC_ERROR_DATA_ECC) {
			quirc_flip(&code);
			err = quirc_decode(&code, out_data);
		}

		/*
		 * 仍失败时，对检测到的二维码做透视矫正，把倾斜/畸变
		 * 区域拉成正方形后再次解码，补全几何畸变适配能力。
		 *
		 * 边长按检测到的网格尺寸动态选取（每模块约 20px），
		 * 既保证 V40 不至于过大，也让小版本二维码在矫正后
		 * 有足够的像素供二次识别。
		 */
		if (err != QUIRC_SUCCESS) {
			int rect_size = code.size * 20;

			if (rect_size < 400)
				rect_size = 400;
			if (rect_size > 1000)
				rect_size = 1000;

			uint8_t *rect_img =
				(uint8_t *)malloc((size_t)rect_size * rect_size);

			if (rect_img &&
			    rectify_qr(img, w, h, code.corners,
				       rect_img, rect_size) == 0) {
				/* 增强矫正后图像的全局对比度，抵消屏幕反光/亮度不均 */
				quirc_pp_histeq(rect_img, rect_size, rect_size);

				struct quirc *qr_rect = quirc_new();

				if (qr_rect &&
				    quirc_resize(qr_rect, rect_size,
						 rect_size) == 0) {
					int rw = rect_size, rh = rect_size;
					uint8_t *rbuf =
						quirc_begin(qr_rect, &rw, &rh);

					memcpy(rbuf, rect_img,
					       (size_t)rw * (size_t)rh);
					quirc_end(qr_rect);

					if (quirc_count(qr_rect) > 0) {
						struct quirc_code rcode;
						struct quirc_data rdata;

						quirc_extract(qr_rect, 0,
							      &rcode);
						err = quirc_decode(&rcode,
								   &rdata);
						if (err ==
						    QUIRC_ERROR_DATA_ECC) {
							quirc_flip(&rcode);
							err = quirc_decode(
								&rcode, &rdata);
						}
						if (err == QUIRC_SUCCESS) {
							*out_data = rdata;
							code = rcode;
						}
					}
				}

				if (qr_rect)
					quirc_destroy(qr_rect);
			}

			free(rect_img);
		}

		if (out_err)
			*out_err = err;

		if (err == QUIRC_SUCCESS) {
			ret = STAGE_DECODED;
			if (out_corners)
				memcpy(out_corners, code.corners,
				       sizeof(code.corners));
		} else {
			ret = STAGE_DECODE_FAIL;
		}
	}

	quirc_destroy(qr);
	return ret;
}

/* 双线性放大 scale 倍（scale 为正整数），返回 malloc 出的新灰度图 */
static uint8_t *scale_gray(const uint8_t *src, int w, int h, int scale,
			   int *out_w, int *out_h)
{
	int nw = w * scale, nh = h * scale;
	uint8_t *dst = (uint8_t *)malloc((size_t)nw * (size_t)nh);
	int y, x;

	if (!dst)
		return NULL;

	for (y = 0; y < nh; y++) {
		/* 目标像素中心映射回源图坐标 */
		float fy = ((float)y + 0.5f) / (float)scale - 0.5f;
		int y0 = (int)fy;
		float by;
		int y1;

		if (y0 < 0) {
			y0 = 0;
			by = 0.0f;
		} else {
			by = fy - (float)y0;
		}
		y1 = y0 + 1;
		if (y1 >= h)
			y1 = h - 1;

		for (x = 0; x < nw; x++) {
			float fx = ((float)x + 0.5f) / (float)scale - 0.5f;
			int x0 = (int)fx;
			float bx;
			int x1;
			float v;

			if (x0 < 0) {
				x0 = 0;
				bx = 0.0f;
			} else {
				bx = fx - (float)x0;
			}
			x1 = x0 + 1;
			if (x1 >= w)
				x1 = w - 1;

			v = (float)src[y0 * w + x0] * (1.0f - bx) * (1.0f - by) +
			    (float)src[y0 * w + x1] * bx * (1.0f - by) +
			    (float)src[y1 * w + x0] * (1.0f - bx) * by +
			    (float)src[y1 * w + x1] * bx * by;

			dst[y * nw + x] = (uint8_t)(v + 0.5f);
		}
	}

	*out_w = nw;
	*out_h = nh;
	return dst;
}

/*
 * 按角点外接矩形裁剪 ROI，并向外扩 margin 像素（自动夹到图像边界内）。
 * 返回 malloc 出的新灰度图；cx0、cy0 返回 ROI 在原图中的左上角。
 */
static uint8_t *crop_gray(const uint8_t *src, int w, int h,
			  const struct quirc_point corners[4], int margin,
			  int *out_w, int *out_h, int *cx0, int *cy0)
{
	int min_x = corners[0].x, max_x = corners[0].x;
	int min_y = corners[0].y, max_y = corners[0].y;
	int cw, ch;
	uint8_t *dst;
	int y, x, j;

	for (j = 1; j < 4; j++) {
		if (corners[j].x < min_x)
			min_x = corners[j].x;
		if (corners[j].x > max_x)
			max_x = corners[j].x;
		if (corners[j].y < min_y)
			min_y = corners[j].y;
		if (corners[j].y > max_y)
			max_y = corners[j].y;
	}

	min_x -= margin;
	min_y -= margin;
	max_x += margin;
	max_y += margin;
	if (min_x < 0)
		min_x = 0;
	if (min_y < 0)
		min_y = 0;
	if (max_x >= w)
		max_x = w - 1;
	if (max_y >= h)
		max_y = h - 1;

	cw = max_x - min_x + 1;
	ch = max_y - min_y + 1;
	dst = (uint8_t *)malloc((size_t)cw * (size_t)ch);
	if (!dst)
		return NULL;

	for (y = 0; y < ch; y++)
		for (x = 0; x < cw; x++)
			dst[y * cw + x] = src[(min_y + y) * w + (min_x + x)];

	*out_w = cw;
	*out_h = ch;
	*cx0 = min_x;
	*cy0 = min_y;
	return dst;
}

/* 一次识别尝试的输入阶段 */
struct stage {
	const uint8_t	*img;
	int		w, h;
	const char	*tag;
	int		owned;		/* 1 表示 img 需要 free() */
};

#define MAX_STAGES 8

int main(int argc, char *argv[])
{
	const char *filename;
	int width, height, channels;
	uint8_t *img_gray;
	struct stage stages[MAX_STAGES];
	struct stage_report reports[MAX_STAGES];
	int nstages = 0;
	int i;
	int decoded = 0;
	struct quirc_data data;
	/* 记录原图坐标系下最近一次检出的角点，用于生成 ROI 阶段 */
	struct quirc_point last_corners[4];
	int have_corners = 0;

	if (argc != 2) {
		printf("usage: %s <image file path>\n", argv[0]);
		return -1;
	}
	filename = argv[1];

	/* 加载图像，并转换为灰度图（单通道） */
	img_gray = stbi_load(filename, &width, &height, &channels, 1);
	if (!img_gray) {
		printf("stbi_load failed: %s\n", stbi_failure_reason());
		return -1;
	}

	memset(stages, 0, sizeof(stages));
	memset(reports, 0, sizeof(reports));

	/* 阶段 1：原图 */
	stages[nstages].img = img_gray;
	stages[nstages].w = width;
	stages[nstages].h = height;
	stages[nstages].tag = "raw";
	stages[nstages].owned = 0;
	nstages++;

	/* 依次执行各阶段；检出成功即可据此补充 ROI 高倍放大阶段 */
	for (i = 0; i < nstages && i < MAX_STAGES && !decoded; i++) {
		struct quirc_point corners[4];
		quirc_decode_error_t err = QUIRC_SUCCESS;
		enum stage_result r;

		r = scan_one(stages[i].img, stages[i].w, stages[i].h,
			     &data, corners, &err);
		reports[i].tag = stages[i].tag;
		reports[i].result = r;
		reports[i].err = err;

		if (r == STAGE_DECODED) {
			decoded = 1;
			break;
		}

		if (r == STAGE_MULTI_CODE)
			break;

		/*
		 * 本阶段检出了二维码：把角点换算回原图坐标并记录。
		 * 当前所有阶段都由原图直接缩放/裁剪而来，ROI 阶段只从
		 * 全帧阶段（raw/x2/x3）生成，换算关系简单。
		 */
		if (r == STAGE_DECODE_FAIL) {
			if (strcmp(stages[i].tag, "raw") == 0) {
				memcpy(last_corners, corners,
				       sizeof(last_corners));
				have_corners = 1;
			} else if (strncmp(stages[i].tag, "x", 1) == 0) {
				int scale = stages[i].w / width;
				int j;

				if (scale > 0) {
					for (j = 0; j < 4; j++) {
						last_corners[j].x =
							corners[j].x / scale;
						last_corners[j].y =
							corners[j].y / scale;
					}
					have_corners = 1;
				}
			}
		}

		/*
		 * 动态补充后续阶段：
		 *   raw 之后补 x2、x3 全帧放大；
		 *   全帧阶段跑完（i>=2 或已无全帧阶段可补）且曾检出角点时，
		 *   补 ROI 裁剪 2x / 4x。
		 */
		if (i == 0 && nstages == 1) {
			int s;

			for (s = 2; s <= 3; s++) {
				int sw, sh;
				uint8_t *big = scale_gray(img_gray, width,
							  height, s, &sw, &sh);

				if (big && nstages < MAX_STAGES) {
					stages[nstages].img = big;
					stages[nstages].w = sw;
					stages[nstages].h = sh;
					stages[nstages].owned = 1;
					if (s == 2)
						stages[nstages].tag = "x2";
					else
						stages[nstages].tag = "x3";
					nstages++;
				} else {
					free(big);
				}
			}
		}

		/*
		 * 全帧阶段（raw/x2/x3）跑完仍未成功，且已检出角点时：
		 * 追加 ROI 裁剪 + 多倍放大阶段，放大倍数覆盖 2x/4x/6x/8x。
		 */
		if (have_corners && strcmp(stages[i].tag, "x3") == 0) {
			int margin, rcw, rch, rx0, ry0;
			uint8_t *roi;
			int s;

			/*
			 * 余量取外接矩形较短边的 1/6（约 4~5 个模块），
			 * 保证安静区完整，避免裁切到格式信息或定位图案边缘。
			 */
			{
				int min_x = last_corners[0].x;
				int max_x = last_corners[0].x;
				int min_y = last_corners[0].y;
				int max_y = last_corners[0].y;
				int bw, bh;
				int j;

				for (j = 1; j < 4; j++) {
					if (last_corners[j].x < min_x)
						min_x = last_corners[j].x;
					if (last_corners[j].x > max_x)
						max_x = last_corners[j].x;
					if (last_corners[j].y < min_y)
						min_y = last_corners[j].y;
					if (last_corners[j].y > max_y)
						max_y = last_corners[j].y;
				}
				bw = max_x - min_x + 1;
				bh = max_y - min_y + 1;
				margin = (bw < bh ? bw : bh) / 6;
			}

			roi = crop_gray(img_gray, width, height,
					last_corners, margin,
					&rcw, &rch, &rx0, &ry0);
			(void)rx0;
			(void)ry0;

			if (roi) {
				for (s = 2; s <= 8; s += 2) {
					int sw, sh;
					uint8_t *big = scale_gray(roi, rcw, rch,
								  s, &sw, &sh);
					const char *tag;

					switch (s) {
					case 2: tag = "roi-x2"; break;
					case 4: tag = "roi-x4"; break;
					case 6: tag = "roi-x6"; break;
					case 8: tag = "roi-x8"; break;
					default: tag = "roi"; break;
					}

					if (big && nstages < MAX_STAGES) {
						stages[nstages].img = big;
						stages[nstages].w = sw;
						stages[nstages].h = sh;
						stages[nstages].owned = 1;
						stages[nstages].tag = tag;
						nstages++;
					} else {
						free(big);
					}
				}
				free(roi);
			}
		}
	}

	if (decoded) {
		printf("%.*s\n", data.payload_len, data.payload);
	} else {
		/* 诊断输出：逐阶段给出失败原因，避免静默退出无法排查 */
		printf("DECODE FAILED, attempts:\n");
		for (i = 0; i < nstages; i++) {
			const char *reason;

			switch (reports[i].result) {
			case STAGE_NO_CODE:
				reason = "no code detected";
				break;
			case STAGE_MULTI_CODE:
				reason = "multiple codes";
				break;
			case STAGE_DECODE_FAIL:
				reason = quirc_strerror(reports[i].err);
				break;
			default:
				reason = "not run";
				break;
			}
			printf("  %-7s: %s\n", reports[i].tag, reason);
		}
	}

	for (i = 0; i < nstages; i++)
		if (stages[i].owned)
			free((void *)stages[i].img);
	stbi_image_free(img_gray);

	return decoded ? 0 : -1;
}
