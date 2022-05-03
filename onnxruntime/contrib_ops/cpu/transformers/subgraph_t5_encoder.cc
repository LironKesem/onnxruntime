// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/framework/framework_common.h"
#include "core/framework/session_state.h"
#include "core/framework/tensorprotoutils.h"
#include "core/framework/utils.h"
#include "core/providers/cpu/tensor/utils.h"
#include "gsl/gsl"
#include "subgraph_t5_encoder.h"
#include "dump_tensor.h"

using namespace ONNX_NAMESPACE;
using namespace onnxruntime::common;

namespace onnxruntime {
namespace contrib {
namespace transformers {

Status T5EncoderSubgraph::Validate(const std::vector<const NodeArg*>& subgraph_inputs,
                                   const std::vector<const NodeArg*>& subgraph_outputs) {
  // inputs: encoder_input_ids, encoder_attention_mask, decoder_input_ids
  // outputs: logits, encoder_hidden_states, present_key_self_0, present_value_self_0, ..., present_key_cross_0, present_key_cross_0, ...
  ORT_RETURN_IF(num_subgraph_inputs != 3, "expect 3 inputs, got:", num_subgraph_inputs);

  ORT_RETURN_IF(num_subgraph_outputs < 6, "expect >=6 outputs, got:", num_subgraph_outputs);
  ORT_RETURN_IF((static_cast<int>(subgraph_outputs.size()) - 2) % 4 != 0, "number of outputs expected to be 2 + 4 * layers, got:", num_subgraph_outputs);

  ORT_RETURN_IF(subgraph_inputs[0]->Name() != "encoder_input_ids", "subgraph input 0 shall be named as encoder_input_ids, got: ",
                subgraph_inputs[0]->Name());
  ORT_RETURN_IF(subgraph_inputs[1]->Name() != "encoder_attention_mask", "subgraph input 1 shall be named as encoder_attention_mask, got: ",
                subgraph_inputs[1]->Name());
  ORT_RETURN_IF(subgraph_inputs[2]->Name() != "decoder_input_ids", "subgraph input 2 shall be named as decoder_input_ids, got: ",
                subgraph_inputs[2]->Name());

  ORT_RETURN_IF(subgraph_outputs[0]->Name() != "logits", "subgraph output 0 shall be named as logits, got: ",
                subgraph_outputs[0]->Name());

  ORT_RETURN_IF(subgraph_outputs[1]->Name() != "encoder_hidden_states", "subgraph output 1 shall be named as encoder_hidden_states, got: ",
                subgraph_outputs[1]->Name());

  ORT_RETURN_IF(subgraph_outputs[2]->Name() != "present_key_self_0", "subgraph output 2 shall be named as present_key_self_0, got: ",
                subgraph_outputs[2]->Name());

  ORT_RETURN_IF(subgraph_outputs[3]->Name() != "present_value_self_0", "subgraph output 3 shall be named as present_value_self_0, got: ",
                subgraph_outputs[3]->Name());

  const ONNX_NAMESPACE::TensorShapeProto* past_shape = subgraph_outputs[2]->Shape();
  const ONNX_NAMESPACE::TensorShapeProto* logits_shape = subgraph_outputs[0]->Shape();

  // Save parameters related to the subgraph.
  ORT_RETURN_IF_ERROR(GetParameters(past_shape, logits_shape, false));
  num_layers = (static_cast<int>(subgraph_outputs.size()) - 2) / 4;

  ORT_RETURN_IF(subgraph_inputs[0]->TypeAsProto()->tensor_type().elem_type() != ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT32,
                "subgraph input 0 (input_ids) shall have int32 type");

  ORT_RETURN_IF(subgraph_inputs[1]->TypeAsProto()->tensor_type().elem_type() != ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT32,
                "subgraph input 1 (position_ids) shall have int32 type");

  ORT_RETURN_IF(subgraph_inputs[2]->TypeAsProto()->tensor_type().elem_type() != ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT32,
                "subgraph input 2 (position_ids) shall have int32 type");

  auto output_type = subgraph_outputs[0]->TypeAsProto()->tensor_type().elem_type();
  ORT_RETURN_IF(output_type != ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_FLOAT && output_type != ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_FLOAT16,
                "subgraph output 0 (logits) shall be float or float16 data type");

  is_output_float16_ = (output_type == ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_FLOAT16);

  return Status::OK();
}

// Create inputs for first inference of subgraph.
Status T5EncoderSubgraph::CreateInitialFeeds(
    const Tensor& encoder_input_ids,
    const std::vector<const OrtValue*>& implicit_inputs,
    int num_beams,
    int pad_token_id,
    int start_token_id,
    gsl::span<int32_t>& sequence_lengths,
    std::vector<OrtValue>& feeds,
    const BeamSearchDeviceHelper::CreateEncoderInputsFunc& create_encoder_inputs_func,
    const BeamSearchDeviceHelper::AddToFeedsFunc& add_to_feeds_func,
    IAllocatorUniquePtr<char>& buffer) {
  ORT_ENFORCE(session_state_ != nullptr, "Setup must be called before CreateInitialFeeds");

  // The ordering is the same as used in Setup
  feeds.reserve(static_cast<size_t>(num_subgraph_inputs) + static_cast<size_t>(num_implicit_inputs));

  // Allocate subgraph inputs to be same device as encoder_input_ids
  AllocatorPtr cpu_alloactor = session_state_->GetAllocator(encoder_input_ids.Location());

  OrtValue expanded_encoder_input_ids;
  OrtValue expanded_encoder_attention_mask;
  OrtValue expanded_decoder_input_ids;
  ORT_RETURN_IF_ERROR(create_encoder_inputs_func(&encoder_input_ids, num_beams, pad_token_id, start_token_id, sequence_lengths, cpu_alloactor, expanded_encoder_input_ids, expanded_encoder_attention_mask, expanded_decoder_input_ids));

  const IExecutionProvider* provider = GetProvider();
  ORT_RETURN_IF_ERROR(add_to_feeds_func(provider, expanded_encoder_input_ids, expanded_encoder_attention_mask, expanded_decoder_input_ids, feeds, buffer));

  for (const auto* entry : implicit_inputs) {
    feeds.push_back(*entry);
  }

  return Status::OK();
}

}  // namespace transformers
}  // namespace contrib
}  // namespace onnxruntime