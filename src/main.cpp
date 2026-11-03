#include <Geode/Geode.hpp>
#include <Geode/modify/CCApplication.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <GL/gl.h>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

// Hằng số mục tiêu 480 FPS thực (2.083333 ms mỗi khung hình)
constexpr double TARGET_FPS = 480.0;
constexpr double TARGET_FRAME_TIME = 1.0 / TARGET_FPS;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
typedef struct _PROCESS_POWER_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} PROCESS_POWER_THROTTLING_STATE, *PPROCESS_POWER_THROTTLING_STATE;
#endif

#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int interval);

namespace HyperEngine480 {
    static LARGE_INTEGER s_qpcFreq = {};
    static LONGLONG s_nextFrameTick = 0;
    static double s_ticksPerFrameExact = 0.0;
    static double s_tickRemainder = 0.0;

    static HANDLE s_highResTimer = nullptr;
    static HANDLE s_mmcssHandle = nullptr;
    static PFNWGLSWAPINTERVALEXTPROC s_wglSwapIntervalEXT = nullptr;

    static bool s_kernelInitialized = false;
    static bool s_glInitialized = false;
    static uint64_t s_frameCounter = 0;
    static float s_timeDebt = 0.0f;

    void initWin11KernelAndTimer() {
        if (s_kernelInitialized) return;

        // 1. Hạ chu kỳ ngắt hệ thống Windows 11 xuống 1ms
        timeBeginPeriod(1);

        // 2. Tạo High-Resolution Waitable Timer (0.5ms)
        s_highResTimer = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS
        );

        // 3. Đăng ký luồng chính với Windows 11 MMCSS ("Games" Critical Priority)
        DWORD taskIndex = 0;
        s_mmcssHandle = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
        if (s_mmcssHandle) {
            AvSetMmThreadPriority(s_mmcssHandle, AVRT_PRIORITY_CRITICAL);
        }

