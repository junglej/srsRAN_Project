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

#include "sdap_tx_vart_notifier.h"
#include "srsran/adt/byte_buffer.h"
#include "srsran/srslog/srslog.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <vart/runner.hpp>
#include <vart/runner_helper.hpp>
#include <xir/graph/graph.hpp>
#include <xir/tensor/tensor.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

using namespace srsran;
using namespace srs_cu_up;

namespace {

const xir::Subgraph* find_dpu_subgraph(const xir::Graph* graph)
{
  auto root = graph->get_root_subgraph();
  if (root->is_leaf()) {
    return nullptr;
  }
  auto children = root->children_topological_sort();
  for (auto* c : children) {
    if (c->has_attr("device") && c->get_attr<std::string>("device") == "DPU") {
      return c;
    }
  }
  return nullptr;
}

} // namespace

class sdap_tx_vart_notifier::impl
{
public:
  explicit impl(const sdap_vart_config& cfg) : cfg_(cfg) {}

  bool init(std::string& error)
  {
    try {
      graph_ = xir::Graph::deserialize(cfg_.xmodel_path);
      if (!graph_) {
        error = "failed to deserialize xmodel: " + cfg_.xmodel_path;
        return false;
      }

      auto* subgraph = find_dpu_subgraph(graph_.get());
      if (subgraph == nullptr) {
        error = "no DPU subgraph found in " + cfg_.xmodel_path;
        return false;
      }

      runner_ = vart::Runner::create_runner(subgraph, "run");
      if (!runner_) {
        error = "failed to create VART runner";
        return false;
      }

      auto input_tensors  = runner_->get_input_tensors();
      auto output_tensors = runner_->get_output_tensors();
      if (input_tensors.empty() || output_tensors.empty()) {
        error = "model has no input/output tensors";
        return false;
      }
      input_tensor_  = input_tensors[0];
      output_tensor_ = output_tensors[0];

      input_buffer_  = vart::alloc_cpu_flat_tensor_buffer(input_tensor_);
      output_buffer_ = vart::alloc_cpu_flat_tensor_buffer(output_tensor_);

      input_shape_  = input_tensor_->get_shape();
      output_shape_ = output_tensor_->get_shape();

      if (input_shape_.size() != 4) {
        error = "unsupported input tensor rank: " + std::to_string(input_shape_.size());
        return false;
      }

      batch_    = input_shape_[0];
      height_   = input_shape_[1];
      width_    = input_shape_[2];
      channels_ = input_shape_[3];

      input_fix_point_ = input_tensor_->get_attr<int32_t>("fix_point");
      input_scale_     = std::pow(2.0f, static_cast<float>(input_fix_point_));

      output_fix_point_ = output_tensor_->get_attr<int32_t>("fix_point");
      output_scale_     = 1.0f / std::pow(2.0f, static_cast<float>(output_fix_point_));

      input_size_  = std::accumulate(input_shape_.begin(), input_shape_.end(), 1, std::multiplies<int32_t>());
      output_size_ = std::accumulate(output_shape_.begin(), output_shape_.end(), 1, std::multiplies<int32_t>());

      logger = &srslog::fetch_basic_logger("SDAP-VART", false);
      logger->info("VART backend ready: xmodel={}, input_shape=[{}x{}x{}x{}], fix_point={}",
                   cfg_.xmodel_path,
                   batch_,
                   height_,
                   width_,
                   channels_,
                   input_fix_point_);
      return true;
    } catch (const std::exception& e) {
      error = std::string("VART init exception: ") + e.what();
      return false;
    }
  }

