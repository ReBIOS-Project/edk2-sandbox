/** @file
  The platform GOP policy protocol, through which a platform firmware gives an
  Intel iGPU's GOP its Video BIOS Table and the lid's state: revision 1, as
  Intel's MinnowBoard platform declares it (edk2-platforms branch
  devel-MinnowBoardMax-UDK2017, Vlv2TbltDevicePkg/Include/Protocol/
  PlatformGopPolicy.h), whose GOP looks it up.

  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef PLATFORM_GOP_POLICY_H_
#define PLATFORM_GOP_POLICY_H_

#define PLATFORM_GOP_POLICY_PROTOCOL_GUID \
  { 0xec2e931b, 0x3281, 0x48a5, { 0x81, 0x07, 0xdf, 0x8a, 0x8b, 0xed, 0x3c, 0x5d } }

#define PLATFORM_GOP_POLICY_PROTOCOL_REVISION_01  0x01

typedef enum {
  LidClosed,
  LidOpen,
  LidStatusMax
} LID_STATUS;

/**
  The lid's state.

  @param[out]  CurrentLidStatus  The state.

  @retval EFI_SUCCESS  CurrentLidStatus is the state.
**/
typedef
EFI_STATUS
(EFIAPI *GET_PLATFORM_LID_STATUS)(
  OUT LID_STATUS  *CurrentLidStatus
  );

/**
  The Video BIOS Table.

  @param[out]  VbtAddress  Its address.
  @param[out]  VbtSize     Its size.

  @retval EFI_SUCCESS            They are the table's.
  @retval EFI_INVALID_PARAMETER  One is NULL.
  @retval EFI_NOT_FOUND          There is no table.
**/
typedef
EFI_STATUS
(EFIAPI *GET_VBT_DATA)(
  OUT EFI_PHYSICAL_ADDRESS  *VbtAddress,
  OUT UINT32                *VbtSize
  );

typedef struct {
  UINT32                     Revision;
  GET_PLATFORM_LID_STATUS    GetPlatformLidStatus;
  GET_VBT_DATA               GetVbtData;
} PLATFORM_GOP_POLICY_PROTOCOL;

extern EFI_GUID  gPlatformGopPolicyProtocolGuid;

#endif
