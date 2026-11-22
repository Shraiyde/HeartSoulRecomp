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
import android.text.InputType;
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
        android.app.ProgressDialog progress = android.app.ProgressDialog.show(this, "CO-OP",
                "Checking the router and preparing a direct connection...", true, false);
        new Thread(() -> {
            CoopNetwork.HostInfo info = CoopNetwork.prepareHost(this, CoopNetwork.PORT);
            runOnUiThread(() -> {
                progress.dismiss();
                StringBuilder message = new StringBuilder();
                if (!info.joinCode.isEmpty()) message.append("JOIN CODE: ").append(info.joinCode).append("\\n\\n");
                if (!info.publicAddress.isEmpty())
                    message.append("INTERNET: ").append(info.publicAddress).append(":").append(CoopNetwork.PORT).append("\\n");
                if (!info.localAddress.isEmpty())
                    message.append("SAME WI-FI: ").append(info.localAddress).append(":").append(CoopNetwork.PORT).append("\\n");
                message.append("\\n");
                if (info.mapped) message.append("Router port opened automatically with UPnP.");
                else message.append("Automatic port mapping was unavailable. For internet play, forward TCP ")
                        .append(CoopNetwork.PORT).append(" to this phone on your router.");
                new AlertDialog.Builder(this).setTitle("HOST CODE")
                        .setMessage(message.toString())
                        .setPositiveButton("START HOSTING", (d, w) -> launchGame("host", "", CoopNetwork.PORT))
                        .setNegativeButton("CANCEL", null).show();
            });
        }).start();
    }

    private void joinGame() {
        EditText address = new EditText(this);
        address.setSingleLine(true);
        address.setHint("8-character code or IP:PORT");
        address.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
                | InputType.TYPE_TEXT_VARIATION_URI);
        new AlertDialog.Builder(this).setTitle("JOIN A GAME")
                .setMessage("Enter the host's join code or direct IPv4 address.")
                .setView(address)
                .setPositiveButton("JOIN", (dialog, which) -> {
                    String[] target = CoopNetwork.parseAddress(address.getText().toString());
                    if (target == null) {
                        new AlertDialog.Builder(this).setMessage("That join code or address is invalid.")
                                .setPositiveButton("OK", null).show();
                        return;
                    }
                    launchGame("join", target[0], Integer.parseInt(target[1]));
                })
                .setNegativeButton("CANCEL", null).show();
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
