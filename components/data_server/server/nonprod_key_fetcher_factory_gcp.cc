// Copyright 2024 Google LLC
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

#include "absl/log/log.h"
#include "components/data_server/server/key_fetcher_factory.h"
#include "components/data_server/server/key_fetcher_utils_gcp.h"
#include "components/data_server/server/nonprod_key_fetcher_factory_cloud.h"

namespace kv_server {
namespace {
using ::google::scp::cpio::PrivateKeyVendingEndpoint;
using ::privacy_sandbox::server_common::CloudPlatform;

class KeyFetcherFactoryGcpNonProd : public NonprodCloudKeyFetcherFactory {
  PrivateKeyVendingEndpoint GetPrimaryKeyFetchingEndpoint(
      const ParameterFetcher& parameter_fetcher) const override {
    PrivateKeyVendingEndpoint endpoint =
        NonprodCloudKeyFetcherFactory::GetPrimaryKeyFetchingEndpoint(
            parameter_fetcher);
    UpdatePrimaryGcpEndpoint(endpoint, parameter_fetcher);
    return endpoint;
  }

  PrivateKeyVendingEndpoint GetSecondaryKeyFetchingEndpoint(
      const ParameterFetcher& parameter_fetcher) const override {
    PrivateKeyVendingEndpoint endpoint =
        NonprodCloudKeyFetcherFactory::GetSecondaryKeyFetchingEndpoint(
            parameter_fetcher);
    UpdateSecondaryGcpEndpoint(endpoint, parameter_fetcher);
    return endpoint;
  }

  CloudPlatform GetCloudPlatform() const override {
    return CloudPlatform::kGcp;
  }
};
}  // namespace

std::unique_ptr<KeyFetcherFactory> KeyFetcherFactory::Create() {
  return std::make_unique<KeyFetcherFactoryGcpNonProd>();
}
}  // namespace kv_server
