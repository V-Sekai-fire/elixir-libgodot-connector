#!/bin/bash


set -eux

BASE_DIR="$( cd "$(dirname "$0")" ; pwd -P )"

GODOT_DIR="$BASE_DIR/godot"
BUILD_DIR="$BASE_DIR/build"

host_arch="$(uname -m)"

$BUILD_DIR/godot --path "$1" --editor
