
#include "mbed.h"
#include "EasyAttach_CameraAndLCD.h"
#include "dcache-control.h"
#include "JPEG_Converter.h"
#include "HTTPServer.h"
#include "FATFileSystem.h"
#include "RomRamBlockDevice.h"
#include "SDBlockDevice_GRBoard.h"
#include "EthernetInterface.h"
#include "DhcpServer.h"
#include "picamera_ctl.h"

#include "r_dk2_if.h"
#include "r_drp_simple_isp.h"

/**** User Selection *********/
/** Network setting **/
#if defined(TARGET_SEMB1402)
  #define USE_DHCP             (1)                 /* Select  0(static configuration) or 1(use DHCP) */
  #define NETWORK_TYPE         (2)                 /* Select  0(Ethernet), 1(BP3595), 2(ESP32 STA) ,3(ESP32 AP) */
#else
  #define USE_DHCP             (0)                 /* Select  0(static configuration) or 1(use DHCP) */
  #define NETWORK_TYPE         (0)                 /* Select  0(Ethernet), 1(BP3595), 2(ESP32 STA) ,3(ESP32 AP) */
#endif
#if (USE_DHCP == 0)
  #define IP_ADDRESS           ("192.168.0.1")     /* IP address      */
  #define SUBNET_MASK          ("255.255.255.0")   /* Subnet mask     */
  #define DEFAULT_GATEWAY      ("192.168.0.1")     /* Default gateway */
#endif
#if (NETWORK_TYPE >= 1)
  #define SCAN_NETWORK         (1)                 /* Select  0(Use WLAN_SSID, WLAN_PSK, WLAN_SECURITY) or 1(To select a network using the terminal.) */
  #define WLAN_SSID            ("SSIDofYourAP")    /* SSID */
  #define WLAN_PSK             ("PSKofYourAP")     /* PSK(Pre-Shared Key) */
  #define WLAN_SECURITY        NSAPI_SECURITY_WPA_WPA2 /* NSAPI_SECURITY_NONE, NSAPI_SECURITY_WEP, NSAPI_SECURITY_WPA, NSAPI_SECURITY_WPA2 or NSAPI_SECURITY_WPA_WPA2 */
#endif
/** JPEG out setting **/
#if MBED_CONF_APP_LCD
#define JPEG_SEND              (0)
#else
#define JPEG_SEND              (1)
#endif
#define JPEG_ENCODE_QUALITY    (75)                /* JPEG encode quality (min:1, max:75 (Considering the size of JpegBuffer, about 75 is the upper limit.)) */
/** EXPOSURE setting **/
#define EXPOSURE_MIN           (1000)
#define EXPOSURE_MAX           (30000)
#define EXPOSURE_INIT          (2600)
#define LUMINANCE_MIN          (32)
#define LUMINANCE_MAX          (38)
/*****************************/

#if (NETWORK_TYPE == 0)
  #include "EthernetInterface.h"
  EthernetInterface network;
#elif (NETWORK_TYPE == 1)
  #error "Not supported"
#elif (NETWORK_TYPE == 2)
  #include "ESP32Interface.h"
  ESP32Interface network;
#elif (NETWORK_TYPE == 3)
  #include "ESP32InterfaceAP.h"
  ESP32InterfaceAP network;
#else
  #error NETWORK_TYPE error
#endif /* NETWORK_TYPE */

#if JPEG_SEND
#include "file_table.h"               //Binary data of web pages
#else
#include "file_table_control_only.h"  //Binary data of web pages
#endif

/*! Frame buffer stride: Frame buffer stride should be set to a multiple of 32 or 128
    in accordance with the frame buffer burst transfer mode. */
#if MBED_CONF_APP_CAMERA_TYPE == CAMERA_RASPBERRY_PI_WIDE_ANGLE
  #define VIDEO_PIXEL_HW       (640)
  #define VIDEO_PIXEL_VW       (616)
