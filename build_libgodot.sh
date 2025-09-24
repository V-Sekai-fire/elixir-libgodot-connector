#!/bin/bash

set -eux

BASE_DIR="$( cd "$(dirname "$0")" ; pwd -P )"

GODOT_DIR="$BASE_DIR/godot"
GODOT_CPP_DIR="$BASE_DIR/godot-cpp"
SWIFT_GODOT_DIR="$BASE_DIR/SwiftGodot"
SWIFT_GODOT_KIT_DIR="$BASE_DIR/SwiftGodotKit"
BUILD_DIR=$BASE_DIR/build
BUILD_GDEXTENSION_DIR="$BUILD_DIR/gdextension"

host_system="$(uname -s)"
host_arch="$(uname -m)"
host_target="editor"
target="editor"
target_arch=""
host_build_options=""
target_build_options=""
lib_suffix="so"
host_debug=1
debug=1
force_host_rebuild=0
update_api=0
simulator=0
library_type="shared_library"

case "$host_system" in
    Linux)
        host_platform="linuxbsd"
        cpus="$(nproc)"
        target_platform="linuxbsd"
    ;;
    Darwin)
        host_platform="macos"
        cpus="$(sysctl -n hw.logicalcpu)"
        target_platform="macos"
        lib_suffix="dylib"
    ;;
    *)
        echo "System $host_system is unsupported"
        exit 1
    ;;
esac


while [ "${1:-}" != "" ]
do
    case "$1" in
        --host-rebuild)
            force_host_rebuild=1
        ;;
        --host-debug)
            host_debug=1
        ;;        
        --host-release)
            host_debug=0
        ;;
        --update-api)
            update_api=1
            force_host_rebuild=1
        ;;
        --debug)
            debug=1
        ;;
        --release)
            debug=0
        ;;
        --target)
            shift
            target_platform="${1:-}"
        ;;
        --simulator)
            simulator=1
        ;;
        --target-arch)
            shift
            target_arch="${1:-}"
        ;;
        *)
            echo "Usage: $0 [--host-debug] [--host-rebuild] [--host-debug] [--host-release] [--debug] [--release] [--update-api] [--target <target platform>] [--target-arch <target platform>]"
            exit 1
        ;;
    esac
    shift
done

if [ "$target_platform" = "ios" ]
then
    library_type="static_library"
    target="template_release"
    lib_suffix="a"
    if [ $simulator -eq 1 ]
    then
        target_build_options="$target_build_options ios_simulator=true"
    fi
    if [ "$target_arch" = "" ]
    then
        target_arch="$host_arch"
    fi
fi

if [ "$target_arch" = "" ]
then
    target_arch="$host_arch"
fi

host_godot_suffix="$host_platform.$host_target"

if [ $host_debug -eq 1 ]
then
    host_build_options="$host_build_options dev_build=yes"
    host_godot_suffix="$host_godot_suffix.dev"
fi

host_godot_suffix="$host_godot_suffix.$host_arch"

target_godot_suffix="$target_platform.$target"

if [ $debug -eq 1 ]
then
    if [ "$target_platform" = "ios" ]
    then
        target="template_debug"
        target_godot_suffix="$target_platform.$target"
    fi
    target_build_options="$target_build_options dev_build=yes"
    target_godot_suffix="$target_godot_suffix.dev"
fi

target_godot_suffix="$target_godot_suffix.$target_arch"

host_godot="$GODOT_DIR/bin/godot.$host_godot_suffix"
target_godot="$GODOT_DIR/bin/obj/bin/libgodot.$target_godot_suffix.$lib_suffix"

mkdir -p $BUILD_DIR

if [ ! -x $host_godot ] || [ $force_host_rebuild -eq 1 ]
then
    rm -f $host_godot
    cd $GODOT_DIR
    scons p=$host_platform target=$host_target $host_build_options
    cp -vf $host_godot $BUILD_DIR/godot
fi

if [ $update_api -eq 1 ] || [ ! -f $BUILD_GDEXTENSION_DIR/extension_api.json ]
then
    mkdir -p $BUILD_GDEXTENSION_DIR
    cd $BUILD_GDEXTENSION_DIR
    $host_godot --dump-extension-api
    cp -v $GODOT_DIR/core/extension/gdextension_interface.h $BUILD_GDEXTENSION_DIR/
    cp -v $GODOT_DIR/core/extension/libgodot.h $BUILD_GDEXTENSION_DIR/

    echo "Successfully updated the GDExtension API."
fi

cd $GODOT_DIR
scons p=$target_platform target=$target arch=$target_arch $target_build_options library_type=$library_type

if [ "$target_platform" != "ios" ]
then
    cp -v $target_godot $BUILD_DIR/libgodot.$lib_suffix
fi
