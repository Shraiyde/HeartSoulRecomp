package com.heartsoul.recomp;

import android.content.Context;
import android.os.Build;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Phone-hosted co-op discovery. The directory only lists peers. Gameplay
 * stays on the ZeroTier bridge between the phones.
 */
final class CoopLobby {
    private static final String TAG = "HeartSoulLobby";
    private static final long ANNOUNCE_INTERVAL_MS = 30000;
    private static final long FIRST_ANNOUNCE_DELAY_MS = 6000;
    private static final int REQUEST_TIMEOUT_MS = 15000;

    static final class Lobby {
        final String address;
        final String name;
        final String host;
        final int players;
        final int max;
        final int port;

        Lobby(String address, String name, String host, int players, int max, int port) {
            this.address = address;
            this.name = name;
            this.host = host;
            this.players = players;
            this.max = max;
            this.port = port;
        }

        String label() {
            return name + "  —  " + host + "  (" + players + "/" + max + ")";
        }
    }

    private static final AtomicBoolean announcing = new AtomicBoolean(false);
    private static volatile Thread announcer;
    private static volatile String hostKey = "";
    private static volatile String lastError = "";

    private CoopLobby() {}

    static String lastError() { return lastError; }

    private static String[] access(Context context) {
        (void) context;
        String url = BuildConfig.LOBBY_URL == null ? "" : BuildConfig.LOBBY_URL.trim();
        String password = BuildConfig.LOBBY_KEY == null ? "" : BuildConfig.LOBBY_KEY;
        while (url.endsWith("/")) url = url.substring(0, url.length() - 1);
        if (!url.startsWith("https://")) url = "";
        return new String[] { url, password };
    }

    private static String randomKey() {
        byte[] bytes = new byte[16];
        new SecureRandom().nextBytes(bytes);
        StringBuilder out = new StringBuilder(32);
        for (byte value : bytes) out.append(String.format(java.util.Locale.US, "%02x", value & 0xff));
        return out.toString();
    }

