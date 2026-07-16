/*******************************************************************************
 * FILE: user_sdcard.c
 * MÔ TẢ: Khởi tạo và quản lý thẻ SD card qua giao tiếp SPI
 * 
 * CHỨC NĂNG CHÍNH:
 *   - Khởi tạo SPI bus và mount thẻ SD
 *   - Liệt kê các file trên thẻ SD
 *   - Cung cấp filesystem tại đường dẫn /sdcard/

 ******************************************************************************/

#include "user_sdcard.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h> 
#include <sys/types.h>
#include "stdbool.h"

#define EXAMPLE_MAX_CHAR_SIZE    64
static const char *TAG = "example";

/* Cờ kiểm tra SD card đã khởi tạo thành công chưa */
bool isCardInited;

/*******************************************************************************
 * CẤU HÌNH CHÂN SPI CHO SD CARD
 * Nếu phần cứng nối khác → sửa số GPIO ở đây
 ******************************************************************************/
#define PIN_NUM_MISO  15     /* GPIO 5  - Chân MISO (dữ liệu vào) */
#define PIN_NUM_MOSI  5    /* GPIO 18 - Chân MOSI (dữ liệu ra)  */
#define PIN_NUM_CLK   4    /* GPIO 19 - Chân SCK  (xung clock)   */
#define PIN_NUM_CS    18    /* GPIO 21 - Chân CS   (chip select)  */


/*******************************************************************************
 * HÀM: Init_Sdcard
 * MÔ TẢ: Khởi tạo SPI bus, mount thẻ SD card vào /sdcard/
 *   - Tốc độ SPI: 2MHz (an toàn)
 *   - Bật internal pull-up trên các chân dữ liệu
 *   - Liệt kê tất cả file trên thẻ SD sau khi mount
 * 
 * KHI LỖI:
 *   - "Failed to initialize the card" → Kiểm tra nối dây + thẻ SD
 *   - "Failed to mount filesystem"    → Thẻ SD chưa format FAT32
 ******************************************************************************/
void Init_Sdcard(void)
{
    esp_err_t ret;

    /* Cấu hình mount: không tự format, tối đa 5 file mở cùng lúc */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(TAG, "Initializing SD card");

    /* Khởi tạo SPI host với tốc độ 2MHz (rất an toàn) */
    ESP_LOGI(TAG, "Using SPI peripheral");
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 2000; // 2MHz

    /* Bật điện trở pull-up nội cho các chân SPI */
    gpio_pullup_en(PIN_NUM_MOSI);
    gpio_pullup_en(PIN_NUM_MISO);
    gpio_pullup_en(PIN_NUM_CLK);

    /* Cấu hình SPI bus */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16384,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return;
    }

    /* Cấu hình chân CS cho SD card */
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    /* Mount filesystem FAT vào /sdcard/ */
    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    
    if (ret != ESP_OK) 
    {
        if (ret == ESP_FAIL) 
        {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                     "If you want the card to be formatted, set the CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return;
    }
    ESP_LOGI(TAG, "Filesystem mounted");

    /* In thông tin thẻ SD */
    sdmmc_card_print_info(stdout, card);

    /* Liệt kê tất cả file trên thẻ SD */
    struct dirent *dp;
    DIR *dir = opendir("/sdcard");
    while ((dp = readdir(dir)) != NULL) {
        printf("File found: %s\n", dp->d_name);
    }
    closedir(dir);

    /* Đánh dấu SD card đã sẵn sàng */
    isCardInited = true;
}



/*******************************************************************************
 * HÀM: User_SDCard_Task
 * MÔ TẢ: Task FreeRTOS cho SD card
 *   - Gọi Init_Sdcard() một lần
 *   - Giữ task sống (vòng lặp vô tận)
 ******************************************************************************/
void User_SDCard_Task(void)
{
    Init_Sdcard();

    while(1)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}