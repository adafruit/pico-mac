/* pico-umac
 *
 * Main loop to initialise umac, and run main event loop (piping
 * keyboard/mouse events in).
 *
 * Copyright 2024 Matt Evans
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hw.h"
#include "video.h"
#include "kbd.h"

#include "pio_usb_configuration.h"
#include "bsp/rp2040/boards/adafruit_fruit_jam/board.h"
#include "tusb.h"

#include "umac.h"
#include "clocking.h"

#if USE_SD
#include "f_util.h"
#include "ff.h"
#include "rtc.h"
#include "hw_config.h"
#endif

#if USE_PSRAM
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip.h"
#endif

#if ENABLE_AUDIO
#include "pico/audio_i2s.h"
#include "hardware/i2c.h"
uint8_t *audio_base;
static void audio_setup();
static bool audio_poll();
#endif

////////////////////////////////////////////////////////////////////////////////
// Imports and data

extern void     hid_app_task(void);
extern int cursor_x;
extern int cursor_y;
extern int cursor_button;

// Mac binary data:  disc and ROM images
static const uint8_t umac_disc[] = {
#include "umac-disc.h"
};
static const uint8_t umac_rom[] = {
#include "umac-rom.h"
};

#if USE_PSRAM
#define umac_ram ((uint8_t*)0x11000000)
#else
static uint8_t umac_ram[RAM_SIZE];
#endif

#define MIRROR_FRAMEBUFFER (USE_PSRAM || DISP_WIDTH != 640)
#if MIRROR_FRAMEBUFFER
static uint32_t umac_framebuffer_mirror[640*480/32];
#endif

////////////////////////////////////////////////////////////////////////////////

static void     io_init()
{
        gpio_init(GPIO_LED_PIN);
        gpio_set_dir(GPIO_LED_PIN, GPIO_OUT);
}

static void     poll_led_etc()
{
        static int led_on = 0;
        static absolute_time_t last = 0;
        absolute_time_t now = get_absolute_time();

        if (absolute_time_diff_us(last, now) > 500*1000) {
                last = now;

                //led_on ^= 1;
                //gpio_put(GPIO_LED_PIN, led_on);
        }
}

static int umac_cursor_x = 0;
static int umac_cursor_y = 0;
static int umac_cursor_button = 0;

#define umac_get_audio_offset() (RAM_SIZE - 768)
#if MIRROR_FRAMEBUFFER
static void copy_framebuffer() {
    uint32_t *src = (uint32_t*)(umac_ram + umac_get_fb_offset());
#if DISP_WIDTH==640 && DISP_HEIGHT==480
    uint32_t *dest = umac_framebuffer_mirror;
    for(int i=0; i<640*480/32; i++) {
        *dest++ = *src++;
    }
#elif DISP_WIDTH==512 && DISP_HEIGHT==342
    #define DISP_XOFFSET ((640 - DISP_WIDTH) / 32 / 2)
    #define DISP_YOFFSET ((480 - DISP_HEIGHT) / 2)
    #define LONGS_PER_INPUT_ROW (DISP_WIDTH / 32)
    #define LONGS_PER_OUTPUT_ROW (640 / 32)
    for(int i=0; i<DISP_HEIGHT; i++) {
        uint32_t *dest = umac_framebuffer_mirror + (DISP_YOFFSET * LONGS_PER_OUTPUT_ROW + DISP_XOFFSET) + LONGS_PER_OUTPUT_ROW * i;
        for(int j=0; j<LONGS_PER_INPUT_ROW; j++) {
          *dest++ = *src++ ^ 0xffffffff;
        }
    }
#else
#error Unsupported display geometry for framebuffer mirroring
#endif
}
#endif

static void     poll_umac()
{
        static absolute_time_t last_1hz = 0;
        static absolute_time_t last_vsync = 0;
        absolute_time_t now = get_absolute_time();

        umac_loop();

        int64_t p_1hz = absolute_time_diff_us(last_1hz, now);
        int64_t p_vsync = absolute_time_diff_us(last_vsync, now);
        bool pending_vsync = p_vsync > 16667;
#if ENABLE_AUDIO
        pending_vsync |= audio_poll();
#endif
        if (pending_vsync) {
#if MIRROR_FRAMEBUFFER
                copy_framebuffer();
#endif
                /* FIXME: Trigger this off actual vsync */
                umac_vsync_event();
                last_vsync = now;
        }
        if (p_1hz >= 1000000) {
                umac_1hz_event();
                last_1hz = now;
        }

        int update = 0;
        int dx = 0;
        int dy = 0;
        int b = umac_cursor_button;
        if (cursor_x != umac_cursor_x) {
                dx = cursor_x - umac_cursor_x;
                umac_cursor_x = cursor_x;
                update = 1;
        }
        if (cursor_y != umac_cursor_y) {
                dy = cursor_y - umac_cursor_y;
                umac_cursor_y = cursor_y;
                update = 1;
        }
        if (cursor_button != umac_cursor_button) {
                b = cursor_button;
                umac_cursor_button = cursor_button;
                update = 1;
        }
        if (update) {
                umac_mouse(dx, -dy, b);
        }

        if (!kbd_queue_empty()) {
                uint16_t k = kbd_queue_pop();
                umac_kbd_event(k & 0xff, !!(k & 0x8000));
        }
}

