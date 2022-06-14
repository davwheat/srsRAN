/**
 * Copyright 2013-2022 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include <libbladeRF.h>
#include <string.h>
#include <unistd.h>

#include "rf_blade_imp.h"
#include "rf_plugin.h"
#include "srsran/phy/common/timestamp.h"
#include "srsran/phy/utils/debug.h"
#include "srsran/phy/utils/vector.h"

#define UNUSED __attribute__((unused))
#define CONVERT_BUFFER_SIZE (240 * 1024)
#define PRINT_RX_STATS 0
#define BLADERF_MAX_PORTS 2

cf_t zero_mem[CONVERT_BUFFER_SIZE * SRSRAN_MAX_PORTS];

typedef struct {
  struct bladerf*     dev;
  bladerf_sample_rate rx_rate;
  bladerf_sample_rate tx_rate;
  int16_t             rx_buffer[CONVERT_BUFFER_SIZE * SRSRAN_MAX_PORTS];
  int16_t             tx_buffer[CONVERT_BUFFER_SIZE * SRSRAN_MAX_PORTS];
  bool                rx_stream_enabled[BLADERF_MAX_PORTS];
  bool                tx_stream_enabled[BLADERF_MAX_PORTS];
  srsran_rf_info_t    info;
  uint32_t            nof_rx_channels;
  uint32_t            nof_tx_channels;
} rf_blade_handler_t;

static srsran_rf_error_handler_t blade_error_handler     = NULL;
static void*                     blade_error_handler_arg = NULL;

void rf_blade_suppress_stdout(UNUSED void* h)
{
  bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_SILENT);
}

void rf_blade_register_error_handler(UNUSED void* ptr, srsran_rf_error_handler_t new_handler, void* arg)
{
  blade_error_handler     = new_handler;
  blade_error_handler_arg = arg;
}

const unsigned int num_buffers       = 256;
const unsigned int ms_buffer_size_rx = 1024;
const unsigned int buffer_size_tx    = 1024;
const unsigned int num_transfers     = 32;
const unsigned int timeout_ms        = 4000;

const char* rf_blade_devname(UNUSED void* h)
{
  return DEVNAME;
}

int rf_blade_start_tx_stream(void* h)
{
  int                     status;
  rf_blade_handler_t*     handler = (rf_blade_handler_t*)h;
  bladerf_channel_layout  layout = BLADERF_TX_X1;

  if (handler->nof_tx_channels > 1) {
    layout = BLADERF_TX_X2;
  }

  status = bladerf_sync_config(handler->dev,
                               layout,
                               BLADERF_FORMAT_SC16_Q11_META,
                               num_buffers,
                               buffer_size_tx,
                               num_transfers,
                               timeout_ms);
  if (status != 0) {
    ERROR("Failed to configure TX sync interface: %s", bladerf_strerror(status));
    return status;
  }
  
  for (int ch = 0; ch < handler->nof_tx_channels; ch++) {
    status = bladerf_enable_module(handler->dev, BLADERF_CHANNEL_TX(ch), true);
    if (status != 0) {
      ERROR("Failed to enable TX module for port %d: %s", ch, bladerf_strerror(status));
      return status;
    }

    handler->tx_stream_enabled[ch] = true;
  }

  return 0;
}

int rf_blade_start_rx_stream(void* h, UNUSED bool now)
{
  int                     status;
  rf_blade_handler_t*     handler = (rf_blade_handler_t*)h;
  bladerf_channel_layout  rx_layout = BLADERF_RX_X1;
  bladerf_channel_layout  tx_layout = BLADERF_TX_X1;

  if (handler->nof_rx_channels > 1) {
    rx_layout = BLADERF_RX_X2;
  }
  if (handler->nof_tx_channels > 1) {
    tx_layout = BLADERF_TX_X2;
  }

  /* Configure the device's RX module for use with the sync interface.
   * SC16 Q11 samples *with* metadata are used. */
  uint32_t buffer_size_rx = ms_buffer_size_rx * (handler->rx_rate / 1000 / 1024);

  status = bladerf_sync_config(handler->dev,
                               rx_layout,
                               BLADERF_FORMAT_SC16_Q11_META,
                               num_buffers,
                               buffer_size_rx,
                               num_transfers,
                               timeout_ms);
  if (status != 0) {
    ERROR("Failed to configure RX sync interface: %s", bladerf_strerror(status));
    return status;
  }
  status = bladerf_sync_config(handler->dev,
                               tx_layout,
                               BLADERF_FORMAT_SC16_Q11_META,
                               num_buffers,
                               buffer_size_tx,
                               num_transfers,
                               timeout_ms);
  if (status != 0) {
    ERROR("Failed to configure TX sync interface: %s", bladerf_strerror(status));
    return status;
  }

  for (int ch = 0; ch < handler->nof_tx_channels; ch++) {
    status = bladerf_enable_module(handler->dev, BLADERF_CHANNEL_TX(ch), true);
    if (status != 0) {
      ERROR("Failed to enable TX module: %s", bladerf_strerror(status));
      return status;
    }

    handler->tx_stream_enabled[ch] = true;
  }

  for (int ch = 0; ch < handler->nof_rx_channels; ch++) {
    status = bladerf_enable_module(handler->dev, BLADERF_CHANNEL_RX(ch), true);
    if (status != 0) {
      ERROR("Failed to enable RX module: %s", bladerf_strerror(status));
      return status;
    }

    handler->rx_stream_enabled[ch] = true;
  }

  return SRSRAN_SUCCESS;
}

