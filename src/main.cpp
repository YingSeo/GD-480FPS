#include <Geode/Geode.hpp>
#include <Geode/modify/CCApplication.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <GL/gl.h>
#include <cmath>

using namespace geode::prelude;

// Hằng số mục tiêu 480 FPS thực (2.083333 ms mỗi khung hình)
constexpr double TARGET_FPS = 480.0;
constexpr double TARGET_FRAME_TIME = 1.0 / TARGET_FPS;

// Đảm bảo tương thích với mọi phiên bản Windows SDK khi biên dịch
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

// Con trỏ hàm mở rộng OpenGL để tắt V-Sync trực tiếp ở tầng Driver GPU
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

    // Bộ tích lũy sai số thời gian giúp mượt hình mà không bao giờ lệch nhạc (Zero-Drift)
    static float s_timeDebt = 0.0f;

    void initWin11KernelAndTimer() {
        if (s_kernelInitialized) return;

        // 1. Hạ chu kỳ ngắt hệ thống xuống 1ms
        timeBeginPeriod(1);

        // 2. Tạo High-Resolution Waitable Timer của Windows 11 (độ phân giải 0.5ms, không bị oversleep như Sleep(1))
        s_highResTimer = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS
        );

        // 3. Đăng ký luồng chính với Windows 11 MMCSS ở chế độ "Games" ưu tiên tối đa
        DWORD taskIndex = 0;
        s_mmcssHandle = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
        if (s_mmcssHandle) {
            AvSetMmThreadPriority(s_mmcssHandle, AVRT_PRIORITY_CRITICAL);
        }

        // 4. Nâng mức ưu tiên tiến trình và tắt hoàn toàn cơ chế bóp xung EcoQoS của Windows 11
        HANDLE hProcess = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();
        SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS);
        SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);

        // Cho phép Windows tự động boost luồng khi có sự kiện đầu vào chuột/bàn phím (FALSE = Enable Boost)
        SetProcessPriorityBoost(hProcess, FALSE);
        SetThreadPriorityBoost(hThread, FALSE);

        PROCESS_POWER_THROTTLING_STATE powerThrottling = {};
        powerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        powerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        powerThrottling.StateMask = 0; // 0 = Cấm Windows 11 hạ xung nhịp tiến trình này
        SetProcessInformation(
            hProcess,
            ProcessPowerThrottling,
            &powerThrottling,
            sizeof(powerThrottling)
        );

        // 5. Khởi tạo bộ đếm phần cứng QPC với bộ bù phần dư thập phân (tránh trôi FPS)
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

    // Cấu hình OpenGL 1 lần duy nhất để tránh nghẽn Driver và tránh lỗi viền đen giữa các block
    void initOpenGLOnce() {
        if (s_glInitialized) return;

        // Lấy hàm điều khiển V-Sync trực tiếp từ Driver NVIDIA/AMD/Intel
        s_wglSwapIntervalEXT = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(
            wglGetProcAddress("wglSwapIntervalEXT")
        );
        if (s_wglSwapIntervalEXT) {
            // 0 = Tắt hoàn toàn giới hạn 60Hz của màn hình ở tầng Driver
            s_wglSwapIntervalEXT(0);
        }

        // Bật Multisample Anti-Aliasing phần cứng, tuyệt đối không bật GL_POLYGON_SMOOTH để tránh rách cạnh block
        glEnable(GL_MULTISAMPLE);
        glDisable(GL_POLYGON_SMOOTH);

        // Giữ nguyên tọa độ pixel chuẩn để triệt tiêu hiện tượng lệch hình nhỏ (Sub-pixel shimmer)
        if (auto* director = CCDirector::sharedDirector()) {
            director->setProjection(kCCDirectorProjection2D);
        }

        s_glInitialized = true;
    }

    // Quét thẳng hàng đợi sự kiện chuột/phím của Windows ngay trước mỗi frame 480Hz
    void pollHardwareInput480Hz() {
        MSG msg;
        // Rút toàn bộ sự kiện Click chuột và Bấm phím đang nằm chờ trong Kernel Windows
        // để đẩy thẳng vào frame hiện tại mà không cần đợi vòng lặp kế tiếp
        while (PeekMessageW(&msg, nullptr, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        while (PeekMessageW(&msg, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // Khóa nhịp 480.00 FPS tuyệt đối + Tán xạ vệt xé hình cho màn 60Hz (Tear-Line Diffuser)
    void paceFrame480() {
        if (!s_kernelInitialized) {
            initWin11KernelAndTimer();
        }

        ++s_frameCounter;

        // Cộng dồn phần lẻ thập phân của số tick để đạt chính xác 480.0000 FPS
        double exactStep = s_ticksPerFrameExact + s_tickRemainder;
        LONGLONG stepTicks = static_cast<LONGLONG>(exactStep);
        s_tickRemainder = exactStep - static_cast<double>(stepTicks);

        // Dịch pha vi mô (+/- 6 micro-giây theo chuỗi lệch pha) để vệt xé hình trên màn 60Hz
        // không đứng im ở 1 vị trí cố định mà tản mịn ra toàn màn hình
        LONGLONG phaseDither = static_cast<LONGLONG>((s_frameCounter % 7) - 3) * (s_qpcFreq.QuadPart / 500000);
        s_nextFrameTick += stepTicks;
        LONGLONG targetTick = s_nextFrameTick + phaseDither;

        LARGE_INTEGER currentTick;
        QueryPerformanceCounter(&currentTick);

        // Nếu game bị khựng quá 3 frame (ví dụ vừa Alt+Tab hoặc vừa load màn chơi),
        // đồng bộ lại mốc thời gian ngay lập tức để không bị tua nhanh (fast-forward)
        if (currentTick.QuadPart > targetTick + (stepTicks * 3)) {
            s_nextFrameTick = currentTick.QuadPart;
            targetTick = s_nextFrameTick;
        }

        LONGLONG ticksRemaining = targetTick - currentTick.QuadPart;
        if (ticksRemaining > 0) {
            double secondsRemaining = static_cast<double>(ticksRemaining) / static_cast<double>(s_qpcFreq.QuadPart);

            // Chỉ dùng High-Res Waitable Timer khi thời gian dư lớn hơn 0.9ms (an toàn tuyệt đối, không oversleep)
            if (secondsRemaining > 0.0009 && s_highResTimer != nullptr) {
                LARGE_INTEGER dueTime;
                // Đổi sang đơn vị 100-nanosecond của Windows (trừ hao 0.45ms để thức dậy sớm rồi Spin-Lock)
                dueTime.QuadPart = -static_cast<LONGLONG>((secondsRemaining - 0.00045) * 10000000.0);
                if (dueTime.QuadPart < 0) {
                    SetWaitableTimerEx(s_highResTimer, &dueTime, 0, nullptr, nullptr, nullptr, 0);
                    WaitForSingleObject(s_highResTimer, 1);
                }
            }

            // Vòng lặp Spin-Wait độ chính xác nano-giây cho phần thời gian cuối cùng
            do {
                YieldProcessor();
                QueryPerformanceCounter(&currentTick);
            } while (currentTick.QuadPart < targetTick);
        }
    }
}

// ---------------------------------------------------------------------------
// 1. Quản lý cấu hình GameManager: Tự động kích hoạt 480 FPS không cần bật/tắt
// ---------------------------------------------------------------------------
class $modify(HyperGameManager, GameManager) {
    void applyHardLock480() {
        // "0030" = V-Sync (Tắt để vượt rào cản 60Hz của màn hình)
        this->setGameVariable("0030", false);
        // "0116" = Custom FPS Bypass (Bật để engine nhận diện mốc 480 FPS)
        this->setGameVariable("0116", true);
        this->m_customFPSTarget = static_cast<float>(TARGET_FPS);

        if (auto* app = CCApplication::sharedApplication()) {
            app->setAnimationInterval(TARGET_FRAME_TIME);
        }
        if (auto* view = CCEGLView::sharedOpenGLView()) {
            view->toggleVerticalSync(false);
        }
        if (HyperEngine480::s_wglSwapIntervalEXT) {
            HyperEngine480::s_wglSwapIntervalEXT(0);
        }
    }

    bool init() {
        if (!GameManager::init()) return false;
        HyperEngine480::initWin11KernelAndTimer();
        this->applyHardLock480();
        return true;
    }

    void reloadAllStep5() {
        GameManager::reloadAllStep5();
        HyperEngine480::s_glInitialized = false;
        this->applyHardLock480();
    }

    void applicationWillEnterForeground() {
        GameManager::applicationWillEnterForeground();
        // Khôi phục lại 480 FPS và quyền ưu tiên CPU ngay khi người chơi Alt+Tab quay lại game
        HyperEngine480::s_kernelInitialized = false;
        HyperEngine480::initWin11KernelAndTimer();
        this->applyHardLock480();
    }
};

// ---------------------------------------------------------------------------
// 2. Khóa khoảng thời gian chuẩn 1/480s ở tầng CCApplication
// ---------------------------------------------------------------------------
class $modify(HyperApp, CCApplication) {
    void setAnimationInterval(double interval) {
        CCApplication::setAnimationInterval(TARGET_FRAME_TIME);
    }
};

// ---------------------------------------------------------------------------
// 3. Làm mượt Delta-Time chống giật hình mà KHÔNG gây lệch nhạc (Zero-Drift)
// ---------------------------------------------------------------------------
class $modify(HyperScheduler, CCScheduler) {
    void update(float dt) {
        constexpr float idealDt = static_cast<float>(TARGET_FRAME_TIME);

        // Nếu thời gian giữa 2 frame chỉ dao động nhẹ quanh mốc 480 FPS (từ 1ms đến 5ms),
        // lọc bỏ nhiễu vi mô và tích lũy phần chênh lệch vào s_timeDebt để không bao giờ lệch nhạc
        if (dt > 0.001f && dt < 0.005f) {
            HyperEngine480::s_timeDebt += (dt - idealDt);

            // Giải phóng dần phần thời gian dư/thiếu (tối đa 5% mỗi frame) để giữ tốc độ thực tế 100%
            float correction = std::clamp(HyperEngine480::s_timeDebt * 0.1f, -0.0001f, 0.0001f);
            HyperEngine480::s_timeDebt -= correction;

            CCScheduler::update(idealDt + correction);
        } else {
            // Trường hợp vừa load level hoặc lag nặng, xả bộ nhớ đệm để tránh văng vật lý
            HyperEngine480::s_timeDebt = 0.0f;
            CCScheduler::update(dt);
        }
    }
};

// ---------------------------------------------------------------------------
// 4. Quét Input 480Hz trực tiếp trước mỗi bước vật lý để hỗ trợ Frame-Perfect
// ---------------------------------------------------------------------------
class $modify(HyperPlayLayer, GJBaseGameLayer) {
    void update(float dt) {
        // Đọc tín hiệu chuột/phím từ Windows ngay trước khi tính toán va chạm của frame này
        HyperEngine480::pollHardwareInput480Hz();
        GJBaseGameLayer::update(dt);
    }
};

// ---------------------------------------------------------------------------
// 5. Đồng bộ xuất hình OpenGL & Khóa nhịp phần cứng trong CCEGLView
// ---------------------------------------------------------------------------
class $modify(HyperEGLView, CCEGLView) {
    void swapBuffers() {
        HyperEngine480::initOpenGLOnce();

        // Đẩy toàn bộ lệnh vẽ trong hàng đợi xuống GPU ngay lập tức để giảm Input Lag
        glFlush();

        // Khóa nhịp phần cứng ở đúng 480.00 FPS trước khi lật buffer hình ảnh
        HyperEngine480::paceFrame480();

        CCEGLView::swapBuffers();
    }

    void toggleVerticalSync(bool vSync) {
        // Luôn ép tắt V-Sync để màn 60Hz không bao giờ khóa game về 60 FPS
        CCEGLView::toggleVerticalSync(false);
        if (HyperEngine480::s_wglSwapIntervalEXT) {
            HyperEngine480::s_wglSwapIntervalEXT(0);
        }
    }
};

// ---------------------------------------------------------------------------
// 6. Vòng đời tự động nạp và dọn dẹp tài nguyên Kernel Windows 11
// ---------------------------------------------------------------------------
$on_mod(Loaded) {
    HyperEngine480::initWin11KernelAndTimer();

    Loader::get()->queueInMainThread([]() {
        HyperEngine480::initOpenGLOnce();
        if (auto* gm = GameManager::sharedState()) {
            gm->setGameVariable("0030", false);
            gm->setGameVariable("0116", true);
            gm->m_customFPSTarget = static_cast<float>(TARGET_FPS);
        }
        if (auto* app = CCApplication::sharedApplication()) {
            app->setAnimationInterval(TARGET_FRAME_TIME);
        }
        if (auto* view = CCEGLView::sharedOpenGLView()) {
            view->toggleVerticalSync(false);
        }
    });
}

$on_mod(Unloaded) {
    HyperEngine480::cleanupKernelResources();
}