#if USE_SD
static int      disc_do_read(void *ctx, uint8_t *data, unsigned int offset, unsigned int len)
{
        FIL *fp = (FIL *)ctx;
        f_lseek(fp, offset);
        unsigned int did_read = 0;
        FRESULT fr = f_read(fp, data, len, &did_read);
        if (fr != FR_OK || len != did_read) {
                printf("disc: f_read returned %d, read %u (of %u)\n", fr, did_read, len);
                return -1;
        }
        return 0;
}

static int      disc_do_write(void *ctx, uint8_t *data, unsigned int offset, unsigned int len)
{
        FIL *fp = (FIL *)ctx;
        f_lseek(fp, offset);
        unsigned int did_write = 0;
        FRESULT fr = f_write(fp, data, len, &did_write);
        if (fr != FR_OK || len != did_write) {
                printf("disc: f_write returned %d, read %u (of %u)\n", fr, did_write, len);
                return -1;
        }
        return 0;
}

static FIL discfp;
#endif

static void     disc_setup(disc_descr_t discs[DISC_NUM_DRIVES])
{
#if USE_SD
        char *disc0_name;
        const char *disc0_ro_name = "umac0ro.img";
        const char *disc0_pattern = "umac0*.img";

        /* Mount SD filesystem */
        printf("Starting SPI/FatFS:\n");
        set_spi_dma_irq_channel(true, false);
        sd_card_t *pSD = sd_get_by_num(0);
        FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
        printf("  mount: %d\n", fr);
        if (fr != FR_OK) {
                printf("  error mounting disc: %s (%d)\n", FRESULT_str(fr), fr);
                goto no_sd;
        }

        /* Look for a disc image */
        DIR di = {0};
        FILINFO fi = {0};
        fr = f_findfirst(&di, &fi, "/", disc0_pattern);
        if (fr != FR_OK) {
                printf("  Can't find images %s: %s (%d)\n", disc0_pattern, FRESULT_str(fr), fr);
                goto no_sd;
        }
        disc0_name = fi.fname;
        f_closedir(&di);

        int read_only = !strcmp(disc0_name, disc0_ro_name);
        printf("  Opening %s (R%c)\n", disc0_name, read_only ? 'O' : 'W');

        /* Open image, set up disc info: */
        fr = f_open(&discfp, disc0_name, FA_OPEN_EXISTING | FA_READ | FA_WRITE);
        if (fr != FR_OK && fr != FR_EXIST) {
                printf("  *** Can't open %s: %s (%d)!\n", disc0_name, FRESULT_str(fr), fr);
                goto no_sd;
        } else {
                printf("  Opened, size %d (0x%x)\n", (unsigned)f_size(&discfp), (unsigned)f_size(&discfp));
                if (read_only)
                        printf("  (disc is read-only)\n");
                discs[0].base = 0; // Means use R/W ops
                discs[0].read_only = read_only;
                discs[0].size = f_size(&discfp);
                discs[0].op_ctx = &discfp;
                discs[0].op_read = disc_do_read;
                discs[0].op_write = disc_do_write;
        }

        /* FIXME: Other files can be stored on SD too, such as logging
         * and NVRAM storage.
         *
         * We could also implement a menu here to select an image,
         * writing text to the framebuffer and checking kbd_queue_*()
         * for user input.
         */
        return;

no_sd:
#endif
        /* If we don't find (or look for) an SD-based image, attempt
         * to use in-flash disc image:
         */
        discs[0].base = (void *)umac_disc;
        discs[0].read_only = 1;
        discs[0].size = sizeof(umac_disc);
}

