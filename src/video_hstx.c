/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 Scott Shawcroft for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "stdlib.h"

// This is from: https://github.com/raspberrypi/pico-examples-rp2350/blob/a1/hstx/dvi_out_hstx_encoder/dvi_out_hstx_encoder.c

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"

// ----------------------------------------------------------------------------
// DVI constants

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define MODE_H_SYNC_POLARITY 0
#define MODE_H_FRONT_PORCH   16
#define MODE_H_SYNC_WIDTH    96
#define MODE_H_BACK_PORCH    48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_SYNC_POLARITY 0
#define MODE_V_FRONT_PORCH   10
#define MODE_V_SYNC_WIDTH    2
#define MODE_V_BACK_PORCH    33
#define MODE_V_ACTIVE_LINES  480

#define MODE_H_TOTAL_PIXELS ( \
    MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + \
    MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS \
    )
#define MODE_V_TOTAL_LINES  ( \
    MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
    MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES \
    )

#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

#define DO_BSWAP (1)
#if DO_BSWAP
#define BSWAP_MAYBE(x) (\
    ((x) & 0xff) << 24  \
|   (((x) >> 8) & 0xff) << 16  \
|   (((x) >> 16) & 0xff) << 8  \
|   (((x) >> 24) & 0xff) \
)
#else
#define BSWAP_MAYBE(x) (x)
#endif

// ----------------------------------------------------------------------------
// HSTX command lists

static uint32_t vblank_line_vsync_off[] = {
    BSWAP_MAYBE(HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH),
    BSWAP_MAYBE(SYNC_V1_H1),
    BSWAP_MAYBE(HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH),
    BSWAP_MAYBE(SYNC_V1_H0),
    BSWAP_MAYBE(HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS)),
    BSWAP_MAYBE(SYNC_V1_H1),
};

static uint32_t vblank_line_vsync_on[] = {
    BSWAP_MAYBE(HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH),
    BSWAP_MAYBE(SYNC_V0_H1),
    BSWAP_MAYBE(HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH),
    BSWAP_MAYBE(SYNC_V0_H0),
    BSWAP_MAYBE(HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS)),
    BSWAP_MAYBE(SYNC_V0_H1),
};

static uint32_t vactive_line[] = {
    BSWAP_MAYBE(HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH),
    BSWAP_MAYBE(SYNC_V1_H1),
    BSWAP_MAYBE(HSTX_CMD_NOP),
    BSWAP_MAYBE(HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH),
    BSWAP_MAYBE(SYNC_V1_H0),
    BSWAP_MAYBE(HSTX_CMD_NOP),
    BSWAP_MAYBE(HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH),
    BSWAP_MAYBE(SYNC_V1_H1),
    BSWAP_MAYBE(HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS),
};

typedef struct {
    uint32_t *dma_commands;
    size_t dma_commands_len; // in words
    uint8_t dma_pixel_channel;
    uint8_t dma_command_channel;
} picodvi_framebuffer_obj_t;

picodvi_framebuffer_obj_t *active_picodvi = NULL;
picodvi_framebuffer_obj_t picodvi;

static void __not_in_flash_func(dma_irq_handler)(void) {
    if (active_picodvi == NULL) {
        return;
    }
    uint ch_num = active_picodvi->dma_pixel_channel;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;

    // Set the read_addr back to the start and trigger the first transfer (which
    // will trigger the pixel channel).
    ch = &dma_hw->ch[active_picodvi->dma_command_channel];
    ch->al3_read_addr_trig = (uintptr_t)active_picodvi->dma_commands;
}

#define REAL_DISP_WIDTH 640
#define REAL_DISP_HEIGHT 480

