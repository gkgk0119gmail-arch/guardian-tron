 /**
 ******************************************************************************
 * @file    main.c
 * @author  GPM Application Team
 *
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2023 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
#include <string.h>
#include <unistd.h>

#include "cmw_camera.h"
#include "stm32n6570_discovery_bus.h"
#include "stm32n6570_discovery_lcd.h"
#include "stm32n6570_discovery_xspi.h"
#include "stm32n6570_discovery.h"
#include "stm32_lcd.h"
#include "app_fuseprogramming.h"
#include "stm32_lcd_ex.h"
#include "app_postprocess.h"
#include "stai.h"
#include "stai_network.h"
#include "app_camerapipeline.h"
#include "main.h"
#include <stdio.h>
#include "app_config.h"
#include "crop_img.h"
#include "gv_vision.h"

CLASSES_TABLE;

#define LCD_FG_WIDTH  SCREEN_WIDTH
#define LCD_FG_HEIGHT SCREEN_HEIGHT
#define LCD_FG_FRAMEBUFFER_SIZE  (LCD_FG_WIDTH * LCD_FG_HEIGHT * 2)

#ifndef APP_GIT_SHA1_STRING
#define APP_GIT_SHA1_STRING "dev"
#endif
#ifndef APP_VERSION_STRING
#define APP_VERSION_STRING "unversioned"
#endif


typedef struct
{
  uint32_t X0;
  uint32_t Y0;
  uint32_t XSize;
  uint32_t YSize;
} Rectangle_TypeDef;

/* Lcd Background area */
Rectangle_TypeDef lcd_bg_area = {
#if ASPECT_RATIO_MODE == ASPECT_RATIO_CROP || ASPECT_RATIO_MODE == ASPECT_RATIO_FIT
  .X0 = (LCD_FG_WIDTH - LCD_FG_HEIGHT) / 2,
#else
  .X0 = 0,
#endif
  .Y0 = 0,
  .XSize = 0,
  .YSize = 0,
};

/* Lcd Foreground area */
Rectangle_TypeDef lcd_fg_area = {
  .X0 = 0,
  .Y0 = 0,
  .XSize = LCD_FG_WIDTH,
  .YSize = LCD_FG_HEIGHT,
};

#define NUMBER_COLORS 10
const uint32_t colors[NUMBER_COLORS] = {
    UTIL_LCD_COLOR_GREEN,
    UTIL_LCD_COLOR_RED,
    UTIL_LCD_COLOR_CYAN,
    UTIL_LCD_COLOR_MAGENTA,
    UTIL_LCD_COLOR_YELLOW,
    UTIL_LCD_COLOR_GRAY,
    UTIL_LCD_COLOR_BLACK,
    UTIL_LCD_COLOR_BROWN,
    UTIL_LCD_COLOR_BLUE,
    UTIL_LCD_COLOR_ORANGE
};

#if POSTPROCESS_TYPE == POSTPROCESS_OD_YOLO_V2_UI
  od_yolov2_pp_static_param_t pp_params;
#elif POSTPROCESS_TYPE == POSTPROCESS_OD_YOLO_V5_UU
  od_yolov5_pp_static_param_t pp_params;
#elif POSTPROCESS_TYPE == POSTPROCESS_OD_YOLO_V8_UI
  od_yolov8_pp_static_param_t pp_params;
#elif POSTPROCESS_TYPE == POSTPROCESS_OD_ST_YOLOX_UI
  od_st_yolox_pp_static_param_t pp_params;
#elif POSTPROCESS_TYPE == POSTPROCESS_OD_SSD_UI
  od_ssd_pp_static_param_t pp_params;
#elif POSTPROCESS_TYPE == POSTPROCESS_OD_ST_YOLOD_UI
  od_yolo_d_pp_static_param_t pp_params;
#elif POSTPROCESS_TYPE == POSTPROCESS_OD_BLAZEFACE_UI
  od_blazeface_pp_static_param_t pp_params;
#else
  #error "PostProcessing type not supported"
#endif

UART_HandleTypeDef huart1;
volatile int32_t cameraFrameReceived;
volatile uint32_t gv_frame_cyc;
volatile uint32_t gv_stage, gv_frames, gv_cam_err, gv_isp_err, gv_ready;
static stai_ptr nn_out[STAI_NETWORK_OUT_NUM];
static int32_t nn_out_len[STAI_NETWORK_OUT_NUM];
static stai_size number_output;
static uint32_t nn_in_len;
stai_ptr nn_in;
BSP_LCD_LayerConfig_t LayerConfig = {0};
void* pp_input;
od_pp_out_t pp_output;

