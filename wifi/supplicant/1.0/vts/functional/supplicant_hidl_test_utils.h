/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef SUPPLICANT_HIDL_TEST_UTILS_H
#define SUPPLICANT_HIDL_TEST_UTILS_H

#include <android/hardware/wifi/supplicant/1.0/ISupplicant.h>
#include <android/hardware/wifi/supplicant/1.0/ISupplicantP2pIface.h>
#include <android/hardware/wifi/supplicant/1.0/ISupplicantStaIface.h>
#include <android/hardware/wifi/supplicant/1.0/ISupplicantStaNetwork.h>
#include <android/hardware/wifi/supplicant/1.1/ISupplicant.h>

#include <getopt.h>

#include "wifi_hidl_test_utils.h"

// Used to stop the android wifi framework before every test.
void stopWifiFramework();
void stopWifiFramework(const std::string& wifi_instance_name);
void startWifiFramework();
void startWifiFramework(const std::string& wifi_instance_name);

void stopSupplicant();
void stopSupplicant(const std::string& wifi_instance_name);
// Used to configure the chip, driver and start wpa_supplicant before every
// test.
void startSupplicantAndWaitForHidlService(
    const std::string& wifi_instance_name,
    const std::string& supplicant_instance_name);

// Helper functions to obtain references to the various HIDL interface objects.
// Note: We only have a single instance of each of these objects currently.
// These helper functions should be modified to return vectors if we support
// multiple instances.
android::sp<android::hardware::wifi::supplicant::V1_0::ISupplicant>
getSupplicant(const std::string& supplicant_instance_name, bool isP2pOn);
android::sp<android::hardware::wifi::supplicant::V1_0::ISupplicantStaIface>
getSupplicantStaIface(
    const android::sp<android::hardware::wifi::supplicant::V1_0::ISupplicant>&
        supplicant);
android::sp<android::hardware::wifi::supplicant::V1_0::ISupplicantStaNetwork>
createSupplicantStaNetwork(
    const android::sp<android::hardware::wifi::supplicant::V1_0::ISupplicant>&
        supplicant);
android::sp<android::hardware::wifi::supplicant::V1_0::ISupplicantP2pIface>
getSupplicantP2pIface(
    const android::sp<android::hardware::wifi::supplicant::V1_0::ISupplicant>&
        supplicant);
bool turnOnExcessiveLogging(
    const android::sp<android::hardware::wifi::supplicant::V1_0::ISupplicant>&
        supplicant);

// TODO(b/143892896): Remove old APIs after all supplicant tests are updated.
void startSupplicantAndWaitForHidlService();
android::sp<android::hardware::wifi::supplicant::V1_0::ISupplicant>
getSupplicant();
android::sp<android::hardware::wifi::supplicant::V1_0::ISupplicantStaIface>
getSupplicantStaIface();
android::sp<android::hardware::wifi::supplicant::V1_0::ISupplicantStaNetwork>
createSupplicantStaNetwork();
android::sp<android::hardware::wifi::supplicant::V1_0::ISupplicantP2pIface>
getSupplicantP2pIface();

bool turnOnExcessiveLogging();

class WifiSupplicantHidlEnvironment
    : public ::testing::VtsHalHidlTargetTestEnvBase {
   protected:
    virtual void HidlSetUp() override { stopSupplicant(); }
    virtual void HidlTearDown() override {
        startSupplicantAndWaitForHidlService();
    }

   public:
    // Whether P2P feature is supported on the device.
    bool isP2pOn = true;

    void usage(char* me, char* arg) {
        fprintf(stderr,
                "unrecognized option: %s\n\n"
                "usage: %s <gtest options> <test options>\n\n"
                "test options are:\n\n"
                "-P, --p2p_on: Whether P2P feature is supported\n",
                arg, me);
    }

    int initFromOptions(int argc, char** argv) {
        static struct option options[] = {{"p2p_off", no_argument, 0, 'P'},
                                          {0, 0, 0, 0}};

        int c;
        while ((c = getopt_long(argc, argv, "P", options, NULL)) >= 0) {
            switch (c) {
                case 'P':
                    isP2pOn = false;
                    break;
                default:
                    usage(argv[0], argv[optind]);
                    return 2;
            }
        }

        if (optind < argc) {
            usage(argv[0], argv[optind]);
            return 2;
        }

        return 0;
    }
};

#endif /* SUPPLICANT_HIDL_TEST_UTILS_H */