#elif MBED_CONF_APP_LCD
  #define VIDEO_PIXEL_HW       ((LCD_PIXEL_WIDTH + 63u) & ~63u)
  #define VIDEO_PIXEL_VW       LCD_PIXEL_HEIGHT
#else
  #define VIDEO_PIXEL_HW       (640)    /* VGA */
  #define VIDEO_PIXEL_VW       (480)    /* VGA */
#endif

#define FRAME_BUFFER_STRIDE    (((VIDEO_PIXEL_HW * 1) + 63u) & ~63u)
#define FRAME_BUFFER_STRIDE_2  (((VIDEO_PIXEL_HW * 2) + 31u) & ~31u)
#define FRAME_BUFFER_HEIGHT    (VIDEO_PIXEL_VW)

#define DRP_FLG_TILE_ALL       (R_DK2_TILE_0 | R_DK2_TILE_1 | R_DK2_TILE_2 | R_DK2_TILE_3 | R_DK2_TILE_4 | R_DK2_TILE_5)
#define DRP_FLG_CAMER_IN       (0x00000100)

static DisplayBase Display;

static uint8_t fbuf_bayer[FRAME_BUFFER_STRIDE * FRAME_BUFFER_HEIGHT]__attribute((aligned(128)));
static uint8_t fbuf_yuv[FRAME_BUFFER_STRIDE_2 * FRAME_BUFFER_HEIGHT]__attribute((aligned(32)));

static FATFileSystem fs("storage");
static RomRamBlockDevice romram_bd(512000, 512);
static SDBlockDevice_GRBoard sd;
static Mutex param_lock;
static Timer frame_timer;

#if JPEG_SEND
static uint8_t JpegBuffer[2][1024 * 128]__attribute((aligned(32)));
static size_t jcu_encode_size[2];
static int image_change = 0;
static int jcu_buf_index_write = 0;
static int jcu_buf_index_write_done = 0;
static int jcu_buf_index_read = 0;
static int jcu_encoding = 0;
#endif

static r_drp_simple_isp_t param_isp __attribute((section("NC_BSS")));
static uint32_t accumulate_tbl[9] __attribute((section("NC_BSS")));
static uint8_t lut[256] __attribute((section("NC_BSS")));
static r_drp_simple_isp_t param_isp_req;
static uint8_t work_lut[256];
static double set_gamma;
static bool ganma_change = false;
static uint8_t drp_lib_id[R_DK2_TILE_NUM] = {0};
static Thread drpTask(osPriorityHigh);
static Thread sdConnectTask;
static uint16_t luminance;
static uint16_t color_comp[3];
static uint32_t frame_interval;
static uint8_t auto_exposure;
#if defined(TARGET_GR_MANGO)
static picamera_ctl picam(I2C_SDA, I2C_SCL);
#else
static picamera_ctl picam(PD_5, PD_4);
#endif
static uint16_t set_exposure = EXPOSURE_INIT;
static bool reset_exposure = false;

static void set_param_16bit(const char* path, char* str, uint16_t* pram, uint16_t max) {
    if (*path != '\0') {
        int32_t val = strtol(path+1, NULL, 16);
        if ((uint16_t)val > max) {
            val = max;
        }
        *pram = (uint16_t)val;
    }
    sprintf(str, "%04x", *pram);
}

static void set_param_bias(const char* path, char* str, int8_t* pram) {
    if (*path != '\0') {
        int32_t val = strtol(path+1, NULL, 16);
        *pram = (int8_t)val;
    }
    sprintf(str, "%02x", *pram & 0x00FF);
}

static void set_param_8bit(const char* path, char* str, uint8_t* pram) {
    if (*path != '\0') {
        int32_t val = strtol(path+1, NULL, 16);
        *pram = (uint32_t)val;
    }
    sprintf(str, "%02x", *pram & 0x00FF);
}

static bool check_cmd(const char** p_path, const char* cmd) {
    size_t len = strlen(cmd);

    if (strncmp(*p_path, cmd, len) != 0) {
        return false;
    }

    *p_path = (char *)((uint32_t)(*p_path) + (uint32_t)len);
    return true;
}

