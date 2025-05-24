#include "quirc.h"

// 定义 STB_IMAGE_IMPLEMENTATION 以实现 stb_image.h 的函数
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
// #define STB_IMAGE_WRITE_IMPLEMENTATION
// #include "stb_image_write.h"

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("usage: %s <image file path>\n", argv[0]);
        return -1;
    }

    const char *filename = argv[1];

    // 加载图像，并转换为灰度图（单通道）
    int width, height, channels;
    unsigned char *img_gray = stbi_load(filename, &width, &height, &channels, 1);
    if (!img_gray)
    {
        printf("stbi_load failed: %s\n", stbi_failure_reason());
        return -1;
    }

    // 初始化 quirc
    struct quirc *qr = quirc_new();
    if (!qr)
    {
        printf("Failed to allocate memory\n");
        stbi_image_free(img_gray);
        return -1;
    }

    // 调整 quirc 缓冲区大小以匹配图像
    if (quirc_resize(qr, width, height) < 0)
    {
        printf("Failed to allocate image memory\n");
        quirc_destroy(qr);
        stbi_image_free(img_gray);
        return -1;
    }

    // 设置图像数据
    uint8_t *buffer = quirc_begin(qr, &width, &height);
    memcpy(buffer, img_gray, width * height);
    quirc_end(qr);

    // 检测 QR 码数量
    int num_codes = quirc_count(qr);

    // 如果 QR 码数量大于 1，清理资源并退出
    if (num_codes > 1) {
        printf("More than one QR code detected, stopping processing.\n");
        quirc_destroy(qr);
        stbi_image_free(img_gray);
        return -1;
    } else {
        printf("%d QR codes detected\n", num_codes);
    }

    for (int i = 0; i < num_codes; i++)
    {
        struct quirc_code code;
        struct quirc_data data;

        // 提取单个 QR 码
        quirc_extract(qr, i, &code);

        // 获取二维码四个角点的坐标
        struct quirc_point corners[4];
        // 直接从 code 结构体中获取角点坐标
        for (int j = 0; j < 4; j++) {
            corners[j] = code.corners[j];
        }

        // 计算截取区域的边界
        int min_x = corners[0].x, max_x = corners[0].x;
        int min_y = corners[0].y, max_y = corners[0].y;
        for (int j = 1; j < 4; j++) {
            if (corners[j].x < min_x) min_x = corners[j].x;
            if (corners[j].x > max_x) max_x = corners[j].x;
            if (corners[j].y < min_y) min_y = corners[j].y;
            if (corners[j].y > max_y) max_y = corners[j].y;
        }
    
        // 增加 20 像素的截取范围，并确保不超出图像边界
        min_x = (min_x - 20) > 0 ? (min_x - 20) : 0;
        min_y = (min_y - 20) > 0 ? (min_y - 20) : 0;
        max_x = (max_x + 20) < width ? (max_x + 20) : width - 1;
        max_y = (max_y + 20) < height ? (max_y + 20) : height - 1;

        int crop_width = max_x - min_x + 1;
        int crop_height = max_y - min_y + 1;

        // 计算二维码区域面积和图片总面积
        size_t qr_area = (size_t)crop_width * crop_height;
        size_t img_area = (size_t)width * height;
        // 计算比例
        double ratio = (double)qr_area / img_area;

        unsigned char *cropped_img;
        struct quirc *qr_cropped = NULL; // 用于处理截取图片的 quirc 实例
        if (ratio >= 0.5) {
            printf("QR code area exceeds 50%% of the image area, no cropping needed.\n");
            cropped_img = img_gray; // 直接使用原始图像
            crop_width = width;
            crop_height = height;
        } else {
            // 截取图片
            cropped_img = (unsigned char *)malloc(crop_width * crop_height);
            if (!cropped_img) {
                printf("Failed to allocate memory for cropped image\n");
                continue;
            }
        
            for (int y = 0; y < crop_height; y++) {
                for (int x = 0; x < crop_width; x++) {
                    cropped_img[y * crop_width + x] = img_gray[(min_y + y) * width + (min_x + x)];
                }
            }
        
            // 清理原来的 quirc 资源
            quirc_destroy(qr);
            qr = NULL;
        
            // 初始化新的 quirc 实例处理截取后的图片
            qr_cropped = quirc_new();
            if (!qr_cropped) {
                printf("Failed to allocate memory for cropped QR processing\n");
                free(cropped_img);
                continue;
            }
        
            // 调整 quirc 缓冲区大小以匹配截取后的图像
            if (quirc_resize(qr_cropped, crop_width, crop_height) < 0) {
                printf("Failed to allocate image memory for cropped QR\n");
                quirc_destroy(qr_cropped);
                free(cropped_img);
                continue;
            }
        
            // 设置截取后图像数据
            uint8_t *buffer = quirc_begin(qr_cropped, &crop_width, &crop_height);
            memcpy(buffer, cropped_img, crop_width * crop_height);
            quirc_end(qr_cropped);
        
            // 检测截取后图像中的 QR 码数量
            int num_codes_cropped = quirc_count(qr_cropped);
            if (num_codes_cropped == 0) {
                printf("No QR code detected in cropped image\n");
                quirc_destroy(qr_cropped);
                free(cropped_img);
                continue;
            }
        
            // 提取截取后图像中的 QR 码
            quirc_extract(qr_cropped, 0, &code);
        }
    
        // 保存截取的图片
        // char output_filename[256];
        // snprintf(output_filename, sizeof(output_filename), "qr_cropped_%d.png", i);
        // stbi_write_png(output_filename, crop_width, crop_height, 1, cropped_img, crop_width);
        // printf("Cropped QR code saved to %s\n", output_filename);
    
        if (ratio < 0.5) {
            free(cropped_img); // 释放截取图片的内存
        }
    
        // 解码 QR 码
        quirc_decode_error_t err = quirc_decode(&code, &data);
        if (err == QUIRC_ERROR_DATA_ECC) {
            quirc_flip(&code);
            err = quirc_decode(&code, &data);
        }
        if (err == QUIRC_SUCCESS)
        {
            printf("QR CODE %d data:%.*s\n", i, data.payload_len, data.payload);
        }
        else
        {
            printf("DECODE FAILED: %s\n", quirc_strerror(err));
        }
    
        if (qr_cropped) {
            quirc_destroy(qr_cropped); // 释放截取图片的 quirc 实例
        }
    }

    // 避免重复释放已销毁的 qr 实例
    if (qr) {
        quirc_destroy(qr);
    }
    stbi_image_free(img_gray);

    return 0;
}