int rf_blade_stop_rx_stream(void* h)
{
  rf_blade_handler_t* handler = (rf_blade_handler_t*)h;
  int                 status  = 0;
  
  for (int ch = 0; ch < handler->nof_rx_channels; ch++) {
    status = bladerf_enable_module(handler->dev, BLADERF_CHANNEL_RX(ch), false);
    if (status != 0) {
      ERROR("Failed to enable RX module: %s", bladerf_strerror(status));
      return status;
    }

    handler->rx_stream_enabled[ch] = false;
  }

  for (int ch = 0; ch < handler->nof_tx_channels; ch++) {
    status = bladerf_enable_module(handler->dev, BLADERF_CHANNEL_TX(ch), false);
    if (status != 0) {
      ERROR("Failed to enable TX module: %s", bladerf_strerror(status));
      return status;
    }

    handler->tx_stream_enabled[ch] = false;
  }

  return SRSRAN_SUCCESS;
}

void rf_blade_flush_buffer(UNUSED void* h) {}

bool rf_blade_has_rssi(UNUSED void* h)
{
  return false;
}

float rf_blade_get_rssi(UNUSED void* h)
{
  return 0;
}

int rf_blade_open_multi(char* args, void** h, uint32_t nof_channels)
{
  if (nof_channels > 2) {
    ERROR("Error opening UHD: maximum number of channels exceeded (%d>%d)", nof_channels, 2);
    return SRSRAN_ERROR;
  }

  const struct bladerf_range* range_tx = NULL;
  const struct bladerf_range* range_rx = NULL;
  *h                                   = NULL;

  rf_blade_handler_t* handler = (rf_blade_handler_t*)malloc(sizeof(rf_blade_handler_t));
  if (!handler) {
    perror("malloc");
    return -1;
  }
  *h = handler;

  handler->nof_rx_channels = nof_channels;
  handler->nof_tx_channels = nof_channels;

  printf("Opening bladeRF...\n");
  int status = bladerf_open(&handler->dev, args);
  if (status) {
    ERROR("Unable to open device: %s", bladerf_strerror(status));
    goto clean_exit;
  }

  for (int ch = 0; ch < handler->nof_rx_channels; ch++) {
    status = bladerf_set_gain_mode(handler->dev, BLADERF_CHANNEL_RX(ch), BLADERF_GAIN_MGC);
    if (status) {
      ERROR("Unable to open device: %s", bladerf_strerror(status));
      goto clean_exit;
    }

    status = bladerf_get_gain_range(handler->dev, BLADERF_CHANNEL_RX(ch), &range_rx);
    if ((status != 0) || (range_rx == NULL)) {
      ERROR("Failed to get RX gain range: %s", bladerf_strerror(status));
      goto clean_exit;
    }

    status = bladerf_set_gain(handler->dev, BLADERF_CHANNEL_RX(ch), (bladerf_gain)range_rx->max);
    if (status != 0) {
      ERROR("Failed to set RX LNA gain: %s", bladerf_strerror(status));
      goto clean_exit;
    }

    handler->rx_stream_enabled[ch] = false;
  }

  // bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_VERBOSE);

  /* Get Gain ranges and set Rx to maximum */
  for (int ch = 0; ch < handler->nof_tx_channels; ch++) {
    status = bladerf_get_gain_range(handler->dev, BLADERF_CHANNEL_TX(ch), &range_tx);
    if ((status != 0) || (range_tx == NULL)) {
      ERROR("Failed to get TX gain range: %s", bladerf_strerror(status));
      goto clean_exit;
    }

    handler->tx_stream_enabled[ch] = false;
  }

  /* Set default sampling rates */
  rf_blade_set_tx_srate(handler, 1.92e6);
  rf_blade_set_rx_srate(handler, 1.92e6);

  /* Set info structure */
  handler->info.min_tx_gain = range_tx->min;
  handler->info.max_tx_gain = range_tx->max;
  handler->info.min_rx_gain = range_rx->min;
  handler->info.max_rx_gain = range_rx->max;

  return SRSRAN_SUCCESS;

clean_exit:
  free(handler);
  return status;
}

