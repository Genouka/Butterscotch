/*
 * Android EGL/GLES backend for the embedded (bsdbg) runner.
 *
 * Single-window hosts (the Avalonia Android app) hand the runner an
 * android.view.Surface through platformSetEmbeddedWindow(). This backend wraps
 * that surface in an ANativeWindow, creates an EGL/GLES3 context on the game
 * thread and presents with eglSwapBuffers. It never creates a window of its own.
 */

#include "common.h"
#include "platformdefs.h"
#include "gettime.h"
#include "runner_mouse.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <dlfcn.h>
#include <jni.h>
#include <time.h>

#define LOG_TAG "bsdbg"

static Runner *g_runner = NULL;

static jobject g_surface = NULL;   /* global ref to android.view.Surface */
static ANativeWindow *g_window = NULL;
static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLContext g_context = EGL_NO_CONTEXT;
static EGLSurface g_eglSurface = EGL_NO_SURFACE;
static bool g_initialized = false;
static int32_t g_width = 0;
static int32_t g_height = 0;

typedef jint (*GetCreatedJavaVMsFn)(JavaVM **, jsize, jsize *);

/* The NDK does not export JNI_GetCreatedJavaVMs from a linkable library; the
 * ART runtime exports it, so resolve it from there on first use. */
static JavaVM *android_vm(void) {
    static JavaVM *vm = NULL;
    static bool tried = false;
    jsize count = 0;

    if (!tried) {
        GetCreatedJavaVMsFn getVms = NULL;
        void *art;

        tried = true;
        art = dlopen("libart.so", RTLD_NOW);
        if (art != NULL)
            getVms = (GetCreatedJavaVMsFn) (void *) dlsym(art, "JNI_GetCreatedJavaVMs");
        if (getVms == NULL || getVms(&vm, 1, &count) != JNI_OK || count == 0)
            vm = NULL;
    }
    return vm;
}

static JNIEnv *android_env(void) {
    JavaVM *vm = android_vm();
    JNIEnv *env = NULL;

    if (vm == NULL)
        return NULL;
    if ((*vm)->GetEnv(vm, (void **) &env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK)
            return NULL;
    }
    return env;
}

bool platformSetEmbeddedWindow(intptr_t handle, int32_t kind) {
    JNIEnv *env;
    jobject global;

    /* BSDBG_EMBED_ANDROID_SURFACE: handle is an android.view.Surface jobject. */
    if (kind != 2 || handle == 0)
        return false;

    env = android_env();
    if (env == NULL)
        return false;

    /* The caller's reference may be local to its JNI frame; keep our own. */
    global = (*env)->NewGlobalRef(env, (jobject) handle);
    if (global == NULL)
        return false;

    if (g_surface != NULL)
        (*env)->DeleteGlobalRef(env, g_surface);
    g_surface = global;
    return true;
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless, bool fullscreen) {
    JNIEnv *env;
    EGLint numConfigs = 0;
    EGLConfig config = NULL;
    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    (void) title;
    (void) fullscreen;

    g_width = reqW > 0 ? reqW : 640;
    g_height = reqH > 0 ? reqH : 480;

    if (headless) {
        /* No surface requested: size only, like the no-op backend. */
        g_initialized = true;
        logInfo("Android backend: headless %dx%d\n", g_width, g_height);
        return true;
    }

    env = android_env();
    if (env == NULL || g_surface == NULL) {
        logError("Android backend: no surface was supplied by the host\n");
        return false;
    }

    g_window = ANativeWindow_fromSurface(env, g_surface);
    if (g_window == NULL) {
        logError("Android backend: ANativeWindow_fromSurface failed\n");
        return false;
    }

    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY || eglInitialize(g_display, NULL, NULL) != EGL_TRUE) {
        logError("Android backend: eglInitialize failed\n");
        return false;
    }

    if (eglChooseConfig(g_display, configAttribs, &config, 1, &numConfigs) != EGL_TRUE ||
        numConfigs < 1) {
        logError("Android backend: no ES3 window config\n");
        return false;
    }

    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, contextAttribs);
    if (g_context == EGL_NO_CONTEXT) {
        logError("Android backend: eglCreateContext failed (0x%x)\n", eglGetError());
        return false;
    }

    g_eglSurface = eglCreateWindowSurface(g_display, config, g_window, NULL);
    if (g_eglSurface == EGL_NO_SURFACE) {
        logError("Android backend: eglCreateWindowSurface failed (0x%x)\n", eglGetError());
        return false;
    }

    if (eglMakeCurrent(g_display, g_eglSurface, g_eglSurface, g_context) != EGL_TRUE) {
        logError("Android backend: eglMakeCurrent failed (0x%x)\n", eglGetError());
        return false;
    }

    g_width = ANativeWindow_getWidth(g_window);
    g_height = ANativeWindow_getHeight(g_window);
    if (g_width <= 0) g_width = reqW > 0 ? reqW : 640;
    if (g_height <= 0) g_height = reqH > 0 ? reqH : 480;

    g_initialized = true;
    logInfo("Android backend: %dx%d on ANativeWindow %p\n", g_width, g_height, (void *) g_window);
    return true;
}

