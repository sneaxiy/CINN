// Copyright (c) 2021 CINN Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cinn/hlir/framework/instruction.h"

#include "cinn/common/test_helper.h"

DECLARE_bool(cinn_sync_run);

namespace cinn {
namespace hlir {
namespace framework {

std::vector<cinn_pod_value_t>& Instruction::PreparePodArgs(
    int i, const std::map<std::string, cinn_pod_value_t>* name2podargs) {
  if (args_cached_.size() > i)
    return args_cached_[i];
  else if (args_cached_.size() < i)
    PreparePodArgs(i - 1, name2podargs);
  common::ArgsBuilder builder;
  // Remove duplicate input arguments
  std::set<std::string> in_args_set;
  std::vector<std::string> all_args;
  for (auto& arg : in_args_[i]) {
    if (in_args_set.count(arg) != 0) continue;
    all_args.push_back(arg);
    in_args_set.insert(arg);
  }

  all_args.insert(std::end(all_args), out_args_[i].begin(), out_args_[i].end());

  if (name2podargs != nullptr) {
    for (auto& arg : all_args) {
      CHECK_NE(name2podargs->count(arg), 0) << "Argument [" << arg << "] not found in the name2podargs";
      builder.Add(name2podargs->at(arg));
    }
  } else {
    for (auto& arg : all_args) {
      auto* var = scope_->FindVar(arg);
      CHECK(var) << "Argument [" << arg << "] not found in the scope";

      // TODO(Superjomn) Support other types.
      auto& tensor = absl::get<Tensor>(*var);
      builder.Add(tensor->buffer());
    }
  }

  args_cached_.emplace_back(builder.Build());
  CHECK_GT(args_cached_.size(), i);
  return args_cached_[i];
}

void Instruction::Finalize() {
  if (fn_.size() > 1 && fn_.size() != in_args_.size()) {
    out_args_.back()[0] = out_args_.front()[0];
    out_args_.erase(out_args_.begin());
    in_args_.erase(in_args_.begin());
  }

  finalized_flag_ = true;
}

void Instruction::Run(const std::map<std::string, cinn_pod_value_t>* name2podargs, bool dryrun, void* stream) {
  CHECK(finalized_flag_) << "Instruction must be finalized before run";
  if (function_name_ == "no_run") {
    VLOG(2) << "skip instruction";
    return;
  }

  VLOG(2) << "Run function " << function_name_;

  if (name2podargs != nullptr) {
    args_cached_.clear();
  }

#if defined(CINN_WITH_CUDA) && !defined(CINN_WITH_CUDNN)
  if (function_name_ == "cublas_gemm" && target_.arch == Target::Arch::NVGPU) {
    auto& pod_args = PreparePodArgs(0, name2podargs);
    VLOG(3) << "The pod_args size of cublas_gemm: " << pod_args.size();
    runtime::cuda::cinn_gpu_cublas_gemm(
        attrs, pod_args[0], pod_args[1], pod_args[2], pod_args[3], static_cast<cudaStream_t>(stream));
  } else if (function_name_ == "cublas_matmul" && target_.arch == Target::Arch::NVGPU) {
    auto& pod_args = PreparePodArgs(0, name2podargs);
    VLOG(3) << "The pod_args size of cublas_matmul: " << pod_args.size();
    runtime::cuda::cinn_gpu_cublas_matmul(
        attrs, pod_args[0], pod_args[1], pod_args[2], static_cast<cudaStream_t>(stream));
  } else {
    int i = 0;
    VLOG(2) << "Runing extern function " << function_name_;
    for (auto& it_fn : fn_) {
      VLOG(6) << "Runing it_fn " << fn_names_[i];
      auto& pod_args = PreparePodArgs(i, name2podargs);
      CHECK(it_fn) << "The LoweredFunc address should be set first by calling SetLoweredFunc method";
      if (!dryrun) {
        it_fn(pod_args.data(), pod_args.size());
      }
      i++;
    }
  }
#elif defined(CINN_WITH_CUDNN)
  auto& pod_args = PreparePodArgs(0, name2podargs);
  // Here conv2d and depthwise_conv2d are implemented by one cudnn api cudnnConvolutionForward
  if ((function_name_ == "conv2d" || function_name_ == "depthwise_conv2d") && target_.arch == Target::Arch::NVGPU) {
    if (str_attrs[0] == "forward") {
      absl::flat_hash_map<std::string, int> attrs_map = {
          {"input_n", attrs[0]},     {"input_c", attrs[1]},     {"input_h", attrs[2]},   {"input_w", attrs[3]},
          {"weights_n", attrs[4]},   {"weights_c", attrs[5]},   {"weights_h", attrs[6]}, {"weights_w", attrs[7]},
          {"pad_h", attrs[8]},       {"pad_w", attrs[9]},       {"stride_h", attrs[10]}, {"stride_w", attrs[11]},
          {"dilation_h", attrs[12]}, {"dilation_w", attrs[13]}, {"groups", attrs[14]},   {"output_n", attrs[15]},
          {"output_c", attrs[16]},   {"output_h", attrs[17]},   {"output_w", attrs[18]},
      };
      // input weight output
      runtime::cuda::cinn_gpu_cudnn_conv2d(
          attrs_map, pod_args[0], pod_args[1], pod_args[2], static_cast<cudaStream_t>(stream));
    } else if (str_attrs[0] == "backward_data") {
      // w, dy, dx
      absl::flat_hash_map<std::string, int> attrs_map = {
          {"input_n", attrs[15]},    {"input_c", attrs[16]},    {"input_h", attrs[17]},  {"input_w", attrs[18]},
          {"weights_n", attrs[0]},   {"weights_c", attrs[1]},   {"weights_h", attrs[2]}, {"weights_w", attrs[3]},
          {"pad_h", attrs[8]},       {"pad_w", attrs[9]},       {"stride_h", attrs[10]}, {"stride_w", attrs[11]},
          {"dilation_h", attrs[12]}, {"dilation_w", attrs[13]}, {"groups", attrs[14]},   {"output_n", attrs[4]},
          {"output_c", attrs[5]},    {"output_h", attrs[6]},    {"output_w", attrs[7]},
      };
      // w, dy, dx
      runtime::cuda::cinn_gpu_cudnn_conv2d_backward_data(
          attrs_map, pod_args[0], pod_args[1], pod_args[2], static_cast<cudaStream_t>(stream));
    } else {
      // x, dy, w
      absl::flat_hash_map<std::string, int> attrs_map = {
          {"input_n", attrs[0]},     {"input_c", attrs[1]},     {"input_h", attrs[2]},    {"input_w", attrs[3]},
          {"weights_n", attrs[15]},  {"weights_c", attrs[16]},  {"weights_h", attrs[17]}, {"weights_w", attrs[18]},
          {"pad_h", attrs[8]},       {"pad_w", attrs[9]},       {"stride_h", attrs[10]},  {"stride_w", attrs[11]},
          {"dilation_h", attrs[12]}, {"dilation_w", attrs[13]}, {"groups", attrs[14]},    {"output_n", attrs[4]},
          {"output_c", attrs[5]},    {"output_h", attrs[6]},    {"output_w", attrs[7]},
      };
      // x, dy, w
      runtime::cuda::cinn_gpu_cudnn_conv2d_backward_filter(
          attrs_map, pod_args[0], pod_args[1], pod_args[2], static_cast<cudaStream_t>(stream));
    }
  } else if (function_name_ == "pool2d" && target_.arch == Target::Arch::NVGPU) {
    runtime::cuda::cinn_gpu_cudnn_pool2d(attrs, str_attrs, pod_args[0], pod_args[1], static_cast<cudaStream_t>(stream));
  } else if (function_name_ == "softmax" && target_.arch == Target::Arch::NVGPU) {
    CHECK_EQ(pod_args.size(), 3);
    runtime::cuda::cinn_gpu_cudnn_softmax(attrs, pod_args[0], pod_args[1], static_cast<cudaStream_t>(stream));
  } else if (function_name_ == "mul" && target_.arch == Target::Arch::NVGPU) {
    CHECK_EQ(pod_args.size(), 4);
    runtime::cuda::cinn_gpu_cublas_mul(attrs, pod_args[0], pod_args[1], pod_args[2], static_cast<cudaStream_t>(stream));
  } else if (function_name_ == "cublas_gemm" && target_.arch == Target::Arch::NVGPU) {
    VLOG(3) << "The pod_args size of cublas_gemm: " << pod_args.size();
    runtime::cuda::cinn_gpu_cublas_gemm(
        attrs, pod_args[0], pod_args[1], pod_args[2], pod_args[3], static_cast<cudaStream_t>(stream));
  } else if (function_name_ == "cublas_matmul" && target_.arch == Target::Arch::NVGPU) {
    auto& pod_args = PreparePodArgs(0, name2podargs);
    VLOG(3) << "The pod_args size of cublas_matmul: " << pod_args.size();
    runtime::cuda::cinn_gpu_cublas_matmul(
        attrs, pod_args[0], pod_args[1], pod_args[2], static_cast<cudaStream_t>(stream));
  } else {
    int i = 0;
    VLOG(2) << "Runing extern function " << function_name_;
    for (auto& it_fn : fn_) {
      auto& pod_args = PreparePodArgs(i, name2podargs);
      CHECK(it_fn) << "The LoweredFunc address should be set first by calling SetLoweredFunc method";
      if (!dryrun) {
        it_fn(pod_args.data(), pod_args.size());
      }
      i++;
    }
  }
#else
  int i = 0;
  CHECK_EQ(fn_names_.size(), fn_.size());
  VLOG(3) << "fn_ size is " << fn_.size() << ", function_name_ is : " << function_name_;
  for (auto& it_fn : fn_) {
    auto& pod_args = PreparePodArgs(i, name2podargs);
    CHECK(it_fn) << "The LoweredFunc address should be set first by calling SetLoweredFunc method";
    if (!dryrun) {
      it_fn(pod_args.data(), pod_args.size());
    }
    i++;
  }
#endif

#ifdef CINN_WITH_CUDA
  if (FLAGS_cinn_sync_run) {
    cudaStreamSynchronize(static_cast<cudaStream_t>(stream));
  }
#endif
}

}  // namespace framework
}  // namespace hlir
}  // namespace cinn
