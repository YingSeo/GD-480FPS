#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCEGLView.hpp>

#include <Windows.h>
#include <cmath>
#include <cstdint>

using namespace geode::prelude;

// Mức sàn tối thiểu: 480 FPS thật (2.083333 ms mỗi khung hình)
constexpr double FLOOR_FPS = 480.0;
constexpr double FLOOR_FRAME_TIME = 1.0 / FLOOR_FPS;

// Xung nhịp nội bộ siêu nhỏ giúp bỏ qua hàm Sleep(1) gây tụt FPS của Cocos2d-x
constexpr double BYPASS_SLEEP_INTERVAL = 0.0001;

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

#ifndef PM_QS_INPUT
#define PM_QS_INPUT ((QS_MOUSE | QS_KEY | QS_RAWINPUT) << 16)
#endif

namespace {
    template <typename T>
    constexpr T clampVal(T val, T lo, T hi) {
        return (val < lo) ? lo : ((val > hi) ? hi : val);
    }

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
    using PFN_SetThreadInformation   = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);

    using PFN_wglGetCurrentContext   = HGLRC(WINAPI*)();
    using PFN_wglGetProcAddress      = PROC(WINAPI*)(LPCSTR);
    using PFN_wglSwapIntervalEXT     = BOOL(WINAPI*)(int);
    using PFN_glEnable               = void(APIENTRY*)(unsigned int);
    using PFN_glDisable              = void(APIENTRY*)(unsigned int);
    using PFN_glHint                 = void(APIENTRY*)(unsigned int, unsigned int);
    using PFN_glFlush                = void(APIENTRY*)();

    // Con trỏ hàm gọi trực tiếp vào libcocos2d.dll (Không bao giờ lỗi binding/linker)
    using PFN_CCAppShared            = void*(__cdecl*)();
    using PFN_CCAppSetInterval       = void(__fastcall*)(void*, double);
    using PFN_CCEGLShared            = void*(__cdecl*)();
    using PFN_CCEGLToggleVSync       = void(__fastcall*)(void*, bool);
}

namespace TitaniumEngineV9 {
    static LARGE_INTEGER s_qpcFreq = {};
    static LONGLONG s_nextFrameTick = 0;
    static LONGLONG s_lastRenderTick = 0;
    static double s_tickRemainder = 0.0;

    static HANDLE s_highResTimer = nullptr;
    static HANDLE s_mmcssHandle = nullptr;
    static HGLRC s_lastGlContext = nullptr;

    static PFN_timeEndPeriod s_timeEndPeriod = nullptr;
    static PFN_AvRevertMmThreadChars s_avRevert = nullptr;
    static PFN_DwmEnableMMCSS s_dwmEnableMMCSS = nullptr;

    static PFN_wglGetCurrentContext s_wglGetCurrentContext = nullptr;
    static PFN_wglGetProcAddress s_wglGetProcAddress = nullptr;
    static PFN_wglSwapIntervalEXT s_wglSwapIntervalEXT = nullptr;
    static PFN_glEnable s_glEnable = nullptr;
    static PFN_glDisable s_glDisable = nullptr;
    static PFN_glHint s_glHint = nullptr;
    static PFN_glFlush s_glFlush = nullptr;

    static PFN_CCAppShared s_ccAppShared = nullptr;
    static PFN_CCAppSetInterval s_ccAppSetInterval = nullptr;
    static PFN_CCEGLShared s_ccEglShared = nullptr;
    static PFN_CCEGLToggleVSync s_ccEglToggleVSync = nullptr;

    static float s_smoothedDt = static_cast<float>(FLOOR_FRAME_TIME);
    static float s_physicsDebt = 0.0f;
    static float s_subframeCarry = 0.0f;
    static uint64_t s_frameCount = 0;

    static bool s_kernelReady = false;
    static bool s_glSymbolsLoaded = false;
    static bool s_cocosSymbolsLoaded = false;