static void simpile_isp(const char* path, char * ret_str) {
    ret_str[0] = '\0';
    if (*path == '\0') {
        return;
    }
    path += 1;

    param_lock.lock();
    if (check_cmd(&path, "gain_r") != false) {
        set_param_16bit(path, ret_str, &param_isp_req.gain_r, 0xFFFF);
    } else if (check_cmd(&path, "gain_g") != false) {
        set_param_16bit(path, ret_str, &param_isp_req.gain_g, 0xFFFF);
    } else if (check_cmd(&path, "gain_b") != false) {
        set_param_16bit(path, ret_str, &param_isp_req.gain_b, 0xFFFF);
    } else if (check_cmd(&path, "bias_r") != false) {
        set_param_bias(path, ret_str, &param_isp_req.bias_r);
    } else if (check_cmd(&path, "bias_g") != false) {
        set_param_bias(path, ret_str, &param_isp_req.bias_g);
    } else if (check_cmd(&path, "bias_b") != false) {
        set_param_bias(path, ret_str, &param_isp_req.bias_b);
    } else if (check_cmd(&path, "blend") != false) {
        set_param_16bit(path, ret_str, &param_isp_req.blend, 0x0100);
    } else if (check_cmd(&path, "strength") != false) {
        set_param_8bit(path, ret_str, &param_isp_req.strength);
    } else if (check_cmd(&path, "coring") != false) {
        set_param_8bit(path, ret_str, &param_isp_req.coring);
    } else if (check_cmd(&path, "gamma") != false) {
        if (*path != '\0') {
            double wk_gamma = atof(path+1);
            if (wk_gamma > 0.0) {
                double gm = 1.0 / wk_gamma;
                for (int i = 0; i < 256; i++) {
                    work_lut[i] = pow(1.0*i/255, gm) * 255;
                }
                set_gamma = wk_gamma;
                ganma_change = true;
            }
        }
        sprintf(ret_str, "%f", set_gamma);
    } else if (check_cmd(&path, "image_info") != false) {
        sprintf(ret_str, "Luminance:%3hu R:%3hu G:%3hu B:%3hu fps:%2d",
                luminance, color_comp[0], color_comp[1], color_comp[2], (int)(1000 / frame_interval));
    } else {
        // do nothing
    }
    param_lock.unlock();
}

static int snapshot_req(const char* rootPath, const char* path, const char ** pp_data) {
    static char ret_str[64];

#if JPEG_SEND
    if (strcmp(rootPath, "/camera") == 0) {
        int encode_size;

        while ((jcu_encoding == 1) || (image_change == 0)) {
            ThisThread::sleep_for(1);
        }
        jcu_buf_index_read = jcu_buf_index_write_done;
        image_change = 0;

        *pp_data = (const char *)JpegBuffer[jcu_buf_index_read];
        encode_size = (int)jcu_encode_size[jcu_buf_index_read];

        return encode_size;
    } else 
#endif
    if (strcmp(rootPath, "/simple_isp") == 0) {
        simpile_isp(path, ret_str);
        *pp_data = (const char *)ret_str;
        return strlen(ret_str);
    } else if (strcmp(rootPath, "/auto_exposure") == 0) {
        if (*path != '\0') {
            if (strcmp(path+1, "on") == 0) {
                auto_exposure = 1;
            } else if (strcmp(path+1, "off") == 0) {
                auto_exposure = 0;
            } else  {
                // do nothing
            }
        }
        if (auto_exposure != 0) {
            sprintf(ret_str, "on");
        } else {
            sprintf(ret_str, "off");
        }
        *pp_data = (const char *)ret_str;
        return strlen(ret_str);
    } else if (strcmp(rootPath, "/reset_exposure") == 0) {
        reset_exposure = true;
        sprintf(ret_str, "ok");
        *pp_data = (const char *)ret_str;
        return strlen(ret_str);
    } else {
        return 0;
    }
}

