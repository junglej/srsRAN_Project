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

#include "sdap_tx_fpga_notifier.h"
#include "fmt/format.h"
#include <cerrno>
#include <cstring>

namespace srsran {
namespace srs_cu_up {

sdap_tx_fpga_notifier::sdap_tx_fpga_notifier(const sdap_fpga_config& cfg_) : cfg(cfg_)
{
  fd = open(cfg.dev_path, O_RDWR);
  if (fd < 0) {
    fmt::print("sdap_tx_fpga_notifier: failed to open {} (errno={})\n", cfg.dev_path, errno);
    return;
  }

  uint32_t bar_size = 0, dma_size = 0;
  if (ioctl(fd, PCIEFPGA_IOCTL_GET_BAR_SIZE, &bar_size) < 0 ||
      ioctl(fd, PCIEFPGA_IOCTL_GET_DMA_SIZE, &dma_size) < 0 ||
      ioctl(fd, PCIEFPGA_IOCTL_GET_DMA_ADDR, &dma_bus_addr) < 0) {
    fmt::print("sdap_tx_fpga_notifier: ioctl failed (errno={})\n", errno);
    close(fd);
    fd = -1;
    return;
  }
  bar_len = bar_size;
  dma_len = dma_size;

  // mmap BAR0
  regs = static_cast<volatile uint32_t*>(mmap(nullptr, bar_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (regs == MAP_FAILED) {
    fmt::print("sdap_tx_fpga_notifier: BAR0 mmap failed (errno={})\n", errno);
    close(fd);
    fd = -1;
    regs = nullptr;
    return;
  }

  // mmap DMA coherent buffer (driver puts it at offset == page_size)
  const int page_size = sysconf(_SC_PAGESIZE);
  dma_buf             = mmap(nullptr, dma_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page_size);
  if (dma_buf == MAP_FAILED) {
    fmt::print("sdap_tx_fpga_notifier: DMA buffer mmap failed (errno={})\n", errno);
    munmap((void*)regs, bar_len);
    regs = nullptr;
    close(fd);
    fd = -1;
    dma_buf = nullptr;
    return;
  }

  // ID register check
  uint32_t id = reg_read(BE_REG_ID_TYPE);
  if ((id & 0x00FFFFFFu) != 0x00800005u) {
    fmt::print("sdap_tx_fpga_notifier: unexpected FPGA ID 0x{:08x} "
               "(expected lower 24-bit 0x800005). FPGA may not be programmed yet.\n",
               id);
  } else {
    fmt::print("sdap_tx_fpga_notifier: ready. BAR0={} bytes, DMA={} bytes @ 0x{:016x}\n",
               bar_len,
               dma_len,
               dma_bus_addr);
  }

  // Pre-program the slot size; batch_engine reads it each trigger.
  reg_write(BE_REG_BATCH_SLOT_SIZE, cfg.slot_size);
}

sdap_tx_fpga_notifier::~sdap_tx_fpga_notifier()
{
  if (dma_buf != nullptr) {
    munmap(dma_buf, dma_len);
    dma_buf = nullptr;
  }
  if (regs != nullptr) {
    munmap((void*)regs, bar_len);
    regs = nullptr;
  }
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
}

void sdap_tx_fpga_notifier::on_new_pdu(byte_buffer pdu)
{
  send_one_slot(std::move(pdu), 0);
}

void sdap_tx_fpga_notifier::on_new_pdu_with_qfi(byte_buffer pdu, qos_flow_id_t qfi)
{
  send_one_slot(std::move(pdu), static_cast<uint32_t>(qfi));
}

void sdap_tx_fpga_notifier::send_one_slot(byte_buffer pdu, uint32_t qfi_value)
{
  if (!is_ready()) {
    return;
  }

  const uint32_t len         = static_cast<uint32_t>(pdu.length());
  const uint32_t max_payload = cfg.slot_size - sizeof(fpga_slot_header);
  if (len == 0 || len > max_payload) {
    if (len > max_payload) {
      fmt::print("sdap_tx_fpga_notifier: PDU too large ({} > {}), dropping\n", len, max_payload);
    }
    return;
  }

  std::lock_guard<std::mutex> lock(send_mutex);

  // Pack the SDU into slot 0 of the DMA buffer: 16-byte header + payload.
  auto* slot_ptr = static_cast<uint8_t*>(dma_buf);
  auto* header   = reinterpret_cast<fpga_slot_header*>(slot_ptr);
  auto* payload  = slot_ptr + sizeof(fpga_slot_header);

  copy_segments(pdu, span<uint8_t>(payload, len));

  header->valid    = 1;
  header->length   = len;
  header->qfi      = qfi_value;
  header->reserved = 0;

  const uint64_t result_addr          = dma_bus_addr + dma_len - 256;
  const uint32_t entry_before_trigger = reg_read(BE_REG_FPGA_TIME_ENTRY);

  // Trigger a count=1 batch: write base address, count, then trigger.
  reg_write(BE_REG_BATCH_BASE_LO, static_cast<uint32_t>(dma_bus_addr));
  reg_write(BE_REG_BATCH_BASE_HI, static_cast<uint32_t>(dma_bus_addr >> 32));
  reg_write(BE_REG_RESULT_BASE_LO, static_cast<uint32_t>(result_addr));
  reg_write(BE_REG_RESULT_BASE_HI, static_cast<uint32_t>(result_addr >> 32));
  reg_write(BE_REG_BATCH_COUNT, 1);
  reg_write(BE_REG_BATCH_TRIGGER, 1);

  if (wait_batch_done(entry_before_trigger)) {
    successful_batches.fetch_add(1, std::memory_order_relaxed);
  } else {
    fmt::print("sdap_tx_fpga_notifier: batch timeout/error (successful_batches={})\n",
               successful_batches.load(std::memory_order_relaxed));
  }
}

bool sdap_tx_fpga_notifier::wait_batch_done(uint32_t entry_before_trigger)
{
  const auto start = std::chrono::steady_clock::now();
  bool       seen_current_batch = false;
  while (true) {
    uint32_t status = reg_read(BE_REG_STATUS);
    uint32_t entry  = reg_read(BE_REG_FPGA_TIME_ENTRY);

    if ((status & BE_STATUS_BUSY) != 0 || entry != entry_before_trigger) {
      seen_current_batch = true;
    }
    if (seen_current_batch && (status & BE_STATUS_DONE) != 0) {
      return true;
    }
    if (seen_current_batch && (status & BE_STATUS_ERROR) != 0 && (status & BE_STATUS_BUSY) == 0) {
      return false;
    }
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    if (elapsed_ms > cfg.wait_timeout_ms) {
      return false;
    }
  }
}

uint32_t sdap_tx_fpga_notifier::get_total_packets_fpga() const
{
  if (!is_ready()) {
    return 0;
  }
  return reg_read(BE_REG_TOTAL_PACKETS);
}

uint32_t sdap_tx_fpga_notifier::get_total_bytes_fpga() const
{
  if (!is_ready()) {
    return 0;
  }
  return reg_read(BE_REG_TOTAL_BYTES);
}

sdap_tx_fpga_notifier::timing_snapshot sdap_tx_fpga_notifier::get_last_timing() const
{
  timing_snapshot snap{0, 0, 0};
  if (!is_ready()) {
    return snap;
  }
  snap.entry_cycles = reg_read(BE_REG_FPGA_TIME_ENTRY);
  snap.exit_cycles  = reg_read(BE_REG_FPGA_TIME_EXIT);
  snap.delta_cycles = reg_read(BE_REG_FPGA_TIME_DELTA);
  return snap;
}

} // namespace srs_cu_up
} // namespace srsran
