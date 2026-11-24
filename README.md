edk2-sandbox
============

ReBIOS's sandbox firmware: ReBiosSandboxPkg, an EDK II platform on which
ReBIOS runs a graphics card's own UEFI GOP driver again, in a VM of its own,
to bring the card back to the mode the firmware left it in once a native
driver of the OS has had the card.

The platform is paravirtual: it reaches ReBIOS for time, PCI configuration
space, and its log through hypercalls, and has no emulated devices.

ReBiosSandboxPkg is built against the upstream EDK II release named in
EDK2_TAG, with this repository on EDK II's PACKAGES_PATH. GitHub workflows
build it and publish the firmware in this repository's releases, from which
ReBIOS's build fetches the release it pins.

To build locally, with EDK II's build prerequisites (Python, NASM, and a GCC
for x86-64):

    git clone --recursive --branch "$(cat EDK2_TAG)" https://github.com/tianocore/edk2.git
    ./build.sh

The firmware is output/rebios-sandbox.fd.

ReBiosSandboxPkg is under the BSD-2-Clause-Patent license, as EDK II is (see
License.txt).