static void IntCallbackFunc_Vfield(DisplayBase::int_type_t int_type) {
    frame_interval = frame_timer.read_ms();
    frame_timer.reset();
    drpTask.flags_set(DRP_FLG_CAMER_IN);
}

static void cb_drp_finish(uint8_t id) {
    uint32_t tile_no;
    uint32_t set_flgs = 0;

    // Change the operation state of the DRP library notified by the argument to finish
    for (tile_no = 0; tile_no < R_DK2_TILE_NUM; tile_no++) {
        if (drp_lib_id[tile_no] == id) {
            set_flgs |= (1 << tile_no);
        }
    }
    drpTask.flags_set(set_flgs);
}

#if JPEG_SEND
static void JcuEncodeCallBackFunc(JPEG_Converter::jpeg_conv_error_t err_code) {
    if (err_code == JPEG_Converter::JPEG_CONV_OK) {
        jcu_buf_index_write_done = jcu_buf_index_write;
        image_change = 1;
    }
    jcu_encoding = 0;
}
#endif

static void Start_Video_Camera(void) {
    // Video capture setting (progressive form fixed)
    Display.Video_Write_Setting(
        DisplayBase::VIDEO_INPUT_CHANNEL_0,
        DisplayBase::COL_SYS_NTSC_358,
        (void *)fbuf_bayer,
        FRAME_BUFFER_STRIDE,
        DisplayBase::VIDEO_FORMAT_RAW8,
        DisplayBase::WR_RD_WRSWA_NON,
        VIDEO_PIXEL_VW,
        VIDEO_PIXEL_HW
    );
    EasyAttach_CameraStart(Display, DisplayBase::VIDEO_INPUT_CHANNEL_0);
}

#if MBED_CONF_APP_LCD
static void Start_LCD_Display(void) {
    DisplayBase::rect_t rect;

    rect.vs = 0;
    rect.vw = VIDEO_PIXEL_VW;
    rect.hs = 0;
    rect.hw = VIDEO_PIXEL_HW;
    Display.Graphics_Read_Setting(
        DisplayBase::GRAPHICS_LAYER_0,
        (void *)fbuf_yuv,
        FRAME_BUFFER_STRIDE_2,
        DisplayBase::GRAPHICS_FORMAT_YCBCR422,
        DisplayBase::WR_RD_WRSWA_32_16_8BIT,
        &rect
    );
    Display.Graphics_Start(DisplayBase::GRAPHICS_LAYER_0);

    ThisThread::sleep_for(50);
    EasyAttach_LcdBacklight(true);
}
#endif

