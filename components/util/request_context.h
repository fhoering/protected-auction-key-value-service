/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef COMPONENTS_UTIL_REQUEST_CONTEXT_H_
#define COMPONENTS_UTIL_REQUEST_CONTEXT_H_

#include <memory>
#include <string>
#include <utility>

#include "components/telemetry/server_definition.h"
#include "src/logger/request_context_impl.h"

namespace kv_server {

// RequestLogContext holds value of LogContext and ConsentedDebugConfiguration
// passed from the upstream application.
class RequestLogContext {
 public:
  explicit RequestLogContext(
      const privacy_sandbox::server_common::LogContext& log_context,
      const privacy_sandbox::server_common::ConsentedDebugConfiguration&
          consented_debug_config);

  privacy_sandbox::server_common::log::ContextImpl& GetRequestLoggingContext();

  const privacy_sandbox::server_common::LogContext& GetLogContext() const;

  const privacy_sandbox::server_common::ConsentedDebugConfiguration&
  GetConsentedDebugConfiguration() const;

 private:
  // Parses the LogContext to btree map which is used to construct request
  // logging context and used as labels for logging messages
  absl::btree_map<std::string, std::string> GetContextMap(
      const privacy_sandbox::server_common::LogContext& log_context);
  const privacy_sandbox::server_common::LogContext log_context_;
  const privacy_sandbox::server_common::ConsentedDebugConfiguration
      consented_debug_config_;
  privacy_sandbox::server_common::log::ContextImpl request_logging_context_;
};

// RequestContext holds the reference of udf request metrics context,
// internal lookup request context, and request log context
// that ties to a single request. The request_id can be either passed
// from upper stream or assigned from uuid generated when
// RequestContext is constructed.

class RequestContext {
 public:
  explicit RequestContext(
      std::string request_id = google::scp::core::common::ToString(
          google::scp::core::common::Uuid::GenerateUuid()))
      : request_id_(request_id),
        udf_request_metrics_context_(KVServerContextMap()->Get(&request_id_)),
        internal_lookup_metrics_context_(
            InternalLookupServerContextMap()->Get(&request_id_)) {
    request_log_context_ = std::make_unique<RequestLogContext>(
        privacy_sandbox::server_common::LogContext(),
        privacy_sandbox::server_common::ConsentedDebugConfiguration());
  }
  // Updates request log context with the new log context and consented debug
  // configuration. This function is typically called after RequestContext is
  // created and the consented debugging information is available after request
  // is decrypted.
  void UpdateLogContext(
      const privacy_sandbox::server_common::LogContext& log_context,
      const privacy_sandbox::server_common::ConsentedDebugConfiguration&
          consented_debug_config);
  UdfRequestMetricsContext& GetUdfRequestMetricsContext() const;
  InternalLookupMetricsContext& GetInternalLookupMetricsContext() const;
  RequestLogContext& GetRequestLogContext() const;

  ~RequestContext() {
    // Remove the metrics context for request_id, This is to ensure that
    // metrics context has the same lifetime with RequestContext and be
    // destroyed when RequestContext goes out of scope.
    LogIfError(KVServerContextMap()->Remove(&request_id_),
               "When removing Udf request metrics context");
    LogIfError(InternalLookupServerContextMap()->Remove(&request_id_),
               "When removing internal lookup metrics context");
  }

 private:
  const std::string request_id_;
  UdfRequestMetricsContext& udf_request_metrics_context_;
  InternalLookupMetricsContext& internal_lookup_metrics_context_;
  std::unique_ptr<RequestLogContext> request_log_context_;
};

// Class that facilitates the passing of request context to public interfaces
// while hiding the implementation details like wrapping the request context
// with a smart pointer
class RequestContextFactory {
 public:
  explicit RequestContextFactory(std::weak_ptr<RequestContext> request_context);
  std::weak_ptr<RequestContext> Get();

 private:
  std::weak_ptr<RequestContext> request_context_;
};

}  // namespace kv_server

#endif  // COMPONENTS_UTIL_REQUEST_CONTEXT_H_