int rf_blade_open(char* args, void** h)
{
  return rf_blade_open_multi(args, h, 1);
}

int rf_blade_close(void* h)
{
  rf_blade_handler_t* handler = (rf_blade_handler_t*)h;
  bladerf_close(handler->dev);
  return 0;
}

double rf_blade_set_rx_srate(void* h, double freq)
{
  uint32_t            bw;
  rf_blade_handler_t* handler = (rf_blade_handler_t*)h;
  int                 status  = 0;

  for (int ch = 0; ch < handler->nof_rx_channels; ch++) {
    status = bladerf_set_sample_rate(handler->dev, BLADERF_CHANNEL_RX(ch), (uint32_t)freq, &handler->rx_rate);
    if (status != 0) {
      ERROR("Failed to set samplerate = %u: %s", (uint32_t)freq, bladerf_strerror(status));
      return -1;
    }

    if (handler->rx_rate < 2000000) {
      status = bladerf_set_bandwidth(handler->dev, BLADERF_CHANNEL_RX(ch), handler->rx_rate, &bw);
      if (status != 0) {
        ERROR("Failed to set bandwidth = %u: %s", handler->rx_rate, bladerf_strerror(status));
        return -1;
      }
    } else {
      status = bladerf_set_bandwidth(handler->dev, BLADERF_CHANNEL_RX(ch), (bladerf_bandwidth)(handler->rx_rate * 0.8), &bw);
      if (status != 0) {
        ERROR("Failed to set bandwidth = %u: %s", handler->rx_rate, bladerf_strerror(status));
        return -1;
      }
    }
  }

  printf("Set RX sampling rate %.2f Mhz, filter BW: %.2f Mhz\n", (float)handler->rx_rate / 1e6, (float)bw / 1e6);
  return (double)handler->rx_rate;
}

double rf_blade_set_tx_srate(void* h, double freq)
{
  uint32_t            bw;
  rf_blade_handler_t* handler = (rf_blade_handler_t*)h;
  int                 status  = 0;
  
  for (int ch = 0; ch < handler->nof_tx_channels; ch++) {
    status = bladerf_set_sample_rate(handler->dev, BLADERF_CHANNEL_TX(ch), (uint32_t)freq, &handler->tx_rate);
    if (status != 0) {
      ERROR("Failed to set samplerate = %u: %s", (uint32_t)freq, bladerf_strerror(status));
      return -1;
    }

    status = bladerf_set_bandwidth(handler->dev, BLADERF_CHANNEL_TX(ch), handler->tx_rate, &bw);
    if (status != 0) {
      ERROR("Failed to set bandwidth = %u: %s", handler->tx_rate, bladerf_strerror(status));
      return -1;
    }
  }

  return (double)handler->tx_rate;
}

int rf_blade_set_rx_gain(void* h, double gain)
{
  int                 status;
  rf_blade_handler_t* handler = (rf_blade_handler_t*)h;

  for (int ch = 0; ch < handler->nof_rx_channels; ch++) {
    status = bladerf_set_gain(handler->dev, BLADERF_CHANNEL_RX(ch), (bladerf_gain)gain);
    if (status != 0) {
      ERROR("Failed to set RX gain: %s", bladerf_strerror(status));
      return SRSRAN_ERROR;
    }
  }

  return SRSRAN_SUCCESS;
}

int rf_blade_set_rx_gain_ch(void* h, uint32_t ch, double gain)
{
  int                 status;
  rf_blade_handler_t* handler = (rf_blade_handler_t*)h;

  status = bladerf_set_gain(handler->dev, BLADERF_CHANNEL_RX(ch), (bladerf_gain)gain);
  if (status != 0) {
    ERROR("Failed to set RX gain: %s", bladerf_strerror(status));
    return SRSRAN_ERROR;
  }

  return SRSRAN_SUCCESS;
}