#define ALIGN_TO_16(value) (((value) + 15) & ~15)

/* When NN input dimensions are not a multiple of 16, the DCMIPP output needs cropping */
#if (STAI_NETWORK_IN_1_WIDTH * STAI_NETWORK_IN_1_CHANNEL) != ALIGN_TO_16(STAI_NETWORK_IN_1_WIDTH * STAI_NETWORK_IN_1_CHANNEL)
#define DCMIPP_NN_NEEDS_CROP 1
#define DCMIPP_OUT_NN_LEN (ALIGN_TO_16(STAI_NETWORK_IN_1_WIDTH * STAI_NETWORK_IN_1_CHANNEL) * STAI_NETWORK_IN_1_HEIGHT)
#define DCMIPP_OUT_NN_BUFF_LEN (DCMIPP_OUT_NN_LEN + 32 - DCMIPP_OUT_NN_LEN%32)

__attribute__ ((aligned (32)))
static uint8_t dcmipp_out_nn[DCMIPP_OUT_NN_BUFF_LEN];
#else
#define DCMIPP_NN_NEEDS_CROP 0
#endif

/* model */
STAI_NETWORK_CONTEXT_DECLARE(network_context, STAI_NETWORK_CONTEXT_SIZE)
/* Lcd Background Buffer */
__attribute__ ((section (".psram_bss")))
__attribute__ ((aligned (32)))
static uint8_t lcd_bg_buffer[800 * 480 * 2];
/* Lcd Foreground Buffer */
__attribute__ ((section (".psram_bss")))
__attribute__ ((aligned (32)))
static uint8_t lcd_fg_buffer[2][LCD_FG_WIDTH * LCD_FG_HEIGHT * 2];
static int lcd_fg_buffer_rd_idx;

static void SystemClock_Config(void);
static void CONSOLE_Config(void);
static void NPURam_enable(void);
static void NPUCache_config(void);
static void LCD_init(void);
static void Security_Config(void);
static void set_clk_sleep_mode(void);
static void IAC_Config(void);
static void NeuralNetwork_init(uint32_t *nn_in_length, stai_ptr *nn_out, stai_size *number_output, int32_t nn_out_len[]);


/**
  * @brief  NN runtime, post-processing, camera and LCD bring-up (runs in the vision task).
  */
int gv_vision_init(void)
{
  static int nn_done;
  if (!nn_done)
  {
    NeuralNetwork_init(&nn_in_len, nn_out, &number_output, nn_out_len);
    stai_network_info info;
    int ret = stai_network_get_info(network_context, &info);
    assert(ret == STAI_SUCCESS);
    app_postprocess_init(&pp_params, &info);
    nn_done = 1;
  }

  uint32_t pitch_nn = 0;
  int cam = CameraPipeline_Init(&lcd_bg_area.XSize, &lcd_bg_area.YSize, &pitch_nn);
  if (cam != 0)
  {
    printf("[vision] camera init failed (%d): check the camera ribbon\n", cam);
    CMW_CAMERA_DeInit();
    return -1;
  }
  LCD_init();
  CameraPipeline_DisplayPipe_Start(lcd_bg_buffer, CMW_MODE_CONTINUOUS);

  printf("========================================\n");
  printf("Guardian-TRON vision (ST OD %s) %s %s\n", APP_VERSION_STRING, __DATE__, __TIME__);
  printf("STEdgeAI %d.%d.%d | NN %s | in %dx%dx%d\n", STAI_TOOLS_VERSION_MAJOR, STAI_TOOLS_VERSION_MINOR,
         STAI_TOOLS_VERSION_MICRO, STAI_NETWORK_ORIGIN_MODEL_NAME,
         STAI_NETWORK_IN_1_WIDTH, STAI_NETWORK_IN_1_HEIGHT, STAI_NETWORK_IN_1_CHANNEL);
  printf("========================================\n");
  return 0;
}

/**
  * @brief  One frame: snapshot into the NN input, NPU inference, box decoding.
  * @retval 0 on success, -1 if no camera frame arrived
  */
