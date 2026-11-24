edk2-sandbox
============

ReBIOS's sandbox firmware: ReBiosSandboxPkg, an EDK II platform on which
ReBIOS runs a graphics card's own UEFI GOP driver again, in a VM of its own,
to have it set the card's modes that ReBIOS shows its guest's display in,
also once a native driver of the OS has had the card, and as the machine
wakes from sleep.

The platform is paravirtual: it reaches ReBIOS for time, PCI configuration
space, and its log through hypercalls, and has no emulated devices. ReBIOS
runs it with the machine's whole address space at its own addresses, wherever
the memory ReBIOS gives it is, relocating its SEC module there. A GOP
built into the platform firmware, as an iGPU's, reads that firmware's dynamic
PCDs, which ReBIOS gives the sandbox as it found them, in place of a PCD
database of the sandbox's own. As the platform firmware did, the sandbox
gives the GOP the platform's ACPI and SMBIOS tables in its configuration
table, and the GPU's ROM image the platform gave the GPU's PCI I/O protocol.
Where the platform firmware gives its GOP the platform GOP policy protocol,
as an Intel iGPU's GOP looks for, the sandbox gives it one too, with the
platform's Video BIOS Table, and the lid's state as ReBIOS finds it. The log
names each protocol that a GOP looks for and the sandbox lacks.

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