        // 4. Nâng mức ưu tiên tiến trình và tắt EcoQoS Power Throttling của Windows 11
        HANDLE hProcess = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();
        SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS);
        SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);

        SetProcessPriorityBoost(hProcess, FALSE);
        SetThreadPriorityBoost(hThread, FALSE);

        PROCESS_POWER_THROTTLING_STATE powerThrottling = {};
        powerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        powerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        powerThrottling.StateMask = 0;
        SetProcessInformation(
            hProcess,
            static_cast<PROCESS_INFORMATION_CLASS>(4), // ProcessPowerThrottling = 4
            &powerThrottling,
            sizeof(powerThrottling)
        );

        // 5. Khởi tạo bộ đếm phần cứng QPC
        QueryPerformanceFrequency(&s_qpcFreq);
        s_ticksPerFrameExact = static_cast<double>(s_qpcFreq.QuadPart) / TARGET_FPS;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        s_nextFrameTick = now.QuadPart;

        s_kernelInitialized = true;
    }

    void cleanupKernelResources() {
        if (!s_kernelInitialized) return;
        if (s_highResTimer) {
            CloseHandle(s_highResTimer);
            s_highResTimer = nullptr;
        }
        if (s_mmcssHandle) {
            AvRevertMmThreadCharacteristics(s_mmcssHandle);
            s_mmcssHandle = nullptr;
        }
        timeEndPeriod(1);
        s_kernelInitialized = false;
    }

    void initOpenGLOnce() {
        if (s_glInitialized) return;

        s_wglSwapIntervalEXT = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(
            wglGetProcAddress("wglSwapIntervalEXT")
        );
        if (s_wglSwapIntervalEXT) {
            s_wglSwapIntervalEXT(0);
        }

        glEnable(GL_MULTISAMPLE);
        glDisable(GL_POLYGON_SMOOTH);

        s_glInitialized = true;
    }

    void pollHardwareInput480Hz() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        while (PeekMessageW(&msg, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void paceFrame480() {
        if (!s_kernelInitialized) {
            initWin11KernelAndTimer();
        }

        ++s_frameCounter;

        double exactStep = s_ticksPerFrameExact + s_tickRemainder;
        LONGLONG stepTicks = static_cast<LONGLONG>(exactStep);
        s_tickRemainder = exactStep - static_cast<double>(stepTicks);

        // Tán xạ vệt xé hình trên màn 60Hz (Tear-Line Diffuser)
        LONGLONG phaseDither = static_cast<LONGLONG>((s_frameCounter % 7) - 3) * (s_qpcFreq.QuadPart / 500000);
        s_nextFrameTick += stepTicks;
        LONGLONG targetTick = s_nextFrameTick + phaseDither;

        LARGE_INTEGER currentTick;
        QueryPerformanceCounter(&currentTick);

        if (currentTick.QuadPart > targetTick + (stepTicks * 3)) {
            s_nextFrameTick = currentTick.QuadPart;
            targetTick = s_nextFrameTick;
        }

        LONGLONG ticksRemaining = targetTick - currentTick.QuadPart;
        if (ticksRemaining > 0) {
            double secondsRemaining = static_cast<double>(ticksRemaining) / static_cast<double>(s_qpcFreq.QuadPart);

            if (secondsRemaining > 0.0009 && s_highResTimer != nullptr) {
                LARGE_INTEGER dueTime;
                dueTime.QuadPart = -static_cast<LONGLONG>((secondsRemaining - 0.00045) * 10000000.0);
                if (dueTime.QuadPart < 0) {
                    SetWaitableTimer(s_highResTimer, &dueTime, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(s_highResTimer, 1);
                }
            }

            do {
                YieldProcessor();
                QueryPerformanceCounter(&currentTick);
            } while (currentTick.QuadPart < targetTick);
        }
    }

    void enforce480State() {
        if (auto* gm = GameManager::sharedState()) {
            gm->setGameVariable("0030", false); // Tắt V-Sync
            gm->setGameVariable("0116", true);  // Bật Custom FPS
            gm->m_customFPSTarget = static_cast<float>(TARGET_FPS);
        }
        if (auto* app = CCApplication::sharedApplication()) {
            app->setAnimationInterval(TARGET_FRAME_TIME);
        }
        if (auto* view = CCEGLView::sharedOpenGLView()) {
            view->toggleVerticalSync(false);
        }
        if (s_wglSwapIntervalEXT) {
            s_wglSwapIntervalEXT(0);
        }
    }
}

class $modify(HyperGameManager, GameManager) {
    bool init() {
        if (!GameManager::init()) return false;
        HyperEngine480::initWin11KernelAndTimer();
        HyperEngine480::enforce480State();
        return true;
    }

    void applicationWillEnterForeground() {
        GameManager::applicationWillEnterForeground();
        HyperEngine480::s_kernelInitialized = false;
        HyperEngine480::initWin11KernelAndTimer();
        HyperEngine480::enforce480State();
    }
};

class $modify(HyperApp, CCApplication) {
    void setAnimationInterval(double interval) {
        CCApplication::setAnimationInterval(TARGET_FRAME_TIME);
    }
};

class $modify(HyperScheduler, CCScheduler) {
    void update(float dt) {
        constexpr float idealDt = static_cast<float>(TARGET_FRAME_TIME);

        if (dt > 0.001f && dt < 0.005f) {
            HyperEngine480::s_timeDebt += (dt - idealDt);
            float correction = (std::max)(-0.0001f, (std::min)(0.0001f, HyperEngine480::s_timeDebt * 0.1f));
            HyperEngine480::s_timeDebt -= correction;
            CCScheduler::update(idealDt + correction);
        } else {
            HyperEngine480::s_timeDebt = 0.0f;
            CCScheduler::update(dt);
        }
    }
};

class $modify(HyperPlayLayer, GJBaseGameLayer) {
    void update(float dt) {
        HyperEngine480::pollHardwareInput480Hz();
        GJBaseGameLayer::update(dt);
    }
};

class $modify(HyperEGLView, CCEGLView) {
    void swapBuffers() {
        HyperEngine480::initOpenGLOnce();
        glFlush();
        HyperEngine480::paceFrame480();
        CCEGLView::swapBuffers();
    }

    void toggleVerticalSync(bool vSync) {
        CCEGLView::toggleVerticalSync(false);
        if (HyperEngine480::s_wglSwapIntervalEXT) {
            HyperEngine480::s_wglSwapIntervalEXT(0);
        }
    }
};

$on_mod(Loaded) {
    HyperEngine480::initWin11KernelAndTimer();
    Loader::get()->queueInMainThread([]() {
        HyperEngine480::initOpenGLOnce();
        HyperEngine480::enforce480State();
    });
}

$on_mod(Unloaded) {
    HyperEngine480::cleanupKernelResources();
}
