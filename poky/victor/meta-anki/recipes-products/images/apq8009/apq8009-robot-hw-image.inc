#
# Minimal hardware API image for Vector.
#
# It intentionally starts from the QCOM robot base image rather than the Anki
# personality image. Add only platform services needed for a remotely managed
# hardware endpoint.

require ${COREBASE}/../victor/meta-qcom/recipes-products/images/apq8009/apq8009-robot-image.inc

# Persistent /data, update support, Wi-Fi and developer access.
IMAGE_INSTALL += "cryptsetup"
IMAGE_INSTALL += "user-data-locker"
IMAGE_INSTALL += "securemsm-noship"
IMAGE_INSTALL += "openssl"
IMAGE_INSTALL += "openssl-bin"
IMAGE_INSTALL += "libatomic"
IMAGE_INSTALL += "libdevmapper"
IMAGE_INSTALL += "lvm2"
IMAGE_INSTALL += "wpa-supplicant"
IMAGE_INSTALL += "connman"
IMAGE_INSTALL += "connman-client"
IMAGE_INSTALL += "extra-conf"
IMAGE_INSTALL += "update-engine"
IMAGE_INSTALL += "update-os"
IMAGE_INSTALL += "bash"
IMAGE_INSTALL += "busybox"
IMAGE_INSTALL += "syslog-ng"
IMAGE_INSTALL += "logrotate"
IMAGE_INSTALL += "rsync"
IMAGE_INSTALL += "jq"
IMAGE_INSTALL += "tar"
IMAGE_INSTALL += "gzip"

# Media and Qualcomm support packages. These preserve access to the existing
# camera/audio path without bringing in Anki behavior, cloud, or animation.
IMAGE_INSTALL += "audiohal"
IMAGE_INSTALL += "alsa-utils"
IMAGE_INSTALL += "adsprpc"
IMAGE_INSTALL += "init-audio"
IMAGE_INSTALL += "mm-camera"
IMAGE_INSTALL += "rmtstorage"

# Hardware API and local extension runner.
IMAGE_INSTALL += "vector-hw-api"
IMAGE_INSTALL += "vector-hw-cli"
IMAGE_INSTALL += "vector-app-runner"

# Useful diagnostics for the hardware image.
IMAGE_INSTALL += "${@oe.utils.conditional('USER_BUILD', '1', '', 'htop', d)}"
IMAGE_INSTALL += "${@oe.utils.conditional('USER_BUILD', '1', '', 'procps', d)}"
IMAGE_INSTALL += "${@oe.utils.conditional('USER_BUILD', '1', '', 'net-tools', d)}"
IMAGE_INSTALL += "${@oe.utils.conditional('USER_BUILD', '1', '', 'util-linux-dmesg', d)}"

# Explicitly keep the Anki personality/service graph out of this image.
IMAGE_INSTALL:remove = "anki-robot-target"
IMAGE_INSTALL:remove = "victor"
IMAGE_INSTALL:remove = "vic-anim"
IMAGE_INSTALL:remove = "vic-bootanim"
IMAGE_INSTALL:remove = "vic-cloud"
IMAGE_INSTALL:remove = "vic-cloudless"
IMAGE_INSTALL:remove = "vic-crashuploader"
IMAGE_INSTALL:remove = "vic-dasmgr"
IMAGE_INSTALL:remove = "vic-engine"
IMAGE_INSTALL:remove = "vic-gateway-cert"
IMAGE_INSTALL:remove = "vic-init"
IMAGE_INSTALL:remove = "vic-log-uploader"
IMAGE_INSTALL:remove = "vic-robot"
IMAGE_INSTALL:remove = "vic-switchboard"
IMAGE_INSTALL:remove = "vic-webserver"
IMAGE_INSTALL:remove = "wired"

# The inherited APQ8009 robot image references a standalone blkdiscard package
# that is not provided by this Yocto tree. Keep the hardware image buildable
# without changing the shared QCOM image include.
IMAGE_INSTALL:remove = "blkdiscard"
IMAGE_INSTALL:remove = "system-core-adbd"
