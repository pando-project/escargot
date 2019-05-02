CMAKE_MINIMUM_REQUIRED (VERSION 2.8)

#######################################################
# CONFIGURATION
#######################################################
SET (ESCARGOT_CXXFLAGS_CONFIG)
SET (ESCARGOT_LDFLAGS_CONFIG)

#######################################################
# PATH
#######################################################
SET (ESCARGOT_ROOT ${PROJECT_SOURCE_DIR})
SET (ESCARGOT_THIRD_PARTY_ROOT ${ESCARGOT_ROOT}/third_party)
SET (GCUTIL_ROOT ${ESCARGOT_THIRD_PARTY_ROOT}/GCutil)

SET (ESCARGOT_OUTDIR ${ESCARGOT_ROOT}/out/${ESCARGOT_HOST}/${ESCARGOT_ARCH}/${ESCARGOT_TYPE}/${ESCARGOT_MODE})

IF (ESCARGOT_OUTPUT STREQUAL "bin")
    SET (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ESCARGOT_OUTDIR}/)
    SET (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ESCARGOT_OUTDIR}/lib)
    SET (CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${ESCARGOT_OUTDIR}/lib)
ENDIF()


#######################################################
# FLAGS FOR COMMON
#######################################################
# ESCARGOT COMMON CXXFLAGS
SET (ESCARGOT_DEFINITIONS_COMMON
    -DESCARGOT
    -DESCARGOT_ENABLE_TYPEDARRAY
    -DESCARGOT_ENABLE_PROMISE
    -DESCARGOT_ENABLE_PROXY_REFLECT
    -DESCARGOT_ENABLE_ES2015
)

SET (CXXFLAGS_FROM_ENV $ENV{CXXFLAGS})
SEPARATE_ARGUMENTS(CXXFLAGS_FROM_ENV)
SET (ESCARGOT_CXXFLAGS_COMMON
    ${CXXFLAGS_FROM_ENV}
    -std=c++11 -g3
    -fno-math-errno
    -fdata-sections -ffunction-sections
    -fno-omit-frame-pointer
    -fvisibility=hidden
    -Wno-unused-parameter
    -Wno-type-limits -Wno-unused-result -Wno-unused-variable -Wno-invalid-offsetof
    -Wno-deprecated-declarations
    -Wno-implicit-fallthrough
)