static void     core1_main()
{
        disc_descr_t discs[DISC_NUM_DRIVES] = {0};

        printf("Core 1 started\n");
        disc_setup(discs);

        umac_init(umac_ram, (void *)umac_rom, discs);
        /* Video runs on core 1, i.e. IRQs/DMA are unaffected by
         * core 0's USB activity.
         */
#if MIRROR_FRAMEBUFFER
        video_init((uint32_t *)(umac_framebuffer_mirror));
#else
        video_init((uint32_t *)(umac_ram + umac_get_fb_offset()));
#endif

#if ENABLE_AUDIO
        audio_base = (uint8_t*)umac_ram + umac_get_audio_offset();
#endif
        printf("Enjoyable Mac times now begin:\n\n");

        while (true) {
                poll_umac();
        }
}

size_t psram_size;

size_t _psram_size;
static void __no_inline_not_in_flash_func(setup_psram)(void) {
    _psram_size = 0;
#if USE_PSRAM
    gpio_set_function(PIN_PSRAM_CS, GPIO_FUNC_XIP_CS1);
    uint32_t save = save_and_disable_interrupts();
    // Try and read the PSRAM ID via direct_csr.
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB |
        QMI_DIRECT_CSR_EN_BITS;
    // Need to poll for the cooldown on the last XIP transfer to expire
    // (via direct-mode BUSY flag) before it is safe to perform the first
    // direct-mode operation
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
    }

    // Exit out of QMI in case we've inited already
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    // Transmit as quad.
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
        QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB |
        0xf5;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
    }
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

    // Read the id
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0;
    uint8_t eid = 0;
    for (size_t i = 0; i < 7; i++) {
        if (i == 0) {
            qmi_hw->direct_tx = 0x9f;
        } else {
            qmi_hw->direct_tx = 0xff;
        }
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {
        }
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
        }
        if (i == 5) {
            kgd = qmi_hw->direct_rx;
        } else if (i == 6) {
            eid = qmi_hw->direct_rx;
        } else {
            (void)qmi_hw->direct_rx;
        }
    }
    // Disable direct csr.
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    if (kgd != 0x5D) {
        restore_interrupts(save);
        return;
    }

    // Enable quad mode.
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB |
        QMI_DIRECT_CSR_EN_BITS;
    // Need to poll for the cooldown on the last XIP transfer to expire
    // (via direct-mode BUSY flag) before it is safe to perform the first
    // direct-mode operation
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
    }

    // RESETEN, RESET and quad enable
    for (uint8_t i = 0; i < 3; i++) {
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        if (i == 0) {
            qmi_hw->direct_tx = 0x66;
        } else if (i == 1) {
            qmi_hw->direct_tx = 0x99;
        } else {
            qmi_hw->direct_tx = 0x35;
        }
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
        }
        qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
        for (size_t j = 0; j < 20; j++) {
            asm ("nop");
        }
        (void)qmi_hw->direct_rx;
    }
    // Disable direct csr.
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    qmi_hw->m[1].timing =
        QMI_M0_TIMING_PAGEBREAK_VALUE_1024 << QMI_M0_TIMING_PAGEBREAK_LSB | // Break between pages.
            3 << QMI_M0_TIMING_SELECT_HOLD_LSB | // Delay releasing CS for 3 extra system cycles.
            1 << QMI_M0_TIMING_COOLDOWN_LSB |
            1 << QMI_M0_TIMING_RXDELAY_LSB |
            16 << QMI_M0_TIMING_MAX_SELECT_LSB | // In units of 64 system clock cycles. PSRAM says 8us max. 8 / 0.00752 / 64 = 16.62
            7 << QMI_M0_TIMING_MIN_DESELECT_LSB | // In units of system clock cycles. PSRAM says 50ns.50 / 7.52 = 6.64
            2 << QMI_M0_TIMING_CLKDIV_LSB;
    qmi_hw->m[1].rfmt = (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
            QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB |
            QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
            QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
            QMI_M0_RFMT_DUMMY_LEN_VALUE_24 << QMI_M0_RFMT_DUMMY_LEN_LSB |
            QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB |
            QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB |
            QMI_M0_RFMT_SUFFIX_LEN_VALUE_NONE << QMI_M0_RFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].rcmd = 0xeb << QMI_M0_RCMD_PREFIX_LSB |
        0 << QMI_M0_RCMD_SUFFIX_LSB;
    qmi_hw->m[1].wfmt = (QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
            QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB |
            QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
            QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
            QMI_M0_WFMT_DUMMY_LEN_VALUE_NONE << QMI_M0_WFMT_DUMMY_LEN_LSB |
            QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB |
            QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB |
            QMI_M0_WFMT_SUFFIX_LEN_VALUE_NONE << QMI_M0_WFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = 0x38 << QMI_M0_WCMD_PREFIX_LSB |
        0 << QMI_M0_WCMD_SUFFIX_LSB;

    restore_interrupts(save);

    _psram_size = 1024 * 1024; // 1 MiB
    uint8_t size_id = eid >> 5;
    if (eid == 0x26 || size_id == 2) {
        _psram_size *= 8;
    } else if (size_id == 0) {
        _psram_size *= 2;
    } else if (size_id == 1) {
        _psram_size *= 4;
    }

    // Mark that we can write to PSRAM.
    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;

    // Test write to the PSRAM.
    volatile uint32_t *psram_nocache = (volatile uint32_t *)0x15000000;
    psram_nocache[0] = 0x12345678;
    volatile uint32_t readback = psram_nocache[0];
    if (readback != 0x12345678) {
        _psram_size = 0;
        return;
    }
#endif
}