int gv_vision_step(gv_result_t *r)
{
  gv_stage = GV_ST_ISP;
  if (CameraPipeline_IspUpdate() != 0) gv_isp_err++;

  cameraFrameReceived = 0;
  gv_stage = GV_ST_START;
#if DCMIPP_NN_NEEDS_CROP
#error "Guardian-TRON: NN input width*channels must be a multiple of 16 (no crop path)"
#else
  /* Start NN camera single capture Snapshot directly into NN input */
  CameraPipeline_NNPipe_Start(nn_in, CMW_MODE_SNAPSHOT);
#endif
  gv_stage = GV_ST_WAIT;
  for (uint32_t waited = 0; cameraFrameReceived == 0; waited++)
  {
    if (waited > 500) return -1;
    gv_os_sleep_ms(1);
  }
  r->frame_cyc = gv_frame_cyc;

  gv_stage = GV_ST_NPU;
  uint32_t c0 = DWT->CYCCNT;
  int ret = stai_network_run(network_context, STAI_MODE_SYNC);
  assert(ret == 0);
  r->infer_cyc = DWT->CYCCNT - c0;

  gv_stage = GV_ST_PP;
  ret = app_postprocess_run((void **) nn_out, number_output, &pp_output, &pp_params);
  assert(ret == 0);

  /* copy the boxes out: pp_output points into the NN output buffers we discard below */
  r->nb = 0;
  for (uint32_t i = 0; i < pp_output.nb_detect && r->nb < GV_MAX_BOXES; i++)
  {
    od_pp_outBuffer_t *b = &pp_output.pOutBuff[i];
    r->box[r->nb].x = b->x_center;
    r->box[r->nb].y = b->y_center;
    r->box[r->nb].w = b->width;
    r->box[r->nb].h = b->height;
    r->box[r->nb].conf = b->conf;
    r->nb++;
  }
  r->done_cyc = DWT->CYCCNT;
  gv_frames++;
  gv_ready = 1;
  gv_stage = GV_ST_DECIDE;

  /* Discard nn_out region (used by pp_input and pp_outputs variables) to avoid Dcache evictions during nn inference */
  for (int i = 0; i < number_output; i++)
  {
    SCB_InvalidateDCache_by_Addr(nn_out[i], nn_out_len[i]);
  }
  return 0;
}

/* boot breadcrumbs (flash-boot debugging): progress word in the bottom of the MSP area,
 * readable afterwards with the debugger (connect under reset keeps SRAM) */
#define GV_BC ((volatile uint32_t *)0x340FC000UL)
void gv_bc(uint32_t v) { GV_BC[0] = v; GV_BC[1]++; __DSB(); }

void gv_hw_init(void)
{
  GV_BC[1] = 0; gv_bc(0xB0070001);
  /* Power on ICACHE */
  MEMSYSCTL->MSCR |= MEMSYSCTL_MSCR_ICACTIVE_Msk;

  /* Set back system and CPU clock source to HSI */
  __HAL_RCC_CPUCLK_CONFIG(RCC_CPUCLKSOURCE_HSI);
  __HAL_RCC_SYSCLK_CONFIG(RCC_SYSCLKSOURCE_HSI);

  HAL_Init();

  SCB_EnableICache();

#if defined(USE_DCACHE)
  /* Power on DCACHE */
  MEMSYSCTL->MSCR |= MEMSYSCTL_MSCR_DCACTIVE_Msk;
  SCB_EnableDCache();
#endif

  gv_bc(0xB0070002);
  SystemClock_Config();
  gv_bc(0xB0070003);

  CONSOLE_Config();
  gv_bc(0xB0070004);

  NPURam_enable();

  Fuse_Programming();

  NPUCache_config();
  gv_bc(0xB0070005);

  /*** External RAM and NOR Flash *********************************************/
  int32_t ram_init = BSP_XSPI_RAM_Init(0);
  int32_t ram_mm = BSP_XSPI_RAM_EnableMemoryMappedMode(0);

  BSP_XSPI_NOR_Init_t NOR_Init;
  NOR_Init.InterfaceMode = BSP_XSPI_NOR_OPI_MODE;
  NOR_Init.TransferRate = BSP_XSPI_NOR_DTR_TRANSFER;
  int32_t nor_init = BSP_XSPI_NOR_Init(0, &NOR_Init);
  int32_t nor_mm = BSP_XSPI_NOR_EnableMemoryMappedMode(0);
  gv_bc(0xB0070006);

  /* Guardian-TRON bring-up check: external memories must be up before the NPU reads its weights */
  volatile uint32_t *w = (volatile uint32_t *) 0x70380000UL;   /* network_data.xSPI2.bin starts 05 fd 98 fc */
  volatile uint32_t *pr = (volatile uint32_t *) 0x90000000UL;
  uint32_t pr_old = pr[0]; pr[0] = 0xA5C3F00DU; __DSB(); uint32_t pr_back = pr[0]; pr[0] = pr_old;
  printf("[hw] PSRAM init %ld mm %ld rw %s | NOR init %ld mm %ld | weights[0] 0x%08lx %s\n",
         (long) ram_init, (long) ram_mm, pr_back == 0xA5C3F00DU ? "ok" : "FAIL",
         (long) nor_init, (long) nor_mm, (unsigned long) w[0], w[0] == 0xfc98fd05UL ? "ok" : "MISMATCH");

  /* Set all required IPs as secure privileged */
  Security_Config();

  IAC_Config();
  set_clk_sleep_mode();
  gv_bc(0xB0070007);

}

