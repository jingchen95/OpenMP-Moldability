add_libc_exhaustive_testsuite(libc_math_exhaustive_tests)

add_object_library(
  exhaustive_test
  HDRS
    exhaustive_test.h
  SRCS
    exhaustive_test.cpp
  DEPENDS
    libc.src.__support.CPP.standalone_cpp
)

add_fp_unittest(
  sqrtf_test
  NEED_MPFR
  SUITE
    libc_math_exhaustive_tests
  SRCS
    sqrtf_test.cpp
  DEPENDS
    libc.include.math
    libc.src.math.sqrtf
    libc.src.__support.FPUtil.fputil
)

add_fp_unittest(
  sinf_test
  NEED_MPFR
  SUITE
    libc_math_exhaustive_tests
  SRCS
    sinf_test.cpp
  DEPENDS
    libc.include.math
    libc.src.math.sinf
    libc.src.__support.FPUtil.fputil
)

add_fp_unittest(
  cosf_test
  NEED_MPFR
  SUITE
    libc_math_exhaustive_tests
  SRCS
    cosf_test.cpp
  DEPENDS
    libc.include.math
    libc.src.math.cosf
    libc.src.__support.FPUtil.fputil
)

add_fp_unittest(
  expm1f_test
  NEED_MPFR
  SUITE
    libc_math_exhaustive_tests
  SRCS
  expm1f_test.cpp
  DEPENDS
    libc.include.math
    libc.src.math.expm1f
    libc.src.__support.FPUtil.fputil
)

add_fp_unittest(
  logf_test
  NO_RUN_POSTBUILD
  NEED_MPFR
  SUITE
    libc_math_exhaustive_tests
  SRCS
    logf_test.cpp
  DEPENDS
    .exhaustive_test
    libc.include.math
    libc.src.math.logf
    libc.src.__support.FPUtil.fputil
  LINK_OPTIONS
    -lpthread
)

add_fp_unittest(
  log10f_test
  NO_RUN_POSTBUILD
  NEED_MPFR
  SUITE
    libc_math_exhaustive_tests
  SRCS
    log10f_test.cpp
  DEPENDS
    .exhaustive_test
    libc.include.math
    libc.src.math.log10f
    libc.src.__support.FPUtil.fputil
  LINK_OPTIONS
    -lpthread
)

add_fp_unittest(
  log1pf_test
  NO_RUN_POSTBUILD
  NEED_MPFR
  SUITE
    libc_math_exhaustive_tests
  SRCS
    log1pf_test.cpp
  DEPENDS
    .exhaustive_test
    libc.include.math
    libc.src.math.log1pf
    libc.src.__support.FPUtil.fputil
  LINK_OPTIONS
    -lpthread
)

add_fp_unittest(
  log2f_test
  NO_RUN_POSTBUILD
  NEED_MPFR
  SUITE
    libc_math_exhaustive_tests
  SRCS
    log2f_test.cpp
  DEPENDS
    .exhaustive_test
    libc.include.math
    libc.src.math.log2f
    libc.src.__support.FPUtil.fputil
  LINK_OPTIONS
    -lpthread
)
