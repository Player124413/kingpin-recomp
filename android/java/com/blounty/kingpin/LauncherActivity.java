package com.blounty.kingpin;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.ScrollView;
import java.io.*;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** SAF folder import; no broad storage permission and no native code until Play. */
public class LauncherActivity extends Activity {
    private TextView status;
    private Button choose, play;
    private boolean busy;
    private File root;
    private static final int PICK = 1;
    private static final String[] REQUIRED = {
        "kingpin.exe", "ref_gl.dll", "track1.iso", "main/gamex86.dll", "main/pak0.pak"
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        root = getExternalFilesDir(null);
        if (root == null) root = getFilesDir();
        // Recover an interrupted directory swap before inspecting the installed game.
        File installed = new File(root, "imported-game"), backup = new File(root, ".import-backup");
        if (!installed.exists() && backup.exists()) backup.renameTo(installed);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        int pad = (int)(24 * getResources().getDisplayMetrics().density);
        layout.setPadding(pad, pad, pad, pad);
        TextView title = new TextView(this);
        title.setText("Kingpin — Life of Crime"); title.setTextSize(26);
        layout.addView(title);
        TextView help = new TextView(this);
        help.setText("Выберите папку с извлечённой игрой: kingpin.exe, ref_gl.dll, track1.iso и main/ (с pak0.pak и gamex86.dll).\n"
            + "Не ZIP и не папку только с BIN/CUE. kpintro.exe и cd/ с музыкой копируются, если есть.\n"
            + "Нужна свободная память для полной копии игры. Повторный импорт заменяет импортированную игру, включая её сохранения.");
        if (hasBundle()) help.setText("Игровые данные встроены в APK. При первом запуске они распакуются автоматически.\n"
            + "Дождитесь окончания распаковки и нажмите «Играть». Нужна свободная память для копии игровых данных.\n"
            + "Выбор папки нужен только для ручной замены игры; повторный импорт заменит её сохранения.");
        layout.addView(help);
        choose = new Button(this); choose.setText("Выбрать папку игры"); layout.addView(choose);
        play = new Button(this); play.setText("Играть"); layout.addView(play);
        status = new TextView(this); layout.addView(status);
        ScrollView scroll = new ScrollView(this); scroll.addView(layout);
        setContentView(scroll);
        choose.setOnClickListener(v -> new AlertDialog.Builder(this)
            .setMessage("Импорт заменит ранее импортированную игру и её сохранения. Продолжить?")
            .setNegativeButton("Отмена", null).setPositiveButton("Выбрать папку", (d, w) -> {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                startActivityForResult(intent, PICK);
            }).show());
        play.setOnClickListener(v -> { if (!busy && ready()) startActivity(new Intent(this, KingpinActivity.class)); });
        update();
        if (!ready() && hasBundle()) installBundle();
    }

