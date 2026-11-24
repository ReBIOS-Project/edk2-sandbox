/** @file
  The sandbox's PCD services. A GOP built into the platform firmware, as an
  iGPU's, reaches that firmware's dynamic PCDs by the token numbers its build
  gave them, and its DynamicEx PCDs by their token spaces: this driver serves
  both from the platform's PCDs as ReBIOS found them when it started, in place
  of a PCD database of the sandbox's own, which has no dynamic PCDs. A PCD the
  platform lacks reads as zeroes, and the log says which.

  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Protocol/Pcd.h>
#include <Protocol/PcdInfo.h>
#include <Protocol/PiPcd.h>
#include <Protocol/PiPcdInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <ReBiosSandbox.h>

///
/// A PCD of the platform's: its entry in the handoff, its value, and how many
/// bytes the value has room for.
///
typedef struct {
  REBIOS_SANDBOX_PCD    *Entry;
  UINT8                 *Value;
  UINT32                Room;
} SANDBOX_PCD;

///
/// A function to call as a PCD is set, the PCD's token space all zeroes for
/// the local one.
///
typedef struct {
  EFI_GUID                 Guid;
  UINTN                    TokenNumber;
  PCD_PROTOCOL_CALLBACK    Function;
} SANDBOX_PCD_CALLBACK;

///
/// A PCD that a driver asked for and the platform lacks.
///
typedef struct {
  EFI_GUID    Guid;
  UINTN       TokenNumber;
} SANDBOX_PCD_MISS;

#define MAX_CALLBACKS      32
#define MAX_MISSES_LOGGED  32

STATIC SANDBOX_PCD           *mPcds;
STATIC UINTN                 mPcdCount;
STATIC UINTN                 mSku;
STATIC SANDBOX_PCD_CALLBACK  mCallbacks[MAX_CALLBACKS];
STATIC UINTN                 mCallbackCount;
STATIC SANDBOX_PCD_MISS      mMisses[MAX_MISSES_LOGGED];
STATIC UINTN                 mMissCount;
STATIC CONST EFI_GUID        mLocalSpace;
STATIC UINT8                 mZeroes[256];
STATIC EFI_LOCK              mLock = EFI_INITIALIZE_LOCK_VARIABLE (TPL_NOTIFY);

/**
  The token space a caller names, the local one where it names none.

  @param[in]  Guid  The token space, or NULL.

  @return  The token space.
**/
STATIC
CONST EFI_GUID *
SpaceOf (
  IN CONST EFI_GUID  *Guid
  )
{
  return Guid != NULL ? Guid : &mLocalSpace;
}

/**
  Logs a PCD that a driver asked for and the platform lacks, once.

  @param[in]  Guid         The token space.
  @param[in]  TokenNumber  The token number.
**/
STATIC
VOID
LogMiss (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber
  )
{
  UINTN  Index;

  for (Index = 0; Index < mMissCount; Index++) {
    if ((mMisses[Index].TokenNumber == TokenNumber) && CompareGuid (&mMisses[Index].Guid, Guid)) {
      return;
    }
  }

  if (mMissCount < MAX_MISSES_LOGGED) {
    CopyGuid (&mMisses[mMissCount].Guid, Guid);
    mMisses[mMissCount].TokenNumber = TokenNumber;
    mMissCount++;
  }

  DEBUG ((DEBUG_ERROR, "ReBIOS sandbox: the platform has no PCD %g %u\n", Guid, (UINT32)TokenNumber));
}

/**
  Finds one of the platform's PCDs.

  @param[in]  Guid         The token space, or NULL for the local one.
  @param[in]  TokenNumber  The token number.
  @param[in]  Log          Whether to log it where the platform lacks it.

  @return  The PCD, or NULL where the platform lacks it.
**/
STATIC
SANDBOX_PCD *
FindPcd (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber,
  IN BOOLEAN         Log
  )
{
  CONST EFI_GUID  *Space;
  UINTN           Index;

  Space = SpaceOf (Guid);
  for (Index = 0; Index < mPcdCount; Index++) {
    if ((mPcds[Index].Entry->TokenNumber == TokenNumber) &&
        CompareGuid (&mPcds[Index].Entry->Guid, Space))
    {
      return &mPcds[Index];
    }
  }

  if (Log) {
    LogMiss (Space, TokenNumber);
  }

  return NULL;
}

