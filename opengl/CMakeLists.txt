cmake_minimum_required(VERSION 3.21)
project(KorTraditionalMaterial LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This renderer requires Windows and the Windows Imaging Component (WIC).")
endif()

set(IMGUI_DIR "" CACHE PATH "Dear ImGui source directory (1.92 or newer, containing imgui.cpp and backends)")
set(GLAD_DIR "" CACHE PATH "GLAD 1 loader generated for OpenGL 4.3+ core, including debug enums/version flags (include/glad/glad.h and src/glad.c); renderer requests a 3.3 context")
set(GLFW_DIR "" CACHE PATH "GLFW 3.4 source directory (containing CMakeLists.txt and include/GLFW)")
set(GLM_DIR "" CACHE PATH "GLM source directory (containing glm/glm.hpp)")
set(KOR_RUNTIME_ASSET_DIR "" CACHE PATH "User-supplied runtime assets: Jagae_BaseMap.png, Jagae_ThicknessMap.png, Jagae_PatternMask.png; optional UnityDefaultSky.f32cube and UnityPostRamp.csv")

foreach(dependency IN ITEMS IMGUI GLAD GLFW GLM)
    if(NOT IS_DIRECTORY "${${dependency}_DIR}")
        message(FATAL_ERROR "Set ${dependency}_DIR to an existing dependency source directory; dependencies are not downloaded or bundled.")
    endif()
endforeach()
foreach(required IN ITEMS "${IMGUI_DIR}/imgui.cpp" "${GLAD_DIR}/src/glad.c" "${GLAD_DIR}/include/glad/glad.h" "${GLFW_DIR}/CMakeLists.txt" "${GLM_DIR}/glm/glm.hpp")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Required dependency file is missing: ${required}")
    endif()
endforeach()

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${GLFW_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/glfw")

add_executable(KorTraditionalMaterial
    main.cpp JagaeScene.cpp EnvironmentLighting.cpp PostProcess.cpp SceneSwitcher.cpp
    DirectionalLight.cpp DirectionalLightController.cpp DirectionalLightTests.cpp NacreTests.cpp
    "${GLAD_DIR}/src/glad.c"
    "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp" "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp")
target_compile_features(KorTraditionalMaterial PRIVATE cxx_std_17)
target_include_directories(KorTraditionalMaterial PRIVATE "${GLAD_DIR}/include" "${IMGUI_DIR}" "${GLM_DIR}")
target_compile_definitions(KorTraditionalMaterial PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(KorTraditionalMaterial PRIVATE /utf-8 /W4 /permissive-)
endif()
target_link_libraries(KorTraditionalMaterial PRIVATE glfw opengl32 windowscodecs ole32 user32 gdi32 shell32 advapi32)
set_property(TARGET KorTraditionalMaterial PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:KorTraditionalMaterial>")
add_custom_command(TARGET KorTraditionalMaterial POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:KorTraditionalMaterial>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:KorTraditionalMaterial>/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/UnityPostRamp.csv" "$<TARGET_FILE_DIR:KorTraditionalMaterial>/assets/UnityPostRamp.csv")

if(KOR_RUNTIME_ASSET_DIR)
    foreach(asset IN ITEMS Jagae_BaseMap.png Jagae_ThicknessMap.png Jagae_PatternMask.png)
        if(NOT EXISTS "${KOR_RUNTIME_ASSET_DIR}/${asset}")
            message(FATAL_ERROR "Missing user-supplied runtime asset: ${asset}")
        endif()
    endforeach()
    foreach(asset IN ITEMS Jagae_BaseMap.png Jagae_ThicknessMap.png Jagae_PatternMask.png UnityDefaultSky.f32cube UnityPostRamp.csv)
        if(EXISTS "${KOR_RUNTIME_ASSET_DIR}/${asset}")
            add_custom_command(TARGET KorTraditionalMaterial POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:KorTraditionalMaterial>/assets"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${KOR_RUNTIME_ASSET_DIR}/${asset}" "$<TARGET_FILE_DIR:KorTraditionalMaterial>/assets/${asset}")
        endif()
    endforeach()
else()
    message(WARNING "Set KOR_RUNTIME_ASSET_DIR to user-supplied Jagae_BaseMap.png, Jagae_ThicknessMap.png and Jagae_PatternMask.png before running the renderer. External texture assets are not bundled.")
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME directional_light COMMAND KorTraditionalMaterial --light-self-test)
    add_test(NAME camera COMMAND KorTraditionalMaterial --camera-self-test)
    if(KOR_RUNTIME_ASSET_DIR)
        add_test(NAME gat_smoke COMMAND KorTraditionalMaterial --smoke-test)
        add_test(NAME base_color COMMAND KorTraditionalMaterial --base-color-self-test)
        add_test(NAME directional_render COMMAND KorTraditionalMaterial --light-render-test --directional-light)
        add_test(NAME area_render COMMAND KorTraditionalMaterial --light-render-test --area-light)
        add_test(NAME nacre_optics COMMAND KorTraditionalMaterial --nacre-self-test)
        add_test(NAME post_process COMMAND KorTraditionalMaterial --post-self-test)
    endif()
    get_property(renderer_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${renderer_tests} PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:KorTraditionalMaterial>" RUN_SERIAL TRUE TIMEOUT 300)
endif()
