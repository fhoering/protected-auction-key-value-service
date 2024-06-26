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

#include "components/udf/udf_client.h"

#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"
#include "google/protobuf/util/json_util.h"
#include "src/roma/config/config.h"
#include "src/roma/interface/roma.h"
#include "src/roma/roma_service/roma_service.h"

namespace kv_server {

namespace {
using google::protobuf::json::MessageToJsonString;
using google::scp::roma::CodeObject;
using google::scp::roma::Config;
using google::scp::roma::InvocationStrRequest;
using google::scp::roma::kTimeoutDurationTag;
using google::scp::roma::ResponseObject;
using google::scp::roma::sandbox::roma_service::RomaService;

constexpr absl::Duration kCodeUpdateTimeout = absl::Seconds(1);

// Roma IDs and version numbers are required for execution.
// We do not currently make use of IDs or the code version number, set them to
// constants.
constexpr char kCodeObjectId[] = "id";
constexpr char kInvocationRequestId[] = "id";
constexpr int kUdfInterfaceVersion = 1;

class UdfClientImpl : public UdfClient {
 public:
  explicit UdfClientImpl(
      Config<RequestContext>&& config = Config<RequestContext>(),
      absl::Duration udf_timeout = absl::Seconds(5), int udf_min_log_level = 0)
      : udf_timeout_(udf_timeout),
        roma_service_(std::move(config)),
        udf_min_log_level_(udf_min_log_level) {}

  // Converts the arguments into plain JSON strings to pass to Roma.
  absl::StatusOr<std::string> ExecuteCode(
      RequestContext request_context, UDFExecutionMetadata&& execution_metadata,
      const google::protobuf::RepeatedPtrField<UDFArgument>& arguments) const {
    execution_metadata.set_udf_interface_version(kUdfInterfaceVersion);
    std::vector<std::string> string_args;
    string_args.reserve(arguments.size() + 1);
    std::string json_metadata;
    if (const auto json_status =
            MessageToJsonString(execution_metadata, &json_metadata);
        !json_status.ok()) {
      return json_status;
    }
    string_args.push_back(json_metadata);

    for (int i = 0; i < arguments.size(); ++i) {
      const auto& arg = arguments[i];
      const google::protobuf::Message* arg_data;
      if (arg.tags().values().empty()) {
        arg_data = &arg.data();
      } else {
        arg_data = &arg;
      }
      std::string json_arg;
      if (const auto json_status = MessageToJsonString(*arg_data, &json_arg);
          !json_status.ok()) {
        return json_status;
      }
      string_args.push_back(json_arg);
    }
    return ExecuteCode(std::move(request_context), std::move(string_args));
  }

  absl::StatusOr<std::string> ExecuteCode(
      RequestContext request_context, std::vector<std::string> input) const {
    std::shared_ptr<absl::Status> response_status =
        std::make_shared<absl::Status>();
    std::shared_ptr<std::string> result = std::make_shared<std::string>();
    std::shared_ptr<absl::Notification> notification =
        std::make_shared<absl::Notification>();
    auto invocation_request =
        BuildInvocationRequest(std::move(request_context), std::move(input));
    VLOG(9) << "Executing UDF with input arg(s): "
            << absl::StrJoin(invocation_request.input, ",");
    const auto status = roma_service_.Execute(
        std::make_unique<InvocationStrRequest<RequestContext>>(
            std::move(invocation_request)),
        [notification, response_status,
         result](absl::StatusOr<ResponseObject> response) {
          if (response.ok()) {
            *result = std::move(response->resp);
          } else {
            response_status->Update(std::move(response.status()));
          }
          notification->Notify();
        });
    if (!status.ok()) {
      LOG(ERROR) << "Error sending UDF for execution: " << status;
      return status;
    }

    notification->WaitForNotificationWithTimeout(udf_timeout_);
    if (!notification->HasBeenNotified()) {
      return absl::InternalError("Timed out waiting for UDF result.");
    }
    if (!response_status->ok()) {
      LOG(ERROR) << "Error executing UDF: " << *response_status;
      return *response_status;
    }
    return *result;
  }
  absl::Status Init() { return roma_service_.Init(); }

