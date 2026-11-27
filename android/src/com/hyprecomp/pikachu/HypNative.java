package com.hyprecomp.pikachu;

/** Calls into libmain.so (src/android/android_glue.cpp). */
public final class HypNative {
    private HypNative() { }

    // N64 button bits (same as src/game/input.cpp)
    public static final int A = 0x8000, B = 0x4000, Z = 0x2000, START = 0x1000;
    public static final int DU = 0x0800, DD = 0x0400, DL = 0x0200, DR = 0x0100;
    public static final int L = 0x0020, R = 0x0010;
    public static final int CU = 0x0008, CD = 0x0004, CL = 0x0002, CR = 0x0001;

    public static native void setButtons(int buttons);
    public static native void setStick(float x, float y);
    /** Words the game listens for right now, as "group\tword". */
    public static native String[] getWords();
    public static native String getSetName();
    public static native boolean sayWord(String word);
    public static native String getHeard();
    /** 0 loading, 1 ready, 2 no model, 3 no microphone */
    public static native int getMicStatus();
    public static native boolean isListening();
    /** Microphone audio (16 kHz mono) for the speech recognizer. */
    public static native void micFeed(float[] data, int count);
    /** The recognizer got pure silence and wants the recorder restarted. */
    public static native boolean takeMicRestart();
    /** Live log: keep each Talk recording as a WAV in this folder (null = stop). */
    public static native void setClipDir(String dir);
    /** VR build: the next spoken bug report to post (JSON), or null. */
    public static native String takeReport();
    /** VR build: tell the VR panel how sending went. */
    public static native void reportResult(boolean ok, String message);

    /** The Quest (VR) build is its own package: com.hyprecomp.pikachu.vr. */
    public static boolean isVrPackage(android.content.Context c) {
        return c.getPackageName().endsWith(".vr");
    }
}