static void NeuralNetwork_init(uint32_t *nn_in_length, stai_ptr *nn_out, stai_size *number_output, int32_t nn_out_len[])
{
  stai_network_info info;
  int ret;

  /* initialize runtime */
  ret = stai_runtime_init();
  assert(ret == STAI_SUCCESS);
  /* init model instance */
  ret = stai_network_init(network_context);
  assert(ret == STAI_SUCCESS);

  ret = stai_network_get_info(network_context, &info);
  assert(ret == STAI_SUCCESS);
  assert(info.n_inputs == 1);
  *number_output = STAI_NETWORK_OUT_NUM;

  /* Get the input buffer size & address */
  *nn_in_length = info.inputs[0].size_bytes;
  ret = stai_network_get_inputs(network_context, &nn_in, (stai_size *)&info.n_inputs);
  assert(ret == STAI_SUCCESS);

  /* Get the output buffers size & address */
  ret = stai_network_get_outputs(network_context, nn_out, number_output);
  assert(ret == STAI_SUCCESS);
  for (int i = 0; i < *number_output; i++)
  {
    nn_out_len[i] = info.outputs[i].size_bytes;
  }
}

static void NPURam_enable(void)
{
  __HAL_RCC_NPU_CLK_ENABLE();
  __HAL_RCC_NPU_FORCE_RESET();
  __HAL_RCC_NPU_RELEASE_RESET();

  /* Enable NPU RAMs (4x448KB) */
  __HAL_RCC_AXISRAM3_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM4_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM5_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM6_MEM_CLK_ENABLE();
  __HAL_RCC_RAMCFG_CLK_ENABLE();
  RAMCFG_HandleTypeDef hramcfg = {0};
  hramcfg.Instance =  RAMCFG_SRAM3_AXI;
  HAL_RAMCFG_EnableAXISRAM(&hramcfg);
  hramcfg.Instance =  RAMCFG_SRAM4_AXI;
  HAL_RAMCFG_EnableAXISRAM(&hramcfg);
  hramcfg.Instance =  RAMCFG_SRAM5_AXI;
  HAL_RAMCFG_EnableAXISRAM(&hramcfg);
  hramcfg.Instance =  RAMCFG_SRAM6_AXI;
  HAL_RAMCFG_EnableAXISRAM(&hramcfg);
}

static void set_clk_sleep_mode(void)
{
  /*** Enable sleep mode support during NPU inference *************************/
  /* Configure peripheral clocks to remain active during sleep mode */
  /* Keep all IP's enabled during WFE so they can wake up CPU. Fine tune
   * this if you want to save maximum power
   */
  __HAL_RCC_XSPI1_CLK_SLEEP_ENABLE();    /* For display frame buffer */
  __HAL_RCC_XSPI2_CLK_SLEEP_ENABLE();    /* For NN weights */
  __HAL_RCC_NPU_CLK_SLEEP_ENABLE();      /* For NN inference */
  __HAL_RCC_CACHEAXI_CLK_SLEEP_ENABLE(); /* For NN inference */
  __HAL_RCC_LTDC_CLK_SLEEP_ENABLE();     /* For display */
  __HAL_RCC_DMA2D_CLK_SLEEP_ENABLE();    /* For display */
  __HAL_RCC_DCMIPP_CLK_SLEEP_ENABLE();   /* For camera configuration retention */
  __HAL_RCC_CSI_CLK_SLEEP_ENABLE();      /* For camera configuration retention */

  __HAL_RCC_FLEXRAM_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM1_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM2_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM3_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM4_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM5_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM6_MEM_CLK_SLEEP_ENABLE(); 

}