/**
  Reads a PCD's value into a buffer, its low bytes where the buffer is
  smaller, the rest zeroes where it is larger or the platform lacks the PCD.

  @param[in]   Guid         The token space, or NULL for the local one.
  @param[in]   TokenNumber  The token number.
  @param[out]  Buffer       The buffer.
  @param[in]   Size         The buffer's size.
**/
STATIC
VOID
ReadPcd (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber,
  OUT VOID           *Buffer,
  IN UINTN           Size
  )
{
  SANDBOX_PCD  *Pcd;

  ZeroMem (Buffer, Size);
  Pcd = FindPcd (Guid, TokenNumber, TRUE);
  if (Pcd != NULL) {
    CopyMem (Buffer, Pcd->Value, MIN (Size, Pcd->Entry->Size));
  }
}

/**
  Sets a PCD's value, and calls the functions that drivers asked to have
  called as it is set.

  @param[in]      Guid         The token space, or NULL for the local one.
  @param[in]      TokenNumber  The token number.
  @param[in, out] Size         The value's size; for a pointer PCD, the room
                               it has where the value is larger.
  @param[in]      Buffer       The value.
  @param[in]      Pointer      Whether the PCD is a pointer PCD, whose value
                               can be smaller than it was.

  @retval EFI_SUCCESS            It is set.
  @retval EFI_NOT_FOUND          The platform lacks the PCD.
  @retval EFI_INVALID_PARAMETER  The value's size does not fit the PCD.
**/
STATIC
EFI_STATUS
SetPcd (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber,
  IN OUT UINTN       *Size,
  IN CONST VOID      *Buffer,
  IN BOOLEAN         Pointer
  )
{
  SANDBOX_PCD  *Pcd;
  UINTN        Index;

  Pcd = FindPcd (Guid, TokenNumber, TRUE);
  if (Pcd == NULL) {
    return EFI_NOT_FOUND;
  }

  if (Pointer && (*Size > Pcd->Room)) {
    *Size = Pcd->Room;
    return EFI_INVALID_PARAMETER;
  }

  if (!Pointer && (*Size != Pcd->Entry->Size)) {
    return EFI_INVALID_PARAMETER;
  }

  EfiAcquireLock (&mLock);
  CopyMem (Pcd->Value, Buffer, *Size);
  Pcd->Entry->Size = (UINT32)*Size;
  EfiReleaseLock (&mLock);

  for (Index = 0; Index < mCallbackCount; Index++) {
    if ((mCallbacks[Index].TokenNumber == TokenNumber) &&
        CompareGuid (&mCallbacks[Index].Guid, SpaceOf (Guid)))
    {
      mCallbacks[Index].Function (Guid, TokenNumber, Pcd->Value, *Size);
    }
  }

  return EFI_SUCCESS;
}

