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

#include "srsran/sdap/sdap_factory.h"
#include "sdap_entity_impl.h"
#include "sdap_tx_fpga_notifier.h"
#include "fmt/format.h"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#ifdef ENABLE_FPGA_VART
#include "sdap_tx_vart_notifier.h"
#endif

/// Notice this would be the only place were we include concrete class implementation files.

using namespace srsran;
using namespace srs_cu_up;

/// Factories are at a low level point of abstraction, as such, they are the "only" (best effort) objects that depend on
/// concrete class implementations instead of interfaces, intrinsically giving them tight coupling to the objects being
/// created. Keeping this coupling in a single file, is the best, as the rest of the code can be kept decoupled.

std::unique_ptr<sdap_entity> srsran::srs_cu_up::create_sdap(sdap_entity_creation_message& msg)
{
  return std::make_unique<sdap_entity_impl>(msg.ue_index, msg.pdu_session_id, *msg.rx_sdu_notifier, msg.fpga_offload);
}

sdap_tx_pdu_notifier* srsran::srs_cu_up::get_fpga_offload_notifier()
{
  // First-call initialisation, gated by env var. Constructed once and reused
  // for the whole process. Order of static destruction is fine: the notifier
  // closes /dev/pcie_fpga0 in its destructor.
  static std::once_flag                          init_once;
  static std::unique_ptr<sdap_tx_fpga_notifier> notifier;
  static bool                                    enabled = false;

  std::call_once(init_once, []() {
    const char* env = std::getenv("SRSRAN_FPGA_OFFLOAD");
    if (env == nullptr || std::strcmp(env, "1") != 0) {
      return;
    }
    sdap_fpga_config cfg{};
    notifier = std::make_unique<sdap_tx_fpga_notifier>(cfg);
    if (!notifier->is_ready()) {
      fmt::print("get_fpga_offload_notifier: notifier not ready, disabling FPGA path\n");
      notifier.reset();
      return;
    }
    enabled = true;
    fmt::print("get_fpga_offload_notifier: FPGA offload ENABLED via SRSRAN_FPGA_OFFLOAD=1\n");
  });

  return enabled ? notifier.get() : nullptr;
}

sdap_tx_pdu_notifier* srsran::srs_cu_up::get_vart_offload_notifier()
{
#ifdef ENABLE_FPGA_VART
  // First-call initialisation, gated by env var. Constructed once and reused
  // for the whole process.
  static std::once_flag                          init_once;
  static std::unique_ptr<sdap_tx_vart_notifier> notifier;
  static bool                                    enabled = false;

  std::call_once(init_once, []() {
    const char* env = std::getenv("SRSRAN_VART_OFFLOAD");
    if (env == nullptr || std::strcmp(env, "1") != 0) {
      return;
    }

    sdap_vart_config cfg{};
    cfg.xmodel_path = []() {
      const char* env_path = std::getenv("SDAP_VART_MODEL");
      return env_path != nullptr ? std::string(env_path) : std::string("/models/resnet50/resnet50.xmodel");
    }();
    cfg.xclbin_path = []() {
      const char* env_path = std::getenv("SDAP_VART_XCLBIN");
      return env_path != nullptr ? std::string(env_path) : std::string("");
    }();

    notifier = std::make_unique<sdap_tx_vart_notifier>(cfg);
    if (!notifier->is_ready()) {
      fmt::print("get_vart_offload_notifier: notifier not ready ({}), disabling VART path\n",
                 notifier->get_last_error());
      notifier.reset();
      return;
    }
    enabled = true;
    fmt::print("get_vart_offload_notifier: VART/U50 offload ENABLED via SRSRAN_VART_OFFLOAD=1\n");
  });

  return enabled ? notifier.get() : nullptr;
#else
  return nullptr;
#endif
}
