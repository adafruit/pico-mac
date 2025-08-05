#include "clocking.h"

#include <stdio.h>
#include "pico.h"
#include "pico/stdio.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"

static void __no_inline_not_in_flash_func(set_qmi_timing)() {
    // Make sure flash is deselected - QMI doesn't appear to have a busy flag(!)
    while ((ioqspi_hw->io[1].status & IO_QSPI_GPIO_QSPI_SS_STATUS_OUTTOPAD_BITS) != IO_QSPI_GPIO_QSPI_SS_STATUS_OUTTOPAD_BITS)
        ;

    qmi_hw->m[0].timing = 0x40000202;
    //qmi_hw->m[0].timing = 0x40000101;
    // Force a read through XIP to ensure the timing is applied
    volatile uint32_t* ptr = (volatile uint32_t*)0x14000000;
    (void) *ptr;
}

#ifndef RP2350_PSRAM_MAX_SELECT_FS64
#define RP2350_PSRAM_MAX_SELECT_FS64 (125000000)
#endif

#ifndef RP2350_PSRAM_MIN_DESELECT_FS
#define RP2350_PSRAM_MIN_DESELECT_FS (50000000)
#endif

#ifndef RP2350_PSRAM_RX_DELAY_FS
#define RP2350_PSRAM_RX_DELAY_FS (3333333)
#endif

#ifndef RP2350_PSRAM_MAX_SCK_HZ
#define RP2350_PSRAM_MAX_SCK_HZ (133000000)
#endif

#define SEC_TO_FS 1000000000000000ll

static void __no_inline_not_in_flash_func(set_psram_timing)(void) {
    // Get secs / cycle for the system clock - get before disabling interrupts.
    uint32_t sysHz = (uint32_t)clock_get_hz(clk_sys);

    // Calculate the clock divider - goal to get clock used for PSRAM <= what
    // the PSRAM IC can handle - which is defined in RP2350_PSRAM_MAX_SCK_HZ
    volatile uint8_t clockDivider = (sysHz + RP2350_PSRAM_MAX_SCK_HZ - 1) / RP2350_PSRAM_MAX_SCK_HZ;

    uint32_t intr_stash = save_and_disable_interrupts();

    // Get the clock femto seconds per cycle.

    uint32_t fsPerCycle = SEC_TO_FS / sysHz;
    uint32_t fsPerHalfCycle = fsPerCycle / 2;

    // the maxSelect value is defined in units of 64 clock cycles
    // So maxFS / (64 * fsPerCycle) = maxSelect = RP2350_PSRAM_MAX_SELECT_FS64/fsPerCycle
    volatile uint8_t maxSelect = RP2350_PSRAM_MAX_SELECT_FS64 / fsPerCycle;

    //  minDeselect time - in system clock cycle
    // Must be higher than 50ns (min deselect time for PSRAM) so add a fsPerCycle - 1 to round up
    // So minFS/fsPerCycle = minDeselect = RP2350_PSRAM_MIN_DESELECT_FS/fsPerCycle

    volatile uint8_t minDeselect = (RP2350_PSRAM_MIN_DESELECT_FS + fsPerCycle - 1) / fsPerCycle;

    // RX delay (RP2350 datasheet 12.14.3.1) delay between between rising edge of SCK and
    // the start of RX sampling. Expressed in 0.5 system clock cycles. Smallest value
    // >= 3.3ns.
    volatile uint8_t rxDelay = (RP2350_PSRAM_RX_DELAY_FS + fsPerHalfCycle - 1) / fsPerHalfCycle;

    printf("syshz=%u\n", sysHz);
    printf("Max Select: %d, Min Deselect: %d, RX delay: %d, clock divider: %d\n", maxSelect, minDeselect, rxDelay, clockDivider);
    printf("PSRAM clock rate %.1fMHz\n", (float)sysHz / clockDivider / 1e6);

    qmi_hw->m[1].timing = QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB | // Break between pages.
                          3 << QMI_M1_TIMING_SELECT_HOLD_LSB | // Delay releasing CS for 3 extra system cycles.
                          rxDelay << QMI_M1_TIMING_RXDELAY_LSB | // Delay between SCK and RX sampling
                          1 << QMI_M1_TIMING_COOLDOWN_LSB | 1 << QMI_M1_TIMING_RXDELAY_LSB |
                          maxSelect << QMI_M1_TIMING_MAX_SELECT_LSB | minDeselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                          clockDivider << QMI_M1_TIMING_CLKDIV_LSB;

    restore_interrupts(intr_stash);
}


