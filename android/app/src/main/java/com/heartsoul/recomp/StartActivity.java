package com.heartsoul.recomp;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Point;
import android.os.Build;
import android.os.Bundle;
import android.view.Display;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

/**
 * What the launcher opens: the start screen (StartScreenView), then the game.
 * If the game is already running, it goes straight back to it, since its
 * choices only apply when the game starts.
 */
public class StartActivity extends Activity implements StartScreenView.Listener {
    private StartScreenView screen;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (HeartSoulActivity.isRunning()) {
            launchGame("solo", 0);
            return;
        }
        Display display = getWindowManager().getDefaultDisplay();
        Point size = new Point();
        display.getRealSize(size);
        Settings settings = Settings.load(this, size.x, size.y);
        boolean hasSecondScreen = SecondScreen.find(this, display.getDisplayId()) != null;
        screen = new StartScreenView(this, settings, size.x, size.y, hasSecondScreen,
                "v" + versionName(), this);
        setContentView(screen);
        hideSystemBars();
        screen.requestFocus();
    }

    @Override
    public void onStartGame(Settings settings) {
        settings.save(this);
        launchGame("solo", 0);
    }

    @Override
    public void onHostGame(Settings settings) {
        settings.save(this);
        note("Starting the phone link…");
        new Thread(() -> {
            HeartSoulLink.Outcome outcome = HeartSoulLink.prepareHost(this);
            runOnUiThread(() -> {
                if (!outcome.ok) {
                    note(outcome.message);
                    if (screen != null) {
                        screen.armAgain();
                    }
                    return;
                }
                launchGame("host", 0);
            });
        }, "hs-host").start();
    }

    @Override
    public void onJoinGame(Settings settings) {
        settings.save(this);
        note("Looking for a hosted game…");
        new Thread(() -> {
            HeartSoulLink.Outcome outcome = HeartSoulLink.prepareJoin(this);
            runOnUiThread(() -> {
                if (!outcome.ok) {
                    note(outcome.message);
                    if (screen != null) {
                        screen.armAgain();
                    }
                    return;
                }
                launchGame("join", outcome.localPort);
            });
        }, "hs-join").start();
    }

    @Override
    public void onJoinPcHost(Settings settings) {
        settings.save(this);
        launchGame("pc", 0);
    }

    private void note(String text) {
        if (screen != null) {
            screen.setStatus(text);
        }
    }

    private void launchGame(String mode, int localPort) {
        Intent intent = new Intent(this, HeartSoulActivity.class);
        intent.putExtra(HeartSoulActivity.EXTRA_LINK_MODE, mode);
        intent.putExtra(HeartSoulActivity.EXTRA_LOCAL_PORT, localPort);
        intent.putExtra(HeartSoulActivity.EXTRA_JOIN_PC_HOST, "pc".equals(mode));
        startActivity(intent);
        overridePendingTransition(0, 0);
        finish();
    }

    private String versionName() {
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return info.versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "";
        }
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }
}
