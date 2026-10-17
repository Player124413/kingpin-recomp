package com.blounty.kingpin;

import android.content.Context;
import java.util.Locale;

/** Launcher-only language preference; does not alter the guest game's language. */
public final class UiText {
    private static volatile boolean russian = Locale.getDefault().getLanguage().equals("ru");
    private UiText() {}
    public static void init(Context context) {
        String fallback = Locale.getDefault().getLanguage().equals("ru") ? "ru" : "en";
        russian = context.getSharedPreferences("launcher", Context.MODE_PRIVATE)
            .getString("language", fallback).equals("ru");
    }
    public static void setLanguage(Context context, String language) {
        russian = "ru".equals(language);
        context.getSharedPreferences("launcher", Context.MODE_PRIVATE).edit()
            .putString("language", russian ? "ru" : "en").apply();
    }
    public static boolean isRussian() { return russian; }
    public static String text(String ru, String en) { return russian ? ru : en; }
}
