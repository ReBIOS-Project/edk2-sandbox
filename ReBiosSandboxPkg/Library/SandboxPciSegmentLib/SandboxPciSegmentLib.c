/** @file
  The sandbox firmware's PCI configuration space, which ReBIOS reaches for it
  through hypercalls, in every segment.

  While PCI is being enumerated, until the PCI enumeration complete protocol
  is installed, only the GPU and the bridges above it are there: the rest
  reads all ones and drops writes, so that enumeration touches none of the
  guest's other devices. Once it is installed, every function is reached.

  Based on MdePkg/Library/BasePciSegmentLibPci.

  Copyright (c) 2016 - 2017, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/HypercallLib.h>
#include <Library/PciSegmentLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/PciEnumerationComplete.h>
#include <ReBiosSandbox.h>

STATIC CONST REBIOS_SANDBOX_HANDOFF  *mHandoff;
STATIC BOOLEAN                       mEnumerated;
STATIC VOID                          *mEnumeratedRegistration;

/**
  Whether a function is the GPU or one of the bridges above it.

  @param[in]  Function  The function.
  @param[in]  Address   An address in it, in PCI_SEGMENT_LIB_ADDRESS () form.

  @retval TRUE   It is.
  @retval FALSE  It is not.
**/
STATIC
BOOLEAN
IsFunction (
  IN CONST REBIOS_SANDBOX_PCI_ADDRESS  *Function,
  IN UINT64                            Address
  )
{
  return (Function->Segment == (UINT16)RShiftU64 (Address, 32)) &&
         (Function->Bus == (UINT8)(Address >> 20)) &&
         (Function->Device == (UINT8)((Address >> 15) & 0x1F)) &&
         (Function->Function == (UINT8)((Address >> 12) & 0x7));
}

