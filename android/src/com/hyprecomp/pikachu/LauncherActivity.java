package com.hyprecomp.pikachu;

import android.Manifest;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.BufferedInputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;

/**
 * First screen. The game itself needs three things before it starts:
 *  - the ROM: picked once with Android's file picker and copied to files/baserom.us.z64
 *    (the game checks it, keeps its own copy in files/data and deletes this one);
 *  - the voice model: unpacked from the APK if it was packed in, else downloaded once from
 *    the whisper.cpp model page (checked by SHA-1); the player can skip it and use Words;
 *  - the microphone permission (asked once; without it Talk still works with the word list).
 */
public class LauncherActivity extends Activity {
    private static final int PICK_ROM = 1, ASK_MIC = 2;
    private static final String STORED_ROM = "data/hyp.n64.us.1.0.z64"; // librecomp's stored copy
    private static final String MODEL = "data/models/ggml-base.en.bin";
    private static final String MODEL_URL = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin";
    private static final String MODEL_SHA1 = "137c40403d78fd54d454da0f9bd998f78703390c";
    private static final long MODEL_SIZE = 147964211L;

    private TextView text;
    private Button pick, download, skip;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setBackgroundColor(Color.rgb(250, 214, 60));
        box.setPadding(48, 48, 48, 48);
        TextView title = new TextView(this);
        title.setText("Hey You, Pikachu!");
        title.setTextSize(30);
        title.setTextColor(Color.rgb(60, 40, 10));
        title.setGravity(Gravity.CENTER);
        box.addView(title);
        text = new TextView(this);
        text.setTextSize(17);
        text.setTextColor(Color.rgb(60, 40, 10));
        text.setGravity(Gravity.CENTER);
        text.setPadding(0, 24, 0, 24);
        box.addView(text);
        pick = new Button(this);
        pick.setText("Choose your ROM file");
        pick.setTextSize(18);
        pick.setOnClickListener(v -> pickRom());
        box.addView(pick);
        download = new Button(this);
        download.setText("Download the voice recognizer (148 MB)");
        download.setTextSize(18);
        download.setOnClickListener(v -> downloadModel());
        box.addView(download);
        skip = new Button(this);
        skip.setText("Skip: use the word list instead");
        skip.setOnClickListener(v -> {
            getPreferences(MODE_PRIVATE).edit().putBoolean("skip_model", true).apply();
            next();
        });
        box.addView(skip);
        setContentView(box);
        if (file(CRASH).exists() && file(CRASH).lastModified() < installedAt()) {
            // From a version installed before this one: not news any more.
            file(CRASH).renameTo(file("data/crash_last.txt"));
        }
        if (file(CRASH).exists()) showCrash();
        else if (!showUnexpectedExit()) {
            text.setText("Checking for updates...");
            updater.check(this::next);
        }
    }

    private final Updater updater = new Updater(this, this::next);

    @Override
    protected void onResume() {
        super.onResume();
        updater.onResume();
    }

    @Override
    protected void onDestroy() {
        updater.release();
        super.onDestroy();
    }

    /**
     * Android 11+: if the game's process ended abnormally last time without a crash report (killed
     * for memory, not responding, killed by a signal...), say why and offer the logs. Once per exit.
     */
    private boolean showUnexpectedExit() {
        if (android.os.Build.VERSION.SDK_INT < 30) return false;
        try {
            android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
            List<android.app.ApplicationExitInfo> exits = am.getHistoricalProcessExitReasons(getPackageName(), 0, 1);
            if (exits.isEmpty()) return false;
            android.app.ApplicationExitInfo e = exits.get(0);
            long seen = getPreferences(MODE_PRIVATE).getLong("seen_exit", 0);
            if (e.getTimestamp() <= seen || e.getTimestamp() < installedAt()) return false;
            getPreferences(MODE_PRIVATE).edit().putLong("seen_exit", e.getTimestamp()).apply();
            String why;
            switch (e.getReason()) {
                case android.app.ApplicationExitInfo.REASON_LOW_MEMORY: why = "Android closed it to free memory"; break;
                case android.app.ApplicationExitInfo.REASON_ANR: why = "Android closed it because it stopped responding"; break;
                case android.app.ApplicationExitInfo.REASON_CRASH: why = "it crashed (Java)"; break;
                case android.app.ApplicationExitInfo.REASON_CRASH_NATIVE: why = "it crashed (native)"; break;
                case android.app.ApplicationExitInfo.REASON_SIGNALED: why = "it was killed by signal " + e.getStatus(); break;
                case android.app.ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: why = "Android closed it for using too much power or CPU"; break;
                case android.app.ApplicationExitInfo.REASON_PERMISSION_CHANGE: why = "a permission was changed"; break;
                case android.app.ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: why = "it failed to start"; break;
                case android.app.ApplicationExitInfo.REASON_DEPENDENCY_DIED: why = "something it depended on stopped"; break;
                case android.app.ApplicationExitInfo.REASON_OTHER: why = "Android closed it (other reason)"; break;
                default: return false;                      // closed normally (by the player or the game)
            }
            StringBuilder r = new StringBuilder();
            r.append("Pikachu Android exit report (version ").append(versionName()).append(", Android ")
                .append(android.os.Build.VERSION.RELEASE).append(", ").append(android.os.Build.MODEL).append(")\n");
            r.append("closed ").append(android.text.format.DateFormat.format("yyyy-MM-dd HH:mm:ss", e.getTimestamp()))
                .append(": ").append(why).append("\n");
            r.append("reason ").append(e.getReason()).append(", status ").append(e.getStatus())
                .append(", importance ").append(e.getImportance())
                .append(", memory ").append(e.getPss() / 1024).append(" MB (pss) / ").append(e.getRss() / 1024).append(" MB (rss)\n");
            if (e.getDescription() != null) r.append("description: ").append(e.getDescription()).append("\n");
            r.append("\n--- game log ---\n").append(readText(file(LOG), 6000));
            r.append("\n--- voice log ---\n").append(readText(file("data/voice.log"), 3000));
            showReport("The game closed unexpectedly last time: " + why + ".", "exit", "exit:" + e.getTimestamp(),
                r.toString(), this::recreate);
            return true;
        } catch (Exception ex) {
            return false;
        }
    }

    static final String CRASH = "data/crash.txt", LOG = "data/log.txt";

    /** The game crashed last time: show the report (crash.txt + the end of log.txt) to copy. */
    private void showCrash() {
        StringBuilder r = new StringBuilder();
        r.append("Pikachu Android crash report (version ").append(versionName()).append(", Android ")
            .append(android.os.Build.VERSION.RELEASE).append(", ").append(android.os.Build.MODEL).append(")\n");
        r.append("crashed ").append(android.text.format.DateFormat.format("yyyy-MM-dd HH:mm:ss", file(CRASH).lastModified()))
            .append(", this version installed ").append(android.text.format.DateFormat.format("yyyy-MM-dd HH:mm:ss", installedAt()))
            .append("\n\n");
        r.append(readText(file(CRASH), 12000));
        r.append("\n--- log ---\n").append(readText(file(LOG), 6000));
        r.append("\n--- voice log ---\n").append(readText(file("data/voice.log"), 3000));
        showReport("The game crashed last time.", "crash", "crash:" + file(CRASH).lastModified(), r.toString(), () -> {
            file(CRASH).renameTo(file("data/crash_last.txt"));
            recreate();
        });
    }

    /** A report screen with Copy report and Start the game. */
    private void showReport(String headline, String kind, String id, final String report, final Runnable start) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackgroundColor(Color.rgb(250, 214, 60));
        box.setPadding(32, 24, 32, 24);
        TextView head = new TextView(this);
        head.setText(headline + " Sending the report to Claude...");
        head.setTextSize(18);
        head.setTextColor(Color.rgb(60, 40, 10));
        box.addView(head);
        sendReport(kind, id, report, ok -> head.setText(headline + (ok
            ? " The report was sent to Claude automatically."
            : " The report could not be sent (no internet?). Tap Copy report and paste it to Claude.")));
        LinearLayout row = new LinearLayout(this);
        Button copy = new Button(this);
        copy.setText("Copy report");
        copy.setOnClickListener(v -> {
            ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            cm.setPrimaryClip(ClipData.newPlainText("crash report", report));
            head.setText("Copied. Paste it into the chat.");
        });
        row.addView(copy);
        Button go = new Button(this);
        go.setText("Start the game");
        go.setOnClickListener(v -> start.run());
        row.addView(go);
        box.addView(row);
        ScrollView sv = new ScrollView(this);
        TextView body = new TextView(this);
        body.setText(report);
        body.setTextSize(11);
        body.setTypeface(android.graphics.Typeface.MONOSPACE);
        body.setTextColor(Color.rgb(30, 20, 5));
        body.setTextIsSelectable(true);
        sv.addView(body);
        box.addView(sv);
        setContentView(box);
    }

    static final String REPORT_INBOX = "https://hyp-reports.pinball-leaderboard.workers.dev/report";

    /**
     * Sends a report to the report inbox (reports-worker/) once per id, on a background thread;
     * done(true) on the UI thread when it got there (or was sent before).
     */
    private void sendReport(String kind, String id, String report, java.util.function.Consumer<Boolean> done) {
        final android.content.SharedPreferences prefs = getPreferences(MODE_PRIVATE);
        if (prefs.getStringSet("sent_reports", new java.util.HashSet<>()).contains(id)) {
            done.accept(true);
            return;
        }
        final String version = versionName();
        new Thread(() -> {
            boolean ok = postReport(kind, version, report);
            if (ok) {
                java.util.Set<String> sent = new java.util.HashSet<>(prefs.getStringSet("sent_reports", new java.util.HashSet<>()));
                sent.add(id);
                prefs.edit().putStringSet("sent_reports", sent).apply();
            }
            final boolean result = ok;
            runOnUiThread(() -> done.accept(result));
        }).start();
    }

    /** Posts one report to the inbox (blocking: call it off the UI thread). True if it got there. */
    static boolean postReport(String kind, String version, String text) {
        try {
            org.json.JSONObject body = new org.json.JSONObject();
            body.put("kind", kind);
            body.put("version", version);
            body.put("device", android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL + ", Android " + android.os.Build.VERSION.RELEASE);
            body.put("text", text);
            byte[] bytes = body.toString().getBytes(StandardCharsets.UTF_8);
            HttpURLConnection c = (HttpURLConnection) new URL(REPORT_INBOX).openConnection();
            c.setConnectTimeout(10000);
            c.setReadTimeout(15000);
            c.setRequestMethod("POST");
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json");
            c.setRequestProperty("X-HYP-App", "hyp-android");
            try (OutputStream o = c.getOutputStream()) { o.write(bytes); }
            boolean ok = c.getResponseCode() == 200;
            c.disconnect();
            return ok;
        } catch (Exception e) {
            return false;
        }
    }

    /** When this version of the app was installed (or updated). */
    private long installedAt() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
        } catch (Exception e) {
            return 0;
        }
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    /** The last maxChars characters of a text file ("" if missing). */
    static String readText(File f, int maxChars) {
        try {
            String t = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
            return t.length() > maxChars ? "..." + t.substring(t.length() - maxChars) : t;
        } catch (Exception e) {
            return "(no " + f.getName() + ")\n";
        }
    }

    private File file(String name) {
        return new File(getFilesDir(), name);
    }

    /** Do whichever step is missing, then start the game. */
    private void next() {
        if (!file(STORED_ROM).exists() && !file("baserom.us.z64").exists()) {
            text.setText("This app runs your own copy of Hey You, Pikachu! (USA, N64).\n"
                + "Choose the .z64 / .n64 / .v64 file (or a .zip with it inside). It is copied into the app once.");
            pick.setVisibility(View.VISIBLE);
            return;
        }
        pick.setVisibility(View.GONE);
        download.setVisibility(View.GONE);
        skip.setVisibility(View.GONE);
        if (!file(MODEL).exists() && !getPreferences(MODE_PRIVATE).getBoolean("skip_model", false)) {
            if (hasModelAsset()) {
                unpackModel();
            } else {
                text.setText("Pikachu understands you through a speech recognizer that runs on the phone "
                    + "(whisper, works offline). It is downloaded once from huggingface.co.\n"
                    + "Without it, tap Words to pick what to say.");
                download.setVisibility(View.VISIBLE);
                skip.setVisibility(View.VISIBLE);
            }
            return;
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED
                && !getPreferences(MODE_PRIVATE).getBoolean("asked_mic", false)) {
            getPreferences(MODE_PRIVATE).edit().putBoolean("asked_mic", true).apply();
            text.setText("Pikachu listens through the microphone while you hold Talk.");
            requestPermissions(new String[] { Manifest.permission.RECORD_AUDIO }, ASK_MIC);
            return;
        }
        startActivity(new Intent(this, HypActivity.class));
        finish();
    }

    private void pickRom() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_ROM);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_ROM || result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        text.setText("Copying the ROM...");
        pick.setVisibility(View.GONE);
        new Thread(() -> {
            String err = copy(uri, file("baserom.us.z64"));
            runOnUiThread(() -> {
                if (err != null) text.setText("Couldn't copy that file: " + err);
                next();
            });
        }).start();
    }

    /** Copy the picked file; a .zip is opened and the first .z64/.n64/.v64 inside is used. */
    private String copy(Uri uri, File out) {
        try (InputStream raw = new BufferedInputStream(getContentResolver().openInputStream(uri));
             OutputStream os = new FileOutputStream(out)) {
            InputStream in = raw;
            raw.mark(4);
            boolean zip = raw.read() == 'P' && raw.read() == 'K';
            raw.reset();
            if (zip) {
                ZipInputStream z = new ZipInputStream(raw);
                ZipEntry e;
                while ((e = z.getNextEntry()) != null) {
                    String n = e.getName().toLowerCase();
                    if (!e.isDirectory() && (n.endsWith(".z64") || n.endsWith(".n64") || n.endsWith(".v64"))) break;
                }
                if (e == null) throw new Exception("there is no .z64 / .n64 / .v64 file in that zip");
                in = z;
            }
            byte[] buf = new byte[1 << 16];
            long total = 0;
            for (int n; (n = in.read(buf)) > 0; ) {
                os.write(buf, 0, n);
                total += n;
                if (total > 128L << 20) throw new Exception("too big for an N64 ROM");
            }
            return null;
        } catch (Exception e) {
            out.delete();
            return e.getMessage();
        }
    }

    private boolean hasModelAsset() {
        try (InputStream in = getAssets().open("ggml-base.en.bin")) {
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private void downloadModel() {
        download.setVisibility(View.GONE);
        skip.setVisibility(View.GONE);
        text.setText("Downloading the voice recognizer...");
        new Thread(() -> {
            File out = file(MODEL);
            File tmp = new File(out.getPath() + ".part");
            String err = null;
            try {
                out.getParentFile().mkdirs();
                HttpURLConnection c = (HttpURLConnection) new URL(MODEL_URL).openConnection();
                c.setInstanceFollowRedirects(true);
                c.setConnectTimeout(20000);
                c.setReadTimeout(30000);
                if (c.getResponseCode() != 200) throw new Exception("server said " + c.getResponseCode());
                MessageDigest sha = MessageDigest.getInstance("SHA-1");
                try (InputStream in = c.getInputStream(); OutputStream os = new FileOutputStream(tmp)) {
                    byte[] buf = new byte[1 << 16];
                    long got = 0, shown = 0;
                    for (int n; (n = in.read(buf)) > 0; ) {
                        os.write(buf, 0, n);
                        sha.update(buf, 0, n);
                        got += n;
                        if (got - shown > (2 << 20)) {
                            shown = got;
                            final int pct = (int) (100 * got / MODEL_SIZE);
                            runOnUiThread(() -> text.setText("Downloading the voice recognizer... " + pct + "%"));
                        }
                    }
                }
                StringBuilder hex = new StringBuilder();
                for (byte b : sha.digest()) hex.append(String.format("%02x", b));
                if (!hex.toString().equals(MODEL_SHA1)) throw new Exception("the file was damaged, try again");
                if (!tmp.renameTo(out)) throw new Exception("couldn't save it");
            } catch (Exception e) {
                tmp.delete();
                err = e.getMessage();
            }
            final String error = err;
            runOnUiThread(() -> {
                next();
                if (error != null) text.setText("Download failed: " + error + "\n\n" + text.getText());
            });
        }).start();
    }

    private void unpackModel() {
        text.setText("Unpacking the voice recognizer (once, about 140 MB)...");
        new Thread(() -> {
            File out = file(MODEL);
            File tmp = new File(out.getPath() + ".part");
            boolean ok = false;
            try {
                out.getParentFile().mkdirs();
                try (InputStream in = getAssets().open("ggml-base.en.bin");
                     OutputStream os = new FileOutputStream(tmp)) {
                    byte[] buf = new byte[1 << 20];
                    for (int n; (n = in.read(buf)) > 0; ) os.write(buf, 0, n);
                }
                ok = tmp.renameTo(out);
            } catch (Exception e) {
                tmp.delete();
            }
            final boolean done = ok;
            runOnUiThread(() -> {
                if (!done) {
                    // The game still runs: Talk falls back to the word list.
                    getPreferences(MODE_PRIVATE).edit().putBoolean("no_model", true).apply();
                    startActivity(new Intent(this, HypActivity.class));
                    finish();
                } else {
                    next();
                }
            });
        }).start();
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] perms, int[] results) {
        next();
    }
}