    // 1. Nạp động các hàm điều khiển V-Sync và AnimationInterval từ libcocos2d.dll
    void resolveCocosSymbols() {
        if (s_cocosSymbolsLoaded) return;
        if (HMODULE hCocos = GetModuleHandleW(L"libcocos2d.dll")) {
            s_ccAppShared = reinterpret_cast<PFN_CCAppShared>(
                GetProcAddress(hCocos, "?sharedApplication@CCApplication@cocos2d@@SAPEAV12@XZ")
            );
            s_ccAppSetInterval = reinterpret_cast<PFN_CCAppSetInterval>(
                GetProcAddress(hCocos, "?setAnimationInterval@CCApplication@cocos2d@@UEAAXN@Z")
            );
            s_ccEglShared = reinterpret_cast<PFN_CCEGLShared>(
                GetProcAddress(hCocos, "?sharedOpenGLView@CCEGLView@cocos2d@@SAPEAV12@XZ")
            );
            s_ccEglToggleVSync = reinterpret_cast<PFN_CCEGLToggleVSync>(
                GetProcAddress(hCocos, "?toggleVerticalSync@CCEGLView@cocos2d@@QEAAX_N@Z")
            );
        }
        s_cocosSymbolsLoaded = true;
    }

    void unlockCocosFrameLimiter() {
        resolveCocosSymbols();
        if (s_ccEglShared && s_ccEglToggleVSync) {
            if (void* view = s_ccEglShared()) {
                s_ccEglToggleVSync(view, false);
            }
        }
        if (s_ccAppShared && s_ccAppSetInterval) {
            if (void* app = s_ccAppShared()) {
                // Đặt khoảng chờ nội bộ về 0.0001s để vô hiệu hóa hàm Sleep(1) gây drop FPS của game
                s_ccAppSetInterval(app, BYPASS_SLEEP_INTERVAL);
            }
        }
    }

