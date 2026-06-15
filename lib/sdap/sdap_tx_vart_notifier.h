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
#include <array>
#include <memory>
#include <string>

namespace srsran {
namespace srs_cu_up {

/// Configuration for the SDAP -> VART/U50 inference backend.
struct sdap_vart_config {
  /// Path to the XIR .xmodel file (e.g. resnet50.xmodel).
  std::string xmodel_path;
  /// Optional path to an .xclbin; when empty VART loads the default
  /// xclbin registered for the target device.
  std::string xclbin_path;
  /// Per-channel mean subtraction in BGR order (default matches the Vitis AI ResNet50 xmodel).
  std::array<float, 3> mean_bgr = {104.0f, 107.0f, 123.0f};
  /// Per-channel scale in BGR order, applied after mean subtraction (default: no scaling).
  std::array<float, 3> scale_bgr = {1.0f, 1.0f, 1.0f};
};

/// SDAP TX PDU notifier that sends the PDU payload through a Xilinx VART DPU runner.
///
/// The incoming byte_buffer is expected to contain a complete JPEG image. The notifier
/// decodes it, applies the ResNet50-style mean/scale preprocessing, runs inference on
/// the Alveo U50 and records the Top-1 class index.
class sdap_tx_vart_notifier final : public sdap_tx_pdu_notifier
{
public:
  explicit sdap_tx_vart_notifier(const sdap_vart_config& cfg);
  ~sdap_tx_vart_notifier();

  /// Called by sdap_entity_tx_impl for every SDU that is tee'd to the FPGA/VART path.
  void on_new_pdu(byte_buffer pdu) override;

  /// True when the xmodel was loaded and the DPU runner is ready.
  bool is_ready() const { return ready; }

  /// Top-1 class index from the last successful inference, or -1 if none.
  int get_top1_class() const { return top1_class; }

  /// Approximate Top-1 probability from the last successful inference.
  float get_top1_score() const { return top1_score; }

  /// Human-readable last error (empty when no error occurred).
  const std::string& get_last_error() const { return last_error; }

private:
  class impl;
  std::unique_ptr<impl> pimpl;

  bool        ready      = false;
  int         top1_class = -1;
  float       top1_score = 0.0f;
  std::string last_error;
};

} // namespace srs_cu_up
} // namespace srsran
