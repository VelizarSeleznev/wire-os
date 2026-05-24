# Qualcomm recipes in this tree depend on the historical standalone provider
# names. meta-oe builds those tools from android-tools-native, so expose the
# old names for dependency resolution.
PROVIDES:append:class-native = " mkbootimg-native ext4-utils-native"
