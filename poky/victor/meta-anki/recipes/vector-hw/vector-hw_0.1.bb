SUMMARY = "Minimal Vector hardware HTTP/WebSocket API and local app runner"
LICENSE = "CLOSED"
PR = "r1"

DEPENDS += "openssl"
RDEPENDS:vector-hw-api += "bash"
RDEPENDS:vector-hw-cli += "curl"
RDEPENDS:vector-app-runner += "bash jq systemd tar gzip"

SRC_URI = " \
	file://vector-hw-api.cpp \
	file://vector-hw-api.service \
	file://vector-hw-stop-motors.service \
	file://vector-hw.target \
	file://vector-hw-cli \
	file://vector-appctl \
	file://vector-app@.service \
	file://vector-app-runner.service \
"

S = "${UNPACKDIR}"

inherit systemd useradd

PACKAGES =+ "vector-hw-api vector-hw-cli vector-app-runner"
ALLOW_EMPTY:${PN} = "1"
RDEPENDS:${PN} = "vector-hw-api vector-hw-cli vector-app-runner"

USERADD_PACKAGES = "vector-app-runner"
GROUPADD_PARAM:vector-app-runner = "--system vectorapp"
USERADD_PARAM:vector-app-runner = "--system --home /data/vector-apps --no-create-home --shell /bin/false --gid vectorapp vectorapp"

do_compile() {
	${CXX} ${CXXFLAGS} ${LDFLAGS} -std=c++17 -Wall -Wextra -O2 \
		${S}/vector-hw-api.cpp -o vector-hw-api -lcrypto -pthread
}

do_install() {
	install -d ${D}${bindir}
	install -m 0755 ${B}/vector-hw-api ${D}${bindir}/vector-hw-api
	install -m 0755 ${S}/vector-hw-cli ${D}${bindir}/vector-hw-cli
	install -m 0755 ${S}/vector-appctl ${D}${bindir}/vector-appctl

	install -d ${D}${systemd_system_unitdir}
	install -m 0644 ${S}/vector-hw-api.service ${D}${systemd_system_unitdir}/vector-hw-api.service
	install -m 0644 ${S}/vector-hw-stop-motors.service ${D}${systemd_system_unitdir}/vector-hw-stop-motors.service
	install -m 0644 ${S}/vector-hw.target ${D}${systemd_system_unitdir}/vector-hw.target
	install -m 0644 ${S}/vector-app@.service ${D}${systemd_system_unitdir}/vector-app@.service
	install -m 0644 ${S}/vector-app-runner.service ${D}${systemd_system_unitdir}/vector-app-runner.service

	install -d ${D}${systemd_system_unitdir}/multi-user.target.wants
	ln -sf ../vector-hw.target ${D}${systemd_system_unitdir}/multi-user.target.wants/vector-hw.target

	install -d ${D}${systemd_system_unitdir}/vector-hw.target.wants
	ln -sf ../vector-hw-api.service ${D}${systemd_system_unitdir}/vector-hw.target.wants/vector-hw-api.service
	ln -sf ../vector-app-runner.service ${D}${systemd_system_unitdir}/vector-hw.target.wants/vector-app-runner.service
}

FILES:vector-hw-api = " \
	${bindir}/vector-hw-api \
	${systemd_system_unitdir}/vector-hw-api.service \
	${systemd_system_unitdir}/vector-hw-stop-motors.service \
	${systemd_system_unitdir}/vector-hw.target \
	${systemd_system_unitdir}/multi-user.target.wants/vector-hw.target \
	${systemd_system_unitdir}/vector-hw.target.wants/vector-hw-api.service \
"
FILES:vector-hw-cli = "${bindir}/vector-hw-cli"
FILES:vector-app-runner = " \
	${bindir}/vector-appctl \
	${systemd_system_unitdir}/vector-app@.service \
	${systemd_system_unitdir}/vector-app-runner.service \
	${systemd_system_unitdir}/vector-hw.target.wants/vector-app-runner.service \
"

SYSTEMD_PACKAGES = "vector-hw-api"
SYSTEMD_SERVICE:vector-hw-api = "vector-hw.target vector-hw-api.service"
