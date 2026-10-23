// ============================================================================
//  movement.cpp — speedhack, fly, super jump, teleport + BUNNYHOP + TIRO NO AR
// ============================================================================
#include "features.h"
#include "sdk.h"
#include "hooks.h"
#include "common.h"

#include <windows.h>
#include <cmath>

namespace Movement {

    static float     g_baseSpeed = 0.f;
    static ULONGLONG g_last = 0;

    enum AirState { AIR_GROUND = 0, AIR_UP = 1, AIR_DOWN = 2 };
    static int       g_air = AIR_GROUND;
    static ULONGLONG g_airSince = 0;
    static bool      g_spaceHeld = false;
    static ULONGLONG g_spaceDownMs = 0;
    static float     g_lastY = 0.f;
    static bool      g_haveLastY = false;
    static Vec3      g_jumpVel{};
    static bool      g_haveJumpVel = false;
    static bool      g_groundLogged = false;
    static bool      g_velJumped = false;
    static ULONGLONG g_velJumpMs = 0;
    static float     g_jumpSpeedMeasured = 0.f;

    struct GunInfo {
        Il2CppObject* obj = nullptr;
        uint32_t cur = 0, max = 0, reserve = 0, reload = 0;
    };
    static GunInfo   s_gun;
    static ULONGLONG s_gunMs = 0;
    static ULONGLONG s_lastAirFire = 0;

    static const ULONGLONG kGunCacheMs = 300;
    static const ULONGLONG kAirStuckMs = 1500;

