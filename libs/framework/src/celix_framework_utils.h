/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *  KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef CELIX_FRAMEWORK_UTILS_H_
#define CELIX_FRAMEWORK_UTILS_H_


#include "celix_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief extracts a bundle for the given cache.
 * @param fw Optional Celix framework (used for logging).
 *           If NULL the result of celix_frameworkLogger_globalLogger() will be used for logging.
 * @param extractPath The path to extract the bundle to.
 * @param bundleURL The bundle url. Which must be the following:
 *  - prefixed with file:// -> url is a file path.
 *  - prefixed with embedded:// -> url is a symbol for the bundle embedded in the current executable.
 *  - *:// -> not supported
 *  - no :// -> assuming that the url is a file path (same as with a file:// prefix)
 * @return CELIX_SUCCESS is the bundle was correctly extracted.
 */
celix_status_t celix_framework_utils_extractBundle(celix_framework_t *fw, const char *bundleURL,  const char* extractPath);

#ifdef __cplusplus
}
#endif

#endif /* CELIX_FRAMEWORK_UTILS_H_ */
