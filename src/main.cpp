#include <Geode/Geode.hpp>
#include <Geode/modify/CCApplication.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <GL/gl.h>

using namespace geode::prelude;

constexpr double TARGET_FPS = 480.0;
constexpr double TARGET_FRAME_TIME = 1.0 / TARGET_FPS;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int interval);

namespace HyperEngine480 {
    static LARGE_INTEGER s_qpcFreq = {};
    static LONGLONG s_nextFrameTick = 0;
    static double s_ticksPerFrameExact = 0.0;

    static HANDLE s_highResTimer = nullptr;
    static HANDLE s_mmcssHandle = nullptr;
    static PFNWGLSWAPINTERVALEXTPROC s_wglSwapIntervalEXT = nullptr;
    static bool s_initialized = false;

    void initSystem() {
        if (s_initialized) return;

        timeBeginPeriod(1);

        s_highResTimer = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS
        );

        DWORD taskIndex = 0;
        s_mmcssHandle = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
        if (s_mmcssHandle) {
            AvSetMmThreadPriority(s_mmcssHandle, AVRT_PRIORITY_CRITICAL);
        }

        HANDLE hProcess = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();
        SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS);
        SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);

        QueryPerformanceFrequency(&s_qpcFreq);
        s_ticksPerFrameExact = static_cast<double>(s_qpcFreq.QuadPart) / TARGET_FPS;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        s_nextFrameTick = now.QuadPart;

        s_initialized = true;
    }

    void cleanup() {
        if (!s_initialized) return;
        if (s_highResTimer) {
            CloseHandle(s_highResTimer);
            s_highResTimer = nullptr;
        }
        if (s_mmcssHandle) {
            AvRevertMmThreadCharacteristics(s_mmcssHandle);
            s_mmcssHandle = nullptr;
        }
        timeEndPeriod(1);
        s_initialized = false;
    }

    void applyFPS() {
        if (auto* app = CCApplication::sharedApplication()) {
            app->setAnimationInterval(TARGET_FRAME_TIME);
        }
        if (auto* view = CCEGLView::sharedOpenGLView()) {
            view->toggleVerticalSync(false);
        }
        if (!s_wglSwapIntervalEXT) {
            s_wglSwapIntervalEXT = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(
                wglGetProcAddress("wglSwapIntervalEXT")
            );
        }
        if (s_wglSwapIntervalEXT) {
            s_wglSwapIntervalEXT(0);
        }
    }

    void paceFrame() {
        if (!s_initialized) initSystem();

        s_nextFrameTick += static_cast<LONGLONG>(s_ticksPerFrameExact);
        LARGE_INTEGER currentTick;
        QueryPerformanceCounter(&currentTick);

        if (currentTick.QuadPart > s_nextFrameTick + (static_cast<LONGLONG>(s_ticksPerFrameExact) * 3)) {
            s_nextFrameTick = currentTick.QuadPart;
        }

        LONGLONG ticksRemaining = s_nextFrameTick - currentTick.QuadPart;
        if (ticksRemaining > 0) {
            double secs = static_cast<double>(ticksRemaining) / static_cast<double>(s_qpcFreq.QuadPart);
            if (secs > 0.0009 && s_highResTimer) {
                LARGE_INTEGER dueTime;
                dueTime.QuadPart = -static_cast<LONGLONG>((secs - 0.00045) * 10000000.0);
                if (dueTime.QuadPart < 0) {
                    SetWaitableTimer(s_highResTimer, &dueTime, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(s_highResTimer, 1);
                }
            }
            do {
                YieldProcessor();
                QueryPerformanceCounter(&currentTick);
            } while (currentTick.QuadPart < s_nextFrameTick);
        }
    }
}

class $modify(MyCCApplication, CCApplication) {
    void setAnimationInterval(double) {
        CCApplication::setAnimationInterval(TARGET_FRAME_TIME);
    }
};

class $modify(MyCCEGLView, CCEGLView) {
    void swapBuffers() {
        HyperEngine480::applyFPS();
        glFlush();
        HyperEngine480::paceFrame();
        CCEGLView::swapBuffers();
    }
    void toggleVerticalSync(bool) {
        CCEGLView::toggleVerticalSync(false);
        HyperEngine480::applyFPS();
    }
};

class $modify(MyScheduler, CCScheduler) {
    void update(float) {
        CCScheduler::update(static_cast<float>(TARGET_FRAME_TIME));
    }
};

class $modify(MyPlayLayer, GJBaseGameLayer) {
    void update(float dt) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        GJBaseGameLayer::update(dt);
    }
};

$on_mod(Loaded) {
    HyperEngine480::initSystem();
    Loader::get()->queueInMainThread([]() {
        HyperEngine480::applyFPS();
    });
}

$on_mod(Unloaded) {
    HyperEngine480::cleanup();
}
