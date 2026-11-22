package com.heartsoul.recomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Point;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import java.util.List;

/**
 * What the launcher opens: the start screen (StartScreenView), then the game.
 * If the game is already running, it goes straight back to it, since its
 * choices only apply when the game starts.
 */
public class StartActivity extends Activity implements StartScreenView.Listener {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (HeartSoulActivity.isRunning()) {
            launchGame(false);
            return;
        }
        Display display = getWindowManager().getDefaultDisplay();
        Point size = new Point();
        display.getRealSize(size);
        Settings settings = Settings.load(this, size.x, size.y);
        boolean hasSecondScreen = SecondScreen.find(this, display.getDisplayId()) != null;
        StartScreenView view = new StartScreenView(this, settings, size.x, size.y, hasSecondScreen,
                "v" + versionName(), this);
        setContentView(view);
        hideSystemBars();
        view.requestFocus();
    }

    @Override
    public void onStartGame(Settings settings) {
        settings.save(this);
        launchGame(false);
    }

    @Override
    public void onJoinPcHost(Settings settings) {
        settings.save(this);
        new AlertDialog.Builder(this)
                .setTitle("CO-OP")
                .setItems(new String[] { "HOST A GAME", "JOIN A GAME" }, (dialog, which) -> {
                    if (which == 0) startHosting();
                    else joinGame();
                })
                .setNegativeButton("CANCEL", null)
                .show();
    }

    private void startHosting() {
        startZeroTier(true);
    }

    private void joinGame() {
        startZeroTier(false);
    }

    private void startZeroTier(boolean host) {
        if (!CoopNetwork.available()) {
            new AlertDialog.Builder(this).setTitle("CO-OP UNAVAILABLE")
                    .setMessage("This APK is missing the embedded ZeroTier library. Rebuild it with libzt enabled.")
                    .setPositiveButton("OK", null).show();
            return;
        }
        android.app.ProgressDialog progress = android.app.ProgressDialog.show(this, "CO-OP",
                host ? "Connecting to the peer network..." : "Connecting to the peer network...", true, false);
        String error = CoopNetwork.start(this);
        if (error != null && !error.isEmpty()) {
            progress.dismiss();
            new AlertDialog.Builder(this).setTitle("CO-OP ERROR").setMessage(error)
                    .setPositiveButton("OK", null).show();
            return;
        }
        Handler handler = new Handler(Looper.getMainLooper());
        int[] attempts = { 0 };
        Runnable poll = new Runnable() {
            @Override public void run() {
                CoopNetwork.Status status = CoopNetwork.status();
                if (status.ready()) {
                    if (host) {
                        progress.dismiss();
                        launchGame("host", "", CoopNetwork.PORT);
                    } else {
                        progress.setMessage("Finding active games...");
                        loadLobbyList(progress);
                    }
                    return;
                }
                if (status.failed() || ++attempts[0] >= 600) {
                    progress.dismiss();
                    String reason = status.message.isEmpty() ? "The peer network did not become ready." : status.message;
                    new AlertDialog.Builder(StartActivity.this).setTitle("CO-OP CONNECTION FAILED")
                            .setMessage(reason).setPositiveButton("OK", null).show();
                    return;
                }
                handler.postDelayed(this, 250);
            }
        };
        handler.postDelayed(poll, 250);
    }

    private void loadLobbyList(android.app.ProgressDialog progress) {
        new Thread(() -> {
            List<CoopLobby.Lobby> lobbies = CoopLobby.fetchLobbies(StartActivity.this);
            String error = CoopLobby.lastError();
            runOnUiThread(() -> {
                if (isFinishing()) return;
                progress.dismiss();
                if (lobbies.isEmpty()) {
                    new AlertDialog.Builder(StartActivity.this).setTitle("JOIN A GAME")
                            .setMessage(error.isEmpty() ? "No active phone-hosted games were found." : error)
                            .setPositiveButton("RETRY", (dialog, which) -> joinGame())
                            .setNegativeButton("CANCEL", null).show();
                    return;
                }
                String[] labels = new String[lobbies.size()];
                for (int i = 0; i < lobbies.size(); i++) labels[i] = lobbies.get(i).label();
                new AlertDialog.Builder(StartActivity.this).setTitle("JOIN A GAME")
                        .setItems(labels, (dialog, which) -> {
                            CoopLobby.Lobby lobby = lobbies.get(which);
                            launchGame("join", lobby.address, lobby.port);
                        })
                        .setNegativeButton("REFRESH", (dialog, which) -> joinGame())
                        .show();
            });
        }, "heartsoul-lobby-list").start();
    }

    private void launchGame(boolean joinPcHost) {
        launchGame(joinPcHost ? "join" : "solo", "", CoopNetwork.PORT);
    }

    private void launchGame(String mode, String address, int port) {
        Intent intent = new Intent(this, HeartSoulActivity.class);
        intent.putExtra(HeartSoulActivity.EXTRA_COOP_MODE, mode);
        intent.putExtra(HeartSoulActivity.EXTRA_COOP_ADDRESS, address);
        intent.putExtra(HeartSoulActivity.EXTRA_COOP_PORT, port);
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
