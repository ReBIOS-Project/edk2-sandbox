/** @file
  The sandbox firmware's entry point: it describes the sandbox ReBIOS gives it
  to DXE in HOBs, and starts DxeCore from its firmware volume.

  The firmware volume search and the PE/COFF loading are based on
  UefiPayloadPkg/UefiPayloadEntry/LoadDxeCore.c.

  Copyright (c) 2020, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CpuLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/HypercallLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PeCoffLib.h>
#include <Register/Intel/Cpuid.h>
#include <ReBiosSandbox.h>

#define DXE_STACK_SIZE  SIZE_128KB

//
// The size a section takes, padded to its alignment.
//
#define GET_OCCUPIED_SIZE(ActualSize, Alignment) \
  ((ActualSize) + (((Alignment) - ((ActualSize) & ((Alignment) - 1))) & ((Alignment) - 1)))

/**
  Builds the PHIT HOB, at the bottom of the free memory, which the HOBs then
  grow up from and allocations down into: UefiPayloadPkg's PayloadEntryHobLib
  has it, but no header of a package's declares it.

  @param[in]  EfiMemoryBottom      The bottom of the memory DXE has.
  @param[in]  EfiMemoryTop         Its top.
  @param[in]  EfiFreeMemoryBottom  The bottom of the free memory in it.
  @param[in]  EfiFreeMemoryTop     The top of the free memory in it.

  @return  The PHIT HOB.
**/
EFI_HOB_HANDOFF_INFO_TABLE *
EFIAPI
HobConstructor (
  IN VOID  *EfiMemoryBottom,
  IN VOID  *EfiMemoryTop,
  IN VOID  *EfiFreeMemoryBottom,
  IN VOID  *EfiFreeMemoryTop
  );

#define SYSTEM_MEMORY_ATTRIBUTES  (EFI_RESOURCE_ATTRIBUTE_PRESENT |                 \
                                   EFI_RESOURCE_ATTRIBUTE_INITIALIZED |             \
                                   EFI_RESOURCE_ATTRIBUTE_TESTED |                  \
                                   EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE |             \
                                   EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |       \
                                   EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE | \
                                   EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE)

#define MMIO_ATTRIBUTES  (EFI_RESOURCE_ATTRIBUTE_PRESENT |     \
                          EFI_RESOURCE_ATTRIBUTE_INITIALIZED | \
                          EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE)

#define IO_ATTRIBUTES  (EFI_RESOURCE_ATTRIBUTE_PRESENT | \
                        EFI_RESOURCE_ATTRIBUTE_INITIALIZED)

/**
  Ends the sandbox's run, with the status given, where the firmware cannot go
  on.

  @param[in]  Status  Why.
**/
STATIC
VOID
Fail (
  IN EFI_STATUS  Status
  )
{
  ReBiosHypercall (REBIOS_SANDBOX_CALL_DONE, Status, 0, 0, 0);
  CpuDeadLoop ();
}