    private static File resolveIgnoringCase(File dir, String path) {
        File current = dir;
        for (String part : path.split("/")) {
            File[] children = current.listFiles();
            if (children == null) return null;
            File match = null;
            for (File child : children) {
                if (child.getName().equalsIgnoreCase(part)) {
                    if (match != null) return null; // ambiguous names on case-sensitive storage
                    match = child;
                }
            }
            if (match == null) return null;
            current = match;
        }
        return current;
    }
    private boolean valid(File dir) {
        for (String path : REQUIRED) {
            File f = resolveIgnoringCase(dir, path);
            if (f == null || !f.isFile() || f.length() == 0) return false;
        }
        return true;
    }
    private boolean ready() {
        File imported = new File(root, "imported-game");
        return valid(imported.exists() ? imported : root);
    }
    private void update() {
        choose.setEnabled(!busy); play.setEnabled(!busy && ready());
        status.setText(ready() ? "Игра готова к запуску." : "Сначала импортируйте игровые данные.");
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK || result != RESULT_OK || data == null || data.getData() == null || busy) return;
        Uri tree = data.getData();
        busy = true; update(); status.setText("Копирование… Не закрывайте приложение.");
        new Thread(() -> {
            String error = null;
            File stage = new File(root, ".import-stage");
            try {
                remove(stage); mkdir(stage);
                Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
                List<Entry> entries = children(tree, doc);
                for (String required : new String[]{"kingpin.exe", "ref_gl.dll", "track1.iso", "main"}) {
                    if (find(entries, required) == null)
                        throw new IOException("Нет " + required + ". Выберите папку с извлечённой игрой, не BIN/CUE.");
                }
                for (String name : new String[]{"kingpin.exe", "ref_gl.dll", "track1.iso", "main", "kpintro.exe", "cd"}) {
                    Entry entry = find(entries, name);
                    if (entry != null) copy(tree, entry, new File(stage, name), 0);
                }
                if (!valid(stage)) throw new IOException("Неполные данные: нужны непустые EXE, DLL, track1.iso, main/gamex86.dll и main/pak0.pak.");
                publish(stage);
            } catch (Exception e) { error = "Импорт не выполнен: " + e.getMessage(); }
            finally { try { remove(stage); } catch (IOException ignored) {} }
            final String message = error;
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                busy = false; update();
                if (message != null) status.setText(message);
            });
        }, "game-import").start();
    }
    private void publish(File stage) throws IOException {
        for (String name : new String[]{"port.cfg", "gamepad.cfg"}) {
            try (InputStream in = getAssets().open(name); OutputStream out = new FileOutputStream(new File(stage, "main/" + name))) {
                transfer(in, out);
            }
        }
        try (Writer out = new OutputStreamWriter(new FileOutputStream(new File(stage, "main/autoexec.cfg"), true), "UTF-8")) {
            out.write("\nexec port.cfg\nexec gamepad.cfg\n");
        }
        File installed = new File(root, "imported-game"), backup = new File(root, ".import-backup");
        remove(backup);
        if (installed.exists() && !installed.renameTo(backup)) throw new IOException("Не удалось сохранить предыдущую игру.");
        if (!stage.renameTo(installed)) {
            backup.renameTo(installed);
            throw new IOException("Не удалось завершить импорт.");
        }
        // A failed cleanup must not report a successfully committed import as failed.
        try { remove(backup); } catch (IOException ignored) {}
    }
    private boolean hasBundle() {
        try (InputStream in = getAssets().open("game-data.zip")) { return true; }
        catch (IOException e) { return false; }
    }
    private void installBundle() {
        busy = true; update();
        status.setText("Первый запуск: распаковка встроенной игры… Не закрывайте приложение.");
        new Thread(() -> {
            String error = null;
            File stage = new File(root, ".import-stage");
            try {
                remove(stage); mkdir(stage);
                String prefix = stage.getCanonicalPath() + File.separator;
                long total = 0;
                try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(getAssets().open("game-data.zip")))) {
                    ZipEntry entry;
                    byte[] buffer = new byte[256 * 1024];
                    while ((entry = zip.getNextEntry()) != null) {
                        File target = new File(stage, entry.getName());
                        if (!target.getCanonicalPath().startsWith(prefix)) throw new IOException("Недопустимый путь в архиве.");
                        if (entry.isDirectory()) { mkdir(target); continue; }
                        mkdir(target.getParentFile());
                        try (OutputStream out = new FileOutputStream(target)) {
                            int n;
                            while ((n = zip.read(buffer)) != -1) {
                                total += n;
                                if (total > 4L * 1024 * 1024 * 1024) throw new IOException("Архив слишком большой.");
                                out.write(buffer, 0, n);
                            }
                        }
                        final long mb = total / (1024 * 1024);
                        runOnUiThread(() -> { if (!isDestroyed()) status.setText("Распаковка игры: " + mb + " МБ"); });
                    }
                }
                if (!valid(stage)) throw new IOException("Во встроенном архиве не хватает игровых файлов.");
                publish(stage);
            } catch (Exception e) { error = "Распаковка не выполнена: " + e.getMessage() + " Закройте и откройте приложение, чтобы повторить."; }
            finally { try { remove(stage); } catch (IOException ignored) {} }
            final String message = error;
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                busy = false; update();
                if (message != null) status.setText(message);
            });
        }, "bundled-game-install").start();
    }
    private static class Entry {
        String name; Uri uri; boolean directory;
        Entry(String n, Uri u, boolean d) { name=n; uri=u; directory=d; }
    }
    private Entry find(List<Entry> entries, String name) {
        for (Entry e : entries) if (e.name.equalsIgnoreCase(name)) return e;
        return null;
    }
    private List<Entry> children(Uri tree, Uri parent) throws IOException {
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getDocumentId(parent));
        List<Entry> result = new ArrayList<>();
        try (Cursor c = getContentResolver().query(uri, new String[]{
            DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            if (c == null) throw new IOException("Папка недоступна.");
            while (c.moveToNext()) {
                String name = c.getString(1);
                if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") || name.contains("/") || name.contains("\\"))
                    throw new IOException("Недопустимое имя файла.");
                result.add(new Entry(name, DocumentsContract.buildDocumentUriUsingTree(tree, c.getString(0)),
                    DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2))));
            }
        }
        return result;
    }
    private void copy(Uri tree, Entry entry, File to, int depth) throws IOException {
        if (depth > 32) throw new IOException("Слишком много вложенных папок.");
        runOnUiThread(() -> { if (!isDestroyed()) status.setText("Копирование: " + entry.name); });
        if (entry.directory) {
            mkdir(to);
            for (Entry child : children(tree, entry.uri)) copy(tree, child, new File(to, child.name), depth + 1);
        } else {
            try (InputStream in = getContentResolver().openInputStream(entry.uri); OutputStream out = new FileOutputStream(to)) {
                if (in == null) throw new IOException("Файл недоступен.");
                transfer(in, out);
            }
        }
    }
    private static void transfer(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[256 * 1024]; int n;
        while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
    }
    private static void mkdir(File f) throws IOException {
        if (!f.isDirectory() && !f.mkdirs()) throw new IOException("Не удалось создать папку (проверьте свободное место).");
    }
    private static void remove(File f) throws IOException {
        if (!f.exists()) return;
        if (f.isDirectory()) {
            File[] files = f.listFiles();
            if (files == null) throw new IOException("Папка недоступна.");
            for (File child : files) remove(child);
        }
        if (!f.delete()) throw new IOException("Не удалось удалить временные данные.");
    }
    @Override public void onBackPressed() { if (!busy) super.onBackPressed(); }
}
