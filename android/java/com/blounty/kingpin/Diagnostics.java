package com.blounty.kingpin;

import android.app.ActivityManager;
import android.app.Application;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.List;

/** Small app-private logs: no storage permissions and no automatic uploads. */
public class Diagnostics extends Application {
    private static File folder;
    @Override public void onCreate() {
        super.onCreate();
        UiText.init(this);
        folder = new File(getFilesDir(), "logs");
        folder.mkdirs();
        final Thread.UncaughtExceptionHandler original = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            StringWriter text = new StringWriter();
            error.printStackTrace(new PrintWriter(text));
            record("UNCAUGHT " + thread.getName() + "\n" + text);
            if (original != null) original.uncaughtException(thread, error);
            else android.os.Process.killProcess(android.os.Process.myPid());
        });
        record("App started; " + Build.MANUFACTURER + " " + Build.MODEL
            + "; Android " + Build.VERSION.RELEASE + "; SDK " + Build.VERSION.SDK_INT
            + "; ABI " + java.util.Arrays.toString(Build.SUPPORTED_ABIS));
    }
    public static synchronized void record(String message) {
        if (folder == null) return;
        File log = new File(folder, "java.log");
        try {
            if (log.length() > 256 * 1024) {
                File old = new File(folder, "java-previous.log");
                if (old.exists()) old.delete();
                log.renameTo(old);
            }
            try (Writer out = new OutputStreamWriter(new FileOutputStream(log, true), StandardCharsets.UTF_8)) {
                out.write(new java.util.Date() + " " + message + "\n");
            }
        } catch (IOException ignored) { /* Diagnostics must never crash the game. */ }
    }
    private static String tail(File path) throws IOException {
        if (!path.isFile()) return UiText.text("(нет записей)\n", "(no entries)\n");
        try (RandomAccessFile file = new RandomAccessFile(path, "r")) {
            long start = Math.max(0, file.length() - 48 * 1024);
            file.seek(start);
            byte[] bytes = new byte[(int)(file.length() - start)];
            file.readFully(bytes);
            return new String(bytes, StandardCharsets.UTF_8);
        }
    }
    public static String report(Context context) {
        StringBuilder text = new StringBuilder("Kingpin diagnostics\n");
        try {
            text.append("App ").append(context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName).append('\n');
            ActivityManager manager = (ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
            List<ApplicationExitInfo> exits = manager.getHistoricalProcessExitReasons(context.getPackageName(), 0, 3);
            for (ApplicationExitInfo exit : exits) {
                text.append("\nAndroid exit: ").append(new java.util.Date(exit.getTimestamp()))
                    .append(" reason=").append(exit.getReason()).append(" status=").append(exit.getStatus())
                    .append(" description=").append(exit.getDescription()).append('\n');
                // Availability varies by OS/device. Limit the trace to avoid a huge clipboard.
                // Native tombstones can be binary protobuf, not displayable text.
                try (InputStream in = exit.getReason() == ApplicationExitInfo.REASON_ANR ? exit.getTraceInputStream() : null) {
                    if (in != null) {
                        byte[] b = new byte[16 * 1024];
                        ByteArrayOutputStream out = new ByteArrayOutputStream();
                        int n;
                        while (out.size() < 32 * 1024 && (n = in.read(b, 0, Math.min(b.length, 32 * 1024 - out.size()))) != -1) out.write(b, 0, n);
                        text.append(out.toString("UTF-8")).append('\n');
                    }
                } catch (IOException | RuntimeException e) { text.append("Trace unavailable\n"); }
            }
        } catch (Exception e) { text.append("Android exit info unavailable: ").append(e).append('\n'); }
        for (String name : new String[]{"java-previous.log", "java.log", "native-previous.log", "native-current.log"}) {
            text.append("\n=== ").append(name).append(" ===\n");
            try { text.append(tail(new File(context.getFilesDir(), "logs/" + name))); }
            catch (IOException e) { text.append(e).append('\n'); }
        }
        return text.toString();
    }
}
