
#include <dlfcn.h>
#include <include/android_build.h>
#include <string>
#include <vector>

#include "include/logging.h"
#include "native_hook.h"

static bool inlineHooksInstalled = false;

static const char *(*getDesc)(void *, std::string *);

static bool (*isInSamePackageBackup)(void *, void *) = nullptr;

void *runtime_ = nullptr;

void (*deoptBootImage)(void *runtime) = nullptr;

bool (*runtimeInitBackup)(void *runtime, void *mapAddr) = nullptr;

bool my_runtimeInit(void *runtime, void *mapAddr) {
    if (!runtimeInitBackup) {
        LOGE("runtimeInitBackup is null");
        return false;
    }
    LOGI("runtimeInit starts");
    bool result = (*runtimeInitBackup)(runtime, mapAddr);
    if (!deoptBootImage) {
        LOGE("deoptBootImageSym is null, skip deoptBootImage");
    } else {
        LOGI("deoptBootImage starts");
        (*deoptBootImage)(runtime);
        LOGI("deoptBootImage finishes");
    }
    LOGI("runtimeInit finishes");
    return result;
}

static bool onIsInSamePackageCalled(void *thiz, void *that) {
    std::string storage1, storage2;
    const char *thisDesc = (*getDesc)(thiz, &storage1);
    const char *thatDesc = (*getDesc)(that, &storage2);
    if (strstr(thisDesc, "EdHooker") != nullptr
        || strstr(thatDesc, "EdHooker") != nullptr
        || strstr(thisDesc, "com/elderdrivers/riru/") != nullptr
        || strstr(thatDesc, "com/elderdrivers/riru/") != nullptr) {
//        LOGE("onIsInSamePackageCalled, %s -> %s", thisDesc, thatDesc);
        return true;
    }
    return (*isInSamePackageBackup)(thiz, that);
}

static bool onInvokeHiddenAPI() {
    return false;
}

/**
 * NOTICE:
 * After Android Q(10.0), GetMemberActionImpl has been renamed to ShouldDenyAccessToMemberImpl,
 * But we don't know the symbols until it's published.
 * @author asLody
 */
static bool disable_HiddenAPIPolicyImpl(int api_level, void *artHandle,
                                        void (*hookFun)(void *, void *, void **)) {
    if (api_level < ANDROID_P) {
        return true;
    }
    void *symbol = nullptr;
    // Android P : Preview 1 ~ 4 version
    symbol = dlsym(artHandle,
                   "_ZN3art9hiddenapi25ShouldBlockAccessToMemberINS_8ArtFieldEEEbPT_PNS_6ThreadENSt3__18functionIFbS6_EEENS0_12AccessMethodE");
    if (symbol) {
        hookFun(symbol, reinterpret_cast<void *>(onInvokeHiddenAPI), nullptr);
    }
    symbol = dlsym(artHandle,
                   "_ZN3art9hiddenapi25ShouldBlockAccessToMemberINS_9ArtMethodEEEbPT_PNS_6ThreadENSt3__18functionIFbS6_EEENS0_12AccessMethodE"
    );

    if (symbol) {
        hookFun(symbol, reinterpret_cast<void *>(onInvokeHiddenAPI), nullptr);
        return true;
    }
    // Android P : Release version
    symbol = dlsym(artHandle,
                   "_ZN3art9hiddenapi6detail19GetMemberActionImplINS_8ArtFieldEEENS0_6ActionEPT_NS_20HiddenApiAccessFlags7ApiListES4_NS0_12AccessMethodE"
    );
    if (symbol) {
        hookFun(symbol, reinterpret_cast<void *>(onInvokeHiddenAPI), nullptr);
    }
    symbol = dlsym(artHandle,
                   "_ZN3art9hiddenapi6detail19GetMemberActionImplINS_9ArtMethodEEENS0_6ActionEPT_NS_20HiddenApiAccessFlags7ApiListES4_NS0_12AccessMethodE"
    );
    if (symbol) {
        hookFun(symbol, reinterpret_cast<void *>(onInvokeHiddenAPI), nullptr);
    }
    return symbol != nullptr;
}

