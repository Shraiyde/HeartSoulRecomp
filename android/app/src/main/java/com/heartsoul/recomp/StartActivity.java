package com.heartsoul.recomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Point;
import android.os.Build;
import android.os.Bundle;
import android.view.Display;
import android.view.inputmethod.InputType;
import android.widget.EditText;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

/**
 * What the launcher opens: the start screen (StartScreenView), then the game.
 * If the game is already running, it goes straight back to it, since its
 * choices only apply when the game starts.
 */
public class StartActivity extends Activity implements StartScreenView.Listener {
    private StartScreenView startScreenView;
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (HeartSoulActivity.isRunning()) {
            launchGame();
            return;
        }
        Display display = getWindowManager().getDefaultDisplay();
        Point size = new Point();
        display.getRealSize(size);
        Settings settings = Settings.load(this, size.x, size.y);
        boolean hasSecondScreen = SecondScreen.find(this, display.getDisplayId()) != null;
        StartScreenView view = new StartScreenView(this, settings, size.x, size.y, hasSecondScreen,
                "v" + versionName(), this);
        startScreenView = view;
        setContentView(view);
        hideSystemBars();
        view.requestFocus();
    }

    @Override
    public void onStartGame(Settings settings, int coopMode) {
        settings.save(this);
        if (coopMode == 2) {
            EditText address = new EditText(this);
            address.setSingleLine(true);
            address.setHint("Host IPv4 address");
            address.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
            new AlertDialog.Builder(this)
                    .setTitle("Join co-op")
                    .setMessage("Enter the host's LAN or private VPN IPv4 address.")
                    .setView(address)
                    .setPositiveButton("JOIN", (dialog, which) -> {
                        String host = address.getText().toString().trim();
                        if (!host.isEmpty()) {
                            launchGame("join", host);
                        } else if (startScreenView != null) {
                            startScreenView.resetStart();
                        }
                    })
                    .setNegativeButton("CANCEL", (dialog, which) -> {
                        if (startScreenView != null)
                            startScreenView.resetStart();
                    })
                    .setOnCancelListener(dialog -> {
                        if (startScreenView != null)
                            startScreenView.resetStart();
                    })
                    .show();
        } else if (coopMode == 1) {
            launchGame("host", null);
        } else {
            launchGame();
        }
    }

    private void launchGame() {
        launchGame(null, null);
    }

    private void launchGame(String coopMode, String host) {
        Intent intent = new Intent(this, HeartSoulActivity.class);
        if (coopMode != null)
            intent.putExtra("coop_mode", coopMode);
        if (host != null)
            intent.putExtra("coop_host", host);
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