static void drp_task(void) {
    bool coarse_send_req = false;

    EasyAttach_Init(Display);
    // Interrupt callback function setting (Field end signal for recording function in scaler 0)
    Display.Graphics_Irq_Handler_Set(DisplayBase::INT_TYPE_S0_VFIELD, 0, IntCallbackFunc_Vfield);
    Start_Video_Camera();
#if MBED_CONF_APP_LCD
    Start_LCD_Display();
#endif
    frame_timer.start();

    R_DK2_Initialize();

    /* Load DRP Library                 */
    /*        +-----------------------+ */
    /* tile 0 |                       | */
    /*        +                       + */
    /* tile 1 |                       | */
    /*        +                       + */
    /* tile 2 |                       | */
    /*        + SimpleIsp bayer2yuv_6 + */
    /* tile 3 |                       | */
    /*        +                       + */
    /* tile 4 |                       | */
    /*        +                       + */
    /* tile 5 |                       | */
    /*        +-----------------------+ */
    R_DK2_Load(g_drp_lib_simple_isp_bayer2yuv_6,
               R_DK2_TILE_0,
               R_DK2_TILE_PATTERN_6, NULL, &cb_drp_finish, drp_lib_id);
    R_DK2_Activate(0, 0);

    memset(&param_isp_req, 0, sizeof(param_isp_req));
    param_isp_req.src    = (uint32_t)fbuf_bayer;
    param_isp_req.dst    = (uint32_t)fbuf_yuv;
    param_isp_req.width  = VIDEO_PIXEL_HW;
    param_isp_req.height = VIDEO_PIXEL_VW;
    param_isp_req.component  = 1;
    param_isp_req.accumulate = (uint32_t)accumulate_tbl;
    param_isp_req.area1_offset_x = 0;
    param_isp_req.area1_offset_y = 0;
    param_isp_req.area1_width    = VIDEO_PIXEL_HW;
    param_isp_req.area1_height   = VIDEO_PIXEL_VW;
    param_isp_req.gain_r = 0x1800;
    param_isp_req.gain_g = 0x1000;
    param_isp_req.gain_b = 0x1C00;

    param_isp_req.bias_r = -16;
    param_isp_req.bias_g = -16;
    param_isp_req.bias_b = -16;

    set_gamma = 1.0;
    for (int i = 0; i < 256; i++) {
        lut[i] = i;
    }
    param_isp_req.table = (uint32_t)lut;
    param_isp_req.gamma = 1;
    auto_exposure = 0;

#if JPEG_SEND
    JPEG_Converter  Jcu;
    JPEG_Converter::bitmap_buff_info_t bitmap_buff_info;
    JPEG_Converter::encode_options_t   encode_options;

    // Jpeg setting
    Jcu.SetQuality(JPEG_ENCODE_QUALITY);
    bitmap_buff_info.width              = VIDEO_PIXEL_HW;
    bitmap_buff_info.height             = VIDEO_PIXEL_VW;
    bitmap_buff_info.format             = JPEG_Converter::WR_RD_YCbCr422;
    bitmap_buff_info.buffer_address     = (void *)fbuf_yuv;
    encode_options.encode_buff_size     = sizeof(JpegBuffer[0]);
    encode_options.p_EncodeCallBackFunc = &JcuEncodeCallBackFunc;
    encode_options.input_swapsetting    = JPEG_Converter::WR_RD_WRSWA_32_16BIT;
#endif

    while (true) {
        ThisThread::flags_wait_all(DRP_FLG_CAMER_IN);

        // Set parameters of SimpleIsp
        param_lock.lock();
        param_isp = param_isp_req;
        if (ganma_change != false) {
            memcpy(lut, work_lut, sizeof(lut));
            ganma_change = false;
        }
        param_lock.unlock();

        // Start DRP and wait for completion
        R_DK2_Start(drp_lib_id[0], (void *)&param_isp, sizeof(r_drp_simple_isp_t));
        ThisThread::flags_wait_all(DRP_FLG_TILE_ALL);

        // Luminance and color component
        param_lock.lock();
        luminance = (uint16_t)((0.299 * accumulate_tbl[0] + 0.587 * accumulate_tbl[1] + 0.114 * accumulate_tbl[2])
                    / (VIDEO_PIXEL_HW * VIDEO_PIXEL_VW));
        color_comp[0] = (uint16_t)(accumulate_tbl[0] / (VIDEO_PIXEL_HW * VIDEO_PIXEL_VW / 4));
        color_comp[1] = (uint16_t)(accumulate_tbl[1] / (VIDEO_PIXEL_HW * VIDEO_PIXEL_VW / 2));
        color_comp[2] = (uint16_t)(accumulate_tbl[2] / (VIDEO_PIXEL_HW * VIDEO_PIXEL_VW / 4));
        param_lock.unlock();

        // Exposure control
        if (auto_exposure != 0) {
            uint32_t wk_exposure;

            if (coarse_send_req != false) {
                // Wait for settings to be reflected.
                coarse_send_req = false;
            } else if ((luminance < LUMINANCE_MIN) && (set_exposure < EXPOSURE_MAX)) {
                wk_exposure = (uint32_t)((float)set_exposure * ((float)LUMINANCE_MIN / (float)luminance));
                if (wk_exposure > EXPOSURE_MAX) {
                    wk_exposure = EXPOSURE_MAX;
                }
                set_exposure = wk_exposure;
                coarse_send_req = true;
            } else if ((luminance > LUMINANCE_MAX) && (set_exposure > EXPOSURE_MIN)) {
                wk_exposure = (uint32_t)((float)set_exposure * ((float)LUMINANCE_MAX / (float)luminance));
                if (wk_exposure < EXPOSURE_MIN) {
                    wk_exposure = EXPOSURE_MIN;
                }
                set_exposure = wk_exposure;
                coarse_send_req = true;
            } else {
                // do nothing
            }
            if (coarse_send_req) {
                picam.SetExposureSpeed(set_exposure);
            }
        }
        if (reset_exposure) {
            reset_exposure = false;
            set_exposure = EXPOSURE_INIT;
            picam.SetExposureSpeed(set_exposure);
        }

#if JPEG_SEND
        // Jpeg convert
        jcu_encoding = 1;
        if (jcu_buf_index_read == jcu_buf_index_write) {
            jcu_buf_index_write ^= 1;  // toggle
        }
        jcu_encode_size[jcu_buf_index_write] = 0;
        dcache_invalid(JpegBuffer[jcu_buf_index_write], sizeof(JpegBuffer[0]));
        if (Jcu.encode(&bitmap_buff_info, JpegBuffer[jcu_buf_index_write],
            &jcu_encode_size[jcu_buf_index_write], &encode_options) != JPEG_Converter::JPEG_CONV_OK) {
            jcu_encode_size[jcu_buf_index_write] = 0;
            jcu_encoding = 0;
        }
#endif
    }
}