    static inline bool Down(int vk) {
        return vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    static void InjectSpace(bool down) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        const UINT sc = MapVirtualKeyA(VK_SPACE, MAPVK_VK_TO_VSC);
        if (sc) {
            in.ki.wScan = (WORD)sc;
            in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0u : KEYEVENTF_KEYUP);
        }
        else {
            in.ki.wVk = VK_SPACE;
            in.ki.dwFlags = down ? 0u : KEYEVENTF_KEYUP;
        }
        SendInput(1, &in, sizeof(INPUT));
    }

    static void InvokeSetPosition(Il2CppObject* tf, Vec3 pos) {
        if (!SDK::M_set_position || !tf) return;
        void* a[1];
        a[0] = &pos;
        il2cpp_runtime_invoke(SDK::M_set_position, tf, a, nullptr);
    }

    static bool IsGrounded(Il2CppObject* p, float dt) {
        if (SDK::O_grounded) {
            unsigned char b = 0;
            if (TryRead<unsigned char>((uintptr_t)p + SDK::O_grounded, b)) {
                if (!g_groundLogged) { g_groundLogged = true; Log("[bhop] solo por CAMPO"); }
                if (b) g_air = AIR_GROUND;
                else if (g_air == AIR_GROUND) { g_air = AIR_UP; g_airSince = GetTickCount64(); }
                return b != 0;
            }
        }

        if (!g_groundLogged) { g_groundLogged = true; Log("[bhop] solo por HEURISTICA"); }

        const Vec3 pos = SDK::GetPos(p);
        const float dy = g_haveLastY ? (pos.y - g_lastY) : 0.f;
        g_lastY = pos.y;
        g_haveLastY = true;

        float vy = 0.f;
        if (SDK::O_vel) vy = SDK::GetVelocity(p).y;
        else if (dt > 0.001f) vy = dy / dt;

        const ULONGLONG now = GetTickCount64();

        switch (g_air) {
        case AIR_GROUND:
            if (vy > 0.4f || dy > 0.015f) { g_air = AIR_UP;   g_airSince = now; }
            else if (vy < -0.4f || dy < -0.015f) { g_air = AIR_DOWN; g_airSince = now; }
            break;
        case AIR_UP:
            if (vy < -0.4f || dy < -0.015f) { g_air = AIR_DOWN; g_airSince = now; }
            else if (now - g_airSince > kAirStuckMs) { g_air = AIR_GROUND; }
            break;
        case AIR_DOWN:
            if (fabsf(vy) < 0.30f && fabsf(dy) < 0.012f) { g_air = AIR_GROUND; }
            else if (now - g_airSince > kAirStuckMs) { g_air = AIR_GROUND; }
            break;
        }
        return g_air == AIR_GROUND;
    }

    static void ReleaseSpace() {
        if (g_spaceHeld) { InjectSpace(false); g_spaceHeld = false; }
    }

    static void BunnyHop(Il2CppObject* p, bool grounded, ULONGLONG now) {
        const bool want =
            C.bhop &&
            (!C.bhopKey || Down(C.bhopKey)) &&
            !(C.menuOpen && C.bhopSkipMenu);

        if (!want) {
            ReleaseSpace();
            g_velJumped = false;
            if (C.bhopKeepSpeed) { g_jumpVel = Vec3(); g_haveJumpVel = false; }
            return;
        }

        const int mode = C.bhopMode;
        const ULONGLONG holdMs = (C.bhopHoldMs > 5) ? (ULONGLONG)C.bhopHoldMs : 25;
        const ULONGLONG reMs = (C.bhopRePressMs > 10) ? (ULONGLONG)C.bhopRePressMs : 70;

        if (mode == 0) {
            if (!g_spaceHeld) { InjectSpace(true); g_spaceHeld = true; g_spaceDownMs = now; }
        }
        else if (mode == 2 && SDK::O_vel) {
            float sp = C.bhopJumpSpeed;
            if (sp <= 0.f) sp = (g_jumpSpeedMeasured > 1.f) ? g_jumpSpeedMeasured : 5.5f;

            if (grounded && !g_velJumped) {
                Vec3 v = SDK::GetVelocity(p);
                v.y = sp;
                SafeWrite<Vec3>((uintptr_t)p + SDK::O_vel, v);
                g_velJumped = true;
                g_velJumpMs = now;
            }
            else if (!grounded) {
                g_velJumped = false;
                const float vy = SDK::GetVelocity(p).y;
                if (vy > g_jumpSpeedMeasured && vy < 30.f) g_jumpSpeedMeasured = vy;
            }
            else if (now - g_velJumpMs > reMs) {
                g_velJumped = false;
            }
        }
        else {
            if (grounded) {
                if (!g_spaceHeld) {
                    InjectSpace(true);
                    g_spaceHeld = true;
                    g_spaceDownMs = now;
                }
                else if (now - g_spaceDownMs >= reMs) {
                    InjectSpace(false);
                    g_spaceHeld = false;
                }
            }
            else if (g_spaceHeld && now - g_spaceDownMs >= holdMs) {
                InjectSpace(false);
                g_spaceHeld = false;
            }
        }

        if (!C.bhopKeepSpeed || !SDK::O_vel) return;

        const Vec3 v = SDK::GetVelocity(p);
        if (grounded) {
            g_jumpVel = v;
            g_haveJumpVel = true;
            return;
        }
        if (!g_haveJumpVel) return;

        const float keep = sqrtf(g_jumpVel.x * g_jumpVel.x + g_jumpVel.z * g_jumpVel.z);
        const float cur = sqrtf(v.x * v.x + v.z * v.z);
        if (keep > 0.5f && cur < keep * 0.98f)
            SafeWrite<Vec3>((uintptr_t)p + SDK::O_vel, Vec3(g_jumpVel.x, v.y, g_jumpVel.z));
    }

    static bool GunReady() {
        const ULONGLONG now = GetTickCount64();
        if (now - s_gunMs > kGunCacheMs || !s_gun.obj) {
            s_gunMs = now;
            s_gun = GunInfo{};
            s_gun.obj = SDK::GetGunStatsLive();
            if (s_gun.obj) {
                uint32_t spread = 0, rate = 0, dmg = 0;
                SDK::ResolveWeaponFields(s_gun.obj, s_gun.cur, s_gun.max,
                    s_gun.reserve, s_gun.reload, spread, rate, dmg);
            }
        }
        if (!s_gun.obj || !SDK::ValidInstance(s_gun.obj)) return true;

        if (s_gun.reload) {
            unsigned char r = 0;
            if (TryRead<unsigned char>((uintptr_t)s_gun.obj + s_gun.reload, r) && r)
                return false;
        }
        if (s_gun.cur) {
            const int c = SafeRead<int>((uintptr_t)s_gun.obj + s_gun.cur, 1);
            if (c <= 0) return false;
        }
        return true;
    }

    static void AirShoot(Il2CppObject* p, bool grounded, ULONGLONG now) {
        if (!C.shootInAir || grounded) return;
        if (C.airFireKey && !Down(C.airFireKey)) return;
        if (C.aimbot && C.aimAutoFire && Aimbot::IsActive()) return;

        if (now - s_lastAirFire < (ULONGLONG)C.airFireDelayMs) return;
        if (!SDK::FireBulletReady() || !GunReady()) return;

        Vec3 aim;
        if (Il2CppObject* t = SDK::g_aimTarget)
            aim = SDK::TargetAimPos(t, C.aimBone);

        if (aim.zero()) {
            const Vec3 fwd = Aimbot::CamForward();
            if (fwd.zero()) return;
            aim = SDK::MuzzlePos() + fwd * 1000.f;
        }

        SDK::FireBulletAt(aim);
        s_lastAirFire = now;
    }

    void Tick() {
        ULONGLONG now = GetTickCount64();
        float dt = g_last ? (float)(now - g_last) / 1000.f : 0.016f;
        if (dt > 0.1f) dt = 0.1f;
        g_last = now;

        if (!SDK::g_localPlayer) return;

        Il2CppObject* tf = SDK::GetTransform(SDK::g_localPlayer);

        const bool grounded = IsGrounded(SDK::g_localPlayer, dt);
        BunnyHop(SDK::g_localPlayer, grounded, now);
        AirShoot(SDK::g_localPlayer, grounded, now);

        if (SDK::O_speed && C.speedHack) {
            if (g_baseSpeed == 0.f)
                g_baseSpeed = ReadAt<float>((uintptr_t)SDK::g_localPlayer + SDK::O_speed);
            if (g_baseSpeed > 0.f)
                WriteAt<float>((uintptr_t)SDK::g_localPlayer + SDK::O_speed, g_baseSpeed * C.speedValue);
        }
        else if (SDK::O_speed && !C.speedHack && g_baseSpeed > 0.f) {
            WriteAt<float>((uintptr_t)SDK::g_localPlayer + SDK::O_speed, g_baseSpeed);
            g_baseSpeed = 0.f;
        }

        if (!tf || !SDK::M_set_position) return;

        bool space = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;

        if (C.fly && space) {
            Vec3 pos = SDK::GetPos(tf);
            pos.y += C.flySpeed * dt;
            InvokeSetPosition(tf, pos);
        }

        if (C.superJump && space) {
            Vec3 pos = SDK::GetPos(tf);
            pos.y += C.jumpPower * dt * 0.5f;
            InvokeSetPosition(tf, pos);
        }

        if (C.teleport && (GetAsyncKeyState('T') & 0x8000)) {
            Il2CppObject* t = SDK::g_aimTarget;
            if (t) {
                Il2CppObject* tt = SDK::GetTransform(t);
                if (tt) InvokeSetPosition(tf, SDK::GetPos(tt));
            }
        }
    }

} // namespace Movement