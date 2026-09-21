cmake_minimum_required(VERSION 3.20)
project(AndroidEmuGPU LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(AE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(EMUGL "${AE_ROOT}/ThirdParty/EmuGL")
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN}")
set(EMUGEN_EXECUTABLE "" CACHE FILEPATH "Native emugen executable for cross compilation")
if(NOT EMUGEN_EXECUTABLE)
  if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Cross compilation requires host EMUGEN_EXECUTABLE")
  endif()
  set(TOOL "${EMUGL}/host/tools/emugen")
  add_executable(ae_emugen ${TOOL}/main.cpp ${TOOL}/ApiGen.cpp ${TOOL}/EntryPoint.cpp
    ${TOOL}/TypeFactory.cpp ${TOOL}/strUtils.cpp ${TOOL}/Parser.cpp)
  set(EMUGEN_EXECUTABLE "$<TARGET_FILE:ae_emugen>")
  set(EMUGEN_DEPENDENCY ae_emugen)
endif()
set(DECODERS)
set(DECODER_INCLUDES)
foreach(PAIR "gles1|GLESv1_dec" "gles2|GLESv2_dec" "renderControl|renderControl_dec")
  string(REPLACE "|" ";" PARTS "${PAIR}")
  list(GET PARTS 0 API)
  list(GET PARTS 1 DIRECTORY)
  set(INPUT "${EMUGL}/host/libs/${DIRECTORY}")
  list(APPEND DECODER_INCLUDES "${INPUT}")
  set(OUTPUTS ${GEN}/${API}_dec.cpp ${GEN}/${API}_dec.h
    ${GEN}/${API}_server_context.cpp ${GEN}/${API}_server_context.h
    ${GEN}/${API}_server_proc.h ${GEN}/${API}_opcodes.h)
  add_custom_command(OUTPUT ${OUTPUTS}
    COMMAND "${EMUGEN_EXECUTABLE}" -i "${INPUT}" -D "${GEN}" "${API}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/harden_decoder.py" "${GEN}/${API}_dec.cpp"
    DEPENDS ${EMUGEN_DEPENDENCY} harden_decoder.py ${INPUT}/${API}.in ${INPUT}/${API}.types ${INPUT}/${API}.attrib
    VERBATIM)
  list(APPEND DECODERS ${GEN}/${API}_dec.cpp ${GEN}/${API}_server_context.cpp)
endforeach()
set(RENDER "${EMUGL}/host/libs/libOpenglRender")
set(COMMON "${EMUGL}/shared/emugl/common")
add_custom_command(OUTPUT ${GEN}/egl_dispatch.inc
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_dispatch.py"
    "${RENDER}/EGLDispatch.h" "${GEN}/egl_dispatch.inc"
  DEPENDS generate_dispatch.py ${RENDER}/EGLDispatch.h VERBATIM)
add_library(androidemu_gpu STATIC Renderer.cpp Dispatch.cpp ${GEN}/egl_dispatch.inc
  ${AE_ROOT}/ThirdParty/AndroidQemuCompat/qemu/gpu.c
  ${DECODERS}
  ${EMUGL}/host/libs/GLESv1_dec/GLESv1Decoder.cpp
  ${EMUGL}/host/libs/GLESv2_dec/GLESv2Decoder.cpp
  ${EMUGL}/shared/OpenglCodecCommon/glUtils.cpp
  ${EMUGL}/shared/OpenglCodecCommon/TimeUtils.cpp
  ${RENDER}/ColorBuffer.cpp ${RENDER}/FbConfig.cpp ${RENDER}/FrameBuffer.cpp
  ${RENDER}/RenderContext.cpp ${RENDER}/RenderControl.cpp ${RENDER}/RenderThreadInfo.cpp
  ${RENDER}/TextureDraw.cpp ${RENDER}/WindowSurface.cpp
  ${COMMON}/lazy_instance.cpp ${COMMON}/thread_store.cpp ${COMMON}/smart_ptr.cpp
  ${COMMON}/shared_library.cpp)
target_include_directories(androidemu_gpu PRIVATE ${GEN} ${DECODER_INCLUDES} ${RENDER}
  ${EMUGL}/host/include ${EMUGL}/host/include/libOpenglRender ${EMUGL}/shared ${EMUGL}/shared/OpenglCodecCommon
  ${EMUGL}/host/libs/Translator/include)
target_include_directories(androidemu_gpu PUBLIC ${AE_ROOT})
target_link_libraries(androidemu_gpu PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
option(AE_GPU_TESTS "Build host renderer integration tests" ON)
if(AE_GPU_TESTS)
  enable_testing()
  add_executable(gpu_renderer_tests ${AE_ROOT}/Tests/GpuRendererTests.cpp)
  target_link_libraries(gpu_renderer_tests PRIVATE androidemu_gpu)
  target_compile_options(gpu_renderer_tests PRIVATE -UNDEBUG)
  add_test(NAME gpu_renderer COMMAND gpu_renderer_tests)
  set_tests_properties(gpu_renderer PROPERTIES TIMEOUT 30 ENVIRONMENT "EGL_PLATFORM=surfaceless;LIBGL_ALWAYS_SOFTWARE=1")
endif()