static void mount_romramfs(void) {
    FILE * fp;

    fs.format(&romram_bd, 512);
    fs.mount(&romram_bd);

    //index.htm
    fp = fopen("/storage/index.htm", "w");
    fwrite(index_htm_tbl, sizeof(char), sizeof(index_htm_tbl), fp);
    fclose(fp);

    //camera.js
    fp = fopen("/storage/camera.js", "w");
    fwrite(camaera_js_tbl, sizeof(char), sizeof(camaera_js_tbl), fp);
    fclose(fp);

    //favicon.ico
    fp = fopen("/storage/favicon.ico", "w");
    fwrite(favicon_ico_tbl, sizeof(char), sizeof(favicon_ico_tbl), fp);
    fclose(fp);
}

#if (SCAN_NETWORK == 1) && (NETWORK_TYPE != 3)
static const char *sec2str(nsapi_security_t sec) {
    switch (sec) {
        case NSAPI_SECURITY_NONE:
            return "None";
        case NSAPI_SECURITY_WEP:
            return "WEP";
        case NSAPI_SECURITY_WPA:
            return "WPA";
        case NSAPI_SECURITY_WPA2:
            return "WPA2";
        case NSAPI_SECURITY_WPA_WPA2:
            return "WPA/WPA2";
        case NSAPI_SECURITY_UNKNOWN:
        default:
            return "Unknown";
    }
}

