# Compatibility shim for Qualcomm image recipes.
#
# The original qimage class supplies get_bblayer_img_inc(), used by generic
# machine image recipes to pick the most specific image include available in
# the active layers.
LICENSE ?= "BSD-3-Clause-Clear"
LIC_FILES_CHKSUM ?= "file://${COREBASE}/../victor/meta-qcom/files/common-licenses/${LICENSE};md5=48b43ba58d0f8e9ef3704313a46b7a43"

def get_bblayer_img_inc(kind, d):
    basemachine = d.getVar("BASEMACHINE") or d.getVar("MACHINE")
    distro = d.getVar("DISTRO")
    candidates = [
        "%s/%s-%s-%s-image.inc" % (basemachine, basemachine, distro, kind),
        "%s/%s-%s-image.inc" % (basemachine, basemachine, kind),
        "common/common-%s-image.inc" % kind,
    ]
    bbpath = d.getVar("BBPATH")
    for candidate in candidates:
        rel = "recipes-products/images/%s" % candidate
        found = bb.utils.which(bbpath, rel)
        if found:
            return found
    bb.fatal("No image include found for %s; tried %s" % (kind, ", ".join(candidates)))
