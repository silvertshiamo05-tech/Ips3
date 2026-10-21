// Ips3: the Android app -- SDL's activity, plus the disc image picker the runtime calls into
// (host/android_files.cpp). The disc image is read in place through an open file descriptor;
// the permission to it is kept, so later launches open it without asking.
package com.ips3.bayonetta;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

public class BayonettaActivity extends SDLActivity {
    private static final int PICK_GAME = 0x1505;
    private static final String GAME_KEY = "game";

    private static ParcelFileDescriptor sGame; // kept open while the game reads it
    private static volatile int sPickedFd = -2; // -2 while the picker is open

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" }; // SDL is linked into libmain.so
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    private int open(Uri uri) {
        try {
            ParcelFileDescriptor file = getContentResolver().openFileDescriptor(uri, "r");
            if (file == null) {
                return -1;
            }
            sGame = file;
            return file.getFd();
        } catch (Exception e) {
            return -1;
        }
    }

    // Called from native code: the disc image picked on an earlier launch, or -1.
    public int openSavedGame() {
        String uri = getPreferences(MODE_PRIVATE).getString(GAME_KEY, null);
        return uri != null ? open(Uri.parse(uri)) : -1;
    }

    // Called from native code: shows the picker; pollPickedGame then reports the result.
    public int pickGame() {
        sPickedFd = -2;
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            try {
                startActivityForResult(intent, PICK_GAME);
            } catch (Exception e) {
                sPickedFd = -1;
            }
        });
        return 0;
    }

    // Called from native code: -2 while picking, then the open file descriptor or -1.
    public int pollPickedGame() {
        return sPickedFd;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != PICK_GAME) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        int fd = -1;
        if (resultCode == RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            try {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                getPreferences(MODE_PRIVATE).edit().putString(GAME_KEY, uri.toString()).apply();
            } catch (Exception e) {
                // Not persistable: it still opens for this launch.
            }
            fd = open(uri);
        }
        sPickedFd = fd;
    }
}