static void NPUCache_config(void)
{
  npu_cache_enable();
}

static void Security_Config(void)
{
  __HAL_RCC_RIFSC_CLK_ENABLE();
  RIMC_MasterConfig_t RIMC_master = {0};
  RIMC_master.MasterCID = RIF_CID_1;
  RIMC_master.SecPriv = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_NPU, &RIMC_master);
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_DMA2D, &RIMC_master);
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_DCMIPP, &RIMC_master);
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_LTDC1 , &RIMC_master);
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_LTDC2 , &RIMC_master);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_NPU , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_DMA2D , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_CSI    , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_DCMIPP , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_LTDC   , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_LTDCL1 , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_LTDCL2 , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
}

static void IAC_Config(void)
{
/* Configure IAC to trap illegal access events */
  __HAL_RCC_IAC_CLK_ENABLE();
  __HAL_RCC_IAC_FORCE_RESET();
  __HAL_RCC_IAC_RELEASE_RESET();
}

void IAC_IRQHandler(void)
{
  while (1)
  {
  }
}

/**
  * @brief  LCD overlay: person boxes, NPU timing, and the Guardian-TRON stop banner.
  */
void gv_vision_draw(const gv_result_t *r, int hazard, uint32_t stop_count, uint32_t fps_x10)
{
  int ret;

  gv_stage = GV_ST_DRAW;
  ret = HAL_LTDC_SetAddress_NoReload(&hlcd_ltdc, (uint32_t) lcd_fg_buffer[lcd_fg_buffer_rd_idx], LTDC_LAYER_2);
  assert(ret == HAL_OK);

  /* Clear previous boxes */
  UTIL_LCD_FillRect(lcd_fg_area.X0, lcd_fg_area.Y0, lcd_fg_area.XSize, lcd_fg_area.YSize, 0x00000000);

  uint32_t box_color = hazard ? UTIL_LCD_COLOR_RED : UTIL_LCD_COLOR_GREEN;
  /* Boxes can extend past the image (a person right in front of the lens gives h > 100%).
   * Clip in signed math: the unsigned clamping of the ST example produces zero-size
   * rectangles there, and a zero-size DMA2D fill never completes (vision task hangs). */
  const int32_t ix0 = (int32_t) lcd_bg_area.X0, iy0 = (int32_t) lcd_bg_area.Y0;
  const int32_t ix1 = ix0 + (int32_t) lcd_bg_area.XSize - 1, iy1 = iy0 + (int32_t) lcd_bg_area.YSize - 1;
  for (int32_t i = 0; i < r->nb; i++)
  {
    const gv_box_t *b = &r->box[i];
    int32_t x0 = ix0 + (int32_t) ((b->x - b->w / 2) * (float32_t) lcd_bg_area.XSize);
    int32_t x1 = ix0 + (int32_t) ((b->x + b->w / 2) * (float32_t) lcd_bg_area.XSize);
    int32_t y0 = iy0 + (int32_t) ((b->y - b->h / 2) * (float32_t) lcd_bg_area.YSize);
    int32_t y1 = iy0 + (int32_t) ((b->y + b->h / 2) * (float32_t) lcd_bg_area.YSize);
    if (x0 < ix0) x0 = ix0;
    if (y0 < iy0) y0 = iy0;
    if (x1 > ix1) x1 = ix1;
    if (y1 > iy1) y1 = iy1;
    if (x1 - x0 < 4 || y1 - y0 < 4) continue;
    UTIL_LCD_DrawRect((uint32_t) x0, (uint32_t) y0, (uint32_t) (x1 - x0), (uint32_t) (y1 - y0), box_color);
    int32_t ty = (y0 + 24 <= iy1) ? y0 + 2 : iy1 - 24;
    UTIL_LCDEx_PrintfAt((uint32_t) (x0 + 2), (uint32_t) ty, LEFT_MODE, "person %.0f%%", b->conf * 100.0f);
  }

  uint32_t infer_us = (uint32_t) (((uint64_t) r->infer_cyc * 1000000ULL) / SystemCoreClock);
  UTIL_LCD_SetBackColor(0x40000000);
  UTIL_LCDEx_PrintfAt(0, LINE(1), CENTER_MODE, "Guardian-TRON  uT-Kernel 3.0 + NPU");
  UTIL_LCDEx_PrintfAt(0, LINE(20), CENTER_MODE, "NPU %lu.%lums  %lu.%lufps  stops %lu",
                      infer_us / 1000, (infer_us % 1000) / 100, fps_x10 / 10, fps_x10 % 10, stop_count);
  if (hazard)
  {
    UTIL_LCD_SetBackColor(UTIL_LCD_COLOR_RED);
    UTIL_LCDEx_PrintfAt(0, LINE(3), CENTER_MODE, "  STOP : PERSON AHEAD  ");
  }
  UTIL_LCD_SetBackColor(0);

  SCB_CleanDCache_by_Addr(lcd_fg_buffer[lcd_fg_buffer_rd_idx], LCD_FG_FRAMEBUFFER_SIZE);
  ret = HAL_LTDC_ReloadLayer(&hlcd_ltdc, LTDC_RELOAD_VERTICAL_BLANKING, LTDC_LAYER_2);
  assert(ret == HAL_OK);
  lcd_fg_buffer_rd_idx = 1 - lcd_fg_buffer_rd_idx;
}