  bool run_inference(const byte_buffer& pdu, int& top1_class, float& top1_score, std::string& error)
  {
    try {
      // Flatten the (possibly segmented) byte_buffer into a contiguous JPEG buffer.
      std::vector<uint8_t> jpeg_bytes(pdu.length());
      span<uint8_t>        dst(jpeg_bytes.data(), jpeg_bytes.size());
      size_t                 copied = copy_segments(pdu, dst);
      if (copied != pdu.length()) {
        error = "failed to copy all bytes from PDU (copied " + std::to_string(copied) + " of " +
                std::to_string(pdu.length()) + ")";
        return false;
      }

      // Decode JPEG bytes to a BGR cv::Mat.
      cv::Mat jpeg_mat(1, static_cast<int>(jpeg_bytes.size()), CV_8UC1, jpeg_bytes.data());
      cv::Mat img = cv::imdecode(jpeg_mat, cv::IMREAD_COLOR);
      if (img.empty()) {
        error = "failed to decode JPEG from SDU";
        return false;
      }

      // Resize to the model's input resolution.
      cv::Mat resized;
      cv::resize(img, resized, cv::Size(width_, height_));

      // Convert to float and apply per-channel mean / scale / fix_scale.
      cv::Mat float_img;
      resized.convertTo(float_img, CV_32F);
      std::vector<cv::Mat> ch(3);
      cv::split(float_img, ch);
      for (int c = 0; c < 3; ++c) {
        ch[c] = (ch[c] - cfg_.mean_bgr[c]) * cfg_.scale_bgr[c] * input_scale_;
      }
      cv::merge(ch, float_img);

      cv::Mat int8_img;
      float_img.convertTo(int8_img, CV_8S);

      // Copy the NHWC int8 data into the VART input tensor buffer.
      auto input_data = vart::get_tensor_buffer_data(input_buffer_.get(), 0);
      if (input_data.size < static_cast<size_t>(input_size_)) {
        error = "input tensor buffer too small";
        return false;
      }

      const int32_t batch_bytes = height_ * width_ * channels_;
      if (int8_img.total() * int8_img.elemSize() != static_cast<size_t>(batch_bytes)) {
        error = "preprocessed image size mismatch";
        return false;
      }

      // Tile the same image across the batch dimension to match the xmodel.
      auto* input_ptr = static_cast<int8_t*>(input_data.data);
      for (int32_t b = 0; b < batch_; ++b) {
        std::memcpy(input_ptr + b * batch_bytes, int8_img.data, static_cast<size_t>(batch_bytes));
      }

      // Run inference.
      auto job = runner_->execute_async({input_buffer_.get()}, {output_buffer_.get()});
      if (job.second != 0) {
        error = "execute_async failed with status " + std::to_string(job.second);
        return false;
      }
      int wait_status = runner_->wait(job.first, -1);
      if (wait_status != 0) {
        error = "runner wait failed with status " + std::to_string(wait_status);
        return false;
      }

      // Find Top-1 on batch 0 of the int8 output (argmax is invariant to softmax).
      auto                 output_data = vart::get_tensor_buffer_data(output_buffer_.get(), 0);
      auto*                out_ptr     = static_cast<const int8_t*>(output_data.data);
      const int32_t        classes     = output_shape_.size() >= 2 ? output_shape_[1] : output_size_;
      int32_t              max_idx     = 0;
      int8_t               max_val     = out_ptr[0];
      for (int32_t i = 1; i < classes; ++i) {
        if (out_ptr[i] > max_val) {
          max_val = out_ptr[i];
          max_idx = i;
        }
      }
      top1_class = static_cast<int>(max_idx);

      // Compute an approximate Top-1 probability with a numerically stable softmax on batch 0.
      float max_float = static_cast<float>(max_val) * output_scale_;
      float sum       = 0.0f;
      for (int32_t i = 0; i < classes; ++i) {
        sum += std::exp((static_cast<float>(out_ptr[i]) * output_scale_) - max_float);
      }
      top1_score = 1.0f / sum;

      if (logger != nullptr) {
        logger->info("Inference done: top1_class={}, top1_score={:.6f}", top1_class, top1_score);
      }
      return true;
    } catch (const std::exception& e) {
      error = std::string("VART inference exception: ") + e.what();
      return false;
    }
  }

private:
  sdap_vart_config              cfg_;
  std::unique_ptr<xir::Graph>   graph_;
  std::unique_ptr<vart::Runner> runner_;
  const xir::Tensor*            input_tensor_  = nullptr;
  const xir::Tensor*            output_tensor_ = nullptr;
  std::unique_ptr<vart::TensorBuffer> input_buffer_;
  std::unique_ptr<vart::TensorBuffer> output_buffer_;
  std::vector<int32_t>          input_shape_;
  std::vector<int32_t>          output_shape_;
  int32_t                       batch_    = 1;
  int32_t                       height_   = 224;
  int32_t                       width_    = 224;
  int32_t                       channels_ = 3;
  int32_t                       input_fix_point_  = 0;
  float                         input_scale_      = 1.0f;
  int32_t                       output_fix_point_ = 0;
  float                         output_scale_     = 1.0f;
  int32_t                       input_size_  = 0;
  int32_t                       output_size_ = 0;
  srslog::basic_logger*         logger       = nullptr;
};