int rf_blade_set_tx_gain(void* h, double gain)
{
  int                 status;
  rf_blade_handler_t* handler = (rf_blade_handler_t*)h;

  for (int ch = 0; ch < handler->nof_tx_channels; ch++) {
    status = bladerf_set_gain(handler->dev, BLADERF_CHANNEL_TX(ch), (bladerf_gain)gain);
    if (status != 0) {
      ERROR("Failed to set TX gain: %s", bladerf_strerror(status));
      return SRSRAN_ERROR;
    }
  }

  return SRSRAN_SUCCESS;
}

int rf_blade_set_tx_gain_ch(void* h, uint32_t ch, double gain)
{
  int                 status;
  rf_blade_handler_t* handler = (rf_blade_handler_t*)h;
  
  status = bladerf_set_gain(handler->dev, BLADERF_CHANNEL_TX(ch), (bladerf_gain)gain);
  if (status != 0) {
    ERROR("Failed to set TX gain: %s", bladerf_strerror(status));
    return SRSRAN_ERROR;
  }

  return SRSRAN_SUCCESS;
}

double rf_blade_get_rx_gain(void* h)
{
  int                 status;
  bladerf_gain        gain    = 0;
  rf_blade_handler_t* handler = (rf_blade_handler_t*)h;

  status = bladerf_get_gain(handler->dev, BLADERF_CHANNEL_RX(0), &gain);
  if (status != 0) {
    ERROR("Failed to get RX gain for port 0: %s", bladerf_strerror(status));
    return -1;
  }
  return gain;
}

double rf_blade_get_tx_gain(void* h)
{
  int                 status;
  bladerf_gain        gain    = 0;
  rf_blade_handler_t* handler = (rf_blade_handler_t*)h;

  status = bladerf_get_gain(handler->dev, BLADERF_CHANNEL_TX(0), &gain);
  if (status != 0) {
    ERROR("Failed to get TX gain for port 0: %s", bladerf_strerror(status));
    return -1;
  }
  return gain;
}

srsran_rf_info_t* rf_blade_get_info(void* h)
{
  srsran_rf_info_t* info = NULL;

  if (h) {
    rf_blade_handler_t* handler = (rf_blade_handler_t*)h;

    info = &handler->info;
  }
  return info;
}

double rf_blade_set_rx_freq(void* h, uint32_t ch, double freq)
{
  rf_blade_handler_t* handler = (rf_blade_handler_t*)h;
  bladerf_frequency   f_int   = (uint32_t)round(freq);
  int                 status  = bladerf_set_frequency(handler->dev, BLADERF_CHANNEL_RX(ch), f_int);
  if (status != 0) {
    ERROR("Failed to set port %d samplerate = %u: %s", ch, (uint32_t)freq, bladerf_strerror(status));
    return -1;
  }
  f_int = 0;
  bladerf_get_frequency(handler->dev, BLADERF_CHANNEL_RX(ch), &f_int);
  printf("set RX frequency for channel %d to %lu\n", ch, f_int);

  return freq;
}

double rf_blade_set_tx_freq(void* h, uint32_t ch, double freq)
{
  rf_blade_handler_t* handler = (rf_blade_handler_t*)h;
  bladerf_frequency   f_int   = (uint32_t)round(freq);
  int                 status  = bladerf_set_frequency(handler->dev, BLADERF_CHANNEL_TX(ch), f_int);
  if (status != 0) {
    ERROR("Failed to set port %d samplerate = %u: %s", ch, (uint32_t)freq, bladerf_strerror(status));
    return -1;
  }

  f_int = 0;
  bladerf_get_frequency(handler->dev, BLADERF_CHANNEL_TX(ch), &f_int);
  printf("set TX frequency for channel %d to %lu\n", ch, f_int);
  return freq;
}

static void timestamp_to_secs(uint32_t rate, uint64_t timestamp, time_t* secs, double* frac_secs)
{
  double totalsecs = (double)timestamp / rate;
  time_t secs_i    = (time_t)totalsecs;
  if (secs) {
    *secs = secs_i;
  }
  if (frac_secs) {
    *frac_secs = totalsecs - secs_i;
  }
}

void rf_blade_get_time(void* h, time_t* secs, double* frac_secs)
{
  rf_blade_handler_t*     handler = (rf_blade_handler_t*)h;
  struct bladerf_metadata meta;

  int status = bladerf_get_timestamp(handler->dev, BLADERF_RX, &meta.timestamp);
  if (status != 0) {
    ERROR("Failed to get current RX timestamp: %s", bladerf_strerror(status));
  }
  timestamp_to_secs(handler->rx_rate, meta.timestamp, secs, frac_secs);
}