/**
  Whether an access reaches its function.

  @param[in]  Address  The address, in PCI_SEGMENT_LIB_ADDRESS () form.

  @retval TRUE   It does.
  @retval FALSE  The function is not there for it.
**/
STATIC
BOOLEAN
Reachable (
  IN UINT64  Address
  )
{
  UINT32  Index;

  if (mEnumerated || (mHandoff == NULL)) {
    return TRUE;
  }

  if (IsFunction (&mHandoff->Gpu, Address)) {
    return TRUE;
  }

  for (Index = 0; Index < mHandoff->BridgeCount && Index < REBIOS_SANDBOX_MAX_BRIDGES; Index++) {
    if (IsFunction (&mHandoff->Bridges[Index], Address)) {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Reads a register of PCI configuration space.

  @param[in]  Address  The register's address, in PCI_SEGMENT_LIB_ADDRESS () form.
  @param[in]  Size     The access's size in bytes: 1, 2, or 4.

  @return  The value read, all ones for a function that is not there.
**/
STATIC
UINT32
SandboxPciRead (
  IN UINT64  Address,
  IN UINTN   Size
  )
{
  if (!Reachable (Address)) {
    return (Size == 4) ? MAX_UINT32 : (UINT32)(LShiftU64 (1, Size * 8) - 1);
  }

  return (UINT32)ReBiosHypercall (REBIOS_SANDBOX_CALL_PCI_READ, Address, Size, 0, 0);
}

/**
  Writes a register of PCI configuration space.

  @param[in]  Address  The register's address, in PCI_SEGMENT_LIB_ADDRESS () form.
  @param[in]  Size     The access's size in bytes: 1, 2, or 4.
  @param[in]  Value    The value to write.

  @return  The value written.
**/
STATIC
UINT32
SandboxPciWrite (
  IN UINT64  Address,
  IN UINTN   Size,
  IN UINT32  Value
  )
{
  if (Reachable (Address)) {
    ReBiosHypercall (REBIOS_SANDBOX_CALL_PCI_WRITE, Address, Size, Value, 0);
  }

  return Value;
}

/**
  Every function is reached from the PCI enumeration complete protocol's
  installation on.

  @param[in]  Event    The event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
OnEnumerationComplete (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  VOID  *Interface;

  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiPciEnumerationCompleteProtocolGuid, NULL, &Interface))) {
    mEnumerated = TRUE;
    gBS->CloseEvent (Event);
  }
}

/**
  Finds the GPU and its bridges in ReBIOS's handoff, and waits for PCI
  enumeration to complete.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  Always.
**/
EFI_STATUS
EFIAPI
SandboxPciSegmentLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *GuidHob;

  GuidHob = GetFirstGuidHob (&gReBiosSandboxHandoffHobGuid);
  ASSERT (GuidHob != NULL);
  if (GuidHob != NULL) {
    mHandoff = GET_GUID_HOB_DATA (GuidHob);
  }

  EfiCreateProtocolNotifyEvent (
    &gEfiPciEnumerationCompleteProtocolGuid,
    TPL_CALLBACK,
    OnEnumerationComplete,
    NULL,
    &mEnumeratedRegistration
    );
  return EFI_SUCCESS;
}

/**
  Assert the validity of a PCI Segment address.
  A valid PCI Segment address should not contain 1's in bits 28..31 and 48..63.

  @param  A The address to validate.
            Layout of this address parameter is as follows:

            PCI Register: Bits 0..11
            PCI Function  Bits 12..14
            PCI Device  Bits 15..19
            PCI Bus Bits 20..27
            Reserved  Bits 28..31.  Must be 0.
            PCI Segment Bits 32..47
            Reserved  Bits 48..63.  Must be 0.

            | Reserved (MBZ) | Segment | Reserved (MBZ) |     Bus     | Device | Function | Register |
            63             48  47    32  31           28 27         20 19    15 14      12 11         0
  @param  M Additional bits to assert to be zero.

**/
#define ASSERT_INVALID_PCI_SEGMENT_ADDRESS(A, M) \
  ASSERT (((A) & (0xffff0000f0000000ULL | (M))) == 0)

/**
  Register a PCI device so PCI configuration registers may be accessed after
  SetVirtualAddressMap().

  If any reserved bits in Address are set, then ASSERT().

  @param  Address The address that encodes the PCI Bus, Device, Function and
                  Register.

  @retval RETURN_SUCCESS           The PCI device was registered for runtime access.
  @retval RETURN_UNSUPPORTED       An attempt was made to call this function
                                   after ExitBootServices().
  @retval RETURN_UNSUPPORTED       The resources required to access the PCI device
                                   at runtime could not be mapped.
  @retval RETURN_OUT_OF_RESOURCES  There are not enough resources available to
                                   complete the registration.

**/
RETURN_STATUS
EFIAPI
PciSegmentRegisterForRuntimeAccess (
  IN UINTN  Address
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 0);
  //
  // The sandbox firmware has no runtime.
  //
  return RETURN_SUCCESS;
}

/**
  Reads an 8-bit PCI configuration register.

  Reads and returns the 8-bit PCI configuration register specified by Address.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().

  @param  Address   Address that encodes the PCI Segment, Bus, Device, Function, and Register.

  @return The 8-bit PCI configuration register specified by Address.

**/
UINT8
EFIAPI
PciSegmentRead8 (
  IN UINT64  Address
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 0);

  return (UINT8)SandboxPciRead (Address, 1);
}