int     main()
{
        setup_psram();
#if OVERCLOCK
        overclock(CLK_SYS_264MHZ, 252000);
#endif
	stdio_init_all();

printf("psram size %u\n", _psram_size);

#ifndef RAM_TEST
#define RAM_TEST (0)
#endif

#if RAM_TEST
for(int pass=0; pass<10; pass++) {
    uint8_t acc = 1;
    for(int i=0; i < _psram_size; i++) {
        umac_ram[i] = acc;
        acc = acc * 13 + 21;
    }

    acc = 1;
    for(int i=0; i < _psram_size; i++) {
        uint8_t ri = umac_ram[i];
        if(ri != acc) {
            panic("[%08x] Stored %02x read %02x", i, acc, ri);
        }
        acc = acc * 13 + 21;
    }

    printf("ram test pass %d OK\n", pass);
}
#endif

        io_init();

#if ENABLE_AUDIO
        audio_setup();
#endif

        multicore_launch_core1(core1_main);

	printf("Starting, init usb\n");

        pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
        pio_cfg.tx_ch = 2;
        pio_cfg.pin_dp = PICO_DEFAULT_PIO_USB_DP_PIN;
        _Static_assert(PIN_USB_HOST_DP + 1 == PIN_USB_HOST_DM || PIN_USB_HOST_DP - 1 == PIN_USB_HOST_DM, "Permitted USB D+/D- configuration");
        pio_cfg.pinout = PIN_USB_HOST_DP + 1 == PIN_USB_HOST_DM ? PIO_USB_PINOUT_DPDM : PIO_USB_PINOUT_DMDP;

#ifdef PICO_DEFAULT_PIO_USB_VBUSEN_PIN
        gpio_init(PICO_DEFAULT_PIO_USB_VBUSEN_PIN);
        gpio_set_dir(PICO_DEFAULT_PIO_USB_VBUSEN_PIN, GPIO_OUT);
        gpio_put(PICO_DEFAULT_PIO_USB_VBUSEN_PIN, PICO_DEFAULT_PIO_USB_VBUSEN_STATE);
#endif

        tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
       
        tuh_init(BOARD_TUH_RHPORT);

        /* This happens on core 0: */
	while (true) {
                tuh_task();
                hid_app_task();
                poll_led_etc();
	}

	return 0;
}