int rf_blade_recv_with_time_multi(void*    h,
                                  void**   data,
                                  uint32_t nsamples,
                                  bool     blocking,
                                  time_t*  secs,
                                  double*  frac_secs)
{
  rf_blade_handler_t*     handler = (rf_blade_handler_t*)h;
  struct bladerf_metadata meta;
  int                     status;

  #if PRINT_RX_STATS
    printf("rx: nsamples=%d\n", nsamples);
  #endif

  memset(&meta, 0, sizeof(meta));
  meta.flags = BLADERF_META_FLAG_RX_NOW;

  if (BLADERF_MAX_PORTS * nsamples > CONVERT_BUFFER_SIZE) {
    ERROR("RX failed: nsamples exceeds buffer size (%d>%d)", nsamples, CONVERT_BUFFER_SIZE);
    return -1;
  }

  status = bladerf_sync_rx(handler->dev, handler->rx_buffer, nsamples, &meta, 2000);
  if (status) {
    ERROR("RX failed: %s; nsamples=%d;", bladerf_strerror(status), nsamples);
    return -1;
  } else if (meta.status & BLADERF_META_STATUS_OVERRUN) {
    if (blade_error_handler) {
      srsran_rf_error_t error;
      error.opt  = meta.actual_count;
      error.type = SRSRAN_RF_ERROR_OVERFLOW;
      blade_error_handler(blade_error_handler_arg, error);
    } else {
      ERROR("Overrun detected in scheduled RX. "
            "%u valid samples were read.", meta.actual_count);
    }
  }

  timestamp_to_secs(handler->rx_rate, meta.timestamp, secs, frac_secs);

  bladerf_channel_layout rx_layout = BLADERF_RX_X1;
  bladerf_channel_layout tx_layout = BLADERF_TX_X1;

  if (handler->nof_rx_channels > 1) {
    rx_layout = BLADERF_RX_X2;
  }
  if (handler->nof_tx_channels > 1) {
    tx_layout = BLADERF_TX_X2;
  }

  float tempBuf[2 * nsamples];
  memset(tempBuf, 0, sizeof tempBuf);

  bladerf_deinterleave_stream_buffer(rx_layout, BLADERF_FORMAT_SC16_Q11_META, 2 * nsamples, handler->rx_buffer);
  srsran_vec_convert_if(handler->rx_buffer, 2048, tempBuf, 2 * nsamples);

  // split interleaved buffer into separate buffers per channel
  for (int i = 0; i < 2 * nsamples; i++) {
    float* channelBuf = data[i >= nsamples / 2 ? 1 : 0];

    int possibleIndex = i - nsamples / 2;
    if (possibleIndex < 0) possibleIndex = i;

    channelBuf[possibleIndex] = tempBuf[i];
  }

  return nsamples;
}

int rf_blade_recv_with_time(void*       h,
                            void*       data,
                            uint32_t    nsamples,
                            UNUSED bool blocking,
                            time_t*     secs,
                            double*     frac_secs)
{
  void* data_multi[BLADERF_MAX_PORTS] = {NULL};
  data_multi[0]                      = data;

  return rf_blade_recv_with_time_multi(h, data_multi, nsamples, false, secs, frac_secs);
}

