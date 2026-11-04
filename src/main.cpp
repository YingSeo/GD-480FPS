#include <Geode/Geode.hpp>
#include <Geode/modify/CCApplication.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCEGLView.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace geode::prelude;

// Mức sàn tối thiểu: 480 FPS thật (2.083333 ms mỗi frame, không giới hạn trần)
constexpr double FLOOR_FPS = 480.0;
constexpr double FLOOR_FRAME_TIME = 1.0 / FLOOR_FPS;

// Xung nhịp nội bộ siêu nhỏ giúp vô hiệu hóa hàm Sleep(1) gây tụt FPS của Cocos2d-x
constexpr double INTERNAL_BYPASS_INTERVAL = 0.0001;

// Hằng số phần cứng OpenGL để khử răng cưa MSAA và chống nhòe hình
constexpr unsigned int GL_LINE_SMOOTH_CAP                = 0x0B20;
constexpr unsigned int GL_POLYGON_SMOOTH_CAP             = 0x0B41;
constexpr unsigned int GL_PERSPECTIVE_CORRECTION_CAP     = 0x0C50;
constexpr unsigned int GL_NICEST_MODE                    = 0x1102;
constexpr unsigned int GL_MULTISAMPLE_CAP                = 0x809D;
constexpr unsigned int GL_MULTISAMPLE_FILTER_HINT_NV_CAP = 0x8534;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {
    struct Win11PowerThrottling {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    };

    using PFN_NtQueryTimerResolution = LONG(NTAPI*)(PULONG, PULONG, PULONG);
    using PFN_NtSetTimerResolution   = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
    using PFN_timeBeginPeriod        = UINT(WINAPI*)(UINT);
    using PFN_timeEndPeriod          = UINT(WINAPI*)(UINT);
    using PFN_AvSetMmThreadCharsW    = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    using PFN_AvSetMmThreadPriority  = BOOL(WINAPI*)(HANDLE, int);
    using PFN_AvRevertMmThreadChars  = BOOL(WINAPI*)(HANDLE);
    using PFN_DwmEnableMMCSS         = HRESULT(WINAPI*)(BOOL);
    using PFN_SetProcessInformation  = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);

    using PFN_wglGetProcAddress      = PROC(WINAPI*)(LPCSTR);
    using PFN_wglSwapIntervalEXT     = BOOL(WINAPI*)(int);
    using PFN_glEnable               = void(APIENTRY*)(unsigned int);
    using PFN_glDisable              = void(APIENTRY*)(unsigned int);
    using PFN_glHint                 = void(APIENTRY*)(unsigned int, unsigned int);
    using PFN_glFlush                = void(APIENTRY*)();
}

namespace OverdriveEngineV8 {
    static LARGE_INTEGER s_qpcFreq = {};
    static LONGLONG s_nextFrameTick = 0;
    static double s_tickRemainder = 0.0;

    static HANDLE s_highResTimer = nullptr;
    static HANDLE s_mmcssHandle = nullptr;

    static PFN_timeEndPeriod s_timeEndPeriod = nullptr;
    static PFN_AvRevertMmThreadChars s_avRevert = nullptr;
    static PFN_DwmEnableMMCSS s_dwmEnableMMCSS = nullptr;

    static PFN_wglGetProcAddress s_wglGetProcAddress = nullptr;
    static PFN_wglSwapIntervalEXT s_wglSwapIntervalEXT = nullptr;
    static PFN_glEnable s_glEnable = nullptr;
    static PFN_glDisable s_glDisable = nullptr;
    static PFN_glHint s_glHint = nullptr;
    static PFN_glFlush s_glFlush = nullptr;

    static double s_targetFrameTime = FLOOR_FRAME_TIME;
    static float s_physicsDebt = 0.0f;
    static float s_subframeCarry = 0.0f;
    static uint64_t s_frameCount = 0;

    static bool s_kernelReady = false;
    static bool s_glReady = false;
    static bool s_internalHookCall = false;

