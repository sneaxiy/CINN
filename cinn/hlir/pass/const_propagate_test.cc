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

#include <gtest/gtest.h>

#include <memory>

#include "cinn/cinn.h"
#include "cinn/frontend/syntax.h"
#include "cinn/hlir/framework/graph.h"
#include "cinn/hlir/framework/graph_compiler.h"
#include "cinn/hlir/framework/pass.h"
#include "cinn/hlir/op/use_ops.h"
#include "cinn/hlir/pass/use_pass.h"

DEFINE_string(model_dir, "", "");

namespace cinn {
namespace frontend {

using hlir::framework::Scope;
using utils::Join;

Target GetTarget() {
#ifdef CINN_WITH_CUDA
  return common::DefaultNVGPUTarget();
#else
  return common::DefaultHostTarget();
#endif
}

void SetRandData(const hlir::framework::Tensor& tensor, Target target) {
#ifdef CINN_WITH_CUDA
  auto* data = tensor->mutable_data<float>(target);
  std::vector<float> host_memory(tensor->shape().numel(), 0);
  for (float& v : host_memory) {
    v = (rand() * 1.f) / RAND_MAX;  // All random data
  }
  CUDA_CALL(cudaMemcpy(reinterpret_cast<void*>(data),
                       host_memory.data(),
                       tensor->shape().numel() * sizeof(float),
                       cudaMemcpyHostToDevice));
#else
  auto* data = tensor->mutable_data<float>(target);
  for (size_t j = 0; j < tensor->shape().numel(); j++) {
    data[j] = (rand() * 1.f) / RAND_MAX;  // All random data
  }
#endif
}

TEST(const_conv, const_conv) {
  Placeholder A(Float(32), {1, 3, 224, 224}, "A");
  Placeholder B(Float(32), {64, 3, 7, 7}, "B", true);

  Program program;
  absl::flat_hash_map<std::string, Program::attr_t> attrs;
  attrs["stride"]        = std::vector<int>({2, 2});
  attrs["dilation"]      = std::vector<int>({1, 1});
  attrs["padding"]       = std::vector<int>({3, 3});
  std::string src_layout = "NCHW";
  attrs["data_format"]   = src_layout;

  auto c = program.conv2d(A, B, attrs);

  Target target = GetTarget();
  program.SetInputs({A, B});
  program.Validate();
  LOG(INFO) << "Program:\n" << program;
  auto graph = std::make_shared<hlir::framework::Graph>(program, target);
  LOG(INFO) << "graph:\n" << graph->Visualize();

  hlir::framework::ApplyPass(graph.get(), "InferShape");
  hlir::framework::ApplyPass(graph.get(), "ConstPropagate");
  auto scope = BuildScope(target, graph);

  hlir::framework::GraphCompiler gc(target, scope, graph);
  auto runtime_program = gc.Build();
  auto& prerun_instrs  = runtime_program->GetPreRunInstructions();
  auto& run_instrs     = runtime_program->GetRunInstructions();
  ASSERT_EQ(prerun_instrs.size(), 0);
  ASSERT_EQ(run_instrs.size(), 1);

  scope->Var<hlir::framework::Tensor>("A");
  scope->Var<hlir::framework::Tensor>("B");

  auto A1 = scope->GetTensor("A");
  auto B1 = scope->GetTensor("B");
  SetRandData(A1, target);
  SetRandData(B1, target);

  runtime_program->Execute();
}

// fused_batch_norm
TEST(const_bn, const_bn) {
  Placeholder A(Float(32), {1, 64, 112, 112}, "A");

  Placeholder Scale(Float(32), {64}, "Scale", true);
  Placeholder Bias(Float(32), {64}, "Bias", true);
  Placeholder Mean(Float(32), {64}, "Mean", true);
  Placeholder Variance(Float(32), {64}, "Variance", true);

  Program program;
  absl::flat_hash_map<std::string, Program::attr_t> attrs;
  attrs["epsilon"] = static_cast<float>(0.001);

  auto a = program.fused_batchnorm_inference(A, Scale, Bias, Mean, Variance, attrs);

  Target target = GetTarget();
  program.SetInputs({A, Scale, Bias, Mean, Variance});
  program.Validate();
  LOG(INFO) << "Program:\n" << program;
  auto graph = std::make_shared<hlir::framework::Graph>(program, target);
  LOG(INFO) << "graph:\n" << graph->Visualize();

  hlir::framework::ApplyPass(graph.get(), "InferShape");
  hlir::framework::ApplyPass(graph.get(), "ConstPropagate");
  auto scope = BuildScope(target, graph);

  hlir::framework::GraphCompiler gc(target, scope, graph);
  auto runtime_program = gc.Build();
  auto& prerun_instrs  = runtime_program->GetPreRunInstructions();
  auto& run_instrs     = runtime_program->GetRunInstructions();
  ASSERT_EQ(prerun_instrs.size(), 7);
  ASSERT_EQ(run_instrs.size(), 2);

  scope->Var<hlir::framework::Tensor>("A");
  scope->Var<hlir::framework::Tensor>("Scale");
  scope->Var<hlir::framework::Tensor>("Bias");
  scope->Var<hlir::framework::Tensor>("Mean");
  scope->Var<hlir::framework::Tensor>("Variance");

  auto A1        = scope->GetTensor("A");
  auto Scale1    = scope->GetTensor("Scale");
  auto Bias1     = scope->GetTensor("Bias");
  auto Mean1     = scope->GetTensor("Mean");
  auto Variance1 = scope->GetTensor("Variance");
  SetRandData(A1, target);
  SetRandData(Scale1, target);
  SetRandData(Bias1, target);
  SetRandData(Mean1, target);
  SetRandData(Variance1, target);

  runtime_program->Execute();
}

}  // namespace frontend
}  // namespace cinn
