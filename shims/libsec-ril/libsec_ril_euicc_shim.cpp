/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

// Makes AOSP's ES10 path work against the eUICC behind Samsung's RIL. Loaded
// by the SMSC shim (libsec-ril.so) and wraps the stock library.

#define LOG_TAG "sec-ril-euicc-shim"

#include <dlfcn.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <log/log.h>
#include <sys/system_properties.h>
#include <telephony/ril.h>

#ifndef REAL_LIB_NAME
#error "REAL_LIB_NAME must be defined by Android.bp"
#endif

static constexpr char kRealPath[] = "/vendor/lib64/" REAL_LIB_NAME;

using SamsungRequestFunc = void (*)(
        int, void*, size_t, RIL_Token, RIL_SOCKET_ID);

// Samsung extends RIL_RadioFunctions after onRequest. Only describe the
// common prefix so the private callbacks remain in the real table.
struct SamsungRilFunctionsPrefix {
    int version;
    SamsungRequestFunc onRequest;
};

static_assert(
        offsetof(SamsungRilFunctionsPrefix, onRequest) == sizeof(void*),
        "unexpected Samsung RIL function-table prefix");

static constexpr int kMaxPlausibleRilVersion = 64;

using RilInit = const RIL_RadioFunctions* (*)(
        const RIL_Env*, int, char**);

static void* gRealHandle = nullptr;
static SamsungRequestFunc gRealOnRequest = nullptr;

// AOSP TransmitApduLogicalChannelInvocation does: cla | channel.
// Samsung OPEN_CHANNEL returns session ids like 101 (not ISO channel 1..19).
// That yields CLA 0xE5 for STORE DATA (0x80|101) and the card returns 6986.
// Modem +CGLA already keys off sessionid — strip the bogus OR from CLA.
static void fixupSamsungApduCla(RIL_SIM_APDU* apdu) {
    const int sid = apdu->sessionid;
    const int before = apdu->cla;
    // Normal ISO channels need no fix. Otherwise act only when every session
    // bit is set, which is what AOSP's OR leaves behind: other callers pass a
    // correct raw CLA, and stripping partial matches would corrupt it.
    if (sid <= 3 || (before & sid) != sid) {
        return;
    }
    apdu->cla = before & ~sid;
    // Fires on every ES10 APDU, so report only the first one.
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true)) {
        ALOGI("APDU-CLA fix active: session=%d cla %02X -> %02X",
                apdu->sessionid, before & 0xff, apdu->cla & 0xff);
    }
}

// The RIL reports MEP-A1 in slot status but none in card status, so after a
// slot switch the framework treats the eUICC as MEP-B and leaves
// targetPortNumber (tag 82) out of EnableProfile, which then fails with 6A80.
// Adding tag 82 on a non-MEP-A1 card fails the same way, hence the gate.
static bool isMepA1() {
    char mep[PROP_VALUE_MAX] = {};
    __system_property_get("ril.esim.mep_mode", mep);
    // Formats seen: "1", "0,1", ",1"
    if (std::strcmp(mep, "1") == 0) {
        return true;
    }
    const char* comma = std::strchr(mep, ',');
    return comma != nullptr && comma[1] == '1'
            && (comma[2] == '\0' || comma[2] == ',');
}