/**
  Finds a file of the type given in a firmware volume, with the GUID given if
  any.

  @param[in]   FvHeader    The firmware volume.
  @param[in]   FileType    The file's type.
  @param[in]   Guid        Its name, or NULL for the first file of the type.
  @param[out]  FileHeader  The file.

  @retval EFI_SUCCESS    The file was found.
  @retval EFI_NOT_FOUND  There is no such file.
**/
STATIC
EFI_STATUS
FvFindFileByTypeGuid (
  IN  EFI_FIRMWARE_VOLUME_HEADER  *FvHeader,
  IN  EFI_FV_FILETYPE             FileType,
  IN  EFI_GUID                    *Guid           OPTIONAL,
  OUT EFI_FFS_FILE_HEADER         **FileHeader
  )
{
  EFI_PHYSICAL_ADDRESS  CurrentAddress;
  EFI_PHYSICAL_ADDRESS  EndOfFirmwareVolume;
  EFI_FFS_FILE_HEADER   *File;
  UINT32                Size;
  EFI_PHYSICAL_ADDRESS  EndOfFile;

  CurrentAddress      = (EFI_PHYSICAL_ADDRESS)(UINTN)FvHeader;
  EndOfFirmwareVolume = CurrentAddress + FvHeader->FvLength;

  for (EndOfFile = CurrentAddress + FvHeader->HeaderLength; ; ) {
    CurrentAddress = (EndOfFile + 7) & ~(EFI_PHYSICAL_ADDRESS)7;
    if (CurrentAddress > EndOfFirmwareVolume) {
      break;
    }

    File = (EFI_FFS_FILE_HEADER *)(UINTN)CurrentAddress;
    if (IS_FFS_FILE2 (File)) {
      Size = FFS_FILE2_SIZE (File);
      if (Size <= 0x00FFFFFF) {
        break;
      }
    } else {
      Size = FFS_FILE_SIZE (File);
      if (Size < sizeof (EFI_FFS_FILE_HEADER)) {
        break;
      }
    }

    EndOfFile = CurrentAddress + Size;
    if (EndOfFile > EndOfFirmwareVolume) {
      break;
    }

    if ((File->Type == FileType) && ((Guid == NULL) || CompareGuid (&File->Name, Guid))) {
      *FileHeader = File;
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}

/**
  Finds a section of the type given in a file.

  @param[in]   FileHeader   The file.
  @param[in]   SectionType  The section's type.
  @param[out]  SectionData  The section's contents.

  @retval EFI_SUCCESS    The section was found.
  @retval EFI_NOT_FOUND  There is no such section.
**/
STATIC
EFI_STATUS
FileFindSection (
  IN  EFI_FFS_FILE_HEADER  *FileHeader,
  IN  EFI_SECTION_TYPE     SectionType,
  OUT VOID                 **SectionData
  )
{
  UINT32                     FileSize;
  EFI_COMMON_SECTION_HEADER  *Section;
  UINT32                     SectionSize;
  UINT32                     Index;

  if (IS_FFS_FILE2 (FileHeader)) {
    FileSize = FFS_FILE2_SIZE (FileHeader) - sizeof (EFI_FFS_FILE_HEADER2);
    Section  = (EFI_COMMON_SECTION_HEADER *)(((EFI_FFS_FILE_HEADER2 *)FileHeader) + 1);
  } else {
    FileSize = FFS_FILE_SIZE (FileHeader) - sizeof (EFI_FFS_FILE_HEADER);
    Section  = (EFI_COMMON_SECTION_HEADER *)(FileHeader + 1);
  }

  for (Index = 0; Index < FileSize; Index += SectionSize) {
    if (Section->Type == SectionType) {
      if (IS_SECTION2 (Section)) {
        *SectionData = (VOID *)((UINT8 *)Section + sizeof (EFI_COMMON_SECTION_HEADER2));
      } else {
        *SectionData = (VOID *)((UINT8 *)Section + sizeof (EFI_COMMON_SECTION_HEADER));
      }

      return EFI_SUCCESS;
    }

    if (IS_SECTION2 (Section)) {
      SectionSize = SECTION2_SIZE (Section);
    } else {
      SectionSize = SECTION_SIZE (Section);
    }

    SectionSize = GET_OCCUPIED_SIZE (SectionSize, 4);
    if (SectionSize == 0) {
      break;
    }

    Section = (EFI_COMMON_SECTION_HEADER *)((UINT8 *)Section + SectionSize);
  }

  return EFI_NOT_FOUND;
}

/**
  Loads DxeCore from the firmware volume into memory the HOBs say it has.

  @param[in]   Fv          The firmware volume.
  @param[out]  EntryPoint  DxeCore's entry point.

  @retval EFI_SUCCESS  DxeCore was loaded.
  @retval Others       It was not found, or could not be loaded.
**/
STATIC
EFI_STATUS
LoadDxeCore (
  IN  EFI_FIRMWARE_VOLUME_HEADER  *Fv,
  OUT EFI_PHYSICAL_ADDRESS        *EntryPoint
  )
{
  EFI_STATUS                    Status;
  EFI_FFS_FILE_HEADER           *FileHeader;
  VOID                          *PeCoffImage;
  PE_COFF_LOADER_IMAGE_CONTEXT  ImageContext;
  VOID                          *Buffer;
  UINTN                         Pages;

  Status = FvFindFileByTypeGuid (Fv, EFI_FV_FILETYPE_DXE_CORE, NULL, &FileHeader);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = FileFindSection (FileHeader, EFI_SECTION_PE32, &PeCoffImage);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&ImageContext, sizeof (ImageContext));
  ImageContext.Handle    = PeCoffImage;
  ImageContext.ImageRead = PeCoffLoaderImageReadFromMemory;
  Status                 = PeCoffLoaderGetImageInfo (&ImageContext);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Pages  = EFI_SIZE_TO_PAGES ((UINTN)ImageContext.ImageSize);
  Buffer = AllocatePages (Pages);
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ImageContext.ImageAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)Buffer;
  Status                    = PeCoffLoaderLoadImage (&ImageContext);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PeCoffLoaderRelocateImage (&ImageContext);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EntryPoint = ImageContext.EntryPoint;
  BuildModuleHob (&FileHeader->Name, ImageContext.ImageAddress, EFI_PAGES_TO_SIZE (Pages), *EntryPoint);
  return EFI_SUCCESS;
}

