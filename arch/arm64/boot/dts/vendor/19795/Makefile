ifeq ($(CONFIG_BUILD_ARM64_DT_OVERLAY),y)
    dtbo-$(CONFIG_ARCH_KONA) += \
        kona-bladerunnerc-overlay.dtbo \
        kona-bladerunner-overlay.dtbo \
        kona-bladerunnerc-t0-evt2-overlay.dtbo \
        kona-bladerunner-t0-evt2-overlay.dtbo \
        kona-bladerunner-48m-overlay.dtbo

    kona-bladerunnerc-overlay.dtbo-base := kona.dtb kona-v2.dtb kona-v2.1.dtb
    kona-bladerunner-overlay.dtbo-base := kona.dtb kona-v2.dtb kona-v2.1.dtb
    kona-bladerunnerc-t0-evt2-overlay.dtbo-base := t0-evt2-c-kona.dtb t0-evt2-c-kona-v2.dtb t0-evt2-c-kona-v2.1.dtb
    kona-bladerunner-t0-evt2-overlay.dtbo-base := t0-evt2-kona.dtb t0-evt2-kona-v2.dtb t0-evt2-kona-v2.1.dtb
    kona-bladerunner-48m-overlay.dtbo-base := bladerunner-48m-kona.dtb bladerunner-48m-kona-v2.dtb bladerunner-48m-kona-v2.1.dtb
else
    dtb-$(CONFIG_ARCH_LITO) += kona-mtp.dtb
endif

always		:= $(dtb-y)
subdir-y	:= $(dts-dirs)
clean-files    := *.dtb *.dtbo
