// The game's activity: NativeActivity (runtime/platform/android.cpp does the work) shown
// immersive full screen, under display cutouts, at up to 120 Hz, with the screen kept on.
package com.blounty.kingpin;

import android.app.NativeActivity;
import android.os.Bundle;
import android.view.Display;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

public class KingpinActivity extends NativeActivity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Window w = getWindow();
        w.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        WindowManager.LayoutParams lp = w.getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        // Prefer a 120 Hz mode at the current resolution (the game runs up to 125 fps).
        Display display = getDisplay();
        if (display != null) {
            Display.Mode current = display.getMode(), best = null;
            for (Display.Mode m : display.getSupportedModes())
                if (m.getPhysicalWidth() == current.getPhysicalWidth() && m.getPhysicalHeight() == current.getPhysicalHeight()
                        && m.getRefreshRate() <= 121f && (best == null || m.getRefreshRate() > best.getRefreshRate()))
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