static void __no_inline_not_in_flash_func(clock_init)(int sys_clk_div) {
    uint32_t intr_stash = save_and_disable_interrupts();

    // Before messing with clock speeds ensure QSPI clock is nice and slow
    hw_write_masked(&qmi_hw->m[0].timing, 6, QMI_M0_TIMING_CLKDIV_BITS);

    // We're going to go fast, boost the voltage a little
    vreg_set_voltage(VREG_VOLTAGE_1_15);

    // Force a read through XIP to ensure the timing is applied before raising the clock rate
    volatile uint32_t* ptr = (volatile uint32_t*)0x14000000;
    (void) *ptr;

    // Before we touch PLLs, switch sys and ref cleanly away from their aux sources.
    hw_clear_bits(&clocks_hw->clk[clk_sys].ctrl, CLOCKS_CLK_SYS_CTRL_SRC_BITS);
    while (clocks_hw->clk[clk_sys].selected != 0x1)
        tight_loop_contents();
    hw_write_masked(&clocks_hw->clk[clk_ref].ctrl, CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC, CLOCKS_CLK_REF_CTRL_SRC_BITS);
    while (clocks_hw->clk[clk_ref].selected != 0x4)
        tight_loop_contents();

    // Stop the other clocks so we don't worry about overspeed
    clock_stop(clk_usb);
    clock_stop(clk_adc);
    clock_stop(clk_peri);
    clock_stop(clk_hstx);

    // Set USB PLL to 528MHz
    pll_init(pll_usb, PLL_COMMON_REFDIV, 1584 * MHZ, 3, 1);

    const uint32_t usb_pll_freq = 528 * MHZ;

    // CLK SYS = PLL USB 528MHz / sys_clk_div = 264MHz, 176MHz, or 132MHz
    clock_configure(clk_sys,
                    CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                    CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    usb_pll_freq, usb_pll_freq / sys_clk_div);

    // CLK PERI = PLL USB 528MHz / 4 = 132MHz
    clock_configure(clk_peri,
                    0, // Only AUX mux on ADC
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    usb_pll_freq, usb_pll_freq / 4);

    // CLK USB = PLL USB 528MHz / 11 = 48MHz
    clock_configure(clk_usb,
                    0, // No GLMUX
                    CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    usb_pll_freq,
                    USB_CLK_KHZ * KHZ);

    // CLK ADC = PLL USB 528MHz / 11 = 48MHz
    clock_configure(clk_adc,
                    0, // No GLMUX
                    CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    usb_pll_freq,
                    USB_CLK_KHZ * KHZ);

    // Now we are running fast set fast QSPI clock and read delay
    set_qmi_timing();

    restore_interrupts(intr_stash);
}

void overclock(enum clk_sys_speed clk_sys_div, uint32_t bit_clk_khz) {
    clock_init(clk_sys_div);
	stdio_init_all();
    set_psram_timing();
#define SHOW_CLK(i) printf("clk_get_hz(%s) -> %u\n", #i, clock_get_hz(i));
        SHOW_CLK(clk_ref);
        SHOW_CLK(clk_sys);
        SHOW_CLK(clk_peri);
        SHOW_CLK(clk_hstx);
        SHOW_CLK(clk_usb);
        SHOW_CLK(clk_adc);


    const uint32_t dvi_clock_khz = bit_clk_khz >> 1;
printf("bit_clk_khz = %u dvi_clock_khz = %u\n", bit_clk_khz, dvi_clock_khz);
    uint vco_freq, post_div1, post_div2;
    if (!check_sys_clock_khz(dvi_clock_khz, &vco_freq, &post_div1, &post_div2))
        panic("System clock of %u kHz cannot be exactly achieved", dvi_clock_khz);
    const uint32_t freq = vco_freq / (post_div1 * post_div2);

    // Set the sys PLL to the requested freq
    pll_init(pll_sys, PLL_COMMON_REFDIV, vco_freq, post_div1, post_div2);

    // CLK HSTX = Requested freq
    clock_configure(clk_hstx,
                    0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    freq, freq);
}
