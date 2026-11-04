#include "display.h"
#include "common.h"

namespace display {

static JavaVM* g_vm = nullptr;

void init(JavaVM* vm) { g_vm = vm; }

bool hdrInfo(bool* supported, int* peak_nits) {
    if (!g_vm) return false;
    JNIEnv* env = nullptr;
    bool attached = false;
    jint r = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    } else if (r != JNI_OK || !env) {
        return false;
    }

    bool ok = false;
    bool sup = false;
    int peak = 0;
    jclass cAt = env->FindClass("android/app/ActivityThread");
    jobject app = nullptr, wm = nullptr, disp = nullptr, caps = nullptr;
    jstring svc = nullptr;
    do {
        if (!cAt || env->ExceptionCheck()) break;
        jmethodID mCur = env->GetStaticMethodID(cAt, "currentApplication", "()Landroid/app/Application;");
        if (!mCur || env->ExceptionCheck()) break;
        app = env->CallStaticObjectMethod(cAt, mCur);
        if (!app || env->ExceptionCheck()) break;
        jclass cCtx = env->FindClass("android/content/Context");
        jmethodID mSvc = cCtx ? env->GetMethodID(cCtx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : nullptr;
        if (cCtx) env->DeleteLocalRef(cCtx);
        if (!mSvc || env->ExceptionCheck()) break;
        svc = env->NewStringUTF("window");
        wm = env->CallObjectMethod(app, mSvc, svc);
        if (!wm || env->ExceptionCheck()) break;
        jclass cWm = env->FindClass("android/view/WindowManager");
        jmethodID mDef = cWm ? env->GetMethodID(cWm, "getDefaultDisplay", "()Landroid/view/Display;") : nullptr;
        if (cWm) env->DeleteLocalRef(cWm);
        if (!mDef || env->ExceptionCheck()) break;
        disp = env->CallObjectMethod(wm, mDef);
        if (!disp || env->ExceptionCheck()) break;
        jclass cDisp = env->FindClass("android/view/Display");
        jmethodID mCaps = cDisp ? env->GetMethodID(cDisp, "getHdrCapabilities", "()Landroid/view/Display$HdrCapabilities;") : nullptr;
        if (cDisp) env->DeleteLocalRef(cDisp);
        if (!mCaps || env->ExceptionCheck()) break;
        caps = env->CallObjectMethod(disp, mCaps);
        if (env->ExceptionCheck()) break;
        if (!caps) { ok = true; break; }                // панель без HDR: getHdrCapabilities() может вернуть null
        jclass cCaps = env->FindClass("android/view/Display$HdrCapabilities");
        jmethodID mTypes = cCaps ? env->GetMethodID(cCaps, "getSupportedHdrTypes", "()[I") : nullptr;
        jmethodID mLum = cCaps ? env->GetMethodID(cCaps, "getDesiredMaxLuminance", "()F") : nullptr;
        if (cCaps) env->DeleteLocalRef(cCaps);
        if (!mTypes || !mLum || env->ExceptionCheck()) break;
        jintArray types = (jintArray)env->CallObjectMethod(caps, mTypes);
        if (env->ExceptionCheck()) break;
        if (types) { sup = env->GetArrayLength(types) > 0; env->DeleteLocalRef(types); }
        float lum = env->CallFloatMethod(caps, mLum);
        if (env->ExceptionCheck()) break;
        peak = lum > 0.0f && lum < 100000.0f ? (int)lum : 0;
        ok = true;
    } while (false);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (caps) env->DeleteLocalRef(caps);
    if (disp) env->DeleteLocalRef(disp);
    if (wm) env->DeleteLocalRef(wm);
    if (svc) env->DeleteLocalRef(svc);
    if (app) env->DeleteLocalRef(app);
    if (cAt) env->DeleteLocalRef(cAt);
    if (attached) g_vm->DetachCurrentThread();
    if (!ok) return false;
    *supported = sup;
    *peak_nits = peak;
    return true;
}

}  // namespace display
