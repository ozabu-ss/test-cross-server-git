/*
 * Copyright (C) 2019 The Android Open Source Project
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
#define LOG_TAG "VtsHalIdentityEndToEndTest"

#include <aidl/Gtest.h>
#include <aidl/Vintf.h>
#include <android-base/logging.h>
#include <android/hardware/identity/IIdentityCredentialStore.h>
#include <android/hardware/identity/support/IdentityCredentialSupport.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <cppbor.h>
#include <cppbor_parse.h>
#include <gtest/gtest.h>
#include <future>
#include <map>

#include "VtsIdentityTestUtils.h"

namespace android::hardware::identity {

using std::endl;
using std::map;
using std::optional;
using std::string;
using std::vector;

using ::android::sp;
using ::android::String16;
using ::android::binder::Status;

using ::android::hardware::keymaster::HardwareAuthToken;
using ::android::hardware::keymaster::VerificationToken;

using test_utils::validateAttestationCertificate;

class IdentityAidl : public testing::TestWithParam<std::string> {
  public:
    virtual void SetUp() override {
        credentialStore_ = android::waitForDeclaredService<IIdentityCredentialStore>(
                String16(GetParam().c_str()));
        ASSERT_NE(credentialStore_, nullptr);
    }

    sp<IIdentityCredentialStore> credentialStore_;
};

TEST_P(IdentityAidl, hardwareInformation) {
    HardwareInformation info;
    ASSERT_TRUE(credentialStore_->getHardwareInformation(&info).isOk());
    ASSERT_GT(info.credentialStoreName.size(), 0);
    ASSERT_GT(info.credentialStoreAuthorName.size(), 0);
    ASSERT_GE(info.dataChunkSize, 256);
}

TEST_P(IdentityAidl, createAndRetrieveCredential) {
    // First, generate a key-pair for the reader since its public key will be
    // part of the request data.
    vector<uint8_t> readerKey;
    optional<vector<uint8_t>> readerCertificate =
            test_utils::generateReaderCertificate("1234", &readerKey);
    ASSERT_TRUE(readerCertificate);

    // Make the portrait image really big (just shy of 256 KiB) to ensure that
    // the chunking code gets exercised.
    vector<uint8_t> portraitImage;
    test_utils::setImageData(portraitImage);

    // Access control profiles:
    const vector<test_utils::TestProfile> testProfiles = {// Profile 0 (reader authentication)
                                                          {0, readerCertificate.value(), false, 0},
                                                          // Profile 1 (no authentication)
                                                          {1, {}, false, 0}};

    // It doesn't matter since no user auth is needed in this particular test,
    // but for good measure, clear out the tokens we pass to the HAL.
    HardwareAuthToken authToken;
    VerificationToken verificationToken;
    authToken.challenge = 0;
    authToken.userId = 0;
    authToken.authenticatorId = 0;
    authToken.authenticatorType = ::android::hardware::keymaster::HardwareAuthenticatorType::NONE;
    authToken.timestamp.milliSeconds = 0;
    authToken.mac.clear();
    verificationToken.challenge = 0;
    verificationToken.timestamp.milliSeconds = 0;
    verificationToken.securityLevel = ::android::hardware::keymaster::SecurityLevel::SOFTWARE;
    verificationToken.mac.clear();

    // Here's the actual test data:
    const vector<test_utils::TestEntryData> testEntries = {
            {"PersonalData", "Last name", string("Turing"), vector<int32_t>{0, 1}},
            {"PersonalData", "Birth date", string("19120623"), vector<int32_t>{0, 1}},
            {"PersonalData", "First name", string("Alan"), vector<int32_t>{0, 1}},
            {"PersonalData", "Home address", string("Maida Vale, London, England"),
             vector<int32_t>{0}},
            {"Image", "Portrait image", portraitImage, vector<int32_t>{0, 1}},
    };
    const vector<int32_t> testEntriesEntryCounts = {static_cast<int32_t>(testEntries.size() - 1),
                                                    1u};
    HardwareInformation hwInfo;
    ASSERT_TRUE(credentialStore_->getHardwareInformation(&hwInfo).isOk());

    string cborPretty;
    sp<IWritableIdentityCredential> writableCredential;
    ASSERT_TRUE(test_utils::setupWritableCredential(writableCredential, credentialStore_));

    string challenge = "attestationChallenge";
    test_utils::AttestationData attData(writableCredential, challenge, {});
    ASSERT_TRUE(attData.result.isOk())
            << attData.result.exceptionCode() << "; " << attData.result.exceptionMessage() << endl;

    EXPECT_TRUE(validateAttestationCertificate(attData.attestationCertificate,
                                               attData.attestationChallenge,
                                               attData.attestationApplicationId, hwInfo));

    // This is kinda of a hack but we need to give the size of
    // ProofOfProvisioning that we'll expect to receive.
    const int32_t expectedProofOfProvisioningSize = 262861 - 326 + readerCertificate.value().size();
    // OK to fail, not available in v1 HAL
    writableCredential->setExpectedProofOfProvisioningSize(expectedProofOfProvisioningSize);
    ASSERT_TRUE(
            writableCredential->startPersonalization(testProfiles.size(), testEntriesEntryCounts)
                    .isOk());

    optional<vector<SecureAccessControlProfile>> secureProfiles =
            test_utils::addAccessControlProfiles(writableCredential, testProfiles);
    ASSERT_TRUE(secureProfiles);

    // Uses TestEntryData* pointer as key and values are the encrypted blobs. This
    // is a little hacky but it works well enough.
    map<const test_utils::TestEntryData*, vector<vector<uint8_t>>> encryptedBlobs;

    for (const auto& entry : testEntries) {
        ASSERT_TRUE(test_utils::addEntry(writableCredential, entry, hwInfo.dataChunkSize,
                                         encryptedBlobs, true));
    }

    vector<uint8_t> credentialData;
    vector<uint8_t> proofOfProvisioningSignature;
    ASSERT_TRUE(
            writableCredential->finishAddingEntries(&credentialData, &proofOfProvisioningSignature)
                    .isOk());

    optional<vector<uint8_t>> proofOfProvisioning =
            support::coseSignGetPayload(proofOfProvisioningSignature);
    ASSERT_TRUE(proofOfProvisioning);
    cborPretty = support::cborPrettyPrint(proofOfProvisioning.value(), 32, {"readerCertificate"});
    EXPECT_EQ(
            "[\n"
            "  'ProofOfProvisioning',\n"
            "  'org.iso.18013-5.2019.mdl',\n"
            "  [\n"
            "    {\n"
            "      'id' : 0,\n"
            "      'readerCertificate' : <not printed>,\n"
            "    },\n"
            "    {\n"
            "      'id' : 1,\n"
            "    },\n"
            "  ],\n"
            "  {\n"
            "    'PersonalData' : [\n"
            "      {\n"
            "        'name' : 'Last name',\n"
            "        'value' : 'Turing',\n"
            "        'accessControlProfiles' : [0, 1, ],\n"
            "      },\n"
            "      {\n"
            "        'name' : 'Birth date',\n"
            "        'value' : '19120623',\n"
            "        'accessControlProfiles' : [0, 1, ],\n"
            "      },\n"
            "      {\n"
            "        'name' : 'First name',\n"
            "        'value' : 'Alan',\n"
            "        'accessControlProfiles' : [0, 1, ],\n"
            "      },\n"
            "      {\n"
            "        'name' : 'Home address',\n"
            "        'value' : 'Maida Vale, London, England',\n"
            "        'accessControlProfiles' : [0, ],\n"
            "      },\n"
            "    ],\n"
            "    'Image' : [\n"
            "      {\n"
            "        'name' : 'Portrait image',\n"
            "        'value' : <bstr size=262134 sha1=941e372f654d86c32d88fae9e41b706afbfd02bb>,\n"
            "        'accessControlProfiles' : [0, 1, ],\n"
            "      },\n"
            "    ],\n"
            "  },\n"
            "  true,\n"
            "]",
            cborPretty);

    optional<vector<uint8_t>> credentialPubKey = support::certificateChainGetTopMostKey(
            attData.attestationCertificate[0].encodedCertificate);
    ASSERT_TRUE(credentialPubKey);
    EXPECT_TRUE(support::coseCheckEcDsaSignature(proofOfProvisioningSignature,
                                                 {},  // Additional data
                                                 credentialPubKey.value()));
    writableCredential = nullptr;

    // Now that the credential has been provisioned, read it back and check the
    // correct data is returned.
    sp<IIdentityCredential> credential;
    ASSERT_TRUE(credentialStore_
                        ->getCredential(
                                CipherSuite::CIPHERSUITE_ECDHE_HKDF_ECDSA_WITH_AES_256_GCM_SHA256,
                                credentialData, &credential)
                        .isOk());
    ASSERT_NE(credential, nullptr);

    optional<vector<uint8_t>> readerEphemeralKeyPair = support::createEcKeyPair();
    ASSERT_TRUE(readerEphemeralKeyPair);
    optional<vector<uint8_t>> readerEphemeralPublicKey =
            support::ecKeyPairGetPublicKey(readerEphemeralKeyPair.value());
    ASSERT_TRUE(credential->setReaderEphemeralPublicKey(readerEphemeralPublicKey.value()).isOk());

    vector<uint8_t> ephemeralKeyPair;
    ASSERT_TRUE(credential->createEphemeralKeyPair(&ephemeralKeyPair).isOk());
    optional<vector<uint8_t>> ephemeralPublicKey = support::ecKeyPairGetPublicKey(ephemeralKeyPair);

    // Calculate requestData field and sign it with the reader key.
    auto [getXYSuccess, ephX, ephY] = support::ecPublicKeyGetXandY(ephemeralPublicKey.value());
    ASSERT_TRUE(getXYSuccess);
    cppbor::Map deviceEngagement = cppbor::Map().add("ephX", ephX).add("ephY", ephY);
    vector<uint8_t> deviceEngagementBytes = deviceEngagement.encode();
    vector<uint8_t> eReaderPubBytes = cppbor::Tstr("ignored").encode();
    cppbor::Array sessionTranscript = cppbor::Array()
                                              .add(cppbor::Semantic(24, deviceEngagementBytes))
                                              .add(cppbor::Semantic(24, eReaderPubBytes));
    vector<uint8_t> sessionTranscriptBytes = sessionTranscript.encode();

    vector<uint8_t> itemsRequestBytes =
            cppbor::Map("nameSpaces",
                        cppbor::Map()
                                .add("PersonalData", cppbor::Map()
                                                             .add("Last name", false)
                                                             .add("Birth date", false)
                                                             .add("First name", false)
                                                             .add("Home address", true))
                                .add("Image", cppbor::Map().add("Portrait image", false)))
                    .encode();
    cborPretty = support::cborPrettyPrint(itemsRequestBytes, 32, {"EphemeralPublicKey"});
    EXPECT_EQ(
            "{\n"
            "  'nameSpaces' : {\n"
            "    'PersonalData' : {\n"
            "      'Last name' : false,\n"
            "      'Birth date' : false,\n"
            "      'First name' : false,\n"
            "      'Home address' : true,\n"
            "    },\n"
            "    'Image' : {\n"
            "      'Portrait image' : false,\n"
            "    },\n"
            "  },\n"
            "}",
            cborPretty);
    vector<uint8_t> dataToSign = cppbor::Array()
                                         .add("ReaderAuthentication")
                                         .add(sessionTranscript.clone())
                                         .add(cppbor::Semantic(24, itemsRequestBytes))
                                         .encode();
    optional<vector<uint8_t>> readerSignature =
            support::coseSignEcDsa(readerKey, {},  // content
                                   dataToSign,     // detached content
                                   readerCertificate.value());
    ASSERT_TRUE(readerSignature);

    // Generate the key that will be used to sign AuthenticatedData.
    vector<uint8_t> signingKeyBlob;
    Certificate signingKeyCertificate;
    ASSERT_TRUE(credential->generateSigningKeyPair(&signingKeyBlob, &signingKeyCertificate).isOk());

    vector<RequestNamespace> requestedNamespaces = test_utils::buildRequestNamespaces(testEntries);
    // OK to fail, not available in v1 HAL
    credential->setRequestedNamespaces(requestedNamespaces).isOk();
    // OK to fail, not available in v1 HAL
    credential->setVerificationToken(verificationToken);
    ASSERT_TRUE(credential
                        ->startRetrieval(secureProfiles.value(), authToken, itemsRequestBytes,
                                         signingKeyBlob, sessionTranscriptBytes,
                                         readerSignature.value(), testEntriesEntryCounts)
                        .isOk());

    for (const auto& entry : testEntries) {
        ASSERT_TRUE(credential
                            ->startRetrieveEntryValue(entry.nameSpace, entry.name,
                                                      entry.valueCbor.size(), entry.profileIds)
                            .isOk());

        auto it = encryptedBlobs.find(&entry);
        ASSERT_NE(it, encryptedBlobs.end());
        const vector<vector<uint8_t>>& encryptedChunks = it->second;

        vector<uint8_t> content;
        for (const auto& encryptedChunk : encryptedChunks) {
            vector<uint8_t> chunk;
            ASSERT_TRUE(credential->retrieveEntryValue(encryptedChunk, &chunk).isOk());
            content.insert(content.end(), chunk.begin(), chunk.end());
        }
        EXPECT_EQ(content, entry.valueCbor);
    }

    vector<uint8_t> mac;
    vector<uint8_t> deviceNameSpacesBytes;
    ASSERT_TRUE(credential->finishRetrieval(&mac, &deviceNameSpacesBytes).isOk());
    cborPretty = support::cborPrettyPrint(deviceNameSpacesBytes, 32, {});
    ASSERT_EQ(
            "{\n"
            "  'PersonalData' : {\n"
            "    'Last name' : 'Turing',\n"
            "    'Birth date' : '19120623',\n"
            "    'First name' : 'Alan',\n"
            "    'Home address' : 'Maida Vale, London, England',\n"
            "  },\n"
            "  'Image' : {\n"
            "    'Portrait image' : <bstr size=262134 "
            "sha1=941e372f654d86c32d88fae9e41b706afbfd02bb>,\n"
            "  },\n"
            "}",
            cborPretty);
    // The data that is MACed is ["DeviceAuthentication", sessionTranscriptBytes, docType,
    // deviceNameSpacesBytes] so build up that structure
    cppbor::Array deviceAuthentication;
    deviceAuthentication.add("DeviceAuthentication");
    deviceAuthentication.add(sessionTranscript.clone());

    string docType = "org.iso.18013-5.2019.mdl";
    deviceAuthentication.add(docType);
    deviceAuthentication.add(cppbor::Semantic(24, deviceNameSpacesBytes));
    vector<uint8_t> encodedDeviceAuthentication = deviceAuthentication.encode();
    optional<vector<uint8_t>> signingPublicKey =
            support::certificateChainGetTopMostKey(signingKeyCertificate.encodedCertificate);
    EXPECT_TRUE(signingPublicKey);

    // Derive the key used for MACing.
    optional<vector<uint8_t>> readerEphemeralPrivateKey =
            support::ecKeyPairGetPrivateKey(readerEphemeralKeyPair.value());
    optional<vector<uint8_t>> sharedSecret =
            support::ecdh(signingPublicKey.value(), readerEphemeralPrivateKey.value());
    ASSERT_TRUE(sharedSecret);
    vector<uint8_t> salt = {0x00};
    vector<uint8_t> info = {};
    optional<vector<uint8_t>> derivedKey = support::hkdf(sharedSecret.value(), salt, info, 32);
    ASSERT_TRUE(derivedKey);
    optional<vector<uint8_t>> calculatedMac =
            support::coseMac0(derivedKey.value(), {},        // payload
                              encodedDeviceAuthentication);  // detached content
    ASSERT_TRUE(calculatedMac);
    EXPECT_EQ(mac, calculatedMac);
}

INSTANTIATE_TEST_SUITE_P(
        Identity, IdentityAidl,
        testing::ValuesIn(android::getAidlHalInstanceNames(IIdentityCredentialStore::descriptor)),
        android::PrintInstanceNameToString);
// INSTANTIATE_TEST_SUITE_P(Identity, IdentityAidl,
// testing::Values("android.hardware.identity.IIdentityCredentialStore/default"));

}  // namespace android::hardware::identity

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::android::ProcessState::self()->setThreadPoolMaxThreadCount(1);
    ::android::ProcessState::self()->startThreadPool();
    return RUN_ALL_TESTS();
}