static bool scan_network(WiFiInterface *wifi) {
    WiFiAccessPoint *ap;
    bool ret = false;
    int i;
    int count = 10;    /* Limit number of network arbitrary to 10 */

    printf("Scan:\r\n");
    ap = new WiFiAccessPoint[count];
    if (ap == NULL) {
        printf("memory error\r\n");
        return 0;
    }
    count = wifi->scan(ap, count);
    for (i = 0; i < count; i++) {
        printf("No.%d Network: %s secured: %s BSSID: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx RSSI: %hhd Ch: %hhd\r\n",
               i, ap[i].get_ssid(), sec2str(ap[i].get_security()),
               ap[i].get_bssid()[0], ap[i].get_bssid()[1], ap[i].get_bssid()[2], ap[i].get_bssid()[3],
               ap[i].get_bssid()[4], ap[i].get_bssid()[5], ap[i].get_rssi(), ap[i].get_channel());
    }
    printf("%d networks available.\r\n", count);

    if (count > 0) {
        char c;
        char pass[64];
        int select_no;
        bool loop_break = false;;

        printf("\nPlease enter the number of the network you want to connect.\r\n");
        printf("Enter key:[0]-[%d], (If inputting the other key, it's scanned again.)\r\n", count - 1);

        c = (uint8_t)getchar();
        select_no = c - 0x30;
        if ((select_no >= 0) && (select_no < count)) {
            printf("[%s] is selected.\r\n", ap[select_no].get_ssid());
            printf("Please enter the PSK.\r\n");
            i = 0;
            while (loop_break == false) {
                c = (uint8_t)getchar();
                switch (c) {
                    case 0x0A:
                    #if MBED_CONF_PLATFORM_STDIO_CONVERT_NEWLINES
                        // fall through
                    #else
                        break;
                    #endif
                    case 0x0D:
                        pass[i] = '\0';
                        putchar('\r');
                        putchar('\n');
                        loop_break = true;
                        break;
                    case 0x08:
                        if (i > 0) {
                            putchar('\b');
                            putchar(' ');
                            putchar('\b');
                            i--;
                        }
                        break;
                    default:
                        if ((i + 1) < sizeof(pass)) {
                            pass[i] = c;
                            i++;
                            putchar(c);
                        }
                        break;
                }
            }
            wifi->set_credentials(ap[select_no].get_ssid(), pass, ap[select_no].get_security());
            ret = true;
        }
    }

    delete[] ap;

    return ret;
}
#endif

static void sd_connect_task(void) {
    int storage_type = 0;

    while (1) {
        if (storage_type == 0) {
            if (sd.connect()) {
                fs.unmount();
                fs.mount(&sd);
                storage_type = 1;
                printf("SDBlockDevice\r\n");
            }
        } else {
            if (sd.connected() == false) {
                fs.unmount();
                fs.mount(&romram_bd);
                storage_type = 0;
                printf("RomRamBlockDevice\r\n");
            }
        }
        ThisThread::sleep_for(250);
    }
}

int main(void) {
    printf("********* PROGRAM START ***********\r\n");

    mount_romramfs();   //RomRamFileSystem Mount

    sdConnectTask.start(&sd_connect_task);

    // Start DRP task
    drpTask.start(callback(drp_task));

    printf("Network Setting up...\r\n");
#if (USE_DHCP == 0)
    network.set_dhcp(false);
    if (network.set_network(IP_ADDRESS, SUBNET_MASK, DEFAULT_GATEWAY) != 0) { //for Static IP Address (IPAddress, NetMasks, Gateway)
        printf("Network Set Network Error \r\n");
    }
#endif

#if (NETWORK_TYPE >= 1)
#if (SCAN_NETWORK == 1) && (NETWORK_TYPE != 3)
    while (!scan_network(&network));
#else
    network.set_credentials(WLAN_SSID, WLAN_PSK, WLAN_SECURITY);
#endif
#endif
    printf("\r\nConnecting...\r\n");
    if (network.connect() != 0) {
        printf("Network Connect Error \r\n");
        return -1;
    }
    printf("MAC Address is %s\r\n", network.get_mac_address());
    printf("IP Address is %s\r\n", network.get_ip_address());
    printf("NetMask is %s\r\n", network.get_netmask());
    printf("Gateway Address is %s\r\n", network.get_gateway());
    printf("Network Setup OK\r\n");

#if (USE_DHCP == 0) && (NETWORK_TYPE == 0)
    DhcpServer dhcp_server(&network, "RZ/A2M");
#endif

    SnapshotHandler::attach_req(&snapshot_req);
    HTTPServerAddHandler<SnapshotHandler>("/camera"); //Camera
    HTTPServerAddHandler<SnapshotHandler>("/simple_isp");
    HTTPServerAddHandler<SnapshotHandler>("/auto_exposure");
    HTTPServerAddHandler<SnapshotHandler>("/reset_exposure");
    FSHandler::mount("/storage", "/");
    HTTPServerAddHandler<FSHandler>("/");
    HTTPServerStart(&network, 80);
}

