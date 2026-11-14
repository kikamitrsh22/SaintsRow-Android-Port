// Symbols the Xendroid GPU/UI libraries reference from parts of Xenia/Xendroid we don't link
// (kernel, CPU JIT, emulator config, audio). In host mode none of the kernel/CPU paths run.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <android/log.h>

#include "xenia/base/cvar.h"
#include "xenia/base/system.h"
#include "xenia/config.h"

// ---- cvars defined in modules we don't link (defaults match Xendroid's) ----
DEFINE_path(storage_root, "", "Root path for persistent internal data storage.", "Storage");
DEFINE_int32(spin_park_mode, 0, "Guest spin-wait parking mode.", "Kernel");
DEFINE_uint32(guest_yield_sleep_us, 0, "Sleep on guest yields (us).", "Kernel");
DEFINE_int32(apu_queue_target, 0, "Audio queue target.", "APU");
DEFINE_bool(apu_adaptive_queue, false, "Adaptive audio queue.", "APU");
DEFINE_bool(log_high_frequency_kernel_calls, false, "Log kernel calls with the kHighFrequency tag.", "Logging");

// Directory of the app's native libraries (used to find a bundled Vulkan driver). Empty = system driver.
std::string g_native_lib_dir;

namespace xe
{
    void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message)
    {
        __android_log_print(ANDROID_LOG_WARN, "SaintsRow", "message box: %.*s", int(message.size()), message.data());
    }
}

namespace config
{
    toml::table LoadGameConfig(uint32_t) { return {}; }
    void SaveGameConfig(uint32_t, const toml::table&) {}
    void SaveGameConfigSetting(xe::Emulator*, const char*, const char*, bool) {}
    void SaveGameConfigSetting(xe::Emulator*, const char*, const char*, double) {}
    void SaveGameConfigSetting(xe::Emulator*, const char*, const char*, int32_t) {}
    void SaveGameConfigSetting(xe::Emulator*, const char*, const char*, uint32_t) {}
}

// ---- kernel / CPU members: never reached in host mode ----
extern "C" __attribute__((used)) void xglue_panic_stub()
{
    __android_log_print(ANDROID_LOG_FATAL, "SaintsRow", "a Xenia kernel function was called in host mode");
    std::abort();
}
extern "C" __attribute__((used)) void* xglue_return_this(void* self) { return self; } // constructors
extern "C" __attribute__((used)) void xglue_noop() {}
extern "C" __attribute__((used)) uint64_t xglue_return_zero() { return 0; }

#define XGLUE_ALIAS(sym, target) __asm__(".globl " sym "\n.set " sym ", " target)

// Constructors that may run from static initializers: leave the object untouched.
XGLUE_ALIAS("_ZN2xe6kernel3xam11UserSettingC1ENS1_13UserSettingIdENSt6__ndk17variantIJjifldNS4_12basic_stringIDsNS4_11char_traitsIDsEENS4_9allocatorIDsEEEENS4_6vectorIhNS9_IhEEEEEEE", "xglue_return_this");
XGLUE_ALIAS("_ZN2xe6kernel11XHostThreadC1EPNS0_11KernelStateEjjNSt6__ndk18functionIFivEEEjb", "xglue_return_this");

XGLUE_ALIAS("_ZN2xe6kernel7XObject7ReleaseEv", "xglue_noop");
XGLUE_ALIAS("_ZNK2xe6kernel11KernelState8title_idEv", "xglue_return_zero");

XGLUE_ALIAS("_ZN2xe6kernel7XThread6CreateEv", "xglue_panic_stub");
XGLUE_ALIAS("_ZN2xe6kernel7XThread10IsInThreadEPS1_", "xglue_panic_stub");
XGLUE_ALIAS("_ZN2xe6kernel7XThread8set_nameENSt6__ndk117basic_string_viewIcNS2_11char_traitsIcEEEE", "xglue_panic_stub");
XGLUE_ALIAS("_ZN2xe6kernel7XObject4WaitEjjjPmb", "xglue_panic_stub");
XGLUE_ALIAS("_ZNK2xe6kernel11KernelState13is_title_openEv", "xglue_panic_stub");
XGLUE_ALIAS("_ZN2xe6kernel11KernelState21EmulateCPInterruptDPCEjjjj", "xglue_panic_stub");
XGLUE_ALIAS("_ZNK2xe3cpu13GuestFunction28MapMachineCodeToGuestAddressEm", "xglue_panic_stub");
