# SPDX-License-Identifier: AGPL-3.0-or-later
# n.q 生成器 (.pt → n.q) の独立ビルド。
#   ★HLS kernel ソース (tasks_kernel.cpp) も OpenCL / OpenCV / torch も要求しない。
#     依存は C++17 標準ライブラリのみ。
#
#   ビルド (640x640 の配布 n.q を再現する定義):
#     cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="\
#       -DY26_NETH=640u -DY26_NETW=640u -DGMEM_DATA64_SIZE_MB=512 -DHEAD_RESERVE_FRONT \
#       -DHB_TOP_WORDS=524288u -DBATCH_IN_BASE=278528 -DBATCH_OUT_BASE=432128 -DMGR_CYC_BASE=456704"
#     cmake --build build -j
#   詳細と再現条件は README.md を参照。
cmake_minimum_required(VERSION 3.13)
project(nq_generator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "既定 Release" FORCE)
endif()
# 生成器は整数/bit 演算のみで最適化レベルに依存しないが、念のため -O1 を既定にする
# (内部ツリーの native 推論 harness が -O2 以上で hang した経緯に合わせた保守的な既定)。
set(CMAKE_CXX_FLAGS_RELEASE "-O1 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG   "-O1 -g"       CACHE STRING "" FORCE)

add_executable(nq
  src/nq_main.cpp
  src/nq.cpp
  src/nq_support.cpp
  src/pt_reader.cpp
)
target_include_directories(nq PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# 生成器は内部ツリーでは VSCODE (非 HLS) ビルドとしてコンパイルされる。同じ分岐を選ぶ。
target_compile_definitions(nq PRIVATE VSCODE)
target_link_libraries(nq PRIVATE pthread)