/**
  Writes an 8-bit PCI configuration register.

  Writes the 8-bit PCI configuration register specified by Address with the value specified by Value.
  Value is returned.  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().

  @param  Address     Address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  Value       The value to write.

  @return The value written to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentWrite8 (
  IN UINT64  Address,
  IN UINT8   Value
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 0);

  return (UINT8)SandboxPciWrite (Address, 1, Value);
}

/**
  Performs a bitwise OR of an 8-bit PCI configuration register with an 8-bit value.

  Reads the 8-bit PCI configuration register specified by Address,
  performs a bitwise OR between the read result and the value specified by OrData,
  and writes the result to the 8-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().

  @param  Address   Address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentOr8 (
  IN UINT64  Address,
  IN UINT8   OrData
  )
{
  return PciSegmentWrite8 (Address, (UINT8)(PciSegmentRead8 (Address) | OrData));
}

/**
  Performs a bitwise AND of an 8-bit PCI configuration register with an 8-bit value.

  Reads the 8-bit PCI configuration register specified by Address,
  performs a bitwise AND between the read result and the value specified by AndData,
  and writes the result to the 8-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.
  If any reserved bits in Address are set, then ASSERT().

  @param  Address   Address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  AndData   The value to AND with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentAnd8 (
  IN UINT64  Address,
  IN UINT8   AndData
  )
{
  return PciSegmentWrite8 (Address, (UINT8)(PciSegmentRead8 (Address) & AndData));
}

/**
  Performs a bitwise AND of an 8-bit PCI configuration register with an 8-bit value,
  followed a  bitwise OR with another 8-bit value.

  Reads the 8-bit PCI configuration register specified by Address,
  performs a bitwise AND between the read result and the value specified by AndData,
  performs a bitwise OR between the result of the AND operation and the value specified by OrData,
  and writes the result to the 8-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().

  @param  Address   Address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  AndData   The value to AND with the PCI configuration register.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentAndThenOr8 (
  IN UINT64  Address,
  IN UINT8   AndData,
  IN UINT8   OrData
  )
{
  return PciSegmentWrite8 (Address, (UINT8)((PciSegmentRead8 (Address) & AndData) | OrData));
}

/**
  Reads a bit field of a PCI configuration register.

  Reads the bit field in an 8-bit PCI configuration register. The bit field is
  specified by the StartBit and the EndBit. The value of the bit field is
  returned.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 7, then ASSERT().
  If EndBit is greater than 7, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().

  @param  Address   PCI configuration register to read.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..7.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..7.

  @return The value of the bit field read from the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentBitFieldRead8 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit
  )
{
  return BitFieldRead8 (PciSegmentRead8 (Address), StartBit, EndBit);
}

/**
  Writes a bit field to a PCI configuration register.

  Writes Value to the bit field of the PCI configuration register. The bit
  field is specified by the StartBit and the EndBit. All other bits in the
  destination PCI configuration register are preserved. The new value of the
  8-bit register is returned.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 7, then ASSERT().
  If EndBit is greater than 7, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If Value is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..7.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..7.
  @param  Value     New value of the bit field.

  @return The value written back to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentBitFieldWrite8 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT8   Value
  )
{
  return PciSegmentWrite8 (
           Address,
           BitFieldWrite8 (PciSegmentRead8 (Address), StartBit, EndBit, Value)
           );
}

/**
  Reads a bit field in an 8-bit PCI configuration, performs a bitwise OR, and
  writes the result back to the bit field in the 8-bit port.

  Reads the 8-bit PCI configuration register specified by Address, performs a
  bitwise OR between the read result and the value specified by
  OrData, and writes the result to the 8-bit PCI configuration register
  specified by Address. The value written to the PCI configuration register is
  returned. This function must guarantee that all PCI read and write operations
  are serialized. Extra left bits in OrData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 7, then ASSERT().
  If EndBit is greater than 7, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If OrData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..7.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..7.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written back to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentBitFieldOr8 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT8   OrData
  )
{
  return PciSegmentWrite8 (
           Address,
           BitFieldOr8 (PciSegmentRead8 (Address), StartBit, EndBit, OrData)
           );
}

/**
  Reads a bit field in an 8-bit PCI configuration register, performs a bitwise
  AND, and writes the result back to the bit field in the 8-bit register.

  Reads the 8-bit PCI configuration register specified by Address, performs a
  bitwise AND between the read result and the value specified by AndData, and
  writes the result to the 8-bit PCI configuration register specified by
  Address. The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are
  serialized. Extra left bits in AndData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 7, then ASSERT().
  If EndBit is greater than 7, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If AndData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..7.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..7.
  @param  AndData   The value to AND with the PCI configuration register.

  @return The value written back to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentBitFieldAnd8 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT8   AndData
  )
{
  return PciSegmentWrite8 (
           Address,
           BitFieldAnd8 (PciSegmentRead8 (Address), StartBit, EndBit, AndData)
           );
}

/**
  Reads a bit field in an 8-bit port, performs a bitwise AND followed by a
  bitwise OR, and writes the result back to the bit field in the 8-bit port.

  Reads the 8-bit PCI configuration register specified by Address, performs a
  bitwise AND followed by a bitwise OR between the read result and
  the value specified by AndData, and writes the result to the 8-bit PCI
  configuration register specified by Address. The value written to the PCI
  configuration register is returned. This function must guarantee that all PCI
  read and write operations are serialized. Extra left bits in both AndData and
  OrData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 7, then ASSERT().
  If EndBit is greater than 7, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If AndData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().
  If OrData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..7.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..7.
  @param  AndData   The value to AND with the PCI configuration register.
  @param  OrData    The value to OR with the result of the AND operation.

  @return The value written back to the PCI configuration register.

**/
UINT8
EFIAPI
PciSegmentBitFieldAndThenOr8 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT8   AndData,
  IN UINT8   OrData
  )
{
  return PciSegmentWrite8 (
           Address,
           BitFieldAndThenOr8 (PciSegmentRead8 (Address), StartBit, EndBit, AndData, OrData)
           );
}