    // 1. Mở khóa Kernel Windows 11 (0.5ms Timer + Tắt EcoQoS + Ưu tiên MMCSS Critical)
    void initKernelBoost() {
        if (s_kernelReady) return;

        if (HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll")) {
            auto ntQuery = reinterpret_cast<PFN_NtQueryTimerResolution>(
                GetProcAddress(hNtdll, "NtQueryTimerResolution")
            );
            auto ntSet = reinterpret_cast<PFN_NtSetTimerResolution>(
                GetProcAddress(hNtdll, "NtSetTimerResolution")
            );
            if (ntSet) {
                ULONG minRes = 0, maxRes = 5000, curRes = 0;
                if (ntQuery) {
                    ntQuery(&minRes, &maxRes, &curRes);
                }
                ntSet(maxRes, TRUE, &curRes);
            }
        }

        if (HMODULE hWinmm = LoadLibraryW(L"winmm.dll")) {
            auto timeBegin = reinterpret_cast<PFN_timeBeginPeriod>(
                GetProcAddress(hWinmm, "timeBeginPeriod")
            );
            s_timeEndPeriod = reinterpret_cast<PFN_timeEndPeriod>(
                GetProcAddress(hWinmm, "timeEndPeriod")
            );
            if (timeBegin) {
                timeBegin(1);
            }
        }

        s_highResTimer = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS
        );