    private static String await(int requestId) {
        long deadline = System.currentTimeMillis() + REQUEST_TIMEOUT_MS;
        while (System.currentTimeMillis() < deadline) {
            String result = HeartSoulZeroTier.httpTake(requestId);
            if (result != null && !result.isEmpty()) return result;
            try { Thread.sleep(100); }
            catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return "0\n";
            }
        }
        return "0\n";
    }

    static List<Lobby> fetchLobbies(Context context) {
        lastError = "";
        String[] config = access(context);
        if (config[0].isEmpty()) {
            lastError = "The online lobby directory is not configured in this build.";
            return Collections.emptyList();
        }
        HeartSoulZeroTier.setLobbyKey(config[1]);
        int requestId = HeartSoulZeroTier.httpStart(config[0] + "/v1/lobbies", "");
        if (requestId <= 0) {
            lastError = "Could not contact the online game list.";
            return Collections.emptyList();
        }
        String response = await(requestId);
        int newline = response.indexOf('\n');
        int code = newline < 0 ? 0 : parseInt(response.substring(0, newline), 0);
        String body = newline < 0 ? "" : response.substring(newline + 1);
        if (code == 401) {
            lastError = "This build is not authorized to use the online game list.";
            return Collections.emptyList();
        }
        if (code != 200) {
            lastError = "The online game list could not be reached. Check the internet connection.";
            return Collections.emptyList();
        }
        List<Lobby> result = new ArrayList<>();
        try {
            JSONArray entries = new JSONObject(body).optJSONArray("lobbies");
            if (entries == null) throw new IllegalArgumentException("Missing lobby list");
            for (int i = 0; i < entries.length(); i++) {
                JSONObject entry = entries.optJSONObject(i);
                if (entry == null || !"game".equals(entry.optString("kind", "game"))
                        || !"heartsoul-direct-v1".equals(entry.optString("protocol", ""))) continue;
                String address = entry.optString("address", "").trim();
                if (!address.matches("[0-9A-Fa-f:]+") || !address.contains(":")) continue;
                int port = entry.optInt("port", CoopNetwork.PORT);
                if (port < 1 || port > 65535) continue;
                result.add(new Lobby(address,
                        clean(entry.optString("name", "HEART & SOUL"), "HEART & SOUL"),
                        clean(entry.optString("host", "PLAYER"), "PLAYER"),
                        clamp(entry.optInt("players", 1), 1, 2),
                        clamp(entry.optInt("max", 2), 1, 2), port));
            }
        } catch (Exception error) {
            Log.e(TAG, "Invalid lobby directory response", error);
            lastError = "The online game list returned an invalid response.";
            return Collections.emptyList();
        }
        Collections.sort(result, Comparator.comparing(lobby -> lobby.name.toLowerCase(java.util.Locale.ROOT)));
        if (result.isEmpty()) lastError = "No active phone-hosted games were found.";
        return result;
    }

    static void startHosting(Context context) {
        stopHosting(context);
        final String[] config = access(context);
        if (config[0].isEmpty()) {
            Log.e(TAG, "Cannot announce host: lobby directory configuration is missing");
            return;
        }
        hostKey = randomKey();
        announcing.set(true);
        announcer = new Thread(() -> {
            try {
                Thread.sleep(FIRST_ANNOUNCE_DELAY_MS);
                while (announcing.get()) {
                    CoopNetwork.Status status = CoopNetwork.status();
                    if (status.ready()) {
                        HeartSoulZeroTier.setLobbyKey(config[1]);
                        JSONObject record = new JSONObject();
                        try {
                            record.put("key", hostKey);
                            record.put("address", status.address);
                            record.put("name", "HEART & SOUL");
                            record.put("host", clean(Build.MODEL, "PLAYER"));
                            record.put("private", false);
                            record.put("players", 1);
                            record.put("max", 2);
                            record.put("kind", "game");
                            record.put("port", CoopNetwork.PORT);
                            record.put("version", "1");
                            record.put("compat", "heartsoul-direct-v1");
                            record.put("protocol", "heartsoul-direct-v1");
                        } catch (Exception ignored) {}
                        int id = HeartSoulZeroTier.httpStart(config[0] + "/v1/lobbies", record.toString());
                        if (id > 0) {
                            String result = await(id);
                            if (!result.startsWith("200\n")) {
                                int split = result.indexOf('\n');
                                String status = split < 0 ? "0" : result.substring(0, split);
                                Log.w(TAG, "Lobby announcement was rejected (" + status + ")");
                            }
                        }
                    }
                    Thread.sleep(ANNOUNCE_INTERVAL_MS);
                }
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }, "heartsoul-lobby-announcer");
        announcer.setDaemon(true);
        announcer.start();
    }

    static void stopHosting(Context context) {
        announcing.set(false);
        Thread thread = announcer;
        if (thread != null) thread.interrupt();
        announcer = null;
        String key = hostKey;
        hostKey = "";
        if (!key.isEmpty()) {
            String[] config = access(context);
            if (!config[0].isEmpty()) {
                HeartSoulZeroTier.setLobbyKey(config[1]);
                try {
                    JSONObject remove = new JSONObject();
                    remove.put("key", key);
                    remove.put("remove", true);
                    HeartSoulZeroTier.httpStart(config[0] + "/v1/lobbies", remove.toString());
                } catch (Exception ignored) {}
            }
        }
    }

    private static String clean(String value, String fallback) {
        String clean = value == null ? "" : value.replaceAll("[\\p{Cntrl}]", "").trim();
        if (clean.isEmpty()) clean = fallback;
        return clean.length() > 24 ? clean.substring(0, 24) : clean;
    }

    private static int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }

    private static int parseInt(String value, int fallback) {
        try { return Integer.parseInt(value.trim()); }
        catch (Exception ignored) { return fallback; }
    }
}
