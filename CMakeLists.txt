cmake_minimum_required(VERSION 3.16)
project("Cap'n Proto Root" CXX)

if (PROJECT_IS_TOP_LEVEL)
  include(CTest)
endif()

add_subdirectory(c++)