#if ENABLE_AUDIO

#define I2C_ADDR 0x18

void writeRegister(uint8_t reg, uint8_t value) {
  char buf[2];
  buf[0] = reg;
  buf[1] = value;
  int res = i2c_write_timeout_us(i2c0, I2C_ADDR, buf, sizeof(buf), /* nostop */ false, 1000);
  if (res != 2) {
printf("res=%d\n", res);
    panic("i2c_write_timeout failed: res=%d\n", res);
  }
}

uint8_t readRegister(uint8_t reg) {
  char buf[1];
  buf[0] = reg;
  int res = i2c_write_timeout_us(i2c0, I2C_ADDR, buf, sizeof(buf), /* nostop */ true, 1000);
  if (res != 1) {
    panic("i2c_write_timeout failed: res=%d\n", res);
  }
  res = i2c_read_timeout_us(i2c0, I2C_ADDR, buf, sizeof(buf), /* nostop */ false, 1000);
  if (res != 1) {
    panic("i2c_read_timeout failed: res=%d\n", res);
  }
  uint8_t value = buf[0];
  return value;
}

void modifyRegister(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t current = readRegister(reg);
  uint8_t new_value = (current & ~mask) | (value & mask);
  writeRegister(reg, new_value);
}

void setPage(uint8_t page) {
  printf("Set page %d\n", page);
  writeRegister(0x00, page);
}


void Wire_begin() {
    i2c_init(i2c0, 100000);
    gpio_set_function(20, GPIO_FUNC_I2C);
    gpio_set_function(21, GPIO_FUNC_I2C);
}


static void setup_i2s_dac() {
gpio_init(22);
gpio_set_dir(22, true);
gpio_put(22, true); // allow i2s to come out of reset

  Wire_begin();
  sleep_ms(1000);
  
  printf("initialize codec\n");

  // Reset codec
  writeRegister(0x01, 0x01);
  sleep_ms(10);

  // Interface Control
  modifyRegister(0x1B, 0xC0, 0x00);
  modifyRegister(0x1B, 0x30, 0x00);

  // Clock MUX and PLL settings
  modifyRegister(0x04, 0x03, 0x03);
  modifyRegister(0x04, 0x0C, 0x04);
  
  writeRegister(0x06, 0x20); // PLL J
  writeRegister(0x08, 0x00); // PLL D LSB
  writeRegister(0x07, 0x00); // PLL D MSB
  
  modifyRegister(0x05, 0x0F, 0x02); // PLL P/R
  modifyRegister(0x05, 0x70, 0x10);

  // DAC/ADC Config
  modifyRegister(0x0B, 0x7F, 0x08); // NDAC
  modifyRegister(0x0B, 0x80, 0x80);
  
  modifyRegister(0x0C, 0x7F, 0x02); // MDAC
  modifyRegister(0x0C, 0x80, 0x80);
  
  modifyRegister(0x12, 0x7F, 0x08); // NADC
  modifyRegister(0x12, 0x80, 0x80);
  
  modifyRegister(0x13, 0x7F, 0x02); // MADC
  modifyRegister(0x13, 0x80, 0x80);

  // PLL Power Up
  modifyRegister(0x05, 0x80, 0x80);

  // Headset and GPIO Config
setPage(1);
modifyRegister(0x2e, 0xFF, 0x0b); 
setPage(0);
  modifyRegister(0x43, 0x80, 0x80); // Headset Detect
  modifyRegister(0x30, 0x80, 0x80); // INT1 Control
  modifyRegister(0x33, 0x3C, 0x14); // GPIO1


  // DAC Setup
  modifyRegister(0x3F, 0xC0, 0xC0);

  // DAC Routing
  setPage(1);
  modifyRegister(0x23, 0xC0, 0x40);
  modifyRegister(0x23, 0x0C, 0x04);

  // DAC Volume Control
  setPage(0);
  modifyRegister(0x40, 0x0C, 0x00);
  writeRegister(0x41, 0x28); // Left DAC Vol
  writeRegister(0x42, 0x28); // Right DAC Vol

  // ADC Setup
  modifyRegister(0x51, 0x80, 0x80);
  modifyRegister(0x52, 0x80, 0x00);
  writeRegister(0x53, 0x68); // ADC Volume

  // Headphone and Speaker Setup
  setPage(1);
  modifyRegister(0x1F, 0xC0, 0xC0); // HP Driver
  modifyRegister(0x28, 0x04, 0x04); // HP Left Gain
  modifyRegister(0x29, 0x04, 0x04); // HP Right Gain
  writeRegister(0x24, 0x0A);  // Left Analog HP
  writeRegister(0x25, 0x0A);  // Right Analog HP
  
  modifyRegister(0x28, 0x78, 0x40); // HP Left Gain
  modifyRegister(0x29, 0x78, 0x40); // HP Right Gain

  // Speaker Amp
  modifyRegister(0x20, 0x80, 0x80);
  modifyRegister(0x2A, 0x04, 0x04);
  modifyRegister(0x2A, 0x18, 0x08);
  writeRegister(0x26, 0x0A);

  // Return to page 0
  setPage(0);

  printf("Initialization complete!\n");
}
static int volscale;