/**
  Keeps DXE from handing out a range of the sandbox's memory that something
  is in already.

  @param[in]  Range       The range, nothing where its size is 0.
  @param[in]  MemoryType  What it holds.
**/
STATIC
VOID
ReserveRange (
  IN CONST REBIOS_SANDBOX_RANGE  *Range,
  IN EFI_MEMORY_TYPE             MemoryType
  )
{
  if (Range->Size != 0) {
    BuildMemoryAllocationHob (
      Range->Base & ~(EFI_PHYSICAL_ADDRESS)EFI_PAGE_MASK,
      ALIGN_VALUE (Range->Size + (Range->Base & EFI_PAGE_MASK), EFI_PAGE_SIZE),
      MemoryType
      );
  }
}

/**
  Describes a range of the GPU's apertures to DXE.

  @param[in]  Range         The range, nothing where its size is 0.
  @param[in]  ResourceType  Memory-mapped I/O or I/O.
  @param[in]  Attributes    Its attributes.
**/
STATIC
VOID
DescribeAperture (
  IN CONST REBIOS_SANDBOX_RANGE   *Range,
  IN EFI_RESOURCE_TYPE            ResourceType,
  IN EFI_RESOURCE_ATTRIBUTE_TYPE  Attributes
  )
{
  if (Range->Size != 0) {
    BuildResourceDescriptorHob (ResourceType, Attributes, Range->Base, Range->Size);
  }
}

