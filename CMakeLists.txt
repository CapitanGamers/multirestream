cmake_minimum_required(VERSION 3.16)
project(MultiRestream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN)
  add_compile_options(/utf-8 /W3)
  # Static CRT so the VPS does not need VCRUNTIME140*.dll
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(SHARED
  shared/protocol.h
  shared/json_mini.h
  shared/config.h
  shared/win_util.h
  shared/crypto_win.h
)

add_executable(mr_server WIN32
  server/main_server.cpp
  server/restream_engine.cpp
  server/chat_hub.cpp
  server/control_server.cpp
  ${SHARED}
)
target_include_directories(mr_server PRIVATE shared server)
target_link_libraries(mr_server PRIVATE ws2_32 winhttp comctl32 user32 gdi32 shell32)

add_executable(mr_client WIN32
  client/main_client.cpp
  client/chrome_bridge.cpp
  client/platform_ops.cpp
  client/obs_bridge.cpp
  ${SHARED}
)
target_include_directories(mr_client PRIVATE shared server client)
target_link_libraries(mr_client PRIVATE
  ws2_32 winhttp comctl32 user32 gdi32 shell32 crypt32 bcrypt advapi32)

add_custom_command(TARGET mr_server POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/config.example.json
          $<TARGET_FILE_DIR:mr_server>/config.example.json
)
add_custom_command(TARGET mr_client POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/scripts/start-chrome-debug.bat
          $<TARGET_FILE_DIR:mr_client>/start-chrome-debug.bat
)
