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

// Tests for the SDAP -> FPGA tee path.
//
// 1. Pure-software tests using a mock fpga_offload notifier: verify the tee
//    in sdap_entity_tx_impl::handle_sdu fires when fpga_offload != nullptr
//    and stays silent otherwise. No hardware required.
//
// 2. get_fpga_offload_notifier() behaviour: env-var gating, idempotent
//    singleton. Default-off when SRSRAN_FPGA_OFFLOAD is not set.
//
// 3. Optional hardware-touching smoke test gated on /dev/pcie_fpga0 being
//    available. Skipped automatically when the device is missing or the
//    bitstream is not loaded.

#include "lib/sdap/sdap_entity_tx_impl.h"
#include "lib/sdap/sdap_tx_fpga_notifier.h"
#include "srsran/sdap/sdap.h"
#include "srsran/sdap/sdap_factory.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <queue>
#include <sys/stat.h>

using namespace srsran;
using namespace srs_cu_up;

namespace {

/// Mock pdu_notifier that records every PDU it receives.
class recording_notifier : public sdap_tx_pdu_notifier
{
public:
  std::queue<byte_buffer> pdus;
  size_t                  call_count = 0;

  void on_new_pdu(byte_buffer pdu) override
  {
    pdus.push(std::move(pdu));
    ++call_count;
  }
};

/// Build a small byte_buffer with deterministic bytes.
byte_buffer make_pdu(size_t len, uint8_t base)
{
  std::vector<uint8_t> bytes(len);
  for (size_t i = 0; i < len; ++i) {
    bytes[i] = static_cast<uint8_t>(base + i);
  }
  return byte_buffer::create(bytes).value();
}

bool fpga_device_present()
{
  struct stat st{};
  return stat("/dev/pcie_fpga0", &st) == 0;
}

class sdap_fpga_tee_test : public ::testing::Test
{
protected:
  void SetUp() override
  {
    srslog::init();
    srslog::fetch_basic_logger("SDAP", false).set_level(srslog::basic_levels::warning);

    downstream = std::make_unique<recording_notifier>();
    fpga       = std::make_unique<recording_notifier>();
  }

  std::unique_ptr<recording_notifier> downstream;
  std::unique_ptr<recording_notifier> fpga;
};

// ---------------------------------------------------------------------------
// Unit tests: handle_sdu tee logic
// ---------------------------------------------------------------------------

TEST_F(sdap_fpga_tee_test, tee_disabled_when_fpga_offload_null)
{
  sdap_entity_tx_impl tx(/*ue_index*/ 1,
                         pdu_session_id_t::min,
                         qos_flow_id_t::min,
                         drb_id_t::drb1,
                         *downstream,
                         /*fpga_offload*/ nullptr);

  tx.handle_sdu(make_pdu(64, 0));
  tx.handle_sdu(make_pdu(32, 0x40));

  EXPECT_EQ(downstream->call_count, 2u) << "downstream must still receive every PDU";
  EXPECT_EQ(fpga->call_count, 0u) << "no fpga_offload pointer was passed, tee must stay silent";
}

TEST_F(sdap_fpga_tee_test, tee_fires_alongside_downstream)
{
  sdap_entity_tx_impl tx(/*ue_index*/ 2,
                         pdu_session_id_t::min,
                         qos_flow_id_t::min,
                         drb_id_t::drb1,
                         *downstream,
                         fpga.get());

  tx.handle_sdu(make_pdu(100, 0xAB));
  tx.handle_sdu(make_pdu(200, 0xCD));
  tx.handle_sdu(make_pdu(50, 0xEF));

  EXPECT_EQ(downstream->call_count, 3u);
  EXPECT_EQ(fpga->call_count, 3u) << "every SDU must also be mirrored to fpga_offload";

  // Downstream receives the move; FPGA receives a shallow copy. Both queues
  // should hold three entries with the same lengths.
  ASSERT_EQ(downstream->pdus.size(), 3u);
  ASSERT_EQ(fpga->pdus.size(), 3u);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(downstream->pdus.front().length(), fpga->pdus.front().length());
    downstream->pdus.pop();
    fpga->pdus.pop();
  }
}

