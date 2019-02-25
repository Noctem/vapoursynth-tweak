project('tweak', 'c',
    default_options: ['buildtype=release', 'b_ndebug=if-release'],
    meson_version: '>=0.46.0',
    version: '0.1')

shared_module('tweak',
    sources: 'tweak.c',
    dependencies: dependency('vapoursynth').partial_dependency(compile_args: true, includes: true),
    c_args: ['-march=native', '-Ofast'],
    install: true)