int rf_blade_send_timed_multi(void*  h,
                              void*  data[4],
                              int    nsamples,
                              time_t secs,
                              double frac_secs,
                              bool   has_time_spec,
                              bool   blocking,
                              bool   is_start_of_burst,
                              bool   is_end_of_burst)
{
  rf_blade_handler_t*     handler = (rf_blade_handler_t*)h;
  struct bladerf_metadata meta;
  int                     status;

  for (int ch = 0; ch < handler->nof_tx_channels; ch++) {
    if (!handler->tx_stream_enabled[ch]) {
      rf_blade_start_tx_stream(h);
    }
  }

  if (BLADERF_MAX_PORTS * nsamples > CONVERT_BUFFER_SIZE) {
    ERROR("TX failed: nsamples exceeds buffer size (%d>%d)", nsamples, CONVERT_BUFFER_SIZE);
    return -1;
  }

  bladerf_channel_layout rx_layout = BLADERF_RX_X1;
  bladerf_channel_layout tx_layout = BLADERF_TX_X1;

  if (handler->nof_rx_channels > 1) {
    rx_layout = BLADERF_RX_X2;
  }
  if (handler->nof_tx_channels > 1) {
    tx_layout = BLADERF_TX_X2;
  }

  float interleavedBuffer[2 * nsamples];
  memset(&interleavedBuffer, 0, sizeof interleavedBuffer);

  // convert split channel buffer into split buffer
  for (int i = 0; i < 2 * nsamples; i++) {
    float* sourceArr = data[i % 2];
    interleavedBuffer[(nsamples * (i % 2)) + (i / 2)] = sourceArr[i / 2];
  }

  srsran_vec_convert_fi(interleavedBuffer, 2048, handler->tx_buffer, 2 * nsamples);

  bladerf_interleave_stream_buffer(tx_layout, BLADERF_FORMAT_SC16_Q11_META, 2 * nsamples, handler->tx_buffer);

  memset(&meta, 0, sizeof(meta));
  if (is_start_of_burst) {
    if (has_time_spec) {
      // Convert time to ticks
      srsran_timestamp_t ts = {.full_secs = secs, .frac_secs = frac_secs};
      meta.timestamp        = srsran_timestamp_uint64(&ts, handler->tx_rate);
    } else {
      meta.flags |= BLADERF_META_FLAG_TX_NOW;
    }
    meta.flags |= BLADERF_META_FLAG_TX_BURST_START;
  }
  if (is_end_of_burst) {
    meta.flags |= BLADERF_META_FLAG_TX_BURST_END;
  }
  srsran_rf_error_t error;
  bzero(&error, sizeof(srsran_rf_error_t));

  status = bladerf_sync_tx(handler->dev, handler->tx_buffer, nsamples, &meta, 2000);
  if (status == BLADERF_ERR_TIME_PAST) {
    if (blade_error_handler) {
      error.type = SRSRAN_RF_ERROR_LATE;
      blade_error_handler(blade_error_handler_arg, error);
    } else {
      ERROR("TX failed: %s", bladerf_strerror(status));
    }
  } else if (status) {
    ERROR("TX failed: %s", bladerf_strerror(status));
    return status;
  } else if (meta.status == BLADERF_META_STATUS_UNDERRUN) {
    if (blade_error_handler) {
      error.type = SRSRAN_RF_ERROR_UNDERFLOW;
      blade_error_handler(blade_error_handler_arg, error);
    } else {
      ERROR("TX warning: underflow detected.");
    }
  }

  return nsamples;
}

int rf_blade_send_timed(void*       h,
                        void*       data,
                        int         nsamples,
                        time_t      secs,
                        double      frac_secs,
                        bool        has_time_spec,
                        UNUSED bool blocking,
                        bool        is_start_of_burst,
                        bool        is_end_of_burst)
{
  void* _data[SRSRAN_MAX_PORTS] = {data, zero_mem, zero_mem, zero_mem};

  return rf_blade_send_timed_multi(h, _data, nsamples, secs, frac_secs, has_time_spec, false, is_start_of_burst, is_end_of_burst);
}

rf_dev_t srsran_rf_dev_blade = {"bladeRF",
                                rf_blade_devname,
                                rf_blade_start_rx_stream,
                                rf_blade_stop_rx_stream,
                                rf_blade_flush_buffer,
                                rf_blade_has_rssi,
                                rf_blade_get_rssi,
                                rf_blade_suppress_stdout,
                                rf_blade_register_error_handler,
                                rf_blade_open,
                                .srsran_rf_open_multi = rf_blade_open_multi,
                                rf_blade_close,
                                rf_blade_set_rx_srate,
                                rf_blade_set_rx_gain,
                                rf_blade_set_rx_gain_ch,
                                rf_blade_set_tx_gain,
                                rf_blade_set_tx_gain_ch,
                                rf_blade_get_rx_gain,
                                rf_blade_get_tx_gain,
                                rf_blade_get_info,
                                rf_blade_set_rx_freq,
                                rf_blade_set_tx_srate,
                                rf_blade_set_tx_freq,
                                rf_blade_get_time,
                                NULL,
                                rf_blade_recv_with_time,
                                rf_blade_recv_with_time_multi,
                                rf_blade_send_timed,
                                .srsran_rf_send_timed_multi = rf_blade_send_timed_multi};

#ifdef ENABLE_RF_PLUGINS
int register_plugin(rf_dev_t** rf_api)
{
  if (rf_api == NULL) {
    return SRSRAN_ERROR;
  }
  *rf_api = &srsran_rf_dev_blade;
  return SRSRAN_SUCCESS;
}
#endif /* ENABLE_RF_PLUGINS */
