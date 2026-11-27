// Android glue: JNI entry points for the Java side (android/src/com/hyprecomp/pikachu/).
//   ControlsView  -> setButtons / setStick       (on-screen controller, Talk = Z)
//   WordsDialog   -> getWords / sayWord           (the word picker)
//   HypActivity   -> getHeard / getMicStatus      (what the microphone heard, shown on screen)
#include <jni.h>

#include <atomic>
#include <set>
#include <string>
#include <vector>

#include "hyp_touch.h"
#include "hyp_vru.h"
#ifdef HYP_VR_ENABLED
#include "vr/hyp_vr.h"
#include "vr/vr_ui.h"
#endif

namespace {
    std::atomic<uint32_t> touch_buttons{ 0 };
    std::atomic<float> touch_x{ 0.0f }, touch_y{ 0.0f };

    std::string to_string(JNIEnv* env, jstring s) {
        if (s == nullptr) return {};
        const char* c = env->GetStringUTFChars(s, nullptr);
        std::string out = c != nullptr ? c : "";
        if (c != nullptr) env->ReleaseStringUTFChars(s, c);
        return out;
    }
}

uint16_t hyp::touch::buttons() {
    return (uint16_t)touch_buttons.load();
}

void hyp::touch::stick(float* x, float* y) {
    *x = touch_x.load();
    *y = touch_y.load();
}

extern "C" {

// Microphone audio from the app's AudioRecord (16 kHz mono float).
JNIEXPORT void JNICALL Java_com_hyprecomp_pikachu_HypNative_micFeed(JNIEnv* env, jclass, jfloatArray data, jint count) {
    jfloat* s = env->GetFloatArrayElements(data, nullptr);
    if (s == nullptr) return;
    hyp::vru::mic_feed(s, (size_t)count);
    env->ReleaseFloatArrayElements(data, s, JNI_ABORT);
}

// Live log: keep each Talk recording as a WAV in this folder (null/empty = stop).
JNIEXPORT void JNICALL Java_com_hyprecomp_pikachu_HypNative_setClipDir(JNIEnv* env, jclass, jstring dir) {
    std::string d;
    if (dir != nullptr) {
        const char* s = env->GetStringUTFChars(dir, nullptr);
        d = s ? s : "";
        if (s) env->ReleaseStringUTFChars(dir, s);
    }
    hyp::vru::mic_set_clip_dir(d);
}

JNIEXPORT jboolean JNICALL Java_com_hyprecomp_pikachu_HypNative_takeMicRestart(JNIEnv*, jclass) {
    return hyp::vru::mic_take_restart() ? JNI_TRUE : JNI_FALSE;
}

// VR (Quest build): spoken bug reports the Java side posts to the report inbox.
JNIEXPORT jstring JNICALL Java_com_hyprecomp_pikachu_HypNative_takeReport(JNIEnv* env, jclass) {
#ifdef HYP_VR_ENABLED
    std::string j = hyp::vr::take_report_json();
    return j.empty() ? nullptr : env->NewStringUTF(j.c_str());
#else
    (void)env;
    return nullptr;
#endif
}

JNIEXPORT void JNICALL Java_com_hyprecomp_pikachu_HypNative_reportResult(JNIEnv* env, jclass, jboolean ok, jstring message) {
#ifdef HYP_VR_ENABLED
    hyp::vr::ui::report_result(ok == JNI_TRUE, to_string(env, message));
#else
    (void)env; (void)ok; (void)message;
#endif
}

JNIEXPORT jboolean JNICALL Java_com_hyprecomp_pikachu_HypNative_isVr(JNIEnv*, jclass) {
#ifdef HYP_VR_ENABLED
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

JNIEXPORT void JNICALL Java_com_hyprecomp_pikachu_HypNative_setButtons(JNIEnv*, jclass, jint buttons) {
    touch_buttons = (uint32_t)buttons;
}

JNIEXPORT void JNICALL Java_com_hyprecomp_pikachu_HypNative_setStick(JNIEnv*, jclass, jfloat x, jfloat y) {
    touch_x = x;
    touch_y = y;
}

// The words the game is listening for right now, as "group\tword" (one entry per spoken form).
JNIEXPORT jobjectArray JNICALL Java_com_hyprecomp_pikachu_HypNative_getWords(JNIEnv* env, jclass) {
    std::vector<std::string> entries;
    std::set<std::string> seen;
    for (const auto& w : hyp::vru::dictionary()) {
        if (!seen.insert(w.display).second) continue;
        entries.push_back(hyp::vru::group_name(w.category, w.display) + "\t" + w.display);
    }
    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray out = env->NewObjectArray((jsize)entries.size(), string_class, nullptr);
    for (size_t i = 0; i < entries.size(); i++) {
        jstring s = env->NewStringUTF(entries[i].c_str());
        env->SetObjectArrayElement(out, (jsize)i, s);
        env->DeleteLocalRef(s);
    }
    return out;
}

// Name of the word set the game is using ("" if none matches).
JNIEXPORT jstring JNICALL Java_com_hyprecomp_pikachu_HypNative_getSetName(JNIEnv* env, jclass) {
    int set = hyp::vru::active_set();
    return env->NewStringUTF(set >= 0 ? hyp::vru::set_name(set).c_str() : "");
}

JNIEXPORT jboolean JNICALL Java_com_hyprecomp_pikachu_HypNative_sayWord(JNIEnv* env, jclass, jstring word) {
    return hyp::vru::say_word(to_string(env, word)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_com_hyprecomp_pikachu_HypNative_getHeard(JNIEnv* env, jclass) {
    return env->NewStringUTF(hyp::vru::mic_last_heard(4.0).c_str());
}

// 0 loading, 1 ready, 2 no model, 3 no microphone
JNIEXPORT jint JNICALL Java_com_hyprecomp_pikachu_HypNative_getMicStatus(JNIEnv*, jclass) {
    return (jint)hyp::vru::mic_status();
}

JNIEXPORT jboolean JNICALL Java_com_hyprecomp_pikachu_HypNative_isListening(JNIEnv*, jclass) {
    return hyp::vru::listening() ? JNI_TRUE : JNI_FALSE;
}

}
