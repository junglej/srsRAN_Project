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

// End-to-end test: SDAP TX entity tees a JPEG picture to the VART/U50 backend
// and verifies the ResNet50 Top-1 class index.
//
// Requirements to run this test:
//   - Xilinx Alveo U50 with XRT shell loaded
//   - xrmd (XRM) running on the host
//   - VART 1.4.1 environment (typically the xilinx/vitis-ai:1.4.1.978 Docker image)
//   - resnet50.xmodel and image.jpg available on the filesystem
//
// The test is skipped automatically if the configured model or image is missing.

#include "lib/sdap/sdap_entity_tx_impl.h"
#include "lib/sdap/sdap_tx_vart_notifier.h"
#include "srsran/sdap/sdap.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

using namespace srsran;
using namespace srs_cu_up;

namespace {

using vart_notifier_t = srs_cu_up::sdap_tx_vart_notifier;
using vart_config_t   = srs_cu_up::sdap_vart_config;

/// Downstream notifier that records PDUs forwarded by the SDAP entity.
class recording_notifier : public sdap_tx_pdu_notifier
{
public:
  size_t call_count = 0;

  void on_new_pdu(byte_buffer pdu) override
  {
    ++call_count;
    last_pdu_length = pdu.length();
  }

  size_t last_pdu_length = 0;
};

/// Read an entire file into a byte vector.
std::vector<uint8_t> read_file(const std::string& path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  file.seekg(0, std::ios::end);
  size_t size = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(size);
  file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
  return data;
}

std::string get_env_or_default(const char* var, const std::string& default_value)
{
  const char* val = std::getenv(var);
  return val != nullptr ? std::string(val) : default_value;
}

class sdap_vart_test : public ::testing::Test
{
protected:
  void SetUp() override
  {
    srslog::init();
    srslog::fetch_basic_logger("SDAP", false).set_level(srslog::basic_levels::warning);
    srslog::fetch_basic_logger("SDAP-VART", false).set_level(srslog::basic_levels::info);

    downstream = std::make_unique<recording_notifier>();

    model_path  = get_env_or_default("SDAP_VART_MODEL", "/models/resnet50/resnet50.xmodel");
    image_path  = get_env_or_default("SDAP_VART_IMAGE", "/models/image.jpg");
    xclbin_path = get_env_or_default("SDAP_VART_XCLBIN", "");
  }

  std::unique_ptr<recording_notifier> downstream;
  std::string                         model_path;
  std::string                         image_path;
  std::string                         xclbin_path;
};

} // namespace

TEST(sdap_vart_test_suite, resnet50_top1_is_samoyed)
{
  srslog::init();
  srslog::fetch_basic_logger("SDAP", false).set_level(srslog::basic_levels::warning);
  srslog::fetch_basic_logger("SDAP-VART", false).set_level(srslog::basic_levels::info);

  std::string model_path  = get_env_or_default("SDAP_VART_MODEL", "/models/resnet50/resnet50.xmodel");
  std::string image_path  = get_env_or_default("SDAP_VART_IMAGE", "/models/image.jpg");
  std::string xclbin_path = get_env_or_default("SDAP_VART_XCLBIN", "");

  auto image_bytes = read_file(image_path);
  if (image_bytes.empty()) {
    std::cerr << "SKIP: image not found: " << image_path << std::endl;
    GTEST_SKIP() << "image not found: " << image_path;
  }
  if (!std::ifstream(model_path).good()) {
    std::cerr << "SKIP: xmodel not found: " << model_path << std::endl;
    GTEST_SKIP() << "xmodel not found: " << model_path;
  }

  recording_notifier downstream;

  vart_config_t cfg{};
  cfg.xmodel_path = model_path;
  cfg.xclbin_path = xclbin_path;

  auto vart_notifier = std::make_unique<vart_notifier_t>(cfg);
  if (!vart_notifier->is_ready()) {
    std::cerr << "SKIP: VART notifier not ready: " << vart_notifier->get_last_error() << std::endl;
    GTEST_SKIP() << "VART notifier not ready: " << vart_notifier->get_last_error();
  }

  sdap_entity_tx_impl tx(/*ue_index*/ 1,
                         pdu_session_id_t::min,
                         qos_flow_id_t::min,
                         drb_id_t::drb1,
                         downstream,
                         vart_notifier.get());

  auto sdu = byte_buffer::create(span<const uint8_t>(image_bytes.data(), image_bytes.size()));
  ASSERT_TRUE(sdu.has_value()) << "failed to create byte_buffer from image bytes";

  tx.handle_sdu(std::move(sdu.value()));

  EXPECT_EQ(downstream.call_count, 1u) << "downstream must receive the original PDU";
  EXPECT_GT(downstream.last_pdu_length, 0u) << "downstream PDU length must be non-zero";

  EXPECT_EQ(vart_notifier->get_top1_class(), 258)
      << "ResNet50 Top-1 class must be Samoyed (idx=258), got error: " << vart_notifier->get_last_error();
  EXPECT_GT(vart_notifier->get_top1_score(), 0.0f) << "Top-1 score must be positive";
}
