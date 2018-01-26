cmake_minimum_required(VERSION 3.8)
project(ch02_assignment)

set(CMAKE_CXX_STANDARD 11)

set(SOURCE_FILES
        hmem.c
        hw07_malloc.c
        ivec.h
        ivec_main.c
        list.h
        list_main.c
        par_malloc.c
        sys_malloc.c
        xmalloc.h hmalloc.h)

add_executable(ch02_assignment ${SOURCE_FILES})