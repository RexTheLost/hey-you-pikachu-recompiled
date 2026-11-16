package com.hyprecomp.pikachu;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
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
        next();
    }

    private File file(String name) {
        return new File(getFilesDir(), name);
    }

    /** Do whichever step is missing, then start the game. */
    private void next() {
        if (!file(STORED_ROM).exists() && !file("baserom.us.z64").exists()) {
            text.setText("This app runs your own copy of Hey You, Pikachu! (USA, N64).\n"
                + "Choose the .z64 / .n64 / .v64 file. It is copied into the app once.");
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

    private String copy(Uri uri, File out) {
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream os = new FileOutputStream(out)) {
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
