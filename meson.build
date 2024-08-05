project('gsr-notify', ['cpp'], version : '1.0.0', default_options : ['warning_level=2'], subproject_dir : 'depends')

if get_option('buildtype') == 'debug'
    add_project_arguments('-g3', language : ['cpp'])
elif get_option('buildtype') == 'release'
    add_project_arguments('-DNDEBUG', language : ['cpp'])
endif

src = [
    'src/main.cpp',
]

mglpp_proj = subproject('mglpp')
mglpp_dep = mglpp_proj.get_variable('mglpp_dep')

dep = [
    mglpp_dep,
    dependency('xfixes'),
]

executable(
    meson.project_name(),
    src,
    install : true,
    dependencies : dep,
)