static void hookIsInSamePackage(int api_level, void *artHandle,
                                void (*hookFun)(void *, void *, void **)) {
    // 5.0 - 7.1
    const char *isInSamePackageSym = "_ZN3art6mirror5Class15IsInSamePackageEPS1_";
    const char *getDescriptorSym = "_ZN3art6mirror5Class13GetDescriptorEPNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE";
    if (api_level >= ANDROID_O) {
        // 8.0 and later
        isInSamePackageSym = "_ZN3art6mirror5Class15IsInSamePackageENS_6ObjPtrIS1_EE";
    }
    void *original = dlsym(artHandle, isInSamePackageSym);
    if (!original) {
        LOGE("can't get isInSamePackageSym: %s", dlerror());
        return;
    }
    void *getDescSym = dlsym(artHandle, getDescriptorSym);
    if (!getDescSym) {
        LOGE("can't get GetDescriptorSym: %s", dlerror());
        return;
    }
    getDesc = reinterpret_cast<const char *(*)(void *, std::string *)>(getDescSym);
    (*hookFun)(original, reinterpret_cast<void *>(onIsInSamePackageCalled),
               reinterpret_cast<void **>(&isInSamePackageBackup));
}

void hookRuntime(int api_level, void *artHandle, void (*hookFun)(void *, void *, void **)) {
    void *runtimeInitSym = nullptr;
    if (api_level >= ANDROID_O) {
        // only oreo has deoptBootImageSym in Runtime
        runtime_ = dlsym(artHandle, "_ZN3art7Runtime9instance_E");
        if (!runtime_) { LOGW("runtime instance not found"); }
        runtimeInitSym = dlsym(artHandle, "_ZN3art7Runtime4InitEONS_18RuntimeArgumentMapE");
        if (!runtimeInitSym) {
            LOGE("can't find runtimeInitSym: %s", dlerror());
            return;
        }
        deoptBootImage = reinterpret_cast<void (*)(void *)>(dlsym(artHandle,
                                                                  "_ZN3art7Runtime19DeoptimizeBootImageEv"));
        if (!deoptBootImage) {
            LOGE("can't find deoptBootImageSym: %s", dlerror());
            return;
        }
        LOGI("start to hook runtimeInitSym");
        (*hookFun)(runtimeInitSym, reinterpret_cast<void *>(my_runtimeInit),
                   reinterpret_cast<void **>(&runtimeInitBackup));
        LOGI("runtimeInitSym hooked");
    } else {
        // TODO support deoptBootImage for Android 7.1 and before?
        LOGI("hooking Runtime skipped");
    }
}

void install_inline_hooks() {
    if (inlineHooksInstalled) {
        LOGI("inline hooks installed, skip");
        return;
    }
    LOGI("start to install inline hooks");
    int api_level = GetAndroidApiLevel();
    if (api_level < ANDROID_LOLLIPOP) {
        LOGE("api level not supported: %d, skip", api_level);
        return;
    }
    LOGI("using api level %d", api_level);
    void *whaleHandle = dlopen(kLibWhalePath, RTLD_LAZY | RTLD_GLOBAL);
    if (!whaleHandle) {
        LOGE("can't open libwhale: %s", dlerror());
        return;
    }
    void *hookFunSym = dlsym(whaleHandle, "WInlineHookFunction");
    if (!hookFunSym) {
        LOGE("can't get WInlineHookFunction: %s", dlerror());
        return;
    }
    void (*hookFun)(void *, void *, void **) = reinterpret_cast<void (*)(void *, void *,
                                                                         void **)>(hookFunSym);
    void *artHandle = dlopen(kLibArtPath, RTLD_LAZY | RTLD_GLOBAL);
    if (!artHandle) {
        LOGE("can't open libart: %s", dlerror());
        return;
    }
    hookIsInSamePackage(api_level, artHandle, hookFun);
    hookRuntime(api_level, artHandle, hookFun);
    if (disable_HiddenAPIPolicyImpl(api_level, artHandle, hookFun)) {
        LOGI("disable_HiddenAPIPolicyImpl done.");
    } else {
        LOGE("disable_HiddenAPIPolicyImpl failed.");
    }
    dlclose(whaleHandle);
    dlclose(artHandle);
    LOGI("install inline hooks done");
}