static void LCD_init(void)
{
  BSP_LCD_Init(0, LCD_ORIENTATION_LANDSCAPE);

  /* Preview layer Init */
  LayerConfig.X0          = lcd_bg_area.X0;
  LayerConfig.Y0          = lcd_bg_area.Y0;
  LayerConfig.X1          = lcd_bg_area.X0 + lcd_bg_area.XSize;
  LayerConfig.Y1          = lcd_bg_area.Y0 + lcd_bg_area.YSize;
  LayerConfig.PixelFormat = LCD_PIXEL_FORMAT_RGB565;
  LayerConfig.Address     = (uint32_t) lcd_bg_buffer;

  BSP_LCD_ConfigLayer(0, LTDC_LAYER_1, &LayerConfig);

  LayerConfig.X0 = lcd_fg_area.X0;
  LayerConfig.Y0 = lcd_fg_area.Y0;
  LayerConfig.X1 = lcd_fg_area.X0 + lcd_fg_area.XSize;
  LayerConfig.Y1 = lcd_fg_area.Y0 + lcd_fg_area.YSize;
  LayerConfig.PixelFormat = LCD_PIXEL_FORMAT_ARGB4444;
  LayerConfig.Address = (uint32_t) lcd_fg_buffer; /* External XSPI1 PSRAM */

  BSP_LCD_ConfigLayer(0, LTDC_LAYER_2, &LayerConfig);
  UTIL_LCD_SetFuncDriver(&LCD_Driver);
  UTIL_LCD_SetLayer(LTDC_LAYER_2);
  UTIL_LCD_Clear(0x00000000);
  UTIL_LCD_SetFont(&Font20);
  UTIL_LCD_SetTextColor(UTIL_LCD_COLOR_WHITE);
}

/**
  * @brief  DCMIPP Clock Config for DCMIPP.
  * @param  hdcmipp  DCMIPP Handle
  *         Being __weak it can be overwritten by the application
  * @retval HAL_status
  */
HAL_StatusTypeDef MX_DCMIPP_ClockConfig(DCMIPP_HandleTypeDef *hdcmipp)
{
  RCC_PeriphCLKInitTypeDef RCC_PeriphCLKInitStruct = {0};
  HAL_StatusTypeDef ret = HAL_OK;

  RCC_PeriphCLKInitStruct.PeriphClockSelection = RCC_PERIPHCLK_DCMIPP;
  RCC_PeriphCLKInitStruct.DcmippClockSelection = RCC_DCMIPPCLKSOURCE_IC17;
  RCC_PeriphCLKInitStruct.ICSelection[RCC_IC17].ClockSelection = RCC_ICCLKSOURCE_PLL2;
  RCC_PeriphCLKInitStruct.ICSelection[RCC_IC17].ClockDivider = 3;
  ret = HAL_RCCEx_PeriphCLKConfig(&RCC_PeriphCLKInitStruct);
  if (ret)
  {
    return ret;
  }

  RCC_PeriphCLKInitStruct.PeriphClockSelection = RCC_PERIPHCLK_CSI;
  RCC_PeriphCLKInitStruct.ICSelection[RCC_IC18].ClockSelection = RCC_ICCLKSOURCE_PLL1;
  RCC_PeriphCLKInitStruct.ICSelection[RCC_IC18].ClockDivider = 40;
  ret = HAL_RCCEx_PeriphCLKConfig(&RCC_PeriphCLKInitStruct);
  if (ret)
  {
    return ret;
  }

  return ret;
}

