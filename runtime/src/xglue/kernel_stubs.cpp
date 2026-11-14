// The Xenia GPU/UI libraries still reference a few kernel (xe::kernel) symbols on code paths that
// only run with a real KernelState. In host mode they are never reached, so we point the linker at
// stand-ins instead of pulling in Xenia's whole kernel.
#include <cstdio>
#include <cstdlib>

extern "C" void XgluePanicStub()
{
    std::fprintf(stderr, "FATAL: a Xenia kernel function was called in host mode\n");
    std::fflush(stderr);
    std::abort();
}

extern "C" void* XglueReturnThis(void* self) { return self; } // constructors
extern "C" void XglueNoOp() {}

#define XGLUE_ALIAS(mangled, target) __pragma(comment(linker, "/alternatename:" mangled "=" target))

// Constructors (may run from static initializers): leave the object untouched.
XGLUE_ALIAS("??0UserSetting@xam@kernel@xe@@QEAA@W4UserSettingId@123@V?$variant@IHM_JNV?$basic_string@_SU?$char_traits@_S@std@@V?$allocator@_S@2@@std@@V?$vector@EV?$allocator@E@std@@@2@@std@@@Z", "XglueReturnThis")
XGLUE_ALIAS("??0XHostThread@kernel@xe@@QEAA@PEAVKernelState@12@IIV?$function@$$A6AHXZ@std@@I@Z", "XglueReturnThis")

// Harmless no-ops.
XGLUE_ALIAS("?SaveConfig@config@@YAXXZ", "XglueNoOp")
XGLUE_ALIAS("?Release@XObject@kernel@xe@@QEAAXXZ", "XglueNoOp")

// Must never run in host mode.
XGLUE_ALIAS("?Create@XThread@kernel@xe@@QEAAIXZ", "XgluePanicStub")
XGLUE_ALIAS("?EmulateCPInterruptDPC@KernelState@kernel@xe@@QEAAXIIII@Z", "XgluePanicStub")
XGLUE_ALIAS("?FindField@XConfig@kernel@xe@@CAPEBUFieldDescriptor@123@W4X_CONFIG_CATEGORY@23@G@Z", "XgluePanicStub")
XGLUE_ALIAS("?IsInThread@XThread@kernel@xe@@SA_NPEAV123@@Z", "XgluePanicStub")
XGLUE_ALIAS("?ReadSetting@XConfig@kernel@xe@@QEAAXW4X_CONFIG_CATEGORY@23@GPEAX@Z", "XgluePanicStub")
XGLUE_ALIAS("?Wait@XObject@kernel@xe@@QEAAIIIIPEA_K@Z", "XgluePanicStub")
XGLUE_ALIAS("?is_title_open@KernelState@kernel@xe@@QEBA_NXZ", "XgluePanicStub")
XGLUE_ALIAS("?set_name@XThread@kernel@xe@@QEAAXV?$basic_string_view@DU?$char_traits@D@std@@@std@@@Z", "XgluePanicStub")
