#!/bin/bash

set -eux

BASE_DIR="$( cd "$(dirname "$0")" ; pwd -P )"

GODOT_DIR="$BASE_DIR/godot"
GODOT_CPP_DIR="$BASE_DIR/godot-cpp"
BUILD_DIR=$BASE_DIR/build
BUILD_GODOT_CPP_DIR=$BUILD_DIR/godot-cpp-android
BUILD_GDEXTENSION_DIR=$BUILD_DIR/gdextension

host_system="$(uname -s)"
host_arch="$(uname -m)"
target="template_debug"
target_arch_list="arm64 arm32"

while [ "${1:-}" != "" ]
do
    case "$1" in
        --target)
            shift
            target="${1:-}"
        ;;
        --target-arch-list)
            shift
            target_arch_list="${1:-}"
        ;;
        *)
            echo "Usage: $0 [--target <template_dev|template_debug|template_release|editor>] [--target-arch-list 'arch1 [arch2...]']"
            exit 1
        ;;
    esac
    shift
done

function godot_to_android_arch() {
    godot_arch="$1"
    case "$godot_arch" in
        arm64)
            echo "arm64-v8a"
            ;;
        arm32)
            echo "armeabi-v7a"
            ;;
        *)
            echo "Unsupported arch: $godot_arch"
            exit 1
    esac
}

target_build_type_dir="release"
target_template="template_release"

mkdir -p $GODOT_CPP_DIR

if [ ! -f $BUILD_GDEXTENSION_DIR/extension_api.json ]
then
    echo "Cannot find extension_api.json in $BUILD_GDEXTENSION_DIR"
    echo "Run build_libgodot.sh first"
    exit 1
fi

tmp_dir="$(mktemp -d)"
prep_dir="$tmp_dir/godot-cpp-android"
mkdir -p $prep_dir

cd $GODOT_CPP_DIR
for target_arch in $target_arch_list
do
    android_arch="$(godot_to_android_arch $target_arch)"

    case $target in
    template_dev)
        suffix="dev.$target_arch"
        target_build_type_dir="dev"
        target_template="template_debug"
        scons gdextension_dir=$BUILD_GDEXTENSION_DIR arch=$target_arch platform=android dev_build=yes debug_symbols=yes target=template_debug generate_engine_classes_bindings=no
    ;;
    template_debug)
        suffix="$target_arch"
        target_build_type_dir="debug"
        target_template="template_debug"
        scons gdextension_dir=$BUILD_GDEXTENSION_DIR arch=$target_arch platform=android target=$target
    ;;
    template_release)
        suffix="$target_arch"
        scons gdextension_dir=$BUILD_GDEXTENSION_DIR arch=$target_arch platform=android target=$target
    ;;
    *)
        echo "Not supported target: $target"
        exit 1
    esac

    mkdir -p $prep_dir/$android_arch
    cp -a $GODOT_CPP_DIR/bin/libgodot-cpp.android.$target_template.$suffix.a $prep_dir/$android_arch/libgodot-cpp.a
done

# Prepare headers
headers_dir="$prep_dir/include"
mkdir -p $headers_dir

cp -a $GODOT_CPP_DIR/include/* $headers_dir/
cp -a $GODOT_CPP_DIR/gen/include/* $headers_dir/
cp $BUILD_GDEXTENSION_DIR/gdextension_interface.h $headers_dir/
cp -a $GODOT_DIR/platform/android/libgodot_android.h $headers_dir/

BUILD_GODOT_CPP_TARGET_DIR=$BUILD_GODOT_CPP_DIR/$target_build_type_dir
rm -rf $BUILD_GODOT_CPP_TARGET_DIR
mkdir -p $BUILD_GODOT_CPP_TARGET_DIR

cd $tmp_dir
zip -r $BUILD_GODOT_CPP_TARGET_DIR/godot-cpp-android.zip godot-cpp-android