static void SystemClock_Config(void)
{
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_PeriphCLKInitTypeDef RCC_PeriphCLKInitStruct = {0};

  /* Ensure VDDCORE=0.9V before increasing the system frequency */
  BSP_SMPS_Init(SMPS_VOLTAGE_OVERDRIVE);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_NONE;

  /* PLL1 = 64 x 25 / 2 = 800MHz */
  RCC_OscInitStruct.PLL1.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL1.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL1.PLLM = 2;
  RCC_OscInitStruct.PLL1.PLLN = 25;
  RCC_OscInitStruct.PLL1.PLLFractional = 0;
  RCC_OscInitStruct.PLL1.PLLP1 = 1;
  RCC_OscInitStruct.PLL1.PLLP2 = 1;

  /* PLL2 = 64 x 125 / 8 = 1000MHz */
  RCC_OscInitStruct.PLL2.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL2.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL2.PLLM = 8;
  RCC_OscInitStruct.PLL2.PLLFractional = 0;
  RCC_OscInitStruct.PLL2.PLLN = 125;
  RCC_OscInitStruct.PLL2.PLLP1 = 1;
  RCC_OscInitStruct.PLL2.PLLP2 = 1;

  /* PLL3 = (64 x 225 / 8) / (1 * 2) = 900MHz */
  RCC_OscInitStruct.PLL3.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL3.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL3.PLLM = 8;
  RCC_OscInitStruct.PLL3.PLLN = 225;
  RCC_OscInitStruct.PLL3.PLLFractional = 0;
  RCC_OscInitStruct.PLL3.PLLP1 = 1;
  RCC_OscInitStruct.PLL3.PLLP2 = 2;

  /* PLL4 = (64 x 225 / 8) / (6 * 6) = 50 MHz */
  RCC_OscInitStruct.PLL4.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL4.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL4.PLLM = 8;
  RCC_OscInitStruct.PLL4.PLLFractional = 0;
  RCC_OscInitStruct.PLL4.PLLN = 225;
  RCC_OscInitStruct.PLL4.PLLP1 = 6;
  RCC_OscInitStruct.PLL4.PLLP2 = 6;

  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    while(1);
  }

  RCC_ClkInitStruct.ClockType = (RCC_CLOCKTYPE_CPUCLK | RCC_CLOCKTYPE_SYSCLK |
                                 RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 |
                                 RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_PCLK4 |
                                 RCC_CLOCKTYPE_PCLK5);

  /* CPU CLock (sysa_ck) = ic1_ck = PLL1 output/ic1_divider = 800 MHz */
  RCC_ClkInitStruct.CPUCLKSource = RCC_CPUCLKSOURCE_IC1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_IC2_IC6_IC11;
  RCC_ClkInitStruct.IC1Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
  RCC_ClkInitStruct.IC1Selection.ClockDivider = 1;

  /* AXI Clock (sysb_ck) = ic2_ck = PLL1 output/ic2_divider = 400 MHz */
  RCC_ClkInitStruct.IC2Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
  RCC_ClkInitStruct.IC2Selection.ClockDivider = 2;

  /* NPU Clock (sysc_ck) = ic6_ck = PLL2 output/ic6_divider = 1000 MHz */
  RCC_ClkInitStruct.IC6Selection.ClockSelection = RCC_ICCLKSOURCE_PLL2;
  RCC_ClkInitStruct.IC6Selection.ClockDivider = 1;

  /* AXISRAM3/4/5/6 Clock (sysd_ck) = ic11_ck = PLL3 output/ic11_divider = 900 MHz */
  RCC_ClkInitStruct.IC11Selection.ClockSelection = RCC_ICCLKSOURCE_PLL3;
  RCC_ClkInitStruct.IC11Selection.ClockDivider = 1;

  /* HCLK = sysb_ck / HCLK divider = 200 MHz */
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;

  /* PCLKx = HCLK / PCLKx divider = 200 MHz */
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV1;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV1;
  RCC_ClkInitStruct.APB5CLKDivider = RCC_APB5_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct) != HAL_OK)
  {
    while(1);
  }

  RCC_PeriphCLKInitStruct.PeriphClockSelection = 0;

  /* XSPI1 kernel clock (ck_ker_xspi1) = HCLK = 200MHz */
  RCC_PeriphCLKInitStruct.PeriphClockSelection |= RCC_PERIPHCLK_XSPI1;
  RCC_PeriphCLKInitStruct.Xspi1ClockSelection = RCC_XSPI1CLKSOURCE_HCLK;

  /* XSPI2 kernel clock (ck_ker_xspi1) = HCLK =  200MHz */
  RCC_PeriphCLKInitStruct.PeriphClockSelection |= RCC_PERIPHCLK_XSPI2;
  RCC_PeriphCLKInitStruct.Xspi2ClockSelection = RCC_XSPI2CLKSOURCE_HCLK;

  if (HAL_RCCEx_PeriphCLKConfig(&RCC_PeriphCLKInitStruct) != HAL_OK)
  {
    { gv_bc(0xBADC0000 | __LINE__); while (1); }
  }
}

