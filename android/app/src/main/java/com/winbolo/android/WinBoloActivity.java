package com.winbolo.android;

import android.content.Intent;
import android.net.Uri;
import org.libsdl.app.SDLActivity;

public class WinBoloActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[]{
            "SDL3",
            "main"
        };
    }

    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();
        if (intent != null && Intent.ACTION_VIEW.equals(intent.getAction())) {
            Uri uri = intent.getData();
            if (uri != null && "winbolo".equals(uri.getScheme())) {
                return new String[]{ uri.toString() };
            }
        }
        return new String[]{};
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (intent != null && Intent.ACTION_VIEW.equals(intent.getAction())) {
            Uri uri = intent.getData();
            if (uri != null && "winbolo".equals(uri.getScheme())) {
                /* SDL3 on Android delivers this via SDL_EVENT_DROP_FILE
                   when using onNewIntent — handled in the event loop. */
            }
        }
    }
}
