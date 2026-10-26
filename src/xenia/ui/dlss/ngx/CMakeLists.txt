# Shared NGX core owner (Milestone 5-B, prerequisite P0-1).
#
# This module owns the NGX core lifecycle (Init_with_ProjectID / Shutdown1 /
# AllocateParameters / DestroyParameters) that the DLSS SR backend (dlss/*)
# and the DLSS Frame Generation backend (fg/*) share. It is the ONLY place in
# the codebase that calls those core entry points. Feature-level entry points
# (GetFeatureRequirements / CreateFeature / EvaluateFeature / ReleaseFeature)
# stay with each backend module.
#
# License isolation: the SDK headers are referenced by path (NOT copied into
# the Xenia source tree) and included only in ngx_core_host.cc. If the SDK
# license (section 4e) is ultimately ruled incompatible with distribution,
# this module and the backend modules that depend on it can be deleted with
# zero changes to the rest of the codebase.
#
# The module depends only on xenia-base; it does not link against the D3D12
# provider utilities (avoiding a circular dependency with the presenter
# target).
add_library(xenia-ngx STATIC)
xe_platform_sources(xenia-ngx ${CMAKE_CURRENT_SOURCE_DIR})

# The NVIDIA SDK headers are not part of the Xenia source tree. They are
# referenced from the local SDK install. Override with -DXENIA_DLSS_SDK_DIR.
if(NOT DEFINED XENIA_DLSS_SDK_DIR)
  set(XENIA_DLSS_SDK_DIR "C:/SDKs/NVIDIA/DLSS/DLSS-310.9.1")
endif()
target_include_directories(xenia-ngx PRIVATE "${XENIA_DLSS_SDK_DIR}/include")

# The runtime DLLs (nvngxruntime.dll, nvngx_dlss.dll, nvngx_dlssg.dll) are
# resolved at runtime by the static NGX core (loaded at CreateFeature time
# from the PathList directories). They are NOT bundled with the build. The
# static NGX core resolves the NVSDK_NGX_* entry points at link time. The "_d"
# variant is the dynamic-CRT (/MD) build, matching Xenia's /MD runtime. The
# static core statically imports NO NVIDIA DLL: the feature DLLs are loaded at
# CreateFeature time, so the executable starts without any NVIDIA runtime
# present (the "off path has zero DLL loads" contract).
target_link_libraries(xenia-ngx PUBLIC xenia-base
  "${XENIA_DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
xe_target_defaults(xenia-ngx)