static void CONSOLE_Config()
{
  GPIO_InitTypeDef gpio_init;

  /* Guardian-TRON: clock USART1 from CLKP = HSI 64 MHz. μT-Kernel's tm_com re-programs
   * USART1 with BRR 0x22C (115200 @ 64 MHz), so HAL printf and tm_printf share that clock. */
  RCC_PeriphCLKInitTypeDef uart_clk = {0};
  uart_clk.PeriphClockSelection = RCC_PERIPHCLK_CKPER | RCC_PERIPHCLK_USART1;
  uart_clk.CkperClockSelection = RCC_CLKPCLKSOURCE_HSI;
  uart_clk.Usart1ClockSelection = RCC_USART1CLKSOURCE_CLKP;
  if (HAL_RCCEx_PeriphCLKConfig(&uart_clk) != HAL_OK)
  {
    gv_bc(0xBADC0001);
    { gv_bc(0xBADC0000 | __LINE__); while (1); }
  }
  __HAL_RCC_USART1_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();

 /* DISCO & NUCLEO USART1 (PE5/PE6) */
  gpio_init.Mode      = GPIO_MODE_AF_PP;
  gpio_init.Pull      = GPIO_PULLUP;
  gpio_init.Speed     = GPIO_SPEED_FREQ_HIGH;
  gpio_init.Pin       = GPIO_PIN_5 | GPIO_PIN_6;
  gpio_init.Alternate = GPIO_AF7_USART1;
  HAL_GPIO_Init(GPIOE, &gpio_init);

  huart1.Instance          = USART1;
  huart1.Init.BaudRate     = 115200;
  huart1.Init.Mode         = UART_MODE_TX_RX;
  huart1.Init.Parity       = UART_PARITY_NONE;
  huart1.Init.WordLength   = UART_WORDLENGTH_8B;
  huart1.Init.StopBits     = UART_STOPBITS_1;
  huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    { gv_bc(0xBADC0000 | __LINE__); while (1); }
  }
}

int _write(int file, char *ptr, int len)
{
  HAL_StatusTypeDef status;

  if ((file != STDOUT_FILENO) && (file != STDERR_FILENO)) {
      errno = EBADF;
      return -1;
  }

  status = HAL_UART_Transmit(&huart1, (uint8_t*)ptr, len, ~0);

  return (status == HAL_OK ? len : 0);
}

void npu_cache_enable_clocks_and_reset(void)
{
  __HAL_RCC_CACHEAXIRAM_MEM_CLK_ENABLE();
  __HAL_RCC_CACHEAXI_CLK_ENABLE();
  __HAL_RCC_CACHEAXI_FORCE_RESET();
  __HAL_RCC_CACHEAXI_RELEASE_RESET();
}

void npu_cache_disable_clocks_and_reset(void)
{
  __HAL_RCC_CACHEAXIRAM_MEM_CLK_DISABLE();
  __HAL_RCC_CACHEAXI_CLK_DISABLE();
  __HAL_RCC_CACHEAXI_FORCE_RESET();
}

#ifdef  USE_FULL_ASSERT

/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t* file, uint32_t line)
{
  UNUSED(file);
  UNUSED(line);
  __BKPT(0);
  while (1)
  {
  }
}

#endif