/**
  Reads a 16-bit PCI configuration register.

  Reads and returns the 16-bit PCI configuration register specified by Address.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().

  @param  Address   Address that encodes the PCI Segment, Bus, Device, Function, and Register.

  @return The 16-bit PCI configuration register specified by Address.

**/
UINT16
EFIAPI
PciSegmentRead16 (
  IN UINT64  Address
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 1);

  return (UINT16)SandboxPciRead (Address, 2);
}

/**
  Writes a 16-bit PCI configuration register.

  Writes the 16-bit PCI configuration register specified by Address with the value specified by Value.
  Value is returned.  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().

  @param  Address     Address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  Value       The value to write.

  @return The parameter of Value.

**/
UINT16
EFIAPI
PciSegmentWrite16 (
  IN UINT64  Address,
  IN UINT16  Value
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 1);

  return (UINT16)SandboxPciWrite (Address, 2, Value);
}

/**
  Performs a bitwise OR of a 16-bit PCI configuration register with
  a 16-bit value.

  Reads the 16-bit PCI configuration register specified by Address, performs a
  bitwise OR between the read result and the value specified by OrData, and
  writes the result to the 16-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned. This function
  must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().

  @param  Address Address that encodes the PCI Segment, Bus, Device, Function and
                  Register.
  @param  OrData  The value to OR with the PCI configuration register.

  @return The value written back to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentOr16 (
  IN UINT64  Address,
  IN UINT16  OrData
  )
{
  return PciSegmentWrite16 (Address, (UINT16)(PciSegmentRead16 (Address) | OrData));
}

/**
  Performs a bitwise AND of a 16-bit PCI configuration register with a 16-bit value.

  Reads the 16-bit PCI configuration register specified by Address,
  performs a bitwise AND between the read result and the value specified by AndData,
  and writes the result to the 16-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().

  @param  Address   Address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  AndData   The value to AND with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentAnd16 (
  IN UINT64  Address,
  IN UINT16  AndData
  )
{
  return PciSegmentWrite16 (Address, (UINT16)(PciSegmentRead16 (Address) & AndData));
}

/**
  Performs a bitwise AND of a 16-bit PCI configuration register with a 16-bit value,
  followed a  bitwise OR with another 16-bit value.

  Reads the 16-bit PCI configuration register specified by Address,
  performs a bitwise AND between the read result and the value specified by AndData,
  performs a bitwise OR between the result of the AND operation and the value specified by OrData,
  and writes the result to the 16-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().

  @param  Address   Address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  AndData   The value to AND with the PCI configuration register.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentAndThenOr16 (
  IN UINT64  Address,
  IN UINT16  AndData,
  IN UINT16  OrData
  )
{
  return PciSegmentWrite16 (Address, (UINT16)((PciSegmentRead16 (Address) & AndData) | OrData));
}

/**
  Reads a bit field of a PCI configuration register.

  Reads the bit field in a 16-bit PCI configuration register. The bit field is
  specified by the StartBit and the EndBit. The value of the bit field is
  returned.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().
  If StartBit is greater than 15, then ASSERT().
  If EndBit is greater than 15, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().

  @param  Address   PCI configuration register to read.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..15.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..15.

  @return The value of the bit field read from the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentBitFieldRead16 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit
  )
{
  return BitFieldRead16 (PciSegmentRead16 (Address), StartBit, EndBit);
}

/**
  Writes a bit field to a PCI configuration register.

  Writes Value to the bit field of the PCI configuration register. The bit
  field is specified by the StartBit and the EndBit. All other bits in the
  destination PCI configuration register are preserved. The new value of the
  16-bit register is returned.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().
  If StartBit is greater than 15, then ASSERT().
  If EndBit is greater than 15, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If Value is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..15.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..15.
  @param  Value     New value of the bit field.

  @return The value written back to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentBitFieldWrite16 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT16  Value
  )
{
  return PciSegmentWrite16 (
           Address,
           BitFieldWrite16 (PciSegmentRead16 (Address), StartBit, EndBit, Value)
           );
}

/**
  Reads a bit field in a 16-bit PCI configuration, performs a bitwise OR, writes
  the result back to the bit field in the 16-bit port.

  Reads the 16-bit PCI configuration register specified by Address, performs a
  bitwise OR between the read result and the value specified by
  OrData, and writes the result to the 16-bit PCI configuration register
  specified by Address. The value written to the PCI configuration register is
  returned. This function must guarantee that all PCI read and write operations
  are serialized. Extra left bits in OrData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().
  If StartBit is greater than 15, then ASSERT().
  If EndBit is greater than 15, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If OrData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..15.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..15.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written back to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentBitFieldOr16 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT16  OrData
  )
{
  return PciSegmentWrite16 (
           Address,
           BitFieldOr16 (PciSegmentRead16 (Address), StartBit, EndBit, OrData)
           );
}

/**
  Reads a bit field in a 16-bit PCI configuration register, performs a bitwise
  AND, writes the result back to the bit field in the 16-bit register.

  Reads the 16-bit PCI configuration register specified by Address, performs a
  bitwise AND between the read result and the value specified by AndData, and
  writes the result to the 16-bit PCI configuration register specified by
  Address. The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are
  serialized. Extra left bits in AndData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 16-bit boundary, then ASSERT().
  If StartBit is greater than 15, then ASSERT().
  If EndBit is greater than 15, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If AndData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   Address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..15.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..15.
  @param  AndData   The value to AND with the PCI configuration register.

  @return The value written back to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentBitFieldAnd16 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT16  AndData
  )
{
  return PciSegmentWrite16 (
           Address,
           BitFieldAnd16 (PciSegmentRead16 (Address), StartBit, EndBit, AndData)
           );
}

/**
  Reads a bit field in a 16-bit port, performs a bitwise AND followed by a
  bitwise OR, and writes the result back to the bit field in the
  16-bit port.

  Reads the 16-bit PCI configuration register specified by Address, performs a
  bitwise AND followed by a bitwise OR between the read result and
  the value specified by AndData, and writes the result to the 16-bit PCI
  configuration register specified by Address. The value written to the PCI
  configuration register is returned. This function must guarantee that all PCI
  read and write operations are serialized. Extra left bits in both AndData and
  OrData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 15, then ASSERT().
  If EndBit is greater than 15, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If AndData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().
  If OrData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..15.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..15.
  @param  AndData   The value to AND with the PCI configuration register.
  @param  OrData    The value to OR with the result of the AND operation.

  @return The value written back to the PCI configuration register.

**/
UINT16
EFIAPI
PciSegmentBitFieldAndThenOr16 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT16  AndData,
  IN UINT16  OrData
  )
{
  return PciSegmentWrite16 (
           Address,
           BitFieldAndThenOr16 (PciSegmentRead16 (Address), StartBit, EndBit, AndData, OrData)
           );
}

