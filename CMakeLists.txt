cmake_minimum_required(VERSION 3.16)
project(earshot LANGUAGES C)

# ---------------------------------------------------------------------------
# Earshot / mod_earshot — FreeSWITCH audio-stream module.
#
# Two things build here:
#   * the FreeSWITCH module  (needs FreeSWITCH dev headers)
#   * the unit tests         (pure C, no FreeSWITCH — always buildable)
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
#   sudo cmake --install build        # -> $FS_MOD_DIR/mod_earshot.so
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(EARSHOT_TESTS "Build the unit tests" ON)
option(EARSHOT_WS_TESTS "Run local WebSocket protocol integration tests (Python websockets required)" OFF)
find_package(Threads REQUIRED)

# Portable, dependency-free core (codec + protobuf + control-channel guards) shared by
# the module and tests.
set(EARSHOT_CORE_SOURCES src/es_codec.c src/es_pb.c src/es_cmdguard.c)

# --- unit tests (no FreeSWITCH) --------------------------------------------
if(EARSHOT_TESTS)
  enable_testing()

  add_executable(test_codec test/test_codec.c src/es_codec.c)
  add_test(NAME codec COMMAND test_codec)

  add_executable(test_pb test/test_pb.c src/es_pb.c)
  add_test(NAME pb COMMAND test_pb)

  add_executable(test_cmdguard test/test_cmdguard.c src/es_cmdguard.c)
  add_test(NAME cmdguard COMMAND test_cmdguard)

  # Integration test: drives the REAL control-channel dispatch (command JSON ->
  # es_proto_on_text -> es_dispatch_command -> es_cmdguard -> sink) without FreeSWITCH,
  # via a test-only <switch.h> stub + minimal cJSON in test/support (scoped to this target
  # only). Proves legitimate commands are unchanged and every attack payload is blocked.
  add_executable(test_command_dispatch
    test/test_command_dispatch.c
    test/support/command_dispatch_stubs.c test/support/cjson/cJSON.c
    src/es_proto.c src/es_cmdguard.c src/es_codec.c src/es_pb.c)
  target_include_directories(test_command_dispatch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/test/support
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME command_dispatch COMMAND test_command_dispatch)

  # Inbound agent audio of any frame size reaches the sink whole (native/deepgram binary,
  # pipecat protobuf); same stub harness.
  add_executable(test_proto_audio
    test/test_proto_audio.c
    test/support/command_dispatch_stubs.c test/support/cjson/cJSON.c
    src/es_proto.c src/es_cmdguard.c src/es_codec.c src/es_pb.c)
  target_include_directories(test_proto_audio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/test/support
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME proto_audio COMMAND test_proto_audio)

  add_executable(test_doubao test/test_doubao.c
    test/support/command_dispatch_stubs.c test/support/cjson/cJSON.c
    src/es_proto.c src/es_cmdguard.c src/es_codec.c src/es_pb.c)
  target_include_directories(test_doubao PRIVATE test/support src)
  add_test(NAME doubao COMMAND test_doubao)
  find_package(Threads REQUIRED)
  foreach(t test_command_dispatch test_proto_audio test_doubao)
    target_link_libraries(${t} PRIVATE Threads::Threads m)
  endforeach()
endif()

# --- the FreeSWITCH module (skipped gracefully if FS headers are absent) ----
find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(FS freeswitch)
  pkg_check_modules(LWS  libwebsockets)   # WebSocket transport
endif()

if(EARSHOT_TESTS AND EARSHOT_WS_TESTS)
  if(NOT LWS_FOUND)
    message(FATAL_ERROR "EARSHOT_WS_TESTS requires libwebsockets")
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_package(OpenSSL REQUIRED)
  add_executable(test_doubao_ws test/test_doubao_ws.c
    test/support/command_dispatch_stubs.c test/support/cjson/cJSON.c
    src/es_proto.c src/es_cmdguard.c src/es_codec.c src/es_pb.c src/es_ws.c)
  target_compile_definitions(test_doubao_ws PRIVATE EARSHOT_REAL_WS)
  target_include_directories(test_doubao_ws PRIVATE test/support src ${LWS_INCLUDE_DIRS})
  target_link_directories(test_doubao_ws PRIVATE ${LWS_LIBRARY_DIRS})
  target_link_libraries(test_doubao_ws PRIVATE ${LWS_LIBRARIES} OpenSSL::SSL Threads::Threads m)
  add_test(NAME doubao_websocket COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test_doubao_ws.py $<TARGET_FILE:test_doubao_ws>)
  set_tests_properties(doubao_websocket PROPERTIES TIMEOUT 60)
endif()

if(NOT FS_FOUND AND EXISTS "/usr/include/freeswitch/switch.h")
  set(FS_FOUND TRUE)
  set(FS_INCLUDE_DIRS "/usr/include/freeswitch")
  set(FS_MOD_DIR      "/usr/lib/freeswitch/mod")
elseif(FS_FOUND)
  pkg_get_variable(FS_MOD_DIR freeswitch modulesdir)
endif()

if(FS_FOUND)
  add_library(mod_earshot SHARED
    src/mod_earshot.c
    ${EARSHOT_CORE_SOURCES}
    src/es_ws.c          # WebSocket transport (needs libwebsockets)
    src/es_proto.c       # protocol adapters: native/twilio/openai/deepgram/elevenlabs/gemini/pipecat
  )
  if(NOT LWS_FOUND)
    message(WARNING "libwebsockets not found — install libwebsockets-dev to build es_ws.c")
  endif()
  set_target_properties(mod_earshot PROPERTIES PREFIX "" OUTPUT_NAME "mod_earshot")
  target_include_directories(mod_earshot PRIVATE ${FS_INCLUDE_DIRS} ${LWS_INCLUDE_DIRS})
  target_link_libraries(mod_earshot PRIVATE ${FS_LIBRARIES} ${LWS_LIBRARIES} Threads::Threads m)
  install(TARGETS mod_earshot LIBRARY DESTINATION "${FS_MOD_DIR}")
  message(STATUS "Earshot: building mod_earshot (FreeSWITCH found)")

  # --- .deb packaging (cpack --config build/CPackConfig.cmake -G DEB) ---------
  set(CPACK_PACKAGE_NAME "mod-earshot")
  set(CPACK_PACKAGE_VERSION "0.4.1")
  set(CPACK_PACKAGE_CONTACT "Varun Pratap Singh")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Earshot — FreeSWITCH <-> AI-agent audio bridge (mod_earshot)")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/wiringai/mod_earshot")
  set(CPACK_GENERATOR "DEB")
  set(CPACK_DEBIAN_PACKAGE_SECTION "comm")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)   # auto-detect libwebsockets runtime deps
  set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
  include(CPack)
else()
  message(STATUS "Earshot: FreeSWITCH headers not found — building tests only. "
                 "Install freeswitch-dev on a build box to compile the module.")
endif()