void platformExit(void) {
    if (g_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_eglSurface != EGL_NO_SURFACE)
            eglDestroySurface(g_display, g_eglSurface);
        if (g_context != EGL_NO_CONTEXT)
            eglDestroyContext(g_display, g_context);
        eglTerminate(g_display);
    }
    g_eglSurface = EGL_NO_SURFACE;
    g_context = EGL_NO_CONTEXT;
    g_display = EGL_NO_DISPLAY;

    if (g_window != NULL) {
        ANativeWindow_release(g_window);
        g_window = NULL;
    }

    if (g_surface != NULL) {
        JNIEnv *env = android_env();
        if (env != NULL)
            (*env)->DeleteGlobalRef(env, g_surface);
        g_surface = NULL;
    }

    g_initialized = false;
}

bool platformGetWindowSize(int32_t *outW, int32_t *outH) {
    if (outW == NULL || outH == NULL || !g_initialized)
        return false;
    *outW = g_width;
    *outH = g_height;
    return true;
}

bool platformGetScaledWindowSize(int32_t *outW, int32_t *outH) {
    return platformGetWindowSize(outW, outH);
}

void platformSetWindowSize(int32_t width, int32_t height) {
    if (width > 0) g_width = width;
    if (height > 0) g_height = height;
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    runner->setCursor = NULL;
    runner->currentCursor = GML_CR_DEFAULT;
}

void platformSetWindowTitle(const char *title) {
    (void) title;
}

void platformGetMousePos(double *xPos, double *yPos) {
    if (xPos) *xPos = 0.0;
    if (yPos) *yPos = 0.0;
}

void platformSwapBuffers(void) {
    if (g_display == EGL_NO_DISPLAY || g_eglSurface == EGL_NO_SURFACE)
        return;

    /* The surface can be resized by the host (rotation, split screen). */
    if (g_window != NULL) {
        int32_t width = ANativeWindow_getWidth(g_window);
        int32_t height = ANativeWindow_getHeight(g_window);
        if (width > 0 && (width != g_width || height != g_height)) {
            g_width = width;
            g_height = height;
        }
    }

    eglSwapBuffers(g_display, g_eglSurface);
}

void *platformGetProcAddress(const char *name) {
    return (void *) eglGetProcAddress(name);
}

bool platformHandleEvents(void) {
    /* The host forwards input; nothing is polled here. */
    return false;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = (int64_t) time - (int64_t) nowNanos();
    if (remaining > 2000000) {
        remaining -= 1000000;
        struct timespec ts;
        ts.tv_sec = (time_t) (remaining / 1000000000);
        ts.tv_nsec = (long) (remaining % 1000000000);
        nanosleep(&ts, NULL);
    }
    while (nowNanos() < time) {
        YIELD();
    }
}
