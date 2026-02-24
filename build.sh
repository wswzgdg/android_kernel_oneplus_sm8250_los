#!/bin/bash

KERNEL_ROOT=$PWD

# Remove out, if exist
if [ -d "$KERNEL_ROOT/out" ]; then
    echo "Delete the existing out folder."
    rm -rf "$KERNEL_ROOT/out"
fi

KERNEL_OUTPUT=$KERNEL_ROOT/out/arch/arm64/boot

# ARCH
export ARCH=arm64
export SUBARCH=arm64

# AnyKernel
AK3_PATH=$KERNEL_ROOT/anykernel
CUSTOM_AK3_NAME=BPF-5.10-BT-Mod
FULL_AK3_NAME=$CUSTOM_AK3_NAME-$(date +%Y-%m-%d)

if [ ! -d "$AK3_PATH" ]; then
    git clone --depth=1 -b realme-sm8250 https://github.com/ermyltsor/AnyKernel3.git $AK3_PATH
fi

# Clenan existing anykernel packages
if find $AK3_PATH -maxdepth 1 -type f -name "*.zip" | grep -q .; then
    find $AK3_PATH -maxdepth 1 -type f -name "*.zip" -delete
fi

# Clang
if [ ! -d "$KERNEL_ROOT/zyc-clang-16" ]; then
    if [ ! -f "/tmp/Clang-16.0.6-20250721.tar.gz" ]; then
        wget -O /tmp/Clang-16.0.6-20250721.tar.gz https://github.com/ZyCromerZ/Clang/releases/download/16.0.6-20250721-release/Clang-16.0.6-20250721.tar.gz
    fi
    mkdir "$KERNEL_ROOT/zyc-clang-16"
    tar -xvf /tmp/Clang-16.0.6-20250721.tar.gz -C "$KERNEL_ROOT/zyc-clang-16"
fi

export CLANG_PATH=$KERNEL_ROOT/zyc-clang-16/bin
export PATH="$CLANG_PATH:$PATH"
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=arm-linux-gnueabi-

KERNEL_DEFCONFIG="vendor/kona-perf_defconfig"

echo "Build for OnePlus?"
read -r -p "Input [y / n]: " select

if [ "$select" = "y" ]; then
    export BRAND_SHOW_FLAG=oneplus
fi

echo
echo "Kernel is going to be built using $KERNEL_DEFCONFIG"
echo

# 修改点：CC 添加 ccache
make CC="ccache clang" AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip LLVM=1 LLVM_IAS=1 O=out $KERNEL_DEFCONFIG

make CC="ccache clang" AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip LLVM=1 LLVM_IAS=1 O=out -j$(nproc --all)

echo "Build complete"

if [ -f "$KERNEL_OUTPUT/dtb" ] && [ -f "$KERNEL_OUTPUT/dtbo.img" ] && [ -f "$KERNEL_OUTPUT/Image" ]; then
    cd $AK3_PATH
    cp "$KERNEL_OUTPUT/dtb"   .
    cp "$KERNEL_OUTPUT/dtbo.img"  .
    cp "$KERNEL_OUTPUT/Image"     .
    zip -r "$FULL_AK3_NAME.zip" *
    cd $KERNEL_ROOT

    echo "out: $AK3_PATH/$FULL_AK3_NAME.zip"

    # Clean existing 
    rm -f "$AK3_PATH/dtb"
    rm -f "$AK3_PATH/dtbo.img"
    rm -f "$AK3_PATH/Image"
fi
