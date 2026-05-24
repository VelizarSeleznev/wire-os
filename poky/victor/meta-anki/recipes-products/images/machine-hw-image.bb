# Minimal Vector hardware-access image.
#
# This keeps the APQ8009/Linux BSP, boot image, OTA shape, Wi-Fi, SSH, /data,
# and selected Qualcomm media HAL packages, but does not include the Anki
# personality service graph.

include ${BASEMACHINE}/${BASEMACHINE}-robot-hw-image.inc

require ${COREBASE}/../victor/meta-qcom/recipes-products/images/include/mdm-bootimg.inc
require ${COREBASE}/../victor/meta-qcom/recipes-products/images/include/mdm-ota-target-image-ubi.inc
require ${COREBASE}/../victor/meta-qcom/recipes-products/images/include/mdm-ota-target-image-ext4.inc

inherit core-image

MULTILIBRE_ALLOW_REP =. "/usr/include/python2.7/*|${base_bindir}|${base_sbindir}|${bindir}|${sbindir}|${libexecdir}|${sysconfdir}|${nonarch_base_libdir}/udev|/lib/modules/[^/]*/modules.*|"

ROOTFS_POSTPROCESS_COMMAND += ' vector_hw_rootfs_hook;'

vector_hw_rootfs_hook () {
	if [ -e ${IMAGE_ROOTFS}/etc/default/ssh ]; then
		echo 'SYSCONFDIR=/data/ssh' > ${IMAGE_ROOTFS}/etc/default/ssh
	fi
	echo "ro.anki.product.name=${ANKI_PRODUCT_NAME}" >> ${IMAGE_ROOTFS}/build.prop
	echo "ro.build.os.cfw.name=wire-os_vector_hw" >> ${IMAGE_ROOTFS}/build.prop
}