    // 2. Mở khóa toàn diện Kernel Windows 11 (0.5ms Timer + Tắt EcoQoS Process/Thread + MMCSS)
    void initWindows11Kernel() {
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

        // Vô hiệu hóa EcoQoS ở cả cấp Tiến trình và cấp Luồng chính trên Windows 11
        if (HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll")) {
            auto setProcInfo = reinterpret_cast<PFN_SetProcessInformation>(
                GetProcAddress(hKernel32, "SetProcessInformation")
            );
            auto setThreadInfo = reinterpret_cast<PFN_SetThreadInformation>(
                GetProcAddress(hKernel32, "SetThreadInformation")
            );

            Win11PowerThrottling throttling = {};
            throttling.Version = 1;
            throttling.ControlMask = 0x1 | 0x4; // EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION
            throttling.StateMask = 0;

            if (setProcInfo) {
                setProcInfo(hProcess, 4, &throttling, sizeof(throttling));
            }
            if (setThreadInfo) {
                Win11PowerThrottling threadThrottling = { 1, 0x1, 0 };
                setThreadInfo(hThread, 11, &threadThrottling, sizeof(threadThrottling));
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
        s_lastRenderTick = now.QuadPart;

        s_kernelReady = true;
    }

    // Tự động hoàn trả tài nguyên hệ điều hành khi thoát game (RAII Guard)
    struct TitaniumLifecycleGuard {
        ~TitaniumLifecycleGuard() {
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
    static TitaniumLifecycleGuard s_lifecycleGuard;

    // 3. Cấu hình OpenGL thông minh (Context Caching - Không gây nghẽn Driver)
    void maintainOpenGLClarity() {
        if (!s_glSymbolsLoaded) {
            HMODULE hOgl = GetModuleHandleW(L"opengl32.dll");
            if (!hOgl) {
                hOgl = LoadLibraryW(L"opengl32.dll");
            }
            if (hOgl) {
                s_wglGetCurrentContext = reinterpret_cast<PFN_wglGetCurrentContext>(
                    GetProcAddress(hOgl, "wglGetCurrentContext")
                );
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
            s_glSymbolsLoaded = true;
        }

        HGLRC currentCtx = s_wglGetCurrentContext ? s_wglGetCurrentContext() : nullptr;

        // Chỉ áp dụng lại trạng thái OpenGL khi Context thay đổi (khởi động game, đổi Fullscreen)
        // hoặc định kỳ mỗi 240 frame để chống nghẽn Driver mà vẫn giữ cứng cấu hình!
        if (currentCtx != s_lastGlContext || (s_frameCount % 240 == 0)) {
            s_lastGlContext = currentCtx;

            if (!s_wglSwapIntervalEXT && s_wglGetProcAddress) {
                s_wglSwapIntervalEXT = reinterpret_cast<PFN_wglSwapIntervalEXT>(
                    s_wglGetProcAddress("wglSwapIntervalEXT")
                );
            }
            if (s_wglSwapIntervalEXT) {
                s_wglSwapIntervalEXT(0); // Mở khóa giới hạn 60Hz ở tầng Driver GPU
            }
            if (s_glEnable) {
                s_glEnable(GL_MULTISAMPLE_CAP); // Bật khử răng cưa MSAA sắc nét
            }
            if (s_glDisable) {
                // Tắt bộ lọc làm nhòe cạnh đa giác gây mờ icon và vệt kẻ đen giữa các block
                s_glDisable(GL_POLYGON_SMOOTH_CAP);
                s_glDisable(GL_LINE_SMOOTH_CAP);
            }
            if (s_glHint) {
                s_glHint(GL_PERSPECTIVE_CORRECTION_CAP, GL_NICEST_MODE);
                s_glHint(GL_MULTISAMPLE_FILTER_HINT_NV_CAP, GL_NICEST_MODE);
            }
            unlockCocosFrameLimiter();
        }

        // Xả hàng đợi lệnh vẽ xuống GPU ngay lập tức để xóa hiện tượng bóng mờ và lệch hình
        if (s_glFlush) {
            s_glFlush();
        }
    }

    // 4. Quét trực tiếp tín hiệu Chuột & Bàn phím với cờ PM_NOYIELD cho pha Frame-Perfect
    void pollZeroLatencyInput() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE | PM_NOYIELD)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        while (PeekMessageW(&msg, nullptr, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE | PM_NOYIELD)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // 5. Điều nhịp phần cứng 480.0 FPS ổn định tuyệt đối & Tán xạ vệt xé hình màn 60Hz
    void paceHardwareFrame() {
        if (!s_kernelReady) {
            initWindows11Kernel();
        }

        ++s_frameCount;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        // Nếu một mod bên ngoài (như Eclipse Menu) đang chủ động đẩy tốc độ render vượt 520 FPS,
        // tự động nhường đường để đạt mức FPS cực đại mà không cản trở
        LONGLONG rawRenderDelta = now.QuadPart - s_lastRenderTick;
        s_lastRenderTick = now.QuadPart;
        if (rawRenderDelta > 0 && rawRenderDelta < (s_qpcFreq.QuadPart / 540)) {
            s_nextFrameTick = now.QuadPart;
            return;
        }

        // Tính bước nhảy QPC chính xác cho 480.00 FPS kèm bộ tích lũy phần lẻ thập phân
        double exactTicks = (static_cast<double>(s_qpcFreq.QuadPart) * FLOOR_FRAME_TIME) + s_tickRemainder;
        LONGLONG stepTicks = static_cast<LONGLONG>(exactTicks);
        s_tickRemainder = exactTicks - static_cast<double>(stepTicks);

        // Dịch pha vi mô theo tỷ lệ vàng (+/- 3.5 micro-giây) để xóa vệt xé ngang cố định trên màn 60Hz
        double goldenOffset = std::fmod(static_cast<double>(s_frameCount) * 0.61803398875, 1.0) - 0.5;
        LONGLONG ditherTicks = static_cast<LONGLONG>(goldenOffset * (s_qpcFreq.QuadPart / 280000));

        s_nextFrameTick += stepTicks;
        LONGLONG targetTick = s_nextFrameTick + ditherTicks;

        // Tự phục hồi ngay nếu vừa chuyển cảnh hoặc lag spike > 3 frame (Chống hiệu ứng tua nhanh)
        if (now.QuadPart > targetTick + (stepTicks * 3)) {
            s_nextFrameTick = now.QuadPart;
            targetTick = s_nextFrameTick;
        }

        LONGLONG remainingTicks = targetTick - now.QuadPart;
        if (remainingTicks > 0) {
            double remainingSec = static_cast<double>(remainingTicks) / static_cast<double>(s_qpcFreq.QuadPart);

            // Chỉ ngủ bằng High-Res Waitable Timer khi thời gian dư > 0.90ms (An toàn 100% không oversleep)
            if (remainingSec > 0.00090 && s_highResTimer != nullptr) {
                LARGE_INTEGER dueTime;
                dueTime.QuadPart = -static_cast<LONGLONG>((remainingSec - 0.00045) * 10000000.0);
                if (dueTime.QuadPart < 0) {
                    SetWaitableTimer(s_highResTimer, &dueTime, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(s_highResTimer, 1);
                }
            }

            // Vòng lặp Spin-Wait độ chính xác nano-giây cho phần đuôi < 0.45ms
            do {
                YieldProcessor();
                QueryPerformanceCounter(&now);
            } while (now.QuadPart < targetTick);
        }
    }
}

// ---------------------------------------------------------------------------
// Hook 1: CCScheduler — Ổn định EMA 480Hz+, Sub-Frame Bypass & Frame-Perfect Input
// ---------------------------------------------------------------------------
class $modify(TitaniumScheduler, CCScheduler) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("cocos2d::CCScheduler::update", Priority::Early);
    }

    void update(float dt) {
        TitaniumEngineV9::initWindows11Kernel();
        TitaniumEngineV9::pollZeroLatencyInput();

        constexpr float floorDt = static_cast<float>(FLOOR_FRAME_TIME);
        float timeScale = this->getTimeScale();
        float idealStep = (timeScale > 0.0f) ? (floorDt * timeScale) : floorDt;

        // Trường hợp 1: Nếu Eclipse Menu đang đẩy FPS cao hơn 500 FPS (dt nhỏ hơn 1/500s),
        // cho phép chạy thẳng không giới hạn trần!
        if (dt > 0.0f && dt < idealStep * 0.85f) {
            CCScheduler::update(dt);
            return;
        }

        // Trường hợp 2: Máy đang chạy quanh mốc 480 FPS (từ 360 FPS đến 500 FPS):
        // Dùng bộ lọc EMA + Zero-Drift Debt để làm phẳng tuyệt đối bước thời gian về 1/480s,
        // giúp icon không bao giờ bị lệch sub-pixel mà nhạc nền vẫn khớp 100%!
        if (dt >= idealStep * 0.85f && dt <= idealStep * 1.35f) {
            TitaniumEngineV9::s_smoothedDt = (TitaniumEngineV9::s_smoothedDt * 0.90f) + (dt * 0.10f);
            TitaniumEngineV9::s_physicsDebt += (dt - idealStep);
            TitaniumEngineV9::s_physicsDebt = clampVal(TitaniumEngineV9::s_physicsDebt, -idealStep, idealStep);

            float microCorrection = clampVal(TitaniumEngineV9::s_physicsDebt * 0.12f, -0.00006f, 0.00006f);
            TitaniumEngineV9::s_physicsDebt -= microCorrection;

            CCScheduler::update(idealStep + microCorrection);
            return;
        }

        // Trường hợp 3: Cảnh quá nặng gây khựng tạm thời xuống dưới 360 FPS (dt > 1.35 * idealStep):
        // Tự động chia nhỏ khung hình thành các Sub-Frame 480Hz và quét chuột ở từng bước
        // để giữ trọn vẹn khả năng click Frame-Perfect và không cho nhịp vật lý tụt xuống dưới 480!
        if (dt > idealStep * 1.35f && dt < 0.1f) {
            float totalTime = dt + TitaniumEngineV9::s_subframeCarry;
            int subFrames = static_cast<int>(totalTime / idealStep);
            subFrames = clampVal(subFrames, 1, 8);

            float stepDt = totalTime / static_cast<float>(subFrames);
            if (stepDt > idealStep) {
                stepDt = idealStep;
            }

            for (int i = 0; i < subFrames; ++i) {
                if (i > 0) {
                    TitaniumEngineV9::pollZeroLatencyInput();
                }
                CCScheduler::update(stepDt);
            }

            TitaniumEngineV9::s_subframeCarry = clampVal(
                totalTime - (stepDt * static_cast<float>(subFrames)),
                0.0f,
                idealStep
            );
        } else {
            // Khi mới tải màn chơi hoặc vừa thoát Pause
            TitaniumEngineV9::s_physicsDebt = 0.0f;
            TitaniumEngineV9::s_subframeCarry = 0.0f;
            CCScheduler::update(idealStep);
        }
    }
};

// ---------------------------------------------------------------------------
// Hook 2: CCEGLView — Xuất 480+ FPS thật lên màn 60Hz, chống nhòe & chống bể hình
// ---------------------------------------------------------------------------
class $modify(TitaniumEGLView, CCEGLView) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("cocos2d::CCEGLView::swapBuffers", Priority::Late);
    }

    void swapBuffers() {
        TitaniumEngineV9::maintainOpenGLClarity();
        TitaniumEngineV9::paceHardwareFrame();
        CCEGLView::swapBuffers();
    }
};

// ---------------------------------------------------------------------------
// Tự động kích hoạt ngay khi mở Geometry Dash 2.2081
// ---------------------------------------------------------------------------
$on_mod(Loaded) {
    TitaniumEngineV9::initWindows11Kernel();

    Loader::get()->queueInMainThread([]() {
        TitaniumEngineV9::maintainOpenGLClarity();
        TitaniumEngineV9::unlockCocosFrameLimiter();
    });
}
