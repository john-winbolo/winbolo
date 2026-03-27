package com.winbolo.android;

import org.libsdl.app.SDLActivity;

public class WinBoloActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[]{
            "SDL3",
            "main"
        };
    }
}
