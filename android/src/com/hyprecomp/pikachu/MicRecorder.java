package com.hyprecomp.pikachu;

import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.util.Log;

/**
 * Records the microphone while the game is on screen and hands the audio to the speech
 * recognizer (HypNative.micFeed). Stopped in the background (Android only gives background
 * apps silence anyway) and restarted when it delivers nothing but zeros.
 */
final class MicRecorder {
    private static final String TAG = "HypMic";
    private static final int RATE = 16000;
    private Thread thread;
    private volatile boolean running;

    synchronized void start() {
        if (running) return;
        running = true;
        thread = new Thread(this::run, "HypMic");
        thread.start();
    }

    synchronized void stop() {
        running = false;
        if (thread != null) {
            thread.interrupt();
            try { thread.join(1000); } catch (InterruptedException ignored) { }
            thread = null;
        }
    }

    private AudioRecord open() {
        int min = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_FLOAT);
        int size = Math.max(min, RATE * 4 / 2);              // half a second of floats
        int[] sources = { MediaRecorder.AudioSource.VOICE_RECOGNITION, MediaRecorder.AudioSource.MIC };
        for (int source : sources) {
            try {
                AudioRecord r = new AudioRecord(source, RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_FLOAT, size);
                if (r.getState() == AudioRecord.STATE_INITIALIZED) {
                    r.startRecording();
                    if (r.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING) return r;
                }
                r.release();
            } catch (Exception e) {
                Log.w(TAG, "AudioRecord source " + source + ": " + e);
            }
        }
        return null;
    }

    private void run() {
        float[] buf = new float[RATE / 50];                    // 20 ms
        AudioRecord rec = null;
        long zeroSince = 0, lastRestart = 0;
        while (running) {
            if (rec == null) {
                rec = open();
                lastRestart = System.currentTimeMillis();
                zeroSince = 0;
                if (rec == null) {                             // no permission / mic busy: retry later
                    try { Thread.sleep(2000); } catch (InterruptedException e) { break; }
                    continue;
                }
            }
            int n = rec.read(buf, 0, buf.length, AudioRecord.READ_BLOCKING);
            if (n < 0) {                                       // the recorder died: open a new one
                rec.release();
                rec = null;
                continue;
            }
            if (n > 0) HypNative.micFeed(buf, n);
            boolean allZero = true;
            for (int i = 0; i < n && allZero; i++) allZero = buf[i] == 0.0f;
            long now = System.currentTimeMillis();
            zeroSince = allZero ? (zeroSince == 0 ? now : zeroSince) : 0;
            boolean stale = zeroSince != 0 && now - zeroSince > 2000 && now - lastRestart > 5000;
            if (HypNative.takeMicRestart() || stale) {
                Log.i(TAG, "restarting the recorder" + (stale ? " (only silence)" : ""));
                rec.stop();
                rec.release();
                rec = null;
            }
        }
        if (rec != null) {
            try { rec.stop(); } catch (Exception ignored) { }
            rec.release();
        }
    }
}