        HANDLE hProcess = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();
        SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS);
        SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);
        SetProcessPriorityBoost(hProcess, FALSE);
        SetThreadPriorityBoost(hThread, FALSE);

        if (HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll")) {
            auto setProcInfo = reinterpret_cast<PFN_SetProcessInformation>(
                GetProcAddress(hKernel32, "SetProcessInformation")
            );
            if (setProcInfo) {
                Win11PowerThrottling throttling = {};
                throttling.Version = 1;
                throttling.ControlMask = 0x1 | 0x4; // EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION
                throttling.StateMask = 0;
                setProcInfo(hProcess, 4, &throttling, sizeof(throttling));
            }
        }

        if (HMODULE hAvrt = LoadLibraryW(L"avrt.dll")) {
            auto avSetChars = reinterpret_cast<PFN_AvSetMmThreadCharsW>(
                GetProcAddress(hAvrt, "AvSetMmThreadCharacteristicsW")
            );
            auto avSetPrio = reinterpret_cast<PFN_AvSetMmThreadPriority>(
                GetProcAddress(hAvrt, "AvSetMmThreadPriority")
            );
            s_avRevert = reinterpret_cast<PFN_AvRevertMmThreadChars>(
                GetProcAddress(hAvrt, "AvRevertMmThreadCharacteristics")
            );
            if (avSetChars) {
                DWORD taskIdx = 0;
                s_mmcssHandle = avSetChars(L"Games", &taskIdx);
                if (s_mmcssHandle && avSetPrio) {
                    avSetPrio(s_mmcssHandle, 2); // AVRT_PRIORITY_CRITICAL
                }
            }
        }

        if (HMODULE hDwm = LoadLibraryW(L"dwmapi.dll")) {
            s_dwmEnableMMCSS = reinterpret_cast<PFN_DwmEnableMMCSS>(
                GetProcAddress(hDwm, "DwmEnableMMCSS")
            );
            if (s_dwmEnableMMCSS) {
                s_dwmEnableMMCSS(TRUE);
            }
        }

        QueryPerformanceFrequency(&s_qpcFreq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        s_nextFrameTick = now.QuadPart;

        s_kernelReady = true;
    }

    struct KernelLifecycleGuard {
        ~KernelLifecycleGuard() {
            if (!s_kernelReady) return;
            if (s_highResTimer) {
                CloseHandle(s_highResTimer);
                s_highResTimer = nullptr;
            }
            if (s_mmcssHandle && s_avRevert) {
                s_avRevert(s_mmcssHandle);
                s_mmcssHandle = nullptr;
            }
            if (s_dwmEnableMMCSS) {
                s_dwmEnableMMCSS(FALSE);
            }
            if (s_timeEndPeriod) {
                s_timeEndPeriod(1);
            }
            s_kernelReady = false;
        }
    };
    static KernelLifecycleGuard s_lifecycleGuard;

    // 2. Tắt V-Sync 60Hz ở tầng Driver GPU, chống nhòe hình và khử răng cưa MSAA
    void configureVisualClarity() {
        if (!s_glReady) {
            HMODULE hOgl = GetModuleHandleW(L"opengl32.dll");
            if (!hOgl) {
                hOgl = LoadLibraryW(L"opengl32.dll");
            }
            if (hOgl) {
                s_wglGetProcAddress = reinterpret_cast<PFN_wglGetProcAddress>(
                    GetProcAddress(hOgl, "wglGetProcAddress")
                );
                s_glEnable = reinterpret_cast<PFN_glEnable>(
                    GetProcAddress(hOgl, "glEnable")
                );
                s_glDisable = reinterpret_cast<PFN_glDisable>(
                    GetProcAddress(hOgl, "glDisable")
                );
                s_glHint = reinterpret_cast<PFN_glHint>(
                    GetProcAddress(hOgl, "glHint")
                );
                s_glFlush = reinterpret_cast<PFN_glFlush>(
                    GetProcAddress(hOgl, "glFlush")
                );
            }
            s_glReady = true;
        }

        if (!s_wglSwapIntervalEXT && s_wglGetProcAddress) {
            s_wglSwapIntervalEXT = reinterpret_cast<PFN_wglSwapIntervalEXT>(
                s_wglGetProcAddress("wglSwapIntervalEXT")
            );
        }

        if (s_wglSwapIntervalEXT) {
            s_wglSwapIntervalEXT(0);
        }

        if (s_glEnable) {
            s_glEnable(GL_MULTISAMPLE_CAP);
        }

        if (s_glDisable) {
            s_glDisable(GL_POLYGON_SMOOTH_CAP);
            s_glDisable(GL_LINE_SMOOTH_CAP);
        }

        if (s_glHint) {
            s_glHint(GL_PERSPECTIVE_CORRECTION_CAP, GL_NICEST_MODE);
            s_glHint(GL_MULTISAMPLE_FILTER_HINT_NV_CAP, GL_NICEST_MODE);
        }

        if (s_glFlush) {
            s_glFlush();
        }
    }

    // 3. Quét trực tiếp tín hiệu chuột và phím từ nhân Windows cho các pha Frame-Perfect
    void pollHardwareInputBypass() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        while (PeekMessageW(&msg, nullptr, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // 4. Điều nhịp phần cứng 480+ FPS thật & Tán xạ vệt xé hình trên màn 60Hz
    void paceTrue480AndScrambleTear() {
        if (!s_kernelReady) {
            initKernelBoost();
        }

        ++s_frameCount;

        double activeDt = s_targetFrameTime;
        if (activeDt <= INTERNAL_BYPASS_INTERVAL) {
            return;
        }

        double exactTicks = (static_cast<double>(s_qpcFreq.QuadPart) * activeDt) + s_tickRemainder;
        LONGLONG stepTicks = static_cast<LONGLONG>(exactTicks);
        s_tickRemainder = exactTicks - static_cast<double>(stepTicks);

        double goldenPhase = std::fmod(static_cast<double>(s_frameCount) * 0.6180339887, 1.0) - 0.5;
        LONGLONG ditherTicks = static_cast<LONGLONG>(goldenPhase * (s_qpcFreq.QuadPart / 250000));

        s_nextFrameTick += stepTicks;
        LONGLONG targetTick = s_nextFrameTick + ditherTicks;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        if (now.QuadPart > targetTick + (stepTicks * 3)) {
            s_nextFrameTick = now.QuadPart;
            targetTick = s_nextFrameTick;
        }

        LONGLONG remainingTicks = targetTick - now.QuadPart;
        if (remainingTicks > 0) {
            double remainingSec = static_cast<double>(remainingTicks) / static_cast<double>(s_qpcFreq.QuadPart);

            if (remainingSec > 0.00085 && s_highResTimer != nullptr) {
                LARGE_INTEGER dueTime;
                dueTime.QuadPart = -static_cast<LONGLONG>((remainingSec - 0.0004) * 10000000.0);
                if (dueTime.QuadPart < 0) {
                    SetWaitableTimer(s_highResTimer, &dueTime, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(s_highResTimer, 1);
                }
            }

            do {
                YieldProcessor();
                QueryPerformanceCounter(&now);
            } while (now.QuadPart < targetTick);
        }
    }

    void unlockInternalLoop() {
        if (auto* app = CCApplication::sharedApplication()) {
            s_internalHookCall = true;
            app->setAnimationInterval(INTERNAL_BYPASS_INTERVAL);
            s_internalHookCall = false;
        }
    }
}

class $modify(OverdriveApp, CCApplication) {
    void setAnimationInterval(double interval) {
        if (!OverdriveEngineV8::s_internalHookCall) {
            if (interval > FLOOR_FRAME_TIME || interval <= 0.0) {
                OverdriveEngineV8::s_targetFrameTime = FLOOR_FRAME_TIME;
            } else {
                OverdriveEngineV8::s_targetFrameTime = interval;
            }
        }
        CCApplication::setAnimationInterval(INTERNAL_BYPASS_INTERVAL);
    }
};

class $modify(OverdriveScheduler, CCScheduler) {
    void update(float dt) {
        OverdriveEngineV8::initKernelBoost();
        OverdriveEngineV8::pollHardwareInputBypass();

        float targetDt = static_cast<float>(OverdriveEngineV8::s_targetFrameTime);
        if (targetDt > static_cast<float>(FLOOR_FRAME_TIME)) {
            targetDt = static_cast<float>(FLOOR_FRAME_TIME);
        }

        float timeScale = this->getTimeScale();
        float idealStep = (timeScale > 0.0f) ? (targetDt * timeScale) : targetDt;

        if (dt > 0.0f && dt <= idealStep * 1.35f) {
            if (dt >= idealStep * 0.70f) {
                OverdriveEngineV8::s_physicsDebt += (dt - idealStep);
                float microAdjust = std::clamp(OverdriveEngineV8::s_physicsDebt * 0.15f, -0.00008f, 0.00008f);
                OverdriveEngineV8::s_physicsDebt -= microAdjust;
                CCScheduler::update(idealStep + microAdjust);
            } else {
                CCScheduler::update(dt);
            }
            return;
        }

        if (dt > idealStep * 1.35f && dt < 0.1f) {
            float totalTime = dt + OverdriveEngineV8::s_subframeCarry;
            int subFrames = static_cast<int>(totalTime / idealStep);
            subFrames = std::clamp(subFrames, 1, 8);

            float stepDt = totalTime / static_cast<float>(subFrames);
            if (stepDt > idealStep) {
                stepDt = idealStep;
            }

            for (int i = 0; i < subFrames; ++i) {
                if (i > 0) {
                    OverdriveEngineV8::pollHardwareInputBypass();
                }
                CCScheduler::update(stepDt);
            }

            OverdriveEngineV8::s_subframeCarry = std::clamp(
                totalTime - (stepDt * static_cast<float>(subFrames)),
                0.0f,
                idealStep
            );
        } else {
            OverdriveEngineV8::s_physicsDebt = 0.0f;
            OverdriveEngineV8::s_subframeCarry = 0.0f;
            CCScheduler::update(idealStep);
        }
    }
};

class $modify(OverdriveEGLView, CCEGLView) {
    void swapBuffers() {
        OverdriveEngineV8::configureVisualClarity();
        OverdriveEngineV8::paceTrue480AndScrambleTear();
        CCEGLView::swapBuffers();
    }
};

$on_mod(Loaded) {
    OverdriveEngineV8::initKernelBoost();

    Loader::get()->queueInMainThread([]() {
        OverdriveEngineV8::configureVisualClarity();
        OverdriveEngineV8::unlockInternalLoop();
    });
}
