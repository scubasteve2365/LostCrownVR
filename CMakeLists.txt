cmake_minimum_required(VERSION 3.20)
project(LostCrownVR C CXX ASM_MASM)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_CXX_STANDARD 20)
set(MINHOOK_DIR "${CMAKE_SOURCE_DIR}/deps/minhook" CACHE PATH "MinHook source")
set(OPENXR_DIR "${CMAKE_SOURCE_DIR}/deps/openxr_loader_1.1.65" CACHE PATH "Khronos OpenXR loader")
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/deps/imgui-1.93.0" CACHE PATH "Dear ImGui")
set(FONTS_DIR "${CMAKE_SOURCE_DIR}/deps/fonts/jost" CACHE PATH "Folder with Jost.ttf (Google Fonts, SIL OFL 1.1)")
set(OUT "${CMAKE_SOURCE_DIR}/../build")

# winhttp.dll proxy (chain-loads winhttp_chain.dll, then loads LostCrownVR\LostCrownVRNative.dll)
add_library(winhttp SHARED proxy/winhttp_proxy.cpp proxy/thunks.asm proxy/winhttp.def)
set_target_properties(winhttp PROPERTIES OUTPUT_NAME winhttp RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUT}")

add_library(minhook STATIC ${MINHOOK_DIR}/src/buffer.c ${MINHOOK_DIR}/src/hook.c ${MINHOOK_DIR}/src/trampoline.c ${MINHOOK_DIR}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${MINHOOK_DIR}/include)
add_library(LostCrownVRNative SHARED native/LostCrownVRNative.cpp xr/LcXR.cpp xr/MenuDraw.cpp
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(LostCrownVRNative PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends ${OPENXR_DIR}/include)
target_compile_definitions(LostCrownVRNative PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(LostCrownVRNative PRIVATE minhook d3d11 dxgi winmm)
set_target_properties(LostCrownVRNative PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUT}")
target_compile_options(LostCrownVRNative PRIVATE /Zi)
target_link_options(LostCrownVRNative PRIVATE /DEBUG /OPT:REF /OPT:ICF /MAP)
add_custom_command(TARGET LostCrownVRNative POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OPENXR_DIR}/x64/bin/openxr_loader.dll" "${OUT}/openxr_loader.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FONTS_DIR}/Jost.ttf" "${FONTS_DIR}/OFL.txt" "${OUT}/")
