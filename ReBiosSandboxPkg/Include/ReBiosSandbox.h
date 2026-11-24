/** @file
  The interface between ReBIOS and its sandbox firmware: the handoff ReBIOS
  gives the firmware's entry point, and the hypercalls through which the
  firmware reaches ReBIOS.

  ReBIOS starts the firmware in 64-bit mode at the entry point of its SEC
  module, as a call with the Microsoft x64 calling convention: interrupts
  off, the FPU and SSE on, as UEFI 2.11, 2.3.4.1 has them, RCX pointing at
  the handoff, and RSP near the top of the stack the handoff describes, with
  the 32 bytes the callee may use above the return address.

  The page tables map all physical addresses below 4 GiB, and the root
  bridge's aperture above, to themselves. The sandbox's memory is the only
  memory there; the rest is the machine's devices, which the firmware reaches
  as ReBIOS allows.

  A hypercall is VMCALL on Intel processors and VMMCALL on AMD ones, with the
  call's number in RAX, its arguments in RBX, RCX, RDX, and RSI, and its
  result in RAX.

  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef REBIOS_SANDBOX_H_
#define REBIOS_SANDBOX_H_

#define REBIOS_SANDBOX_HANDOFF_SIGNATURE  SIGNATURE_64 ('R', 'B', 'S', 'A', 'N', 'D', 'B', 'X')
#define REBIOS_SANDBOX_HANDOFF_VERSION    1

//
// What the handoff says the firmware is to do.
//
#define REBIOS_SANDBOX_TASK_GOP_SET_MODE  1

//
// Where the GPU's GOP comes from: nowhere; the GPU's option ROM, which the
// handoff's image is, as the PCI I/O protocol of the platform firmware gave
// it, for the PCI platform protocol to give the PCI bus driver; or the
// handoff's image, a PE32 image of the driver, as an iGPU's GOP from the
// platform firmware's volumes.
//
#define REBIOS_SANDBOX_GOP_SOURCE_NONE        0
#define REBIOS_SANDBOX_GOP_SOURCE_OPTION_ROM  1
#define REBIOS_SANDBOX_GOP_SOURCE_IMAGE       2

#define REBIOS_SANDBOX_MAX_BRIDGES  8

#pragma pack(1)

///
/// A PCI function as segment, bus, device, and function.
///
typedef struct {
  UINT16    Segment;
  UINT8     Bus;
  UINT8     Device;
  UINT8     Function;
  UINT8     Reserved[3];
} REBIOS_SANDBOX_PCI_ADDRESS;

///
/// A range of addresses: Base to Base + Size - 1, none where Size is 0.
///
typedef struct {
  UINT64    Base;
  UINT64    Size;
} REBIOS_SANDBOX_RANGE;

///
/// The mode the GOP is to set, by its resolution and pixel format, in the
/// handoff; in the result, the mode it set.
///
typedef struct {
  UINT32    HorizontalResolution;
  UINT32    VerticalResolution;
  ///
  /// EFI_GRAPHICS_PIXEL_FORMAT, with the masks where it is PixelBitMask.
  ///
  UINT32    PixelFormat;
  UINT32    RedMask;
  UINT32    GreenMask;
  UINT32    BlueMask;
  UINT32    ReservedMask;
  UINT32    PixelsPerScanLine;
  UINT64    FrameBufferBase;
} REBIOS_SANDBOX_MODE;

typedef struct {
  UINT64                        Signature;
  UINT32                        Version;
  UINT32                        Size;
  UINT32                        Task;
  UINT32                        Reserved;
  ///
  /// The sandbox's memory, which the handoff, the stack, the firmware's
  /// volume, and the images the handoff gives are in; the part of it that
  /// holds none of them; the stack; and the firmware's volume.
  ///
  REBIOS_SANDBOX_RANGE          Memory;
  REBIOS_SANDBOX_RANGE          FreeMemory;
  REBIOS_SANDBOX_RANGE          Stack;
  REBIOS_SANDBOX_RANGE          FirmwareVolume;
  ///
  /// The page tables and the GDT ReBIOS starts the firmware with, which stay
  /// in use.
  ///
  REBIOS_SANDBOX_RANGE          Tables;
  ///
  /// The processor's time-stamp counter frequency, in hertz.
  ///
  UINT64                        TscFrequency;
  ///
  /// The GPU, the bridges above it, from the root bus down, and the root
  /// bridge's buses and apertures, which hold the GPU's BARs and the
  /// bridges' windows.
  ///
  REBIOS_SANDBOX_PCI_ADDRESS    Gpu;
  UINT32                        BridgeCount;
  UINT32                        Reserved2;
  REBIOS_SANDBOX_PCI_ADDRESS    Bridges[REBIOS_SANDBOX_MAX_BRIDGES];
  UINT8                         RootBusBase;
  UINT8                         RootBusLimit;
  UINT8                         Reserved3[6];
  REBIOS_SANDBOX_RANGE          Mem32Aperture;
  REBIOS_SANDBOX_RANGE          Mem64Aperture;
  REBIOS_SANDBOX_RANGE          IoAperture;
  ///
  /// Where the GPU's GOP comes from, and the image of its option ROM or of
  /// the driver, in the sandbox's memory.
  ///
  UINT32                        GopSource;
  UINT32                        Reserved4;
  REBIOS_SANDBOX_RANGE          GopImage;
  ///
  /// An Intel iGPU's Video BIOS Table, in the sandbox's memory, where there is
  /// one.
  ///
  REBIOS_SANDBOX_RANGE          Vbt;
  ///
  /// The platform's ACPI RSDP, whose tables the sandbox reaches read-only, 0
  /// where there is none.
  ///
  UINT64                        AcpiRsdp;
  REBIOS_SANDBOX_MODE           Mode;
} REBIOS_SANDBOX_HANDOFF;

#pragma pack()

//
// Hypercalls.
//

///
/// RBX: the physical address of the text, RCX: its length in bytes. The text
/// goes to ReBIOS's log.
///
#define REBIOS_SANDBOX_CALL_LOG  0

///
/// RBX: the timer's period in 100 ns units, 0 for none. ReBIOS raises
/// REBIOS_SANDBOX_TIMER_VECTOR at that period while the firmware takes
/// interrupts.
///
#define REBIOS_SANDBOX_CALL_TIMER    1
#define REBIOS_SANDBOX_TIMER_VECTOR  0x40

///
/// RBX: the function's address in PCI_SEGMENT_LIB_ADDRESS () form, RCX: the
/// access's size in bytes, 1, 2, or 4, RDX: for a write, the value. RAX: for a
/// read, the value. Absent functions read all ones.
///
#define REBIOS_SANDBOX_CALL_PCI_READ   2
#define REBIOS_SANDBOX_CALL_PCI_WRITE  3

///
/// RBX: the physical address of an EFI_TIME that ReBIOS fills with the
/// platform's time.
///
#define REBIOS_SANDBOX_CALL_GET_TIME  4

///
/// RBX: the EFI_RESET_TYPE. The firmware asked to reset the platform, which
/// ends the sandbox's run as a failure.
///
#define REBIOS_SANDBOX_CALL_RESET  5

///
/// RBX: the EFI_STATUS of the task, RCX: the physical address of a
/// REBIOS_SANDBOX_MODE with the mode set, or 0. Ends the sandbox's run.
///
#define REBIOS_SANDBOX_CALL_DONE  6

#endif
