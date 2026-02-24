ifeq ($(CONFIG_BUILD_ARM64_DT_OVERLAY),y)
    dtbo-$(CONFIG_ARCH_KONA) += \
        21623-kona-mtp-overlay.dtbo \
        21733-kona-mtp-overlay.dtbo

    21623-kona-mtp-overlay.dtbo-base := 21623-kona.dtb 21623-kona-v2.dtb 21623-kona-v2.1.dtb
    21733-kona-mtp-overlay.dtbo-base := 21733-kona.dtb 21733-kona-v2.dtb 21733-kona-v2.1.dtb

else
    dtb-$(CONFIG_ARCH_LITO) += kona-mtp.dtb
endif

always		:= $(dtb-y)
subdir-y	:= $(dts-dirs)
clean-files    := *.dtb *.dtbo
