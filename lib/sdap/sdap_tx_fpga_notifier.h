/*
 *
 * Copyright 2021-2026 Software Radio Systems Limited
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

#pragma once

#include "srsran/sdap/sdap.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <mutex>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace srsran {
namespace srs_cu_up {

/* ioctl commands (must match pcie_fpga driver) */
#define PCIEFPGA_IOCTL_MAGIC        'P'
#define PCIEFPGA_IOCTL_GET_DMA_ADDR _IOR(PCIEFPGA_IOCTL_MAGIC, 0, uint64_t)
#define PCIEFPGA_IOCTL_GET_BAR_SIZE _IOR(PCIEFPGA_IOCTL_MAGIC, 1, uint32_t)
#define PCIEFPGA_IOCTL_GET_DMA_SIZE _IOR(PCIEFPGA_IOCTL_MAGIC, 2, uint32_t)

/* BAR0 register offsets (must match batch_engine.v, corundum variant) */
#define BE_REG_ID_TYPE         0x000
#define BE_REG_VERSION         0x004
#define BE_REG_CONTROL         0x00C
#define BE_REG_STATUS          0x010
#define BE_REG_BATCH_BASE_LO   0x040
#define BE_REG_BATCH_BASE_HI   0x044
#define BE_REG_BATCH_SLOT_SIZE 0x048
#define BE_REG_BATCH_COUNT     0x04C
#define BE_REG_BATCH_TRIGGER   0x050
#define BE_REG_BATCH_CUR_SLOT  0x054
#define BE_REG_TOTAL_PACKETS   0x060
#define BE_REG_TOTAL_BYTES     0x064
#define BE_REG_RESULT_BASE_LO  0x070
#define BE_REG_RESULT_BASE_HI  0x074
#define BE_REG_FPGA_TIME_ENTRY 0x078
#define BE_REG_FPGA_TIME_EXIT  0x07C
#define BE_REG_FPGA_TIME_DELTA 0x080

#define BE_STATUS_BUSY (1u << 0)
#define BE_STATUS_DONE (1u << 1)
#define BE_STATUS_ERROR (1u << 2)

/* Slot header layout (16 bytes, must match batch_engine.v) */
struct alignas(16) fpga_slot_header {
  uint32_t valid;   // 1 = valid slot
  uint32_t length;  // payload length in bytes
  uint32_t qfi;     // SDAP QoS Flow ID (informational; FPGA ignores)
  uint32_t reserved;
};

/// Configuration for the FPGA batch transfer.
struct sdap_fpga_config {
  /// Path to the PCIe device (e.g., "/dev/pcie_fpga0").
  const char* dev_path = "/dev/pcie_fpga0";
  /// Slot size in bytes. Must match the FPGA's max payload (default 4096
  /// is the conservative bound; batch_engine.v reads slot_size from BAR0
  /// at trigger time).
  uint32_t slot_size = 4096;
  /// Per-batch guard timeout. The notifier sends one slot per SDU and
  /// serializes calls with send_mutex.
  uint32_t wait_timeout_ms = 50;
};

/// SDAP TX PDU notifier that offloads PDUs to FPGA via PCIe batch DMA.
///
/// One SDU == one batch (slot_count=1). The FPGA receives the data via
/// DMA pull triggered by writing the trigger register. Counters at
/// 0x060/0x064 and timing at 0x078/0x07C/0x080 confirm the transfer.
class sdap_tx_fpga_notifier : public sdap_tx_pdu_notifier
{
public:
  explicit sdap_tx_fpga_notifier(const sdap_fpga_config& cfg_);
  ~sdap_tx_fpga_notifier() override;

  // Non-copyable, non-movable.
  sdap_tx_fpga_notifier(const sdap_tx_fpga_notifier&)            = delete;
  sdap_tx_fpga_notifier& operator=(const sdap_tx_fpga_notifier&) = delete;

  /// Core interface: receive SDAP PDU, push to FPGA via single-slot batch.
  void on_new_pdu(byte_buffer pdu) override;

  /// Same as on_new_pdu but stamps QFI into the slot header (informational).
  void on_new_pdu_with_qfi(byte_buffer pdu, qos_flow_id_t qfi);

  /// Check if device is ready.
  bool is_ready() const { return fd >= 0 && dma_buf != nullptr && regs != nullptr; }

  /// Stats from the FPGA (BAR0 reads).
  uint32_t get_total_packets_fpga() const;
  uint32_t get_total_bytes_fpga() const;

  struct timing_snapshot {
    uint32_t entry_cycles;
    uint32_t exit_cycles;
    uint32_t delta_cycles;
  };
  timing_snapshot get_last_timing() const;

private:
  /// Push exactly one SDU into slot 0, trigger a count=1 batch, wait done.
  void send_one_slot(byte_buffer pdu, uint32_t qfi_value);

  /// Poll STATUS.DONE up to wait_timeout_ms. The entry timestamp from before
  /// trigger is used to reject stale DONE from the previous batch.
  bool wait_batch_done(uint32_t entry_before_trigger);

  /// Write a 32-bit value to BAR0 register.
  inline void reg_write(uint32_t offset, uint32_t value) { regs[offset / 4] = value; }

  /// Read a 32-bit value from BAR0 register.
  inline uint32_t reg_read(uint32_t offset) const { return regs[offset / 4]; }

  const sdap_fpga_config cfg;

  int                fd           = -1;
  volatile uint32_t* regs         = nullptr;
  void*              dma_buf      = nullptr;
  uint64_t           dma_bus_addr = 0;
  size_t             dma_len      = 0;
  size_t             bar_len      = 0;

  /// Count of successful FPGA batches since startup. Use this as a quick
  /// sanity check ("did the first SDU make it through").
  std::atomic<uint32_t> successful_batches{0};

  /// Serialise so multiple sdap_entity_tx_impl instances do not interleave
  /// slot writes on the single shared notifier.
  mutable std::mutex send_mutex;
};

} // namespace srs_cu_up
} // namespace srsran