IF (${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
    SET (ESCARGOT_CXXFLAGS_COMMON ${ESCARGOT_CXXFLAGS_COMMON} -frounding-math -fsignaling-nans -Wno-unused-but-set-variable -Wno-unused-but-set-parameter -Wno-unused-parameter)
ELSEIF (${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
    SET (ESCARGOT_CXXFLAGS_COMMON ${ESCARGOT_CXXFLAGS_COMMON} -fno-fast-math -fno-unsafe-math-optimizations -fdenormal-fp-math=ieee -Wno-parentheses-equality -Wno-unused-parameter -Wno-dynamic-class-memaccess -Wno-deprecated-register -Wno-expansion-to-defined -Wno-return-type)
ELSE()
    MESSAGE (FATAL_ERROR ${CMAKE_CXX_COMPILER_ID} " is Unsupported Compiler")
ENDIF()


# ESCARGOT COMMON LDFLAGS
SET (ESCARGOT_LDFLAGS_COMMON -fvisibility=hidden)

IF (${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
    SET (ESCARGOT_LDFLAGS_COMMON ${ESCARGOT_LDFLAGS_COMMON})
ELSEIF (${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
    SET (ESCARGOT_LDFLAGS_COMMON ${ESCARGOT_LDFLAGS_COMMON})
ELSE()
    MESSAGE (FATAL_ERROR ${CMAKE_CXX_COMPILER_ID} " is Unsupported Compiler")
ENDIF()



# bdwgc
IF (${ESCARGOT_MODE} STREQUAL "debug")
    SET (ESCARGOT_DEFINITIONS_COMMON ${ESCARGOT_DEFINITIONS_COMMON} -DGC_DEBUG)
ENDIF()

#######################################################
# FLAGS FOR $(ESCARGOT_HOST)
#######################################################
FIND_PACKAGE (PkgConfig REQUIRED)

# LINUX FLAGS
SET (ESCARGOT_CXXFLAGS_LINUX -fno-rtti)
SET (ESCARGOT_LDFLAGS_LINUX -lpthread -lrt)
SET (ESCARGOT_DEFINITIONS_LINUX -DENABLE_INTL)

# DARWIN FLAGS
SET (ESCARGOT_CXXFLAGS_DARWIN -fno-rtti)
SET (ESCARGOT_LDFLAGS_DARWIN -lpthread)
SET (ESCARGOT_DEFINITIONS_DARWIN -DENABLE_INTL)

# TIZEN FLAGS
SET (ESCARGOT_CXXFLAGS_TIZEN)
SET (ESCARGOT_LDFLAGS_TIZEN -lpthread -lrt)
SET (ESCARGOT_DEFINITIONS_TIZEN -DESCARGOT_SMALL_CONFIG=1 -DESCARGOT_TIZEN -DENABLE_INTL)

# ANDROID FLAGS
SET (ESCARGOT_CXXFLAGS_ANDROID -fPIE -mthumb -march=armv7-a -mfloat-abi=softfp -mfpu=neon)
SET (ESCARGOT_LDFLAGS_ANDROID -fPIE -pie  -march=armv7-a -Wl,--fix-cortex-a8 -llog)
SET (ESCARGOT_DEFINITIONS_ANDROID -DANDROID=1)
IF (${ESCARGOT_OUTPUT} STREQUAL "shared_lib")
    SET (ESCARGOT_LDFLAGS_ANDROID ${ESCARGOT_LDFLAGS_ANDROID} -shared)
ENDIF()

IF (NOT DEFINED ESCARGOT_LIBICU_SUPPORT)
  SET (ESCARGOT_LIBICU_SUPPORT ON)
ENDIF()

# LIBICU SUPPORT
IF (${ESCARGOT_LIBICU_SUPPORT} STREQUAL "ON")
    SET (ESCARGOT_DEFINITIONS_LINUX ${ESCARGOT_DEFINITIONS_LINUX} -DENABLE_ICU)
    SET (ESCARGOT_DEFINITIONS_DARWIN ${ESCARGOT_DEFINITIONS_DARWIN} -DENABLE_ICU)
    SET (ESCARGOT_DEFINITIONS_TIZEN ${ESCARGOT_DEFINITIONS_TIZEN} -DENABLE_ICU)

    # LINUX LIBRARIES
    IF (${ESCARGOT_HOST} STREQUAL "linux")
        IF (${ESCARGOT_ARCH} STREQUAL "x64" OR ${ESCARGOT_ARCH} STREQUAL "x86" OR ${ESCARGOT_ARCH} STREQUAL "arm")
            PKG_CHECK_MODULES (ICUI18N REQUIRED icu-i18n)
            PKG_CHECK_MODULES (ICUUC REQUIRED icu-uc)
            SET (ESCARGOT_LIBRARIES_LINUX ${ESCARGOT_LIBRARIES_LINUX} ${ICUI18N_LIBRARIES} ${ICUUC_LIBRARIES})
            SET (ESCARGOT_INCDIRS_LINUX ${ESCARGOT_INCDIRS_LINUX} ${ICUI18N_INCLUDE_DIRS} ${ICUUC_INCLUDE_DIRS})
            SET (ESCARGOT_CXXFLAGS_LINUX ${ESCARGOT_CXXFLAGS_LINUX} ${ICUI18N_CFLAGS_OTHER} ${ICUUC_CFLAGS_OTHER})
        ENDIF()
    ENDIF()

    # DARWIN LIBRARIES
    IF (${ESCARGOT_HOST} STREQUAL "darwin")
        IF (${ESCARGOT_ARCH} STREQUAL "x64")
            PKG_CHECK_MODULES (ICUI18N REQUIRED icu-i18n)
            PKG_CHECK_MODULES (ICUUC REQUIRED icu-uc)
            FOREACH (ICU_LDFLAG ${ICUI18N_LDFLAGS} ${ICUUC_LDFLAGS})
                SET (ESCARGOT_LDFLAGS_DARWIN ${ESCARGOT_LDFLAGS_DARWIN} ${ICU_LDFLAG})
            ENDFOREACH()
            SET (ESCARGOT_INCDIRS_DARWIN ${ESCARGOT_INCDIRS_DARWIN} ${ICUI18N_INCLUDE_DIRS} ${ICUUC_INCLUDE_DIRS})
            SET (ESCARGOT_CXXFLAGS_DARWIN ${ESCARGOT_CXXFLAGS_DARWIN} ${ICUI18N_CFLAGS_OTHER} ${ICUUC_CFLAGS_OTHER})
        ENDIF()
    ENDIF()

    # TIZEN LIBRARIES
    IF (${ESCARGOT_HOST} STREQUAL "tizen_obs")
        PKG_CHECK_MODULES (DLOG REQUIRED dlog)
        PKG_CHECK_MODULES (ICUI18N REQUIRED icu-i18n)
        PKG_CHECK_MODULES (ICUUC REQUIRED icu-uc)
        SET (ESCARGOT_LIBRARIES_TIZEN ${ESCARGOT_LIBRARIES_TIZEN} ${DLOG_LIBRARIES} ${ICUI18N_LIBRARIES} ${ICUUC_LIBRARIES})
        SET (ESCARGOT_INCDIRS_TIZEN ${ESCARGOT_INCDIRS_TIZEN} ${DLOG_INCLUDE_DIRS} ${ICUI18N_INCLUDE_DIRS} ${ICUUC_INCLUDE_DIRS})
        SET (ESCARGOT_CXXFLAGS_TIZEN ${ESCARGOT_CXXFLAGS_TIZEN} ${DLOG_CFLAGS_OTHER} ${ICUI18N_CFLAGS_OTHER} ${ICUUC_CFLAGS_OTHER})
    ENDIF()
ENDIF()

#######################################################
# FLAGS FOR $(ARCH) : x64/x86/arm
#######################################################
# x64 FLAGS
SET (ESCARGOT_DEFINITIONS_X64 -DESCARGOT_64=1)


# x86 FLAGS
IF (NOT ${ESCARGOT_HOST} STREQUAL "tizen_obs")
    SET (ESCARGOT_CXXFLAGS_X86 -m32 -mfpmath=sse -msse -msse2)
ENDIF()
IF (NOT ${ESCARGOT_HOST} STREQUAL "tizen_obs")
    SET (ESCARGOT_LDFLAGS_X86 -m32)
ENDIF()
SET (ESCARGOT_DEFINITIONS_X86 -DESCARGOT_32=1)


# arm FLAGS
IF (NOT ${ESCARGOT_HOST} STREQUAL "tizen_obs")
    SET (ESCARGOT_CXXFLAGS_ARM -march=armv7-a -mthumb)
ENDIF()
SET (ESCARGOT_DEFINITIONS_ARM -DESCARGOT_32=1)


# aarch64 FLAGS
SET (ESCARGOT_DEFINITIONS_AARCH64 -DESCARGOT_64=1)


#######################################################
# flags for $(MODE) : debug/release
#######################################################
# DEBUG FLAGS
SET (ESCARGOT_CXXFLAGS_DEBUG -O0 -Wall -Wextra -Werror)
IF (${ESCARGOT_HOST} STREQUAL "tizen_obs")
    SET (ESCARGOT_CXXFLAGS_DEBUG ${ESCARGOT_CXXFLAGS_DEBUG} -O1)
ENDIF()
SET (ESCARGOT_DEFINITIONS_DEBUG -D_GLIBCXX_DEBUG)


# RELEASE FLAGS
SET (ESCARGOT_CXXFLAGS_RELEASE -O2 -fno-stack-protector)
IF (${ESCARGOT_HOST} MATCHES "tizen")
    SET (ESCARGOT_CXXFLAGS_RELEASE ${ESCARGOT_CXXFLAGS_RELEASE} -Os -finline-limit=64)
ENDIF()
SET (ESCARGOT_DEFINITIONS_RELEASE -DNDEBUG)


#######################################################
# FLAGS FOR $(ESCARGOT_OUTPUT) : bin/shared_lib/static_lib
#######################################################
# BIN FLAGS
IF (NOT ${ESCARGOT_HOST} STREQUAL "darwin")
    SET (ESCARGOT_LDFLAGS_BIN -Wl,--gc-sections)
ELSE()
    SET (ESCARGOT_LDFLAGS_BIN -Wl,-dead_strip)
ENDIF()
SET (ESCARGOT_DEFINITIONS_BIN -DESCARGOT_STANDALONE -DESCARGOT_SHELL)


# SHARED_LIB FLAGS
SET (ESCARGOT_CXXFLAGS_SHAREDLIB -fPIC)
SET (ESCARGOT_LDFLAGS_SHAREDLIB -ldl)


# STATIC_LIB FLAGS
SET (ESCARGOT_CXXFLAGS_STATICLIB -fPIC)
SET (ESCARGOT_LDFLAGS_STATICLIB -Wl,--gc-sections)


#######################################################
# FLAGS FOR TEST
#######################################################
SET (ESCARGOT_DEFINITIONS_VENDORTEST -DESCARGOT_ENABLE_VENDORTEST)


#######################################################
# FLAGS FOR MEMORY PROFILING
#######################################################
SET (PROFILER_FLAGS)

IF (ESCARGOT_PROFILE_BDWGC)
    SET (PROFILER_FLAGS "${PROFILE_FLAGS} -DPROFILE_BDWGC")
ENDIF()

IF (ESCARGOT_PROFILE_MASSIF)
    SET (PROFILER_FLAGS "${PROFILE_FLAGS} -DPROFILE_MASSIF")
ENDIF()

string(STRIP "${PROFILER_FLAGS}" PROFILER_FLAGS)