  absl::Status Stop() { return roma_service_.Stop(); }

  absl::Status SetCodeObject(CodeConfig code_config) {
    // Only update code if logical commit time is larger.
    if (logical_commit_time_ >= code_config.logical_commit_time) {
      VLOG(1) << "Not updating code object. logical_commit_time "
              << code_config.logical_commit_time
              << " too small, should be greater than " << logical_commit_time_;
      return absl::OkStatus();
    }
    std::shared_ptr<absl::Status> response_status =
        std::make_shared<absl::Status>();
    std::shared_ptr<absl::Notification> notification =
        std::make_shared<absl::Notification>();
    VLOG(9) << "Setting UDF: " << code_config.js;
    CodeObject code_object =
        BuildCodeObject(std::move(code_config.js), std::move(code_config.wasm),
                        code_config.version);
    absl::Status load_status = roma_service_.LoadCodeObj(
        std::make_unique<CodeObject>(code_object),
        [notification, response_status](absl::StatusOr<ResponseObject> resp) {
          if (!resp.ok()) {
            response_status->Update(std::move(resp.status()));
          }
          notification->Notify();
        });
    if (!load_status.ok()) {
      LOG(ERROR) << "Error setting UDF Code object: " << load_status;
      return load_status;
    }

    notification->WaitForNotificationWithTimeout(kCodeUpdateTimeout);
    if (!notification->HasBeenNotified()) {
      return absl::InternalError("Timed out setting UDF code object.");
    }
    if (!response_status->ok()) {
      LOG(ERROR) << "Error compiling UDF code object. " << *response_status;
      return *response_status;
    }
    handler_name_ = std::move(code_config.udf_handler_name);
    logical_commit_time_ = code_config.logical_commit_time;
    version_ = code_config.version;
    VLOG(5) << "Successfully set UDF code object with handler_name "
            << handler_name_;
    return absl::OkStatus();
  }

  absl::Status SetWasmCodeObject(CodeConfig code_config) {
    const auto code_object_status = SetCodeObject(std::move(code_config));
    if (!code_object_status.ok()) {
      return code_object_status;
    }
    return absl::OkStatus();
  }

 private:
  InvocationStrRequest<RequestContext> BuildInvocationRequest(
      RequestContext request_context, std::vector<std::string> input) const {
    return {.id = kInvocationRequestId,
            .version_string = absl::StrCat("v", version_),
            .handler_name = handler_name_,
            .tags = {{std::string(kTimeoutDurationTag),
                      FormatDuration(udf_timeout_)}},
            .input = std::move(input),
            .metadata = std::move(request_context),
            .min_log_level = absl::LogSeverity(udf_min_log_level_)};
  }

  CodeObject BuildCodeObject(std::string js, std::string wasm,
                             int64_t version) {
    return {.id = kCodeObjectId,
            .version_string = absl::StrCat("v", version),
            .js = std::move(js),
            .wasm = std::move(wasm)};
  }

  std::string handler_name_;
  int64_t logical_commit_time_ = -1;
  int64_t version_ = 1;
  const absl::Duration udf_timeout_;
  int udf_min_log_level_;
  // Per b/299667930, RomaService has been extended to support metadata storage
  // as a side effect of RomaService::Execute(), making it no longer const.
  // However, UDFClient::ExecuteCode() remains logically const, so RomaService
  // is marked as mutable to allow usage within UDFClient::ExecuteCode(). For
  // concerns about mutable or go/totw/174, RomaService is thread-safe, so
  // losing the thread-safety of usage within a const function is a lesser
  // concern.
  mutable RomaService<RequestContext> roma_service_;
};

}  // namespace

absl::StatusOr<std::unique_ptr<UdfClient>> UdfClient::Create(
    Config<RequestContext>&& config, absl::Duration udf_timeout,
    int udf_min_log_level) {
  auto udf_client = std::make_unique<UdfClientImpl>(
      std::move(config), udf_timeout, udf_min_log_level);
  const auto init_status = udf_client->Init();
  if (!init_status.ok()) {
    return init_status;
  }
  return udf_client;
}

}  // namespace kv_server
