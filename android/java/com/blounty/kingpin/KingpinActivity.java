// The game's activity: NativeActivity (runtime/platform/android.cpp does the work) shown
// immersive full screen, under display cutouts, at 60 Hz, with the screen kept on.
package com.blounty.kingpin;

import android.app.NativeActivity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.graphics.Color;
import android.view.Gravity;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.view.Display;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

public class KingpinActivity extends NativeActivity {
    private static native int nativeGetFps();
    private final Handler fpsHandler = new Handler(Looper.getMainLooper());
    private TextView fpsLabel;
    private final Runnable fpsTick = new Runnable() {
        @Override public void run() {
            fpsLabel.setText(nativeGetFps() + " FPS");
            fpsHandler.postDelayed(this, 500);
        }
    };
    @Override protected void onResume() {
        super.onResume(); fpsHandler.post(fpsTick);
    }
    @Override protected void onPause() {
        fpsHandler.removeCallbacks(fpsTick); super.onPause();
    }
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        fpsLabel = new TextView(this);
        fpsLabel.setText("— FPS"); fpsLabel.setTextSize(14);
        fpsLabel.setTextColor(Color.GREEN); fpsLabel.setBackgroundColor(0x99000000);
        fpsLabel.setPadding(12, 4, 12, 4);
        fpsLabel.setClickable(false); fpsLabel.setFocusable(false);
        FrameLayout.LayoutParams overlay = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP | Gravity.RIGHT);
        overlay.setMargins(0, 24, 48, 0);
        addContentView(fpsLabel, overlay);
        Window w = getWindow();
        w.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        WindowManager.LayoutParams lp = w.getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        // Prefer a 60 Hz mode at the current resolution for consistent pacing.
        Display display = getDisplay();
        if (display != null) {
            Display.Mode current = display.getMode(), best = null;
            for (Display.Mode m : display.getSupportedModes())
                if (m.getPhysicalWidth() == current.getPhysicalWidth() && m.getPhysicalHeight() == current.getPhysicalHeight()
                        && m.getRefreshRate() <= 61f && (best == null || m.getRefreshRate() > best.getRefreshRate()))
                    best = m;
            if (best != null)
                lp.preferredDisplayModeId = best.getModeId();
        }
        w.setAttributes(lp);
        w.setDecorFitsSystemWindows(false);
        immersive();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus)
            immersive();
    }

    private void immersive() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }
}