static void injectMepA1Port(RIL_SIM_APDU* apdu) {
    const char* hex = apdu->data;
    // Only EnableProfile — DisableProfile must stay without tag 82.
    if (hex == nullptr || std::strncmp(hex, "BF31", 4) != 0 || !isMepA1()) {
        return;
    }
    const size_t hexLen = std::strlen(hex);
    // AOSP appends 8201xx itself when the framework has MEP-A1.
    if (hexLen >= 6 && std::strncmp(hex + hexLen - 6, "8201", 4) == 0) {
        return;
    }
    // Short-form length only: BF31 LL <LL bytes>
    unsigned contentLen = 0;
    if (hexLen < 6 || std::sscanf(hex + 4, "%2x", &contentLen) != 1
            || contentLen > 0x7f) {
        ALOGW("BF31: skip port inject (unexpected len encoding)");
        return;
    }
    if (6 + contentLen * 2 != hexLen) {
        ALOGW("BF31: skip port inject (len mismatch %zu vs %u)",
                hexLen, 6 + contentLen * 2);
        return;
    }
    const unsigned newContentLen = contentLen + 3;  // + 82 01 01
    if (newContentLen > 0x7f) {
        return;
    }
    // Framework port 0 is eUICC port 1 under MEP-A1. libril_sem free()s
    // apdu->data after transmit, so the replacement must come from malloc;
    // static storage aborts under MTE.
    const size_t newLen = hexLen + 6 + 1;
    char* rewritten = static_cast<char*>(std::malloc(newLen));
    if (rewritten == nullptr) {
        return;
    }
    std::snprintf(rewritten, newLen, "%.4s%02X%s820101",
            hex, newContentLen, hex + 6);
    ALOGI("APDU-MEP inject port1: %s -> %s", hex, rewritten);
    std::free(apdu->data);
    apdu->data = rewritten;
    apdu->p3 = static_cast<int>(std::strlen(rewritten) / 2);
}

static void* getRealHandle() {
    if (gRealHandle != nullptr) {
        return gRealHandle;
    }

    ALOGI("loading %s", kRealPath);
    gRealHandle = dlopen(kRealPath, RTLD_NOW);
    if (gRealHandle == nullptr) {
        ALOGE("dlopen failed: %s", dlerror());
    }

    return gRealHandle;
}

static RilInit getRealInit(const char* name) {
    void* realHandle = getRealHandle();
    if (realHandle == nullptr) {
        return nullptr;
    }

    dlerror();
    auto realInit = reinterpret_cast<RilInit>(dlsym(realHandle, name));
    const char* error = dlerror();
    if (error != nullptr) {
        ALOGE("dlsym %s failed: %s", name, error);
        return nullptr;
    }

    return realInit;
}

static void shimOnRequest(
        int request, void* data, size_t datalen, RIL_Token token,
        RIL_SOCKET_ID socketId) {
    // Logical channels only: the basic channel always has session 0, and
    // EnableProfile never goes over it.
    if (request == RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL && data != nullptr
            && datalen >= sizeof(RIL_SIM_APDU)) {
        auto* apdu = static_cast<RIL_SIM_APDU*>(data);
        fixupSamsungApduCla(apdu);
        injectMepA1Port(apdu);
    }

    gRealOnRequest(request, data, datalen, token, socketId);
}

extern "C" const RIL_RadioFunctions* RIL_Init(
        const RIL_Env* env, int argc, char** argv) {
    RilInit realRilInit = getRealInit("RIL_Init");
    if (realRilInit == nullptr) {
        return nullptr;
    }

    const RIL_RadioFunctions* real = realRilInit(env, argc, argv);
    if (real == nullptr) {
        ALOGE("invalid real RIL function table");
        return real;
    }

    auto* realPrefix = reinterpret_cast<SamsungRilFunctionsPrefix*>(
            const_cast<RIL_RadioFunctions*>(real));
    // An implausible version means the table does not start with AOSP's
    // {version, onRequest} prefix, and patching it would corrupt it.
    ALOGI("real RIL function table version=%d", realPrefix->version);
    if (realPrefix->version < RIL_VERSION_MIN
            || realPrefix->version > kMaxPlausibleRilVersion) {
        ALOGE("implausible RIL version %d; not hooking onRequest",
                realPrefix->version);
        return real;
    }
    if (realPrefix->onRequest == nullptr) {
        ALOGE("real onRequest is null");
        return real;
    }

    if (realPrefix->onRequest != shimOnRequest) {
        gRealOnRequest = realPrefix->onRequest;
        realPrefix->onRequest = shimOnRequest;
    }

    ALOGI("installed eUICC APDU shim");
    return real;
}

extern "C" const RIL_RadioFunctions* RIL_SAP_Init(
        const RIL_Env* env, int argc, char** argv) {
    RilInit realSapInit = getRealInit("RIL_SAP_Init");
    if (realSapInit == nullptr) {
        return nullptr;
    }

    return realSapInit(env, argc, argv);
}
