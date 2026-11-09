package org.huxerui.lib.live2d.preview;

import android.app.Application;

public final class MainApplication extends Application {
    static {
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
    }
}
