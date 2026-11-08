
examples = [
    'enroll',
    'identify',
    'img-capture',
    'manage-prints',
    'verify',
    'clear-storage',
]

foreach example: examples
    executable(example,
        [ example + '.c', 'storage.c', 'utilities.c' ],
        dependencies: [
            libfprint_dep,
            glib_dep,
        ],
    )
endforeach

# Diagnostics for the CS9711 / Acl16 reader; public API only, never stores
# images. When installed into a private prefix, find that prefix's library.
diag_rpath = ''
if get_option('prefix') != '/usr' and get_option('prefix') != '/usr/local'
    diag_rpath = get_option('prefix') / get_option('libdir')
endif
executable('fprint-cs9711-diag',
    'cs9711-diag.c',
    dependencies: [
        libfprint_dep,
        glib_dep,
        mathlib_dep,
    ],
    install: get_option('cs9711-diag'),
    install_rpath: diag_rpath,
)

executable('cpp-test',
    'cpp-test.cpp',
    dependencies: libfprint_dep,
)

if installed_tests
    install_subdir('prints',
        install_dir: installed_tests_testdir)
endif