/**
  Has a function called as a PCD is set.

  @param[in]  Guid         The token space, or NULL for the local one.
  @param[in]  TokenNumber  The token number.
  @param[in]  Function     The function.

  @retval EFI_SUCCESS            It is called from now on.
  @retval EFI_INVALID_PARAMETER  Function is NULL.
  @retval EFI_NOT_FOUND          The platform lacks the PCD.
  @retval EFI_OUT_OF_RESOURCES   No more functions can be called.
**/
STATIC
EFI_STATUS
AddCallback (
  IN CONST EFI_GUID         *Guid,
  IN UINTN                  TokenNumber,
  IN PCD_PROTOCOL_CALLBACK  Function
  )
{
  if (Function == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (FindPcd (Guid, TokenNumber, TRUE) == NULL) {
    return EFI_NOT_FOUND;
  }

  if (mCallbackCount == MAX_CALLBACKS) {
    DEBUG ((DEBUG_ERROR, "ReBIOS sandbox: no room for another PCD callback\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  CopyGuid (&mCallbacks[mCallbackCount].Guid, SpaceOf (Guid));
  mCallbacks[mCallbackCount].TokenNumber = TokenNumber;
  mCallbacks[mCallbackCount].Function    = Function;
  mCallbackCount++;
  return EFI_SUCCESS;
}

/**
  Stops calling a function as a PCD is set.

  @param[in]  Guid         The token space, or NULL for the local one.
  @param[in]  TokenNumber  The token number.
  @param[in]  Function     The function.

  @retval EFI_SUCCESS            It is no longer called.
  @retval EFI_INVALID_PARAMETER  It was not called.
**/
STATIC
EFI_STATUS
RemoveCallback (
  IN CONST EFI_GUID         *Guid,
  IN UINTN                  TokenNumber,
  IN PCD_PROTOCOL_CALLBACK  Function
  )
{
  UINTN  Index;

  for (Index = 0; Index < mCallbackCount; Index++) {
    if ((mCallbacks[Index].TokenNumber == TokenNumber) &&
        (mCallbacks[Index].Function == Function) &&
        CompareGuid (&mCallbacks[Index].Guid, SpaceOf (Guid)))
    {
      mCallbacks[Index] = mCallbacks[mCallbackCount - 1];
      mCallbackCount--;
      return EFI_SUCCESS;
    }
  }

  return EFI_INVALID_PARAMETER;
}

/**
  The token number after one in a token space, or its first.

  @param[in]      Guid         The token space, or NULL for the local one.
  @param[in, out] TokenNumber  The token number, 0 for the first; 0 where
                               there is no next.

  @retval EFI_SUCCESS    TokenNumber is the next.
  @retval EFI_NOT_FOUND  There is no next.
**/
STATIC
EFI_STATUS
NextToken (
  IN CONST EFI_GUID  *Guid,
  IN OUT UINTN       *TokenNumber
  )
{
  CONST EFI_GUID  *Space;
  BOOLEAN         Next;
  UINTN           Index;

  Space = SpaceOf (Guid);
  Next  = *TokenNumber == 0;
  for (Index = 0; Index < mPcdCount; Index++) {
    if (!CompareGuid (&mPcds[Index].Entry->Guid, Space)) {
      continue;
    }

    if (Next) {
      *TokenNumber = mPcds[Index].Entry->TokenNumber;
      return EFI_SUCCESS;
    }

    Next = mPcds[Index].Entry->TokenNumber == *TokenNumber;
  }

  *TokenNumber = 0;
  return EFI_NOT_FOUND;
}

/**
  The DynamicEx token space after one, or the first.

  @param[in, out] Guid  The token space, or NULL for the first; NULL where
                        there is no next.

  @retval EFI_SUCCESS    Guid is the next.
  @retval EFI_NOT_FOUND  There is no next.
**/
STATIC
EFI_STATUS
NextTokenSpace (
  IN OUT CONST EFI_GUID  **Guid
  )
{
  BOOLEAN         Next;
  UINTN           Index;
  UINTN           Before;
  CONST EFI_GUID  *Space;

  Next = *Guid == NULL;
  for (Index = 0; Index < mPcdCount; Index++) {
    Space = &mPcds[Index].Entry->Guid;
    if (IsZeroGuid (Space)) {
      continue;
    }

    for (Before = 0; Before < Index; Before++) {
      if (CompareGuid (&mPcds[Before].Entry->Guid, Space)) {
        break;
      }
    }

    if (Before < Index) {
      continue;
    }

    if (Next) {
      *Guid = Space;
      return EFI_SUCCESS;
    }

    Next = CompareGuid (Space, *Guid);
  }

  *Guid = NULL;
  return EFI_NOT_FOUND;
}

/**
  What GET_PCD_INFO_PROTOCOL tells of a PCD: its type, where the platform
  gave it, else as its size suggests, and its size.

  @param[in]   Guid         The token space, or NULL for the local one.
  @param[in]   TokenNumber  The token number.
  @param[out]  PcdInfo      What it tells.

  @retval EFI_SUCCESS    PcdInfo tells it.
  @retval EFI_NOT_FOUND  The platform lacks the PCD.
**/
STATIC
EFI_STATUS
PcdInfoOf (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber,
  OUT EFI_PCD_INFO   *PcdInfo
  )
{
  SANDBOX_PCD  *Pcd;

  Pcd = FindPcd (Guid, TokenNumber, TRUE);
  if (Pcd == NULL) {
    return EFI_NOT_FOUND;
  }

  if (Pcd->Entry->Type != REBIOS_SANDBOX_PCD_TYPE_UNKNOWN) {
    PcdInfo->PcdType = (EFI_PCD_TYPE)Pcd->Entry->Type;
  } else if (Pcd->Room == 1) {
    PcdInfo->PcdType = EFI_PCD_TYPE_8;
  } else if (Pcd->Room == 2) {
    PcdInfo->PcdType = EFI_PCD_TYPE_16;
  } else if (Pcd->Room == 4) {
    PcdInfo->PcdType = EFI_PCD_TYPE_32;
  } else if (Pcd->Room == 8) {
    PcdInfo->PcdType = EFI_PCD_TYPE_64;
  } else {
    PcdInfo->PcdType = EFI_PCD_TYPE_PTR;
  }

  PcdInfo->PcdSize = Pcd->Entry->Size;
  PcdInfo->PcdName = NULL;
  return EFI_SUCCESS;
}

//
// PCD_PROTOCOL and EFI_PCD_PROTOCOL.
//

STATIC
VOID
EFIAPI
SandboxSetSku (
  IN UINTN  SkuId
  )
{
  if (SkuId != mSku) {
    DEBUG ((DEBUG_ERROR, "ReBIOS sandbox: SKU %Lu asked for, the platform's PCDs are SKU %Lu's\n", (UINT64)SkuId, (UINT64)mSku));
  }
}

STATIC
UINT8
EFIAPI
SandboxGet8 (
  IN UINTN  TokenNumber
  )
{
  UINT8  Value;

  ReadPcd (NULL, TokenNumber, &Value, sizeof (Value));
  return Value;
}

STATIC
UINT16
EFIAPI
SandboxGet16 (
  IN UINTN  TokenNumber
  )
{
  UINT16  Value;

  ReadPcd (NULL, TokenNumber, &Value, sizeof (Value));
  return Value;
}

STATIC
UINT32
EFIAPI
SandboxGet32 (
  IN UINTN  TokenNumber
  )
{
  UINT32  Value;

  ReadPcd (NULL, TokenNumber, &Value, sizeof (Value));
  return Value;
}

STATIC
UINT64
EFIAPI
SandboxGet64 (
  IN UINTN  TokenNumber
  )
{
  UINT64  Value;

  ReadPcd (NULL, TokenNumber, &Value, sizeof (Value));
  return Value;
}

STATIC
VOID *
EFIAPI
SandboxGetPtr (
  IN UINTN  TokenNumber
  )
{
  SANDBOX_PCD  *Pcd;

  Pcd = FindPcd (NULL, TokenNumber, TRUE);
  return Pcd != NULL ? Pcd->Value : mZeroes;
}

STATIC
BOOLEAN
EFIAPI
SandboxGetBool (
  IN UINTN  TokenNumber
  )
{
  return SandboxGet8 (TokenNumber) != 0;
}

STATIC
UINTN
EFIAPI
SandboxGetSize (
  IN UINTN  TokenNumber
  )
{
  SANDBOX_PCD  *Pcd;

  Pcd = FindPcd (NULL, TokenNumber, TRUE);
  return Pcd != NULL ? Pcd->Entry->Size : 0;
}

STATIC
UINT8
EFIAPI
SandboxGet8Ex (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber
  )
{
  UINT8  Value;

  ReadPcd (Guid, TokenNumber, &Value, sizeof (Value));
  return Value;
}

STATIC
UINT16
EFIAPI
SandboxGet16Ex (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber
  )
{
  UINT16  Value;

  ReadPcd (Guid, TokenNumber, &Value, sizeof (Value));
  return Value;
}

STATIC
UINT32
EFIAPI
SandboxGet32Ex (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber
  )
{
  UINT32  Value;

  ReadPcd (Guid, TokenNumber, &Value, sizeof (Value));
  return Value;
}

STATIC
UINT64
EFIAPI
SandboxGet64Ex (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber
  )
{
  UINT64  Value;

  ReadPcd (Guid, TokenNumber, &Value, sizeof (Value));
  return Value;
}

STATIC
VOID *
EFIAPI
SandboxGetPtrEx (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber
  )
{
  SANDBOX_PCD  *Pcd;

  Pcd = FindPcd (Guid, TokenNumber, TRUE);
  return Pcd != NULL ? Pcd->Value : mZeroes;
}

STATIC
BOOLEAN
EFIAPI
SandboxGetBoolEx (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber
  )
{
  return SandboxGet8Ex (Guid, TokenNumber) != 0;
}

STATIC
UINTN
EFIAPI
SandboxGetSizeEx (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber
  )
{
  SANDBOX_PCD  *Pcd;

  Pcd = FindPcd (Guid, TokenNumber, TRUE);
  return Pcd != NULL ? Pcd->Entry->Size : 0;
}

STATIC
EFI_STATUS
EFIAPI
SandboxSet8 (
  IN UINTN  TokenNumber,
  IN UINT8  Value
  )
{
  UINTN  Size;

  Size = sizeof (Value);
  return SetPcd (NULL, TokenNumber, &Size, &Value, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
SandboxSet16 (
  IN UINTN   TokenNumber,
  IN UINT16  Value
  )
{
  UINTN  Size;

  Size = sizeof (Value);
  return SetPcd (NULL, TokenNumber, &Size, &Value, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
SandboxSet32 (
  IN UINTN   TokenNumber,
  IN UINT32  Value
  )
{
  UINTN  Size;

  Size = sizeof (Value);
  return SetPcd (NULL, TokenNumber, &Size, &Value, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
SandboxSet64 (
  IN UINTN   TokenNumber,
  IN UINT64  Value
  )
{
  UINTN  Size;

  Size = sizeof (Value);
  return SetPcd (NULL, TokenNumber, &Size, &Value, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
SandboxSetPtr (
  IN UINTN      TokenNumber,
  IN OUT UINTN  *SizeOfBuffer,
  IN VOID       *Buffer
  )
{
  return SetPcd (NULL, TokenNumber, SizeOfBuffer, Buffer, TRUE);
}

STATIC
EFI_STATUS
EFIAPI
SandboxSetBool (
  IN UINTN    TokenNumber,
  IN BOOLEAN  Value
  )
{
  UINTN  Size;

  Size = sizeof (Value);
  return SetPcd (NULL, TokenNumber, &Size, &Value, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
SandboxSet8Ex (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber,
  IN UINT8           Value
  )
{
  UINTN  Size;

  Size = sizeof (Value);
  return SetPcd (Guid, TokenNumber, &Size, &Value, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
SandboxSet16Ex (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber,
  IN UINT16          Value
  )
{
  UINTN  Size;

  Size = sizeof (Value);
  return SetPcd (Guid, TokenNumber, &Size, &Value, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
SandboxSet32Ex (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber,
  IN UINT32          Value
  )
{
  UINTN  Size;

  Size = sizeof (Value);
  return SetPcd (Guid, TokenNumber, &Size, &Value, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
SandboxSet64Ex (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber,
  IN UINT64          Value
  )
{
  UINTN  Size;

  Size = sizeof (Value);
  return SetPcd (Guid, TokenNumber, &Size, &Value, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
SandboxSetPtrEx (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber,
  IN OUT UINTN       *SizeOfBuffer,
  IN VOID            *Buffer
  )
{
  return SetPcd (Guid, TokenNumber, SizeOfBuffer, Buffer, TRUE);
}

STATIC
EFI_STATUS
EFIAPI
SandboxSetBoolEx (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber,
  IN BOOLEAN         Value
  )
{
  UINTN  Size;

  Size = sizeof (Value);
  return SetPcd (Guid, TokenNumber, &Size, &Value, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
SandboxCallbackOnSet (
  IN CONST EFI_GUID         *Guid  OPTIONAL,
  IN UINTN                  TokenNumber,
  IN PCD_PROTOCOL_CALLBACK  CallBackFunction
  )
{
  return AddCallback (Guid, TokenNumber, CallBackFunction);
}

STATIC
EFI_STATUS
EFIAPI
SandboxCancelCallback (
  IN CONST EFI_GUID         *Guid  OPTIONAL,
  IN UINTN                  TokenNumber,
  IN PCD_PROTOCOL_CALLBACK  CallBackFunction
  )
{
  return RemoveCallback (Guid, TokenNumber, CallBackFunction);
}

STATIC
EFI_STATUS
EFIAPI
SandboxGetNextToken (
  IN CONST EFI_GUID  *Guid  OPTIONAL,
  IN OUT UINTN       *TokenNumber
  )
{
  return NextToken (Guid, TokenNumber);
}

STATIC
EFI_STATUS
EFIAPI
SandboxGetNextTokenSpace (
  IN OUT CONST EFI_GUID  **Guid
  )
{
  return NextTokenSpace (Guid);
}

STATIC
EFI_STATUS
EFIAPI
SandboxPiCallbackOnSet (
  IN CONST EFI_GUID             *Guid  OPTIONAL,
  IN UINTN                      TokenNumber,
  IN EFI_PCD_PROTOCOL_CALLBACK  CallBackFunction
  )
{
  return AddCallback (Guid, TokenNumber, (PCD_PROTOCOL_CALLBACK)CallBackFunction);
}

STATIC
EFI_STATUS
EFIAPI
SandboxPiCancelCallback (
  IN CONST EFI_GUID             *Guid  OPTIONAL,
  IN UINTN                      TokenNumber,
  IN EFI_PCD_PROTOCOL_CALLBACK  CallBackFunction
  )
{
  return RemoveCallback (Guid, TokenNumber, (PCD_PROTOCOL_CALLBACK)CallBackFunction);
}

//
// GET_PCD_INFO_PROTOCOL and EFI_GET_PCD_INFO_PROTOCOL.
//

STATIC
EFI_STATUS
EFIAPI
SandboxGetInfo (
  IN UINTN          TokenNumber,
  OUT EFI_PCD_INFO  *PcdInfo
  )
{
  return PcdInfoOf (NULL, TokenNumber, PcdInfo);
}

STATIC
EFI_STATUS
EFIAPI
SandboxGetInfoEx (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           TokenNumber,
  OUT EFI_PCD_INFO   *PcdInfo
  )
{
  return PcdInfoOf (Guid, TokenNumber, PcdInfo);
}

STATIC
UINTN
EFIAPI
SandboxGetSku (
  VOID
  )
{
  return mSku;
}

STATIC PCD_PROTOCOL  mPcd = {
  SandboxSetSku,
  SandboxGet8,
  SandboxGet16,
  SandboxGet32,
  SandboxGet64,
  SandboxGetPtr,
  SandboxGetBool,
  SandboxGetSize,
  SandboxGet8Ex,
  SandboxGet16Ex,
  SandboxGet32Ex,
  SandboxGet64Ex,
  SandboxGetPtrEx,
  SandboxGetBoolEx,
  SandboxGetSizeEx,
  SandboxSet8,
  SandboxSet16,
  SandboxSet32,
  SandboxSet64,
  SandboxSetPtr,
  SandboxSetBool,
  SandboxSet8Ex,
  SandboxSet16Ex,
  SandboxSet32Ex,
  SandboxSet64Ex,
  SandboxSetPtrEx,
  SandboxSetBoolEx,
  SandboxCallbackOnSet,
  SandboxCancelCallback,
  SandboxGetNextToken,
  SandboxGetNextTokenSpace
};

STATIC EFI_PCD_PROTOCOL  mPiPcd = {
  SandboxSetSku,
  SandboxGet8Ex,
  SandboxGet16Ex,
  SandboxGet32Ex,
  SandboxGet64Ex,
  SandboxGetPtrEx,
  SandboxGetBoolEx,
  SandboxGetSizeEx,
  SandboxSet8Ex,
  SandboxSet16Ex,
  SandboxSet32Ex,
  SandboxSet64Ex,
  SandboxSetPtrEx,
  SandboxSetBoolEx,
  SandboxPiCallbackOnSet,
  SandboxPiCancelCallback,
  SandboxGetNextToken,
  SandboxGetNextTokenSpace
};

STATIC GET_PCD_INFO_PROTOCOL  mGetPcdInfo = {
  SandboxGetInfo,
  SandboxGetInfoEx,
  SandboxGetSku
};

STATIC EFI_GET_PCD_INFO_PROTOCOL  mPiGetPcdInfo = {
  SandboxGetInfoEx,
  SandboxGetSku
};

/**
  Indexes the platform's PCDs that the handoff gives, and installs the PCD
  services.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  The services are installed.
  @retval Others       They could not be.
**/
EFI_STATUS
EFIAPI
SandboxPcdDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE             *GuidHob;
  CONST REBIOS_SANDBOX_HANDOFF  *Handoff;
  UINT8                         *Base;
  UINT64                        Size;
  UINT64                        Offset;
  UINT64                        Next;
  REBIOS_SANDBOX_PCD            *Entry;
  UINTN                         Pass;
  EFI_HANDLE                    Handle;

  GuidHob = GetFirstGuidHob (&gReBiosSandboxHandoffHobGuid);
  if (GuidHob == NULL) {
    return EFI_NOT_FOUND;
  }

  Handoff = GET_GUID_HOB_DATA (GuidHob);
  mSku    = (UINTN)Handoff->PcdSku;
  Base    = (UINT8 *)(UINTN)Handoff->Pcds.Base;
  Size    = Handoff->Pcds.Size;

  //
  // The entries are counted, then indexed.
  //
  for (Pass = 0; Pass < 2; Pass++) {
    mPcdCount = 0;
    for (Offset = 0; Offset + sizeof (REBIOS_SANDBOX_PCD) <= Size; Offset = Next) {
      Entry = (REBIOS_SANDBOX_PCD *)(Base + Offset);
      Next  = Offset + sizeof (REBIOS_SANDBOX_PCD) + ALIGN_VALUE ((UINT64)Entry->Size, 8);
      if (Next > Size) {
        DEBUG ((DEBUG_ERROR, "ReBIOS sandbox: the handoff's PCD at 0x%lx runs past its end\n", Offset));
        break;
      }

      if (Pass == 1) {
        mPcds[mPcdCount].Entry = Entry;
        mPcds[mPcdCount].Value = (UINT8 *)(Entry + 1);
        mPcds[mPcdCount].Room  = Entry->Size;
      }

      mPcdCount++;
    }

    if ((Pass == 0) && (mPcdCount != 0)) {
      mPcds = AllocatePool (mPcdCount * sizeof (SANDBOX_PCD));
      if (mPcds == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }
    }
  }

  DEBUG ((DEBUG_INFO, "ReBIOS sandbox: %u of the platform's PCDs\n", (UINT32)mPcdCount));

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gPcdProtocolGuid,
                &mPcd,
                &gEfiPcdProtocolGuid,
                &mPiPcd,
                &gGetPcdInfoProtocolGuid,
                &mGetPcdInfo,
                &gEfiGetPcdInfoProtocolGuid,
                &mPiGetPcdInfo,
                NULL
                );
}