/**
  Reads a 32-bit PCI configuration register.

  Reads and returns the 32-bit PCI configuration register specified by Address.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().

  @param  Address   Address that encodes the PCI Segment, Bus, Device, Function, and Register.

  @return The 32-bit PCI configuration register specified by Address.

**/
UINT32
EFIAPI
PciSegmentRead32 (
  IN UINT64  Address
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 3);

  return SandboxPciRead (Address, 4);
}

/**
  Writes a 32-bit PCI configuration register.

  Writes the 32-bit PCI configuration register specified by Address with the value specified by Value.
  Value is returned.  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().

  @param  Address     Address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  Value       The value to write.

  @return The parameter of Value.

**/
UINT32
EFIAPI
PciSegmentWrite32 (
  IN UINT64  Address,
  IN UINT32  Value
  )
{
  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (Address, 3);

  return SandboxPciWrite (Address, 4, Value);
}

/**
  Performs a bitwise OR of a 32-bit PCI configuration register with a 32-bit value.

  Reads the 32-bit PCI configuration register specified by Address,
  performs a bitwise OR between the read result and the value specified by OrData,
  and writes the result to the 32-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().

  @param  Address   Address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentOr32 (
  IN UINT64  Address,
  IN UINT32  OrData
  )
{
  return PciSegmentWrite32 (Address, PciSegmentRead32 (Address) | OrData);
}

/**
  Performs a bitwise AND of a 32-bit PCI configuration register with a 32-bit value.

  Reads the 32-bit PCI configuration register specified by Address,
  performs a bitwise AND between the read result and the value specified by AndData,
  and writes the result to the 32-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().

  @param  Address   Address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  AndData   The value to AND with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentAnd32 (
  IN UINT64  Address,
  IN UINT32  AndData
  )
{
  return PciSegmentWrite32 (Address, PciSegmentRead32 (Address) & AndData);
}

/**
  Performs a bitwise AND of a 32-bit PCI configuration register with a 32-bit value,
  followed a  bitwise OR with another 32-bit value.

  Reads the 32-bit PCI configuration register specified by Address,
  performs a bitwise AND between the read result and the value specified by AndData,
  performs a bitwise OR between the result of the AND operation and the value specified by OrData,
  and writes the result to the 32-bit PCI configuration register specified by Address.
  The value written to the PCI configuration register is returned.
  This function must guarantee that all PCI read and write operations are serialized.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().

  @param  Address   Address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  AndData   The value to AND with the PCI configuration register.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentAndThenOr32 (
  IN UINT64  Address,
  IN UINT32  AndData,
  IN UINT32  OrData
  )
{
  return PciSegmentWrite32 (Address, (PciSegmentRead32 (Address) & AndData) | OrData);
}

/**
  Reads a bit field of a PCI configuration register.

  Reads the bit field in a 32-bit PCI configuration register. The bit field is
  specified by the StartBit and the EndBit. The value of the bit field is
  returned.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().
  If StartBit is greater than 31, then ASSERT().
  If EndBit is greater than 31, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().

  @param  Address   PCI configuration register to read.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..31.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..31.

  @return The value of the bit field read from the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentBitFieldRead32 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit
  )
{
  return BitFieldRead32 (PciSegmentRead32 (Address), StartBit, EndBit);
}

/**
  Writes a bit field to a PCI configuration register.

  Writes Value to the bit field of the PCI configuration register. The bit
  field is specified by the StartBit and the EndBit. All other bits in the
  destination PCI configuration register are preserved. The new value of the
  32-bit register is returned.

  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().
  If StartBit is greater than 31, then ASSERT().
  If EndBit is greater than 31, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If Value is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..31.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..31.
  @param  Value     New value of the bit field.

  @return The value written back to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentBitFieldWrite32 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT32  Value
  )
{
  return PciSegmentWrite32 (
           Address,
           BitFieldWrite32 (PciSegmentRead32 (Address), StartBit, EndBit, Value)
           );
}

/**
  Reads a bit field in a 32-bit PCI configuration, performs a bitwise OR, and
  writes the result back to the bit field in the 32-bit port.

  Reads the 32-bit PCI configuration register specified by Address, performs a
  bitwise OR between the read result and the value specified by
  OrData, and writes the result to the 32-bit PCI configuration register
  specified by Address. The value written to the PCI configuration register is
  returned. This function must guarantee that all PCI read and write operations
  are serialized. Extra left bits in OrData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 31, then ASSERT().
  If EndBit is greater than 31, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If OrData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..31.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..31.
  @param  OrData    The value to OR with the PCI configuration register.

  @return The value written back to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentBitFieldOr32 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT32  OrData
  )
{
  return PciSegmentWrite32 (
           Address,
           BitFieldOr32 (PciSegmentRead32 (Address), StartBit, EndBit, OrData)
           );
}

/**
  Reads a bit field in a 32-bit PCI configuration register, performs a bitwise
  AND, and writes the result back to the bit field in the 32-bit register.


  Reads the 32-bit PCI configuration register specified by Address, performs a bitwise
  AND between the read result and the value specified by AndData, and writes the result
  to the 32-bit PCI configuration register specified by Address. The value written to
  the PCI configuration register is returned.  This function must guarantee that all PCI
  read and write operations are serialized.  Extra left bits in AndData are stripped.
  If any reserved bits in Address are set, then ASSERT().
  If Address is not aligned on a 32-bit boundary, then ASSERT().
  If StartBit is greater than 31, then ASSERT().
  If EndBit is greater than 31, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If AndData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   Address that encodes the PCI Segment, Bus, Device, Function, and Register.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..31.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..31.
  @param  AndData   The value to AND with the PCI configuration register.

  @return The value written back to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentBitFieldAnd32 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT32  AndData
  )
{
  return PciSegmentWrite32 (
           Address,
           BitFieldAnd32 (PciSegmentRead32 (Address), StartBit, EndBit, AndData)
           );
}

/**
  Reads a bit field in a 32-bit port, performs a bitwise AND followed by a
  bitwise OR, and writes the result back to the bit field in the
  32-bit port.

  Reads the 32-bit PCI configuration register specified by Address, performs a
  bitwise AND followed by a bitwise OR between the read result and
  the value specified by AndData, and writes the result to the 32-bit PCI
  configuration register specified by Address. The value written to the PCI
  configuration register is returned. This function must guarantee that all PCI
  read and write operations are serialized. Extra left bits in both AndData and
  OrData are stripped.

  If any reserved bits in Address are set, then ASSERT().
  If StartBit is greater than 31, then ASSERT().
  If EndBit is greater than 31, then ASSERT().
  If EndBit is less than StartBit, then ASSERT().
  If AndData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().
  If OrData is larger than the bitmask value range specified by StartBit and EndBit, then ASSERT().

  @param  Address   PCI configuration register to write.
  @param  StartBit  The ordinal of the least significant bit in the bit field.
                    Range 0..31.
  @param  EndBit    The ordinal of the most significant bit in the bit field.
                    Range 0..31.
  @param  AndData   The value to AND with the PCI configuration register.
  @param  OrData    The value to OR with the result of the AND operation.

  @return The value written back to the PCI configuration register.

**/
UINT32
EFIAPI
PciSegmentBitFieldAndThenOr32 (
  IN UINT64  Address,
  IN UINTN   StartBit,
  IN UINTN   EndBit,
  IN UINT32  AndData,
  IN UINT32  OrData
  )
{
  return PciSegmentWrite32 (
           Address,
           BitFieldAndThenOr32 (PciSegmentRead32 (Address), StartBit, EndBit, AndData, OrData)
           );
}

