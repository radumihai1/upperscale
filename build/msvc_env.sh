#!/usr/bin/env bash
# msvc_env.sh - source this to get cl/link usable from git-bash without vcvars.
# PATH entries MUST be MSYS forward-slash form (bash splits on ':' and mangles backslash paths).
# INCLUDE/LIB are read by native cl.exe, so they use Windows backslash form.
MSVC_ROOT='/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools'
SDK_VER="10.0.26100.0"

MSVC_TOOLSET_MSYS=$(ls -d "$MSVC_ROOT"/VC/Tools/MSVC/*/ 2>/dev/null | sort -V | tail -1)
MSVC_TOOLSET_MSYS="${MSVC_TOOLSET_MSYS%/}"
MSVC_TOOLSET_W=$(cygpath -w "$MSVC_TOOLSET_MSYS")

SDK_ROOT_MSYS='/c/Program Files (x86)/Windows Kits/10'
SDK_ROOT_W='C:\Program Files (x86)\Windows Kits\10'

export PATH="$MSVC_TOOLSET_MSYS/bin/Hostx64/x64:$SDK_ROOT_MSYS/bin/$SDK_VER/x64:$PATH"
export INCLUDE="$MSVC_TOOLSET_W\\include;$SDK_ROOT_W\\Include\\$SDK_VER\\ucrt;$SDK_ROOT_W\\Include\\$SDK_VER\\shared;$SDK_ROOT_W\\Include\\$SDK_VER\\um"
export LIB="$MSVC_TOOLSET_W\\lib\\x64;$SDK_ROOT_W\\Lib\\$SDK_VER\\ucrt\\x64;$SDK_ROOT_W\\Lib\\$SDK_VER\\um\\x64"