/**
  The entry point ReBIOS starts the firmware at, in 64-bit mode, with paging
  identity-mapping the sandbox and interrupts off.

  @param[in]  Handoff  What ReBIOS gives the firmware.
**/
VOID
EFIAPI
_ModuleEntryPoint (
  IN REBIOS_SANDBOX_HANDOFF  *Handoff
  )
{
  EFI_STATUS            Status;
  UINT32                MaxExtendedLeaf;
  UINT32                AddressSizes;
  UINT8                 PhysicalAddressBits;
  EFI_PHYSICAL_ADDRESS  DxeCoreEntryPoint;
  VOID                  *Stack;
  VOID                  *TopOfStack;

  if ((Handoff->Signature != REBIOS_SANDBOX_HANDOFF_SIGNATURE) ||
      (Handoff->Version != REBIOS_SANDBOX_HANDOFF_VERSION) ||
      (Handoff->Size < sizeof (REBIOS_SANDBOX_HANDOFF)))
  {
    Fail (EFI_INCOMPATIBLE_VERSION);
  }

  InitializeFloatingPointUnits ();

  HobConstructor (
    (VOID *)(UINTN)Handoff->Memory.Base,
    (VOID *)(UINTN)(Handoff->Memory.Base + Handoff->Memory.Size),
    (VOID *)(UINTN)Handoff->FreeMemory.Base,
    (VOID *)(UINTN)(Handoff->FreeMemory.Base + Handoff->FreeMemory.Size)
    );

  DEBUG ((
    DEBUG_INFO,
    "ReBIOS sandbox: memory 0x%lx-0x%lx, task %u\n",
    Handoff->Memory.Base,
    Handoff->Memory.Base + Handoff->Memory.Size - 1,
    Handoff->Task
    ));

  BuildResourceDescriptorHob (
    EFI_RESOURCE_SYSTEM_MEMORY,
    SYSTEM_MEMORY_ATTRIBUTES,
    Handoff->Memory.Base,
    Handoff->Memory.Size
    );
  ReserveRange (&Handoff->FirmwareVolume, EfiBootServicesCode);
  ReserveRange (&Handoff->Stack, EfiBootServicesData);
  ReserveRange (&Handoff->Tables, EfiBootServicesData);
  ReserveRange (&Handoff->GopImage, EfiBootServicesData);
  ReserveRange (&Handoff->Vbt, EfiBootServicesData);
  ReserveRange (&Handoff->Pcds, EfiBootServicesData);
  ReserveRange (&Handoff->Rom, EfiBootServicesData);
  BuildMemoryAllocationHob (
    (UINTN)Handoff & ~(UINTN)EFI_PAGE_MASK,
    ALIGN_VALUE (Handoff->Size + ((UINTN)Handoff & EFI_PAGE_MASK), EFI_PAGE_SIZE),
    EfiBootServicesData
    );

  DescribeAperture (&Handoff->Mem32Aperture, EFI_RESOURCE_MEMORY_MAPPED_IO, MMIO_ATTRIBUTES);
  DescribeAperture (&Handoff->Mem64Aperture, EFI_RESOURCE_MEMORY_MAPPED_IO, MMIO_ATTRIBUTES);
  DescribeAperture (&Handoff->IoAperture, EFI_RESOURCE_IO, IO_ATTRIBUTES);

  AsmCpuid (CPUID_EXTENDED_FUNCTION, &MaxExtendedLeaf, NULL, NULL, NULL);
  PhysicalAddressBits = 36;
  if (MaxExtendedLeaf >= CPUID_VIR_PHY_ADDRESS_SIZE) {
    AsmCpuid (CPUID_VIR_PHY_ADDRESS_SIZE, &AddressSizes, NULL, NULL, NULL);
    PhysicalAddressBits = (UINT8)AddressSizes;
  }

  BuildCpuHob (PhysicalAddressBits, 16);
  BuildFvHob (Handoff->FirmwareVolume.Base, Handoff->FirmwareVolume.Size);
  BuildGuidDataHob (&gReBiosSandboxHandoffHobGuid, Handoff, Handoff->Size);

  Status = LoadDxeCore ((EFI_FIRMWARE_VOLUME_HEADER *)(UINTN)Handoff->FirmwareVolume.Base, &DxeCoreEntryPoint);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ReBIOS sandbox: no DxeCore to load: %r\n", Status));
    Fail (Status);
  }

  Stack = AllocatePages (EFI_SIZE_TO_PAGES (DXE_STACK_SIZE));
  if (Stack == NULL) {
    Fail (EFI_OUT_OF_RESOURCES);
  }

  BuildStackHob ((EFI_PHYSICAL_ADDRESS)(UINTN)Stack, DXE_STACK_SIZE);
  TopOfStack = ALIGN_POINTER ((UINT8 *)Stack + DXE_STACK_SIZE - CPU_STACK_ALIGNMENT, CPU_STACK_ALIGNMENT);

  SwitchStack (
    (SWITCH_STACK_ENTRY_POINT)(UINTN)DxeCoreEntryPoint,
    GetHobList (),
    NULL,
    TopOfStack
    );
}