/**
  Reads a range of PCI configuration registers into a caller supplied buffer.

  Reads the range of PCI configuration registers specified by StartAddress and
  Size into the buffer specified by Buffer. This function only allows the PCI
  configuration registers from a single PCI function to be read. Size is
  returned. When possible 32-bit PCI configuration read cycles are used to read
  from StartAddress to StartAddress + Size. Due to alignment restrictions, 8-bit
  and 16-bit PCI configuration read cycles may be used at the beginning and the
  end of the range.

  If any reserved bits in StartAddress are set, then ASSERT().
  If ((StartAddress & 0xFFF) + Size) > 0x1000, then ASSERT().
  If Size > 0 and Buffer is NULL, then ASSERT().

  @param  StartAddress  Starting address that encodes the PCI Segment, Bus, Device,
                        Function and Register.
  @param  Size          Size in bytes of the transfer.
  @param  Buffer        Pointer to a buffer receiving the data read.

  @return Size

**/
UINTN
EFIAPI
PciSegmentReadBuffer (
  IN  UINT64  StartAddress,
  IN  UINTN   Size,
  OUT VOID    *Buffer
  )
{
  UINTN  ReturnValue;

  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (StartAddress, 0);
  ASSERT (((StartAddress & 0xFFF) + Size) <= 0x1000);

  if (Size == 0) {
    return Size;
  }

  ASSERT (Buffer != NULL);

  //
  // Save Size for return
  //
  ReturnValue = Size;

  if ((StartAddress & BIT0) != 0) {
    //
    // Read a byte if StartAddress is byte aligned
    //
    *(volatile UINT8 *)Buffer = PciSegmentRead8 (StartAddress);
    StartAddress             += sizeof (UINT8);
    Size                     -= sizeof (UINT8);
    Buffer                    = (UINT8 *)Buffer + 1;
  }

  if ((Size >= sizeof (UINT16)) && ((StartAddress & BIT1) != 0)) {
    //
    // Read a word if StartAddress is word aligned
    //
    WriteUnaligned16 (Buffer, PciSegmentRead16 (StartAddress));
    StartAddress += sizeof (UINT16);
    Size         -= sizeof (UINT16);
    Buffer        = (UINT16 *)Buffer + 1;
  }

  while (Size >= sizeof (UINT32)) {
    //
    // Read as many double words as possible
    //
    WriteUnaligned32 (Buffer, PciSegmentRead32 (StartAddress));
    StartAddress += sizeof (UINT32);
    Size         -= sizeof (UINT32);
    Buffer        = (UINT32 *)Buffer + 1;
  }

  if (Size >= sizeof (UINT16)) {
    //
    // Read the last remaining word if exist
    //
    WriteUnaligned16 (Buffer, PciSegmentRead16 (StartAddress));
    StartAddress += sizeof (UINT16);
    Size         -= sizeof (UINT16);
    Buffer        = (UINT16 *)Buffer + 1;
  }

  if (Size >= sizeof (UINT8)) {
    //
    // Read the last remaining byte if exist
    //
    *(volatile UINT8 *)Buffer = PciSegmentRead8 (StartAddress);
  }

  return ReturnValue;
}

