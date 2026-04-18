project('gsr-notify', ['cpp'], version : '1.2.1', default_options : ['warning_level=2'], subproject_dir : 'depends')

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
    dependency('xext'),
]

prefix = get_option('prefix')
datadir = get_option('datadir')
gsr_notify_resources_path = join_paths(prefix, datadir, 'gsr-notify')

executable(
    meson.project_name(),
    src,
    install : true,
    dependencies : dep,
    cpp_args : '-DGSR_NOTIFY_RESOURCES_PATH="' + gsr_notify_resources_path + '"',
)

install_subdir('images', install_dir : gsr_notify_resources_path)
