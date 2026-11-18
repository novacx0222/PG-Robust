# Copyright (c) 2022-2023, PostgreSQL Global Development Group

test_robust_sources = files(
  'test_robust.c',
)

if host_system == 'windows'
  test_robust_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_robust',
    '--FILEDESC', 'test_robust - unit tests for RobDP robust planning',])
endif

test_robust = shared_module('test_robust',
  test_robust_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_robust

test_install_data += files(
  'test_robust.control',
  'test_robust--1.0.sql',
)

tests += {
  'name': 'test_robust',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_robust',
    ],
  },
}
