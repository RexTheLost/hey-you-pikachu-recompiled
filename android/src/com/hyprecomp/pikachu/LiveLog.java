package com.hyprecomp.pikachu;

import android.content.Context;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/**
 * Live log: while switched on (Words list), every few seconds the new lines of the game log and
 * each Talk recording (a short WAV) go to the report server, where tools/live_log.py shows them.
 * Off by default; the corner shows LIVE while it runs.
 */
final class LiveLog {
    static volatile boolean on = false;
    private static Thread thread;
    private static final String SESSION = Long.toHexString(System.nanoTime() & 0xFFFFFFL);

    static synchronized void start(Context context) {
        if (thread != null) return;
        on = true;
        final File data = new File(context.getFilesDir(), "data");
        final File clips = new File(data, "clips");
        clips.mkdirs();
        HypNative.setClipDir(clips.getAbsolutePath());
        thread = new Thread(() -> run(new File(data, "log.txt"), clips), "HypLiveLog");
        thread.start();
    }

    static synchronized void stop() {
        on = false;
        try { HypNative.setClipDir(null); } catch (Throwable ignored) { }
        if (thread != null) {
            thread.interrupt();
            thread = null;
        }
    }

    private static void run(File log, File clips) {
        long offset = log.length();                    // from now on (the old part can go with Send logs)
        post("/live", ("{\"session\":\"" + SESSION + "\",\"text\":" + json("== live log on (" + android.os.Build.MODEL + ")\n") + "}").getBytes(StandardCharsets.UTF_8), "application/json");
        while (on && !Thread.currentThread().isInterrupted()) {
            try {
                long len = log.length();
                if (len < offset) offset = 0;          // a new game session started a fresh log
                if (len > offset) {
                    long n = Math.min(len - offset, 16000);
                    byte[] buf = new byte[(int) n];
                    try (RandomAccessFile f = new RandomAccessFile(log, "r")) {
                        f.seek(offset);
                        f.readFully(buf);
                    }
                    String text = new String(buf, StandardCharsets.UTF_8);
                    String body = "{\"session\":\"" + SESSION + "\",\"text\":" + json(text) + "}";
                    if (post("/live", body.getBytes(StandardCharsets.UTF_8), "application/json")) offset += n;
                }
                File[] wavs = clips.listFiles((d, name) -> name.endsWith(".wav"));
                if (wavs != null) {
                    Arrays.sort(wavs);
                    for (File w : wavs) {
                        if (System.currentTimeMillis() - w.lastModified() < 1000) continue; // still being written
                        byte[] bytes = new byte[(int) w.length()];
                        try (InputStream in = new FileInputStream(w)) {
                            int got = 0;
                            while (got < bytes.length) {
                                int r = in.read(bytes, got, bytes.length - got);
                                if (r < 0) break;
                                got += r;
                            }
                        }
                        String q = "/clip?session=" + SESSION + "&name=" + java.net.URLEncoder.encode(w.getName(), "UTF-8");
                        if (post(q, bytes, "audio/wav")) w.delete();
                    }
                }
                Thread.sleep(4000);
            } catch (InterruptedException e) {
                break;
            } catch (Exception e) {
                try { Thread.sleep(4000); } catch (InterruptedException ie) { break; }
            }
        }
    }

    private static String json(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            switch (c) {
                case '"': b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\r': break;
                case '\t': b.append("\\t"); break;
                default:
                    if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
                    else b.append(c);
            }
        }
        return b.append('"').toString();
    }

    private static boolean post(String path, byte[] body, String type) {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(Updater.SERVER + path).openConnection();
            c.setConnectTimeout(8000);
            c.setReadTimeout(10000);
            c.setRequestMethod("POST");
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", type);
            c.setRequestProperty("X-HYP-App", "hyp-android");
            try (OutputStream o = c.getOutputStream()) { o.write(body); }
            boolean ok = c.getResponseCode() == 200;
            c.disconnect();
            return ok;
        } catch (Exception e) {
            return false;
        }
    }
}
