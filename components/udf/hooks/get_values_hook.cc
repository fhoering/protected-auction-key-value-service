// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "components/udf/hooks/get_values_hook.h"

#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/status/statusor.h"
#include "components/internal_server/lookup.grpc.pb.h"
#include "glog/logging.h"
#include "google/protobuf/util/json_util.h"
#include "nlohmann/json.hpp"
#include "public/udf/binary_get_values.pb.h"

namespace kv_server {
namespace {

using google::protobuf::json::MessageToJsonString;
using google::scp::roma::proto::FunctionBindingIoProto;

constexpr char kOkStatusMessage[] = "ok";

void SetBinaryGetValuesAsBytes(const BinaryGetValuesResponse& binary_response,
                               FunctionBindingIoProto& io) {
  std::string& buffer = *io.mutable_output_bytes();
  int size = binary_response.ByteSizeLong();
  buffer.resize(size);
  binary_response.SerializeToArray(&buffer[0], size);
}

Status GetStatus(int code, std::string_view message) {
  Status status;
  status.set_code(code);
  status.set_message(message);
  return status;
}

void SetStatusAsBytes(absl::StatusCode code, std::string_view message,
                      FunctionBindingIoProto& io) {
  BinaryGetValuesResponse binary_response;
  *binary_response.mutable_status() =
      GetStatus(static_cast<int>(code), message);
  SetBinaryGetValuesAsBytes(binary_response, io);
}

void SetOutputAsBytes(const InternalLookupResponse& response,
                      FunctionBindingIoProto& io) {
  BinaryGetValuesResponse binary_response;
  for (auto&& [k, v] : response.kv_pairs()) {
    Value value;
    if (v.has_status()) {
      *value.mutable_status() =
          GetStatus(v.status().code(), v.status().message());
    }
    if (v.has_value()) {
      value.set_data(v.value());
    }
    (*binary_response.mutable_kv_pairs())[k] = std::move(value);
  }
  *binary_response.mutable_status() = GetStatus(0, kOkStatusMessage);
  SetBinaryGetValuesAsBytes(binary_response, io);
}

void SetStatusAsString(absl::StatusCode code, std::string_view message,
                       FunctionBindingIoProto& io) {
  nlohmann::json status;
  status["code"] = code;
  status["message"] = std::string(message);
  io.set_output_string(status.dump());
}

void SetOutputAsString(const InternalLookupResponse& response,
                       FunctionBindingIoProto& io) {
  VLOG(9) << "Processing internal lookup response";
  std::string kv_pairs_json;
  if (const auto json_status = MessageToJsonString(response, &kv_pairs_json);
      !json_status.ok()) {
    SetStatusAsString(json_status.code(), json_status.message(), io);
    LOG(ERROR) << "MessageToJsonString failed with " << json_status;
    VLOG(1) << "getValues result: " << io.DebugString();
    return;
  }

  auto kv_pairs_json_object = nlohmann::json::parse(kv_pairs_json, nullptr,
                                                    /*allow_exceptions=*/false,
                                                    /*ignore_comments=*/true);
  if (kv_pairs_json_object.is_discarded()) {
    SetStatusAsString(absl::StatusCode::kInvalidArgument,
                      "Error while parsing JSON string.", io);
    LOG(ERROR) << "json parse failed for " << kv_pairs_json;

    return;
  }
  kv_pairs_json_object["status"]["code"] = 0;
  kv_pairs_json_object["status"]["message"] = kOkStatusMessage;
  io.set_output_string(kv_pairs_json_object.dump());
}

class GetValuesHookImpl : public GetValuesHook {
 public:
  explicit GetValuesHookImpl(absl::AnyInvocable<std::unique_ptr<LookupClient>()>
                                 lookup_client_supplier,
                             OutputType output_type)
      : lookup_client_supplier_(std::move(lookup_client_supplier)),
        output_type_(output_type) {}

  void operator()(FunctionBindingIoProto& io) {
    VLOG(9) << "getValues request: " << io.DebugString();
    if (!io.has_input_list_of_string()) {
      SetStatus(absl::StatusCode::kInvalidArgument,
                "getValues input must be list of strings", io);
      VLOG(1) << "getValues result: " << io.DebugString();
      return;
    }

    // Lazy load lookup client on first call.
    if (lookup_client_ == nullptr) {
      lookup_client_ = lookup_client_supplier_();
    }

    std::vector<std::string> keys;
    for (const auto& key : io.input_list_of_string().data()) {
      keys.emplace_back(key);
    }

    VLOG(9) << "Calling internal lookup client";
    absl::StatusOr<InternalLookupResponse> response_or_status =
        lookup_client_->GetValues(keys);
    if (!response_or_status.ok()) {
      SetStatus(response_or_status.status().code(),
                response_or_status.status().message(), io);
      VLOG(1) << "getValues result: " << io.DebugString();
      return;
    }

    SetOutput(response_or_status.value(), io);
    VLOG(9) << "getValues result: " << io.DebugString();
  }

 private:
  void SetStatus(absl::StatusCode code, std::string_view message,
                 FunctionBindingIoProto& io) {
    if (output_type_ == OutputType::kString) {
      SetStatusAsString(code, message, io);
    } else {
      SetStatusAsBytes(code, message, io);
    }
  }

  void SetOutput(const InternalLookupResponse& response,
                 FunctionBindingIoProto& io) {
    if (output_type_ == OutputType::kString) {
      SetOutputAsString(response, io);
    } else {
      SetOutputAsBytes(response, io);
    }
  }

  // `lookup_client_` is lazy loaded because getting one can cause thread
  // creation. Lazy load is used to ensure that it only happens after Roma
  // forks.
  absl::AnyInvocable<std::unique_ptr<LookupClient>()> lookup_client_supplier_;
  std::unique_ptr<LookupClient> lookup_client_;
  OutputType output_type_;
};
}  // namespace

std::unique_ptr<GetValuesHook> GetValuesHook::Create(
    absl::AnyInvocable<std::unique_ptr<LookupClient>()> lookup_client_supplier,
    OutputType output_type) {
  return std::make_unique<GetValuesHookImpl>(std::move(lookup_client_supplier),
                                             output_type);
}

}  // namespace kv_server