/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <memory>
#include <string>
#include <vector>

#ifndef COMPONENTS_DATA_SERVER_SERVER_NONPROD_KEY_FETCHER_FACTORY_CLOUD_H_
#define COMPONENTS_DATA_SERVER_SERVER_NONPROD_KEY_FETCHER_FACTORY_CLOUD_H_

#include "components/data_server/server/key_fetcher_factory.h"
#include "components/data_server/server/parameter_fetcher.h"
#include "src/encryption/key_fetcher/interface/key_fetcher_manager_interface.h"

namespace kv_server {

// Constructs KeyFetcherManager. This factory allows to override the public key
// endpoint. This is a security risk for produciton build. Which is why this
// implementation is only allowed in `nonprod` build mode.
class NonprodCloudKeyFetcherFactory : public CloudKeyFetcherFactory {
 protected:
  google::scp::cpio::PrivateKeyVendingEndpoint GetPrimaryKeyFetchingEndpoint(
      const ParameterFetcher& parameter_fetcher) const override;
  google::scp::cpio::PrivateKeyVendingEndpoint GetSecondaryKeyFetchingEndpoint(
      const ParameterFetcher& parameter_fetcher) const override;
  std::vector<std::string> GetPublicKeyFetchingEndpoint(
      const ParameterFetcher& parameter_fetcher) const override;
  ::privacy_sandbox::server_common::CloudPlatform GetCloudPlatform()
      const override;
};

}  // namespace kv_server
#endif  // COMPONENTS_DATA_SERVER_SERVER_NONPROD_KEY_FETCHER_FACTORY_CLOUD_H_
