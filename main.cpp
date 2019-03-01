
#include "mbed.h"
#include "EasyAttach_CameraAndLCD.h"
#include "dcache-control.h"
#include "JPEG_Converter.h"
#include "HTTPServer.h"
#include "FATFileSystem.h"
#include "RomRamBlockDevice.h"
#include "SDBlockDevice_GRBoard.h"
#include "EthernetInterface.h"
#include "file_table.h"         //Binary data of web pages
#include "DhcpServer.h"
#include "Pi_ExposureControl.h"

#include "r_dk2_if.h"
#include "r_drp_simple_isp.h"

/**** User Selection *********/
/** Network setting **/
#define USE_DHCP               (0)                 /* Select  0(static configuration) or 1(use DHCP) */
#if (USE_DHCP == 0)
  #define IP_ADDRESS           ("192.168.0.1")     /* IP address      */
  #define SUBNET_MASK          ("255.255.255.0")   /* Subnet mask     */
  #define DEFAULT_GATEWAY      ("192.168.0.1")     /* Default gateway */
#endif
/** JPEG out setting **/
#define JPEG_ENCODE_QUALITY    (75)                /* JPEG encode quality (min:1, max:75 (Considering the size of JpegBuffer, about 75 is the upper limit.)) */
/*****************************/

/*! Frame buffer stride: Frame buffer stride should be set to a multiple of 32 or 128
    in accordance with the frame buffer burst transfer mode. */
#define VIDEO_PIXEL_HW         (640)
#define VIDEO_PIXEL_VW         (480)

#define FRAME_BUFFER_STRIDE    (((VIDEO_PIXEL_HW * 1) + 31u) & ~31u)
#define FRAME_BUFFER_STRIDE_2  (((VIDEO_PIXEL_HW * 2) + 31u) & ~31u)
#define FRAME_BUFFER_HEIGHT    (VIDEO_PIXEL_VW)

#define DRP_FLG_TILE_ALL       (R_DK2_TILE_0 | R_DK2_TILE_1 | R_DK2_TILE_2 | R_DK2_TILE_3 | R_DK2_TILE_4 | R_DK2_TILE_5)
#define DRP_FLG_CAMER_IN       (0x00000100)

static DisplayBase Display;
static EthernetInterface network;

static uint8_t fbuf_bayer[FRAME_BUFFER_STRIDE * FRAME_BUFFER_HEIGHT]__attribute((aligned(128)));
static uint8_t fbuf_yuv[FRAME_BUFFER_STRIDE_2 * FRAME_BUFFER_HEIGHT]__attribute((aligned(32)));

static FATFileSystem fs("storage");
static RomRamBlockDevice romram_bd(512000, 512);
static SDBlockDevice_GRBoard sd;
static Mutex param_lock;
static Timer frame_timer;

static uint8_t JpegBuffer[2][1024 * 128]__attribute((aligned(32)));
static size_t jcu_encode_size[2];
static int image_change = 0;
static JPEG_Converter Jcu;
static int jcu_buf_index_write = 0;
static int jcu_buf_index_write_done = 0;
static int jcu_buf_index_read = 0;
static int jcu_encoding = 0;

#if defined(__ICCARM__)
static r_drp_simple_isp_t param_isp @ ".mirrorram";
static uint32_t accumulate_tbl[9] @ ".mirrorram";
static uint8_t lut[256] @ ".mirrorram";
#else
static r_drp_simple_isp_t param_isp __attribute((section("NC_BSS")));
static uint32_t accumulate_tbl[9] __attribute((section("NC_BSS")));
static uint8_t lut[256] __attribute((section("NC_BSS")));
#endif
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

// Temporary bug fix for Simple ISP library.
static void set_param_bias(const char* path, char* str, int8_t* pram) {
    int8_t wk_data;

    if (*path != '\0') {
        int32_t val = strtol(path+1, NULL, 16);
        wk_data = (int8_t)val;
        if (wk_data < 0) {
            wk_data = (-129 - wk_data);
        }
        *pram = wk_data;
    }
    wk_data = *pram;
    if (wk_data < 0) {
        wk_data = (-129 - wk_data);
    }
    sprintf(str, "%02x", wk_data & 0x00FF);
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
    } else if (strcmp(rootPath, "/simple_isp") == 0) {
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

static void JcuEncodeCallBackFunc(JPEG_Converter::jpeg_conv_error_t err_code) {
    if (err_code == JPEG_Converter::JPEG_CONV_OK) {
        jcu_buf_index_write_done = jcu_buf_index_write;
        image_change = 1;
    }
    jcu_encoding = 0;
}

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

static void drp_task(void) {
    JPEG_Converter  Jcu;
    JPEG_Converter::bitmap_buff_info_t bitmap_buff_info;
    JPEG_Converter::encode_options_t   encode_options;

    EasyAttach_Init(Display);
    // Interrupt callback function setting (Field end signal for recording function in scaler 0)
    Display.Graphics_Irq_Handler_Set(DisplayBase::INT_TYPE_S0_VFIELD, 0, IntCallbackFunc_Vfield);
    Start_Video_Camera();
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

    // Temporary bug fix for Simple ISP library.
    param_isp_req.bias_r = (-129 +16); // -16
    param_isp_req.bias_g = (-129 +16); // -16
    param_isp_req.bias_b = (-129 +16); // -16

    set_gamma = 1.0;
    for (int i = 0; i < 256; i++) {
        lut[i] = i;
    }
    param_isp_req.table = (uint32_t)lut;
    param_isp_req.gamma = 1;
    auto_exposure = 0;

    // Jpeg setting
    bitmap_buff_info.width              = VIDEO_PIXEL_HW;
    bitmap_buff_info.height             = VIDEO_PIXEL_VW;
    bitmap_buff_info.format             = JPEG_Converter::WR_RD_YCbCr422;
    bitmap_buff_info.buffer_address     = (void *)fbuf_yuv;
    encode_options.encode_buff_size     = sizeof(JpegBuffer[0]);
    encode_options.p_EncodeCallBackFunc = &JcuEncodeCallBackFunc;
    encode_options.input_swapsetting    = JPEG_Converter::WR_RD_WRSWA_32_16BIT;

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
            Pi_ExposureControl(luminance);
        }

        // Jpeg convert
        dcache_invalid(JpegBuffer, sizeof(JpegBuffer));
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
    }
}

static void mount_romramfs(void) {
    FILE * fp;

    romram_bd.SetRomAddr(0x20000000, 0x2FFFFFFF);
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
}

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

#if (USE_DHCP == 0)
    DhcpServer dhcp_server(&network, "RZ/A2M");
#endif

    SnapshotHandler::attach_req(&snapshot_req);
    HTTPServerAddHandler<SnapshotHandler>("/camera"); //Camera
    HTTPServerAddHandler<SnapshotHandler>("/simple_isp");
    HTTPServerAddHandler<SnapshotHandler>("/auto_exposure");
    FSHandler::mount("/storage", "/");
    HTTPServerAddHandler<FSHandler>("/");
    HTTPServerStart(&network, 80);
}

