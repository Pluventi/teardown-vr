// Command-line check of an OpenXR runtime through the same static loader as
// the mod.  usage: tdvr_xr_probe [runtime.json]
#include <windows.h>
#include <openxr/openxr.h>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    if (argc > 1) SetEnvironmentVariableA("XR_RUNTIME_JSON", argv[1]);
    SetEnvironmentVariableA("XR_LOADER_DEBUG", "all");
    uint32_t n = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    printf("xrEnumerateInstanceExtensionProperties -> %d (%u extensions)\n", r, n);
    if (XR_FAILED(r)) return 1;
    std::vector<XrExtensionProperties> e(n, { XR_TYPE_EXTENSION_PROPERTIES });
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, e.data());
    for (auto& x : e) printf("  %s\n", x.extensionName);
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ci.applicationInfo.applicationName, "probe");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    const char* ext = "XR_KHR_D3D12_enable";
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = &ext;
    XrInstance inst = XR_NULL_HANDLE;
    r = xrCreateInstance(&ci, &inst);
    printf("xrCreateInstance -> %d\n", r);
    if (XR_FAILED(r)) return 1;
    XrInstanceProperties p = { XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(inst, &p);
    printf("runtime: %s\n", p.runtimeName);
    XrSystemGetInfo gi = { XR_TYPE_SYSTEM_GET_INFO };
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = 0;
    r = xrGetSystem(inst, &gi, &sys);
    printf("xrGetSystem -> %d\n", r);
    xrDestroyInstance(inst);
    return 0;
}