TEST_F(sdap_fpga_tee_test, tee_does_not_block_downstream_on_fpga_drop)
{
  // Treat a notifier that immediately discards as a stand-in for an FPGA
  // notifier that timed out: downstream PDU flow must keep moving regardless.
  class dropping_notifier : public sdap_tx_pdu_notifier
  {
  public:
    void on_new_pdu(byte_buffer /*pdu*/) override { /* discarded */ }
  } dropper;

  sdap_entity_tx_impl tx(/*ue_index*/ 3,
                         pdu_session_id_t::min,
                         qos_flow_id_t::min,
                         drb_id_t::drb1,
                         *downstream,
                         &dropper);

  for (int i = 0; i < 16; ++i) {
    tx.handle_sdu(make_pdu(128, static_cast<uint8_t>(i)));
  }

  EXPECT_EQ(downstream->call_count, 16u) << "downstream must see all PDUs even when FPGA drops";
}

// ---------------------------------------------------------------------------
// get_fpga_offload_notifier() factory behaviour
// ---------------------------------------------------------------------------

TEST_F(sdap_fpga_tee_test, factory_returns_null_when_env_unset)
{
  // Make sure the env is clean. Note: the factory is a singleton so this test
  // only exercises the "unset" branch when run before any test that enables
  // it. We document the limitation here.
  if (std::getenv("SRSRAN_FPGA_OFFLOAD") != nullptr) {
    GTEST_SKIP() << "SRSRAN_FPGA_OFFLOAD is set in the environment; cannot test the unset branch";
  }

  sdap_tx_pdu_notifier* p1 = get_fpga_offload_notifier();
  sdap_tx_pdu_notifier* p2 = get_fpga_offload_notifier();
  EXPECT_EQ(p1, nullptr);
  EXPECT_EQ(p2, nullptr) << "factory should be idempotent (same answer each call)";
}

// ---------------------------------------------------------------------------
// Hardware-touching smoke test (skipped when /dev/pcie_fpga0 missing)
// ---------------------------------------------------------------------------

TEST_F(sdap_fpga_tee_test, hardware_smoke_one_pdu_increments_counter)
{
  if (!fpga_device_present()) {
    GTEST_SKIP() << "/dev/pcie_fpga0 not present, skipping hardware smoke";
  }

  sdap_fpga_config      cfg{};
  sdap_tx_fpga_notifier hw_notifier(cfg);
  if (!hw_notifier.is_ready()) {
    GTEST_SKIP() << "FPGA notifier failed to initialise (bitstream loaded? driver loaded? "
                    "permissions on /dev/pcie_fpga0?)";
  }

  auto timing_before = hw_notifier.get_last_timing();

  sdap_entity_tx_impl tx(/*ue_index*/ 4,
                         pdu_session_id_t::min,
                         qos_flow_id_t::min,
                         drb_id_t::drb1,
                         *downstream,
                         &hw_notifier);

  tx.handle_sdu(make_pdu(128, 0x55));

  // The notifier polled wait_batch_done internally before returning, so the
  // stat register should already be settled. Give the FPGA an extra ms of
  // slack just in case.
  usleep(1000);

  uint32_t packets_after = hw_notifier.get_total_packets_fpga();
  uint32_t bytes_after   = hw_notifier.get_total_bytes_fpga();
  auto     timing        = hw_notifier.get_last_timing();

  EXPECT_NE(timing.entry_cycles, timing_before.entry_cycles)
      << "FPGA entry timestamp should change after handle_sdu triggers a new batch";
  EXPECT_EQ(packets_after, 1u)
      << "FPGA total_packets reports the last completed batch, not a cumulative counter";
  EXPECT_EQ(bytes_after, cfg.slot_size)
      << "batch_engine reads one full configured slot for a single SDAP PDU";

  // Sanity: a real batch took at least 1 cycle.
  EXPECT_GT(timing.delta_cycles, 0u) << "FPGA delta cycles should be positive after a real batch";
}

} // namespace