sdap_tx_vart_notifier::sdap_tx_vart_notifier(const sdap_vart_config& cfg) : pimpl(std::make_unique<impl>(cfg))
{
  ready = pimpl->init(last_error);
}

sdap_tx_vart_notifier::~sdap_tx_vart_notifier() = default;

/// Try to extract the UDP payload from a flattened IPv4/UDP packet.
/// Returns an empty vector if the buffer does not look like IPv4/UDP.
static std::vector<uint8_t> extract_ipv4_udp_payload(span<const uint8_t> pkt)
{
  if (pkt.size() < 20) {
    return {};
  }
  const uint8_t* data = pkt.data();
  // IPv4?
  if ((data[0] >> 4) != 4) {
    return {};
  }
  // Protocol UDP (17)?
  if (data[9] != 17) {
    return {};
  }
  const size_t ip_hdr_len = (data[0] & 0x0Fu) * 4;
  if (pkt.size() < ip_hdr_len + 8) {
    return {};
  }
  const size_t udp_hdr_len = 8;
  return std::vector<uint8_t>(data + ip_hdr_len + udp_hdr_len, data + pkt.size());
}

void sdap_tx_vart_notifier::on_new_pdu(byte_buffer pdu)
{
  if (!ready) {
    last_error = "VART notifier is not initialized";
    return;
  }

  // Flatten the PDU so we can inspect headers and feed the JPEG decoder.
  std::vector<uint8_t> raw_bytes(pdu.length());
  span<uint8_t>        raw_dst(raw_bytes.data(), raw_bytes.size());
  const size_t         copied = copy_segments(pdu, raw_dst);
  if (copied != pdu.length()) {
    last_error = "failed to copy PDU bytes";
    return;
  }

  // Real over-the-air SDUs carry an IPv4/UDP packet. Extract the UDP payload
  // (the JPEG bytes) before inference. If this fails, fall back to treating
  // the whole buffer as a raw JPEG (unit-test mode).
  std::vector<uint8_t> payload = extract_ipv4_udp_payload(span<const uint8_t>(raw_bytes));
  if (payload.empty()) {
    payload = std::move(raw_bytes);
  }

  // Re-wrap the (possibly extracted) payload into a byte_buffer for inference.
  auto jpeg_sdu = byte_buffer::create(span<const uint8_t>(payload.data(), payload.size()));
  if (!jpeg_sdu.has_value()) {
    last_error = "failed to create JPEG byte_buffer";
    return;
  }

  int         cls   = -1;
  float       score = 0.0f;
  std::string err;
  if (pimpl->run_inference(jpeg_sdu.value(), cls, score, err)) {
    top1_class = cls;
    top1_score = score;
  } else {
    last_error = std::move(err);
  }
}
