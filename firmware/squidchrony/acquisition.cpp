#include "acquisition.h"

#include <Arduino.h>
#include <hardware/adc.h>
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <hardware/sync.h>
#include <math.h>
#include <string.h>

#include "config.h"

#if defined(PICO_RP2040) && PICO_RP2040
#error "This sketch needs the RP2350 (Pico 2 W): it uses the DMA's self-retrigger mode."
#endif

namespace acq {
namespace {

constexpr uint32_t RING_N = 8192;  // ADC samples (radar and battery interleaved), 20 ms
constexpr uint32_t RING_BYTES = RING_N * sizeof(uint16_t);
constexpr uint32_t RING_BITS = 14;  // log2(RING_BYTES)
static_assert((1u << RING_BITS) == RING_BYTES, "ring size");
constexpr uint32_t DMA_BLOCK = 1024;  // samples per DMA interrupt (2.56 ms)
// RP2350 TRANS_COUNT mode bits: 0x1 = re-trigger itself when the count runs out.
constexpr uint32_t TRANS_COUNT_SELF_TRIGGER = 0x1u << 28;
// Longer than this between services and the ring may have lapped: start again.
constexpr uint32_t GAP_US = 15000;
constexpr uint32_t LEVEL_PAIRS = RAW_RATE / 10;  // battery/level averages over 0.1 s
constexpr uint32_t NBLK = HIST_N >> CLIP_BLOCK_BITS;

uint16_t ring[RING_N] __attribute__((aligned(RING_BYTES)));
int16_t hist[HIST_N];
uint8_t clip_blk[NBLK];
volatile uint32_t hist_head = 0;
volatile uint32_t gap_count = 0;

int dma_ch = -1;
uint32_t ridx = 0;
uint32_t last_us = 0;
bool running = false;
bool clip_now = false;
dsp::Decimator decim;

// Running sums for the 0.1 s levels
uint32_t lv_pairs = 0;
uint64_t batt_sum = 0;
int64_t rad_sum = 0;
uint64_t rad_sq = 0;
uint16_t rad_min = 4095, rad_max = 0;
uint32_t adc_err = 0;

// Published levels, seqlock-protected (odd = being written)
volatile uint32_t lv_seq = 0;
Levels lv_pub = {};

void publishLevels() {
  Levels l;
  l.battery_counts = (float)batt_sum / lv_pairs;
  l.radar_min = rad_min;
  l.radar_max = rad_max;
  float mean = (float)rad_sum / lv_pairs;
  float var = (float)rad_sq / lv_pairs - mean * mean;
  l.radar_mean = mean;
  l.radar_rms = var > 0.0f ? sqrtf(var) : 0.0f;
  l.adc_errors = adc_err;
  lv_seq = lv_seq + 1;
  __dmb();
  lv_pub = l;
  __dmb();
  lv_seq = lv_seq + 1;
  lv_pairs = 0;
  batt_sum = 0;
  rad_sum = 0;
  rad_sq = 0;
  rad_min = 4095;
  rad_max = 0;
}

void process() {
  uint32_t now = time_us_32();
  uint32_t w = dma_hw->ch[dma_ch].write_addr;
  uint32_t widx = ((w - (uint32_t)(uintptr_t)ring) >> 1) & (RING_N - 1) & ~1u;
  if (running && now - last_us > GAP_US) {
    // Held up long enough for the DMA to lap the ring: what is there is not contiguous.
    ridx = widx;
    decim.reset();
    clip_now = false;
    gap_count = gap_count + 1;
  }
  last_us = now;
  running = true;

  uint32_t head = hist_head;
  uint32_t avail = (widx - ridx) & (RING_N - 1);
  for (uint32_t n = 0; n < avail; n += 2) {
    uint16_t r = ring[ridx];
    uint16_t b = ring[ridx + 1];
    ridx = (ridx + 2) & (RING_N - 1);
    if ((r | b) & 0x8000) adc_err++;  // conversion error flag
    r &= 0x0FFF;
    b &= 0x0FFF;
    batt_sum += b;
    rad_sum += r;
    rad_sq += (uint32_t)r * r;
    if (r < rad_min) rad_min = r;
    if (r > rad_max) rad_max = r;
    if (r < CLIP_LO || r > CLIP_HI) clip_now = true;
    if (++lv_pairs >= LEVEL_PAIRS) publishLevels();

    int16_t y;
    if (decim.push((int16_t)((int)r - 2048), &y)) {
      hist[head & (HIST_N - 1)] = y;
      head++;
      if ((head & ((1u << CLIP_BLOCK_BITS) - 1)) == 0) {
        clip_blk[((head >> CLIP_BLOCK_BITS) - 1) & (NBLK - 1)] = clip_now ? 1 : 0;
        clip_now = false;
      }
    }
  }
  __dmb();  // samples land before the new head is visible
  hist_head = head;
}

void __not_in_flash_func(dmaIrq)() {
  uint32_t mask = 1u << dma_ch;
  if (!(dma_hw->ints1 & mask)) return;
  dma_hw->ints1 = mask;
  process();
}

}  // namespace

void begin() {
  adc_init();
  adc_gpio_init(PIN_RADAR_ADC);
  adc_gpio_init(PIN_BATT_ADC);
  adc_select_input(ADC_CH_RADAR);
  adc_set_round_robin((1u << ADC_CH_RADAR) | (1u << ADC_CH_BATT));
  adc_fifo_setup(true, true, 1, true, false);  // FIFO + DREQ, error flag in bit 15
  adc_set_clkdiv(48000000.0f / (2.0f * RAW_RATE) - 1.0f);  // 400 kS/s in total
  adc_fifo_drain();

  dma_ch = dma_claim_unused_channel(true);
  dma_channel_config c = dma_channel_get_default_config(dma_ch);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
  channel_config_set_read_increment(&c, false);
  channel_config_set_write_increment(&c, true);
  channel_config_set_ring(&c, true, RING_BITS);  // write address wraps inside the ring
  channel_config_set_dreq(&c, DREQ_ADC);
  dma_channel_configure(dma_ch, &c, ring, &adc_hw->fifo, TRANS_COUNT_SELF_TRIGGER | DMA_BLOCK,
                        false);

  dma_channel_set_irq1_enabled(dma_ch, true);
  irq_add_shared_handler(DMA_IRQ_1, dmaIrq, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
  irq_set_enabled(DMA_IRQ_1, true);

  ridx = 0;
  decim.reset();
  dma_channel_start(dma_ch);
  adc_run(true);  // first conversion is the radar channel, so radar samples sit at even indices
}

void service() {
  if (dma_ch < 0) return;
  uint32_t save = save_and_disable_interrupts();
  process();
  restore_interrupts(save);
}

uint32_t head() { return hist_head; }

uint32_t gaps() { return gap_count; }

bool read(uint32_t first, int count, int16_t* out) {
  uint32_t h = hist_head;
  __dmb();
  if (count <= 0) return true;
  if ((int32_t)(h - (first + (uint32_t)count)) < 0) return false;  // not written yet
  if (h - first > HIST_N - 1024) return false;                      // overwritten (or nearly)
  uint32_t i = first & (HIST_N - 1);
  uint32_t n1 = HIST_N - i;
  if (n1 >= (uint32_t)count) {
    memcpy(out, &hist[i], (size_t)count * sizeof(int16_t));
  } else {
    memcpy(out, &hist[i], n1 * sizeof(int16_t));
    memcpy(out + n1, &hist[0], ((uint32_t)count - n1) * sizeof(int16_t));
  }
  return true;
}

bool clippedBetween(uint32_t first, uint32_t last) {
  uint32_t h = hist_head;
  if (h - first > HIST_N - 1024) return false;
  uint32_t b0 = first >> CLIP_BLOCK_BITS, b1 = last >> CLIP_BLOCK_BITS;
  uint32_t open_blk = h >> CLIP_BLOCK_BITS;  // block still being filled
  for (uint32_t b = b0; b <= b1; b++) {
    if (b >= open_blk) return clip_now;
    if (clip_blk[b & (NBLK - 1)]) return true;
  }
  return false;
}

Levels levels() {
  Levels l = {};
  for (int tries = 0; tries < 100; tries++) {
    uint32_t s0 = lv_seq;
    __dmb();
    l = lv_pub;
    __dmb();
    if (!(s0 & 1) && s0 == lv_seq) break;
  }
  return l;
}

}  // namespace acq
