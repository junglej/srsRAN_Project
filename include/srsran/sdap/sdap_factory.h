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

#include "srsran/ran/cu_types.h"
#include "srsran/sdap/sdap.h"
#include "srsran/support/timers.h"
#include <memory>

/// This factory header file depends on the SDAP interfaces (see above include list). It is kept separate as clients of
/// the SDAP interfaces dont need to call factory methods.

namespace srsran {

namespace srs_cu_up {

struct sdap_entity_creation_message {
  uint32_t              ue_index;
  pdu_session_id_t      pdu_session_id;
  sdap_rx_sdu_notifier* rx_sdu_notifier;
  /// Optional FPGA offload tee target. When non-null, every DL SDU passed to
  /// the SDAP entity's TX path is also forwarded to this notifier (in
  /// addition to the per-DRB pdu_notifier supplied via add_mapping). nullptr
  /// keeps the FPGA path disabled, which is the production default.
  sdap_tx_pdu_notifier* fpga_offload = nullptr;
};

/// Creates an instance of a SDAP interface.
std::unique_ptr<sdap_entity> create_sdap(sdap_entity_creation_message& msg);

/// Returns a process-wide singleton FPGA offload notifier, or nullptr when
/// the FPGA path is disabled.
///
/// Gating:
///   - Returns nullptr unless the SRSRAN_FPGA_OFFLOAD env var is set to "1".
///   - First call constructs the notifier (opens /dev/pcie_fpga0, mmaps BAR0
///     and DMA buffer). If the device can't be opened, returns nullptr after
///     logging.
///   - Same pointer is returned across all subsequent calls.
///
/// Intended use: pdu_session_manager_impl plumbs the returned pointer into
/// every sdap_entity_creation_message::fpga_offload so that DL SDUs from any
/// UE/PDU session are tee'd to the FPGA in addition to the normal pipeline.
sdap_tx_pdu_notifier* get_fpga_offload_notifier();

/// Returns a process-wide singleton VART/U50 inference offload notifier, or
/// nullptr when the VART path is disabled.
///
/// Gating:
///   - Returns nullptr unless the SRSRAN_VART_OFFLOAD env var is set to "1".
///   - First call constructs the notifier (loads the ResNet50 xmodel and
///     creates a VART runner on the Alveo U50). If the model/device can't be
///     opened, returns nullptr after logging.
///   - Same pointer is returned across all subsequent calls.
///
/// Intended use: pdu_session_manager_impl plumbs the returned pointer into
/// every sdap_entity_creation_message::fpga_offload so that DL SDUs from any
/// UE/PDU session are tee'd to the U50 DPU for inference.
sdap_tx_pdu_notifier* get_vart_offload_notifier();

} // namespace srs_cu_up

} // namespace srsran
