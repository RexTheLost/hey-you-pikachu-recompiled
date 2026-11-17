package com.hyprecomp.pikachu;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.graphics.Color;
import android.net.Uri;
import android.provider.Settings;
import android.view.Gravity;
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
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

/**
 * Self-update: on start the launcher asks the update server (reports-worker/, filled by
 * tools/publish_android.py) for the latest build. If it is newer, it offers Update / Not now;
 * Update downloads the APK, checks its SHA-256 and hands it to Android's package installer,
 * which asks the player to confirm. Android only accepts it if it is signed like this app.
 */
final class Updater {
    static final String SERVER = "https://hyp-reports.pinball-leaderboard.workers.dev";
    private static final String ACTION = "com.hyprecomp.pikachu.INSTALL_STATUS";
    private static long skippedCode = 0;          // "Not now" for this run of the app

    interface Done { void run(); }

    private final Activity activity;
    private final Done proceed;                   // no update: carry on with the normal start
    private org.json.JSONObject latest;
    private boolean waitingForPermission;
    private TextView status;
    private BroadcastReceiver receiver;

    Updater(Activity activity, Done proceed) {
        this.activity = activity;
        this.proceed = proceed;
    }

    private long installedCode() {
        try {
            return activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0).getLongVersionCode();
        } catch (Exception e) {
            return Long.MAX_VALUE;
        }
    }

    /** Checks the server (a few seconds at most); shows the offer if newer, else proceeds. */
    void check(Done noUpdate) {
        new Thread(() -> {
            org.json.JSONObject info = null;
            try {
                HttpURLConnection c = (HttpURLConnection) new URL(SERVER + "/latest").openConnection();
                c.setConnectTimeout(4000);
                c.setReadTimeout(4000);
                if (c.getResponseCode() == 200) {
                    try (InputStream in = c.getInputStream()) {
                        java.io.ByteArrayOutputStream all = new java.io.ByteArrayOutputStream();
                        byte[] buf = new byte[4096];
                        for (int n; (n = in.read(buf)) > 0; ) all.write(buf, 0, n);
                        info = new org.json.JSONObject(new String(all.toByteArray(), StandardCharsets.UTF_8));
                    }
                }
                c.disconnect();
            } catch (Exception ignored) { }
            final org.json.JSONObject found = info;
            activity.runOnUiThread(() -> {
                long code = found == null ? 0 : found.optLong("versionCode", 0);
                if (code > installedCode() && code != skippedCode) {
                    latest = found;
                    offer();
                } else {
                    noUpdate.run();
                }
            });
        }).start();
    }

    private void offer() {
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setBackgroundColor(Color.rgb(250, 214, 60));
        box.setPadding(48, 48, 48, 48);
        TextView head = new TextView(activity);
        head.setText("Update available: " + latest.optString("versionName"));
        head.setTextSize(26);
        head.setTextColor(Color.rgb(60, 40, 10));
        head.setGravity(Gravity.CENTER);
        box.addView(head);
        status = new TextView(activity);
        String notes = latest.optString("notes", "");
        status.setText((notes.isEmpty() ? "" : notes + "\n\n") + String.format("%.1f MB. Your saves are kept.", latest.optLong("size") / 1e6));
        status.setTextSize(17);
        status.setTextColor(Color.rgb(60, 40, 10));
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, 24, 0, 24);
        box.addView(status);
        LinearLayout row = new LinearLayout(activity);
        row.setGravity(Gravity.CENTER);
        Button update = new Button(activity);
        update.setText("Update");
        update.setTextSize(18);
        Button later = new Button(activity);
        later.setText("Not now");
        later.setTextSize(18);
        update.setOnClickListener(v -> {
            update.setEnabled(false);
            later.setEnabled(false);
            start();
        });
        later.setOnClickListener(v -> {
            skippedCode = latest.optLong("versionCode");
            activity.recreate();                  // the start screen again, without the offer
        });
        row.addView(update);
        row.addView(later);
        box.addView(row);
        activity.setContentView(box);
    }

    private void start() {
        if (!activity.getPackageManager().canRequestPackageInstalls()) {
            // One time: Android's "Install unknown apps" switch for this app.
            status.setText("Android needs your OK once: turn on \"Allow from this source\", then come back.");
            waitingForPermission = true;
            activity.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                Uri.parse("package:" + activity.getPackageName())));
            return;
        }
        download();
    }

    /** Call from the activity's onResume. */
    void onResume() {
        if (!waitingForPermission) return;
        waitingForPermission = false;
        if (activity.getPackageManager().canRequestPackageInstalls()) {
            download();
        } else {
            failed("Installing updates was not allowed.");
        }
    }

    private void download() {
        status.setText("Downloading...");
        final long code = latest.optLong("versionCode");
        final String sha = latest.optString("sha256");
        final File file = new File(activity.getCacheDir(), "update.apk");
        new Thread(() -> {
            String error = null;
            try {
                HttpURLConnection c = (HttpURLConnection) new URL(SERVER + "/apk/" + code).openConnection();
                c.setConnectTimeout(10000);
                c.setReadTimeout(20000);
                if (c.getResponseCode() != 200) throw new Exception("server said " + c.getResponseCode());
                long total = latest.optLong("size", 1), got = 0, shown = 0;
                MessageDigest md = MessageDigest.getInstance("SHA-256");
                try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(file)) {
                    byte[] buf = new byte[65536];
                    for (int n; (n = in.read(buf)) > 0; ) {
                        out.write(buf, 0, n);
                        md.update(buf, 0, n);
                        got += n;
                        if (got - shown > 256 * 1024) {
                            shown = got;
                            final int pct = (int) (100 * got / Math.max(1, total));
                            activity.runOnUiThread(() -> status.setText("Downloading... " + pct + "%"));
                        }
                    }
                }
                c.disconnect();
                StringBuilder hex = new StringBuilder();
                for (byte b : md.digest()) hex.append(String.format("%02x", b));
                if (!hex.toString().equals(sha)) throw new Exception("the download was damaged (checksum)");
            } catch (Exception e) {
                error = e.getMessage();
            }
            final String err = error;
            activity.runOnUiThread(() -> {
                if (err != null) failed("Download failed: " + err);
                else install(file);
            });
        }).start();
    }

    private void install(File apk) {
        status.setText("Installing... confirm on the next screen.");
        try {
            PackageInstaller pi = activity.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(activity.getPackageName());
            params.setSize(apk.length());
            int id = pi.createSession(params);
            try (PackageInstaller.Session session = pi.openSession(id)) {
                try (InputStream in = new FileInputStream(apk); OutputStream out = session.openWrite("base.apk", 0, apk.length())) {
                    byte[] buf = new byte[65536];
                    for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                    session.fsync(out);
                }
                listen();
                Intent i = new Intent(ACTION).setPackage(activity.getPackageName());
                PendingIntent p = PendingIntent.getBroadcast(activity, id, i,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
                session.commit(p.getIntentSender());
            }
        } catch (Exception e) {
            failed("Install failed: " + e.getMessage());
        }
    }

    /** The installer reports back here: show its confirmation screen, or the error. */
    private void listen() {
        if (receiver != null) return;
        receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                int result = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                if (result == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    @SuppressWarnings("deprecation")
                    Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                    if (confirm != null) {
                        confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                        activity.startActivity(confirm);
                    }
                } else if (result == PackageInstaller.STATUS_SUCCESS) {
                    status.setText("Updated. Open the game again.");
                } else {
                    failed("Not installed: " + intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
                }
            }
        };
        IntentFilter filter = new IntentFilter(ACTION);
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            activity.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            activity.registerReceiver(receiver, filter);
        }
    }

    /** Call from the activity's onDestroy. */
    void release() {
        if (receiver != null) {
            try { activity.unregisterReceiver(receiver); } catch (Exception ignored) { }
            receiver = null;
        }
    }

    private void failed(String why) {
        status.setText(why + "\nYou can keep playing this version.");
        LinearLayout parent = (LinearLayout) status.getParent();
        Button go = new Button(activity);
        go.setText("Start the game");
        go.setOnClickListener(v -> {
            skippedCode = latest.optLong("versionCode");
            activity.recreate();
        });
        parent.addView(go);
    }
}
