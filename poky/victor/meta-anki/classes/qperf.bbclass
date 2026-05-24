# Compatibility shim for Qualcomm/WireOS recipes that inherit qperf.
#
# The original class is not present in this checkout. These defaults match the
# variables consumed by local recipes closely enough for parse/build selection:
# msm-perf sets PERF_BUILD itself, msm-user sets USER_BUILD, and debug builds
# use VARIANT=debug.
PERF_BUILD ?= "${@['0', '1'][d.getVar('VARIANT') != ('' or 'debug')]}"
USER_BUILD ?= "0"
DEBUG_BUILD ?= "${@['0', '1'][d.getVar('VARIANT') == ('' or 'debug')]}"