#define SAMPLES_PER_BUFFER (370)
int16_t audio[SAMPLES_PER_BUFFER];

void umac_audio_trap() {
static int led_on;
                led_on ^= 1;
                gpio_put(GPIO_LED_PIN, 1);
    int32_t  offset = 128;
    uint16_t *audiodata = (uint16_t*)audio_base;
    int scale = volscale;
    if (!scale) {
        memset(audio, 0, sizeof(audio));
        return;
    }
    int16_t *stream = audio;
    for(int i=0; i<SAMPLES_PER_BUFFER; i++) {
        int32_t a = (*audiodata++ & 0xff) - offset; 
        a = (a * scale) >> 8;
        *stream++ = a;
    }
}

struct audio_buffer_pool *producer_pool;

static audio_format_t audio_format = {
        .format = AUDIO_BUFFER_FORMAT_PCM_S16,
        .sample_freq = 22256, // 60.15Hz*370, rounded up
        .channel_count = 1,
};

const struct audio_i2s_config config =
        {
            .data_pin = PICO_AUDIO_I2S_DATA_PIN,
            .clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE,
            .pio_sm = 0,
            .dma_channel = 3
        };

static struct audio_buffer_format producer_format = {
        .format = &audio_format,
        .sample_stride = 2
};

static void audio_setup() {
setup_i2s_dac();
    const struct audio_format *output_format = audio_i2s_setup(&audio_format, &config);
    assert(output_format);
    if (!output_format) {
        panic("PicoAudio: Unable to open audio device.\n");
    }
    producer_pool = audio_new_producer_pool(&producer_format, 3, SAMPLES_PER_BUFFER);
    assert(producer_pool);
    bool ok = audio_i2s_connect(producer_pool);
    assert(ok);
    audio_i2s_set_enabled(true);
}

static bool audio_poll() {
    audio_buffer_t *buffer = take_audio_buffer(producer_pool, false);
    if (!buffer) return false;
                gpio_put(GPIO_LED_PIN, 0);
    memcpy(buffer->buffer->bytes, audio, sizeof(audio));
    buffer->sample_count = SAMPLES_PER_BUFFER;
    give_audio_buffer(producer_pool, buffer);
    return true;
}

void umac_audio_cfg(int volume, int sndres) {
    volscale = sndres ? 0 : 65536 * volume / 7;
}
#endif
