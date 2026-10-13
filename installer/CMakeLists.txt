# The players' installer (one .exe carrying the mod). Built by tools/package_release.ps1, which first
# writes gen/payload.h and gen/installer_gen.rc (the DLL, the Lua mod, the icon and the manifest, with absolute
# paths; gen/ is git-ignored).
cmake_minimum_required(VERSION 3.20)
project(TeardownVRInstaller CXX RC)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # (static CRT: nothing to install on the player's PC)
add_executable(TeardownVR_Installer WIN32 installer.cpp logs.cpp ${CMAKE_CURRENT_SOURCE_DIR}/gen/installer_gen.rc)
target_include_directories(TeardownVR_Installer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/gen)
target_compile_definitions(TeardownVR_Installer PRIVATE UNICODE _UNICODE)
target_compile_options(TeardownVR_Installer PRIVATE /utf-8 /W4 /permissive- /O2)
target_link_options(TeardownVR_Installer PRIVATE /MANIFEST:NO)   # (ours is in the .rc)
set_target_properties(TeardownVR_Installer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