/**
  Copies the data in a caller supplied buffer to a specified range of PCI
  configuration space.

  Writes the range of PCI configuration registers specified by StartAddress and
  Size from the buffer specified by Buffer. This function only allows the PCI
  configuration registers from a single PCI function to be written. Size is
  returned. When possible 32-bit PCI configuration write cycles are used to
  write from StartAddress to StartAddress + Size. Due to alignment restrictions,
  8-bit and 16-bit PCI configuration write cycles may be used at the beginning
  and the end of the range.

  If any reserved bits in StartAddress are set, then ASSERT().
  If ((StartAddress & 0xFFF) + Size) > 0x1000, then ASSERT().
  If Size > 0 and Buffer is NULL, then ASSERT().

  @param  StartAddress  Starting address that encodes the PCI Segment, Bus, Device,
                        Function and Register.
  @param  Size          Size in bytes of the transfer.
  @param  Buffer        Pointer to a buffer containing the data to write.

  @return The parameter of Size.

**/
UINTN
EFIAPI
PciSegmentWriteBuffer (
  IN UINT64  StartAddress,
  IN UINTN   Size,
  IN VOID    *Buffer
  )
{
  UINTN  ReturnValue;

  ASSERT_INVALID_PCI_SEGMENT_ADDRESS (StartAddress, 0);
  ASSERT (((StartAddress & 0xFFF) + Size) <= 0x1000);

  if (Size == 0) {
    return 0;
  }

  ASSERT (Buffer != NULL);

  //
  // Save Size for return
  //
  ReturnValue = Size;

  if ((StartAddress & BIT0) != 0) {
    //
    // Write a byte if StartAddress is byte aligned
    //
    PciSegmentWrite8 (StartAddress, *(UINT8 *)Buffer);
    StartAddress += sizeof (UINT8);
    Size         -= sizeof (UINT8);
    Buffer        = (UINT8 *)Buffer + 1;
  }

  if ((Size >= sizeof (UINT16)) && ((StartAddress & BIT1) != 0)) {
    //
    // Write a word if StartAddress is word aligned
    //
    PciSegmentWrite16 (StartAddress, ReadUnaligned16 (Buffer));
    StartAddress += sizeof (UINT16);
    Size         -= sizeof (UINT16);
    Buffer        = (UINT16 *)Buffer + 1;
  }

  while (Size >= sizeof (UINT32)) {
    //
    // Write as many double words as possible
    //
    PciSegmentWrite32 (StartAddress, ReadUnaligned32 (Buffer));
    StartAddress += sizeof (UINT32);
    Size         -= sizeof (UINT32);
    Buffer        = (UINT32 *)Buffer + 1;
  }

  if (Size >= sizeof (UINT16)) {
    //
    // Write the last remaining word if exist
    //
    PciSegmentWrite16 (StartAddress, ReadUnaligned16 (Buffer));
    StartAddress += sizeof (UINT16);
    Size         -= sizeof (UINT16);
    Buffer        = (UINT16 *)Buffer + 1;
  }

  if (Size >= sizeof (UINT8)) {
    //
    // Write the last remaining byte if exist
    //
    PciSegmentWrite8 (StartAddress, *(UINT8 *)Buffer);
  }

  return ReturnValue;
}