void    video_init(uint32_t *framebuffer) {
    picodvi_framebuffer_obj_t *self = &picodvi;

    // We compute all DMA transfers needed for a single frame. This ensure we don't have any super
    // quick interrupts that we need to respond to. Each transfer takes two words, trans_count and
    // read_addr. Active pixel lines need two transfers due to different read addresses. When pixel
    // doubling, then we must also set transfer size.
    size_t dma_command_size = 2;
    self->dma_commands_len = (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH + 2 * MODE_V_ACTIVE_LINES + 1) * dma_command_size;
    self->dma_commands = (uint32_t *)malloc(self->dma_commands_len * sizeof(uint32_t));
    if (self->dma_commands == NULL) {
        return;
    }

    self->dma_pixel_channel = dma_claim_unused_channel(true);
    self->dma_command_channel = dma_claim_unused_channel(true);

    size_t pixels_per_word = 32;
    size_t words_per_line = REAL_DISP_WIDTH / pixels_per_word;
    uint8_t rot = 24; // 24 + color_depth;
    size_t shift_amount = 31; // color_depth % 32;

    size_t command_word = 0;
    size_t frontporch_start = MODE_V_TOTAL_LINES - MODE_V_FRONT_PORCH;
    size_t frontporch_end = frontporch_start + MODE_V_FRONT_PORCH;
    size_t vsync_start = 0;
    size_t vsync_end = vsync_start + MODE_V_SYNC_WIDTH;
    size_t backporch_start = vsync_end;
    size_t backporch_end = backporch_start + MODE_V_BACK_PORCH;
    size_t active_start = backporch_end;

    uint32_t dma_ctrl = (self->dma_command_channel << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB) |
        (DREQ_HSTX << DMA_CH0_CTRL_TRIG_TREQ_SEL_LSB) |
        DMA_CH0_CTRL_TRIG_IRQ_QUIET_BITS |
        DMA_CH0_CTRL_TRIG_INCR_READ_BITS |
        DMA_CH0_CTRL_TRIG_EN_BITS;
    uint32_t dma_pixel_ctrl = dma_ctrl | ((pixels_per_word == 32 ? DMA_SIZE_32 : DMA_SIZE_16) << DMA_CH0_CTRL_TRIG_DATA_SIZE_LSB);
#if DO_BSWAP
    dma_pixel_ctrl |= DMA_CH0_CTRL_TRIG_BSWAP_BITS;
#endif
    dma_ctrl |= (DMA_SIZE_32 << DMA_CH0_CTRL_TRIG_DATA_SIZE_LSB);

    uint32_t dma_write_addr = (uint32_t)&hstx_fifo_hw->fifo;
    // Write ctrl and write_addr once when not pixel doubling because they don't
    // change. (write_addr doesn't change when pixel doubling either but we need
    // to rewrite it because it is after the ctrl register.)
    dma_channel_hw_addr(self->dma_pixel_channel)->al1_ctrl = dma_pixel_ctrl;
    dma_channel_hw_addr(self->dma_pixel_channel)->al1_write_addr = dma_write_addr;

    for (size_t v_scanline = 0; v_scanline < MODE_V_TOTAL_LINES; v_scanline++) {
        if (vsync_start <= v_scanline && v_scanline < vsync_end) {
            self->dma_commands[command_word++] = count_of(vblank_line_vsync_on);
            self->dma_commands[command_word++] = (uintptr_t)vblank_line_vsync_on;
        } else if (backporch_start <= v_scanline && v_scanline < backporch_end) {
            self->dma_commands[command_word++] = count_of(vblank_line_vsync_off);
            self->dma_commands[command_word++] = (uintptr_t)vblank_line_vsync_off;
        } else if (frontporch_start <= v_scanline && v_scanline < frontporch_end) {
            self->dma_commands[command_word++] = count_of(vblank_line_vsync_off);
            self->dma_commands[command_word++] = (uintptr_t)vblank_line_vsync_off;
        } else {
            self->dma_commands[command_word++] = count_of(vactive_line);
            self->dma_commands[command_word++] = (uintptr_t)vactive_line;
            size_t row = v_scanline - active_start;
            size_t transfer_count = words_per_line;
            self->dma_commands[command_word++] = transfer_count;
            uintptr_t row_start = row * (REAL_DISP_WIDTH / 8) + (uintptr_t)framebuffer;
            self->dma_commands[command_word++] = row_start;
        }
    }
    // Last command is NULL which will trigger an IRQ.
    self->dma_commands[command_word++] = 0;
    self->dma_commands[command_word++] = 0;

    // B&W
    size_t color_depth = 1;
    hstx_ctrl_hw->expand_tmds =
        (color_depth - 1) << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
            rot << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
                (color_depth - 1) << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
            rot << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
                (color_depth - 1) << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
            rot << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
    size_t shifts_before_empty = (pixels_per_word % 32);

    // Pixels come in 32 bits at a time. color_depth dictates the number
    // of pixels per word. Control symbols (RAW) are an entire 32-bit word.
    hstx_ctrl_hw->expand_shift =
        (shifts_before_empty << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB) |
            (shift_amount << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB) |
            1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
            0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // Serial output config: clock period of 5 cycles, pop from command
    // expander every 5 cycles, shift the output shiftreg by 2 every cycle.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        (5u << HSTX_CTRL_CSR_CLKDIV_LSB) |
            (5u << HSTX_CTRL_CSR_N_SHIFTS_LSB) |
            (2u << HSTX_CTRL_CSR_SHIFT_LSB) |
            HSTX_CTRL_CSR_EN_BITS;

    // XXX this may be wrong, because pico-mac is using an overclock (but is it OC'ing HSTX? not sure)

    // Note we are leaving the HSTX clock at the SDK default of 125 MHz; since
    // we shift out two bits per HSTX clock cycle, this gives us an output of
    // 250 Mbps, which is very close to the bit clock for 480p 60Hz (252 MHz).
    // If we want the exact rate then we'll have to reconfigure PLLs.

#define HSTX_FIRST_PIN 12

    // Setup the data to pin mapping.
    hstx_ctrl_hw->bit[(HSTX_CKP    ) - HSTX_FIRST_PIN] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[(HSTX_CKP ^ 1) - HSTX_FIRST_PIN] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;

    const int pinout[] = { HSTX_D0P, HSTX_D1P, HSTX_D2P };

    for(uint lane = 0; lane < 3; lane++ ) {
        int bit = pinout[lane];

        uint32_t lane_data_sel_bits =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        // The two halves of each pair get identical data, but one pin is inverted.
        hstx_ctrl_hw->bit[(bit    ) - HSTX_FIRST_PIN] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[(bit ^ 1) - HSTX_FIRST_PIN] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    }

    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, 0); // HSTX
    }

    dma_channel_config c;
    c = dma_channel_get_default_config(self->dma_command_channel);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    // This wraps the transfer back to the start of the write address.
    size_t wrap = 3; // 8 bytes because we write two DMA registers.
    volatile uint32_t *write_addr = &dma_hw->ch[self->dma_pixel_channel].al3_transfer_count;
    channel_config_set_ring(&c, true, wrap);
    // No chain because we use an interrupt to reload this channel instead of a
    // third channel.
    dma_channel_configure(
        self->dma_command_channel,
        &c,
        write_addr,
        self->dma_commands,
        (1 << wrap) / sizeof(uint32_t),
        false
        );

    dma_hw->ints2 = (1u << self->dma_pixel_channel);
    dma_hw->inte2 = (1u << self->dma_pixel_channel);
    irq_set_exclusive_handler(DMA_IRQ_2, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_2, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    active_picodvi = self;

    dma_irq_handler();
}
