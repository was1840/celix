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

#include <gtest/gtest.h>

#include <filesystem>

#include "celix_framework_factory.h"
#include "celix_framework_utils.h"
#include "celix_file_utils.h"


class CelixCFrameworkUtilsTestSuite : public ::testing::Test {
public:
    CelixCFrameworkUtilsTestSuite() {
        auto* config = celix_properties_create();
        celix_properties_set(config, "CELIX_LOGGING_DEFAULT_ACTIVE_LOG_LEVEL", "trace");

        auto* fw = celix_frameworkFactory_createFramework(config);
        framework = std::shared_ptr<celix_framework_t>{fw, [](celix_framework_t* cFw) {
            celix_frameworkFactory_destroyFramework(cFw);
        }};
    }

    std::shared_ptr<celix_framework_t> framework{};
};

static void checkBundleCacheDir(const char* extractDir) {
    EXPECT_TRUE(extractDir != nullptr);
    if (extractDir) {
        EXPECT_TRUE(std::filesystem::is_directory(extractDir));
    }
}

TEST_F(CelixCFrameworkUtilsTestSuite, testExtractBundlePath) {
    const char* testExtractDir = "extractBundleTestDir";
    celix_utils_deleteDirectory(testExtractDir, nullptr);

    //invalid bundle url -> no extraction
    auto status = celix_framework_utils_extractBundle(framework.get(), nullptr, testExtractDir);
    EXPECT_NE(status, CELIX_SUCCESS);

    //invalid bundle path -> no extraction
    status = celix_framework_utils_extractBundle(nullptr, "non-existing.zip", testExtractDir); //note nullptr framwork is allowed, fallback to global logger.
    EXPECT_NE(status, CELIX_SUCCESS);

    //invalid url prefix -> no extraction
    std::string path = std::string{"bla://"} + SIMPLE_TEST_BUNDLE1_LOCATION;
    status = celix_framework_utils_extractBundle(framework.get(), path.c_str(), testExtractDir);
    EXPECT_NE(status, CELIX_SUCCESS);

    //invalid url prefix -> no extraction
    path = std::string{"bla://"};
    status = celix_framework_utils_extractBundle(framework.get(), path.c_str(), testExtractDir);
    EXPECT_NE(status, CELIX_SUCCESS);

    //invalid url prefix -> no extraction
    path = std::string{"file://"};
    status = celix_framework_utils_extractBundle(framework.get(), path.c_str(), testExtractDir);
    EXPECT_NE(status, CELIX_SUCCESS);

    //valid bundle path -> extraction
    status = celix_framework_utils_extractBundle(framework.get(), SIMPLE_TEST_BUNDLE1_LOCATION, testExtractDir);
    EXPECT_EQ(status, CELIX_SUCCESS);
    checkBundleCacheDir(testExtractDir);
    celix_utils_deleteDirectory(testExtractDir, nullptr);

    //valid bundle path with file:// prefix -> extraction
    path = std::string{"file://"} + SIMPLE_TEST_BUNDLE1_LOCATION;
    status = celix_framework_utils_extractBundle(framework.get(), path.c_str(), testExtractDir);
    EXPECT_EQ(status, CELIX_SUCCESS);
    checkBundleCacheDir(testExtractDir);
    celix_utils_deleteDirectory(testExtractDir, nullptr);
}

TEST_F(CelixCFrameworkUtilsTestSuite, testExtractEmbeddedBundle) {
    const char* testExtractDir = "extractEmbeddedBundleTestDir";
    celix_utils_deleteDirectory(testExtractDir, nullptr);

    //invalid bundle symbol -> no extraction
    auto status = celix_framework_utils_extractBundle(framework.get(), "embedded://nonexisting", testExtractDir);
    EXPECT_NE(status, CELIX_SUCCESS);

    //valid bundle path -> extraction
    status = celix_framework_utils_extractBundle(framework.get(), "embedded://simple_test_bundle1", testExtractDir);
    EXPECT_EQ(status, CELIX_SUCCESS);
    checkBundleCacheDir(testExtractDir);
    celix_utils_deleteDirectory(testExtractDir, nullptr);
}