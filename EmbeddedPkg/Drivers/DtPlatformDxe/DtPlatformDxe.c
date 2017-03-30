/** @file
*
*  Copyright (c) 2017, Linaro, Ltd. All rights reserved.
*
*  This program and the accompanying materials
*  are licensed and made available under the terms and conditions of the BSD License
*  which accompanies this distribution.  The full text of the license may be found at
*  http://opensource.org/licenses/bsd-license.php
*
*  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
*  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
*
**/

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/HiiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "DtPlatformDxe.h"

extern  UINT8                     DtPlatformHiiBin[];
extern  UINT8                     DtPlatformDxeStrings[];

typedef struct {
  VENDOR_DEVICE_PATH              VendorDevicePath;
  EFI_DEVICE_PATH_PROTOCOL        End;
} HII_VENDOR_DEVICE_PATH;

STATIC HII_VENDOR_DEVICE_PATH     mDtPlatformDxeVendorDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8) (sizeof (VENDOR_DEVICE_PATH)),
        (UINT8) ((sizeof (VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    DT_PLATFORM_FORMSET_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8) (END_DEVICE_PATH_LENGTH),
      (UINT8) ((END_DEVICE_PATH_LENGTH) >> 8)
    }
  }
};

STATIC
EFI_STATUS
InstallHiiPages (
  VOID
  )
{
  EFI_STATUS                      Status;
  EFI_HII_HANDLE                  HiiHandle;
  EFI_HANDLE                      DriverHandle;

  DriverHandle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (&DriverHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mDtPlatformDxeVendorDevicePath,
                  NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  HiiHandle = HiiAddPackages (&gDtPlatformFormSetGuid,
                              DriverHandle,
                              DtPlatformDxeStrings,
                              DtPlatformHiiBin,
                              NULL);

  if (HiiHandle == NULL) {
    gBS->UninstallMultipleProtocolInterfaces (DriverHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mDtPlatformDxeVendorDevicePath,
                  NULL);
    return EFI_OUT_OF_RESOURCES;
  }
  return EFI_SUCCESS;
}

/**
  The entry point for DtPlatformDxe driver.

  @param[in] ImageHandle     The image handle of the driver.
  @param[in] SystemTable     The system table.

  @retval EFI_ALREADY_STARTED     The driver already exists in system.
  @retval EFI_OUT_OF_RESOURCES    Fail to execute entry point due to lack of
                                  resources.
  @retval EFI_SUCCES              All the related protocols are installed on
                                  the driver.

**/
EFI_STATUS
EFIAPI
DtPlatformDxeEntryPoint (
  IN EFI_HANDLE                   ImageHandle,
  IN EFI_SYSTEM_TABLE             *SystemTable
  )
{
  EFI_STATUS                      Status;
  DT_ACPI_VARSTORE_DATA           DtAcpiPref;
  UINTN                           BufferSize;
  VOID                            *Dtb;
  UINTN                           DtbSize;
  VOID                            *DtbCopy;

  //
  // Check whether a DTB blob is included in the firmware image.
  //
  Dtb = NULL;
  Status = GetSectionFromAnyFv (&gDtPlatformDefaultDtbFileGuid,
             EFI_SECTION_RAW, 0, &Dtb, &DtbSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: no DTB blob found, defaulting to ACPI\n",
      __FUNCTION__));
    DtAcpiPref.Pref = DT_ACPI_SELECT_ACPI;
  } else {
    //
    // Get the current DT/ACPI preference from the DtAcpiPref variable.
    //
    BufferSize = sizeof (DtAcpiPref);
    Status = gRT->GetVariable(DT_ACPI_VARIABLE_NAME, &gDtPlatformFormSetGuid,
                    NULL, &BufferSize, &DtAcpiPref);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "%a: no DT/ACPI preference found, defaulting to DT\n",
        __FUNCTION__));
      DtAcpiPref.Pref = DT_ACPI_SELECT_DT;
    }
  }

  if (!EFI_ERROR (Status) &&
      DtAcpiPref.Pref != DT_ACPI_SELECT_ACPI &&
      DtAcpiPref.Pref != DT_ACPI_SELECT_DT) {
    DEBUG ((DEBUG_WARN, "%a: invalid value for %s, defaulting to DT\n",
      __FUNCTION__, DT_ACPI_VARIABLE_NAME));
    DtAcpiPref.Pref = DT_ACPI_SELECT_DT;
    Status = EFI_INVALID_PARAMETER; // trigger setvar below
  }

  //
  // Write the newly selected default value back to the variable store.
  //
  if (EFI_ERROR (Status)) {
    Status = gRT->SetVariable(DT_ACPI_VARIABLE_NAME, &gDtPlatformFormSetGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
                    sizeof (DtAcpiPref), &DtAcpiPref);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (DtAcpiPref.Pref == DT_ACPI_SELECT_ACPI) {
    //
    // ACPI was selected: install the gEdkiiPlatformHasAcpiGuid GUID as a
    // NULL protocol to unlock dispatch of ACPI related drivers.
    //
    Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle,
                    &gEdkiiPlatformHasAcpiGuid, NULL, NULL);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR,
        "%a: failed to install gEdkiiPlatformHasAcpiGuid as a protocol\n",
        __FUNCTION__));
      return Status;
    }
  } else if (DtAcpiPref.Pref == DT_ACPI_SELECT_DT) {
    //
    // DT was selected: copy the blob into newly allocated memory and install
    // a reference to it as the FDT configuration table.
    //
    DtbCopy = AllocateCopyPool (DtbSize, Dtb);
    if (DtbCopy == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    Status = gBS->InstallConfigurationTable (&gFdtTableGuid, DtbCopy);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: failed to install FDT configuration table\n",
        __FUNCTION__));
      FreePool (DtbCopy);
      return Status;
    }
  } else {
    ASSERT (FALSE);
  }

  //
  // No point in installing the HII pages if ACPI is the only description
  // we have
  //
  if (Dtb == NULL) {
    return EFI_SUCCESS;
  }

  //
  // Note that we don't uninstall the gEdkiiPlatformHasAcpiGuid protocol nor
  // the FDT configuration table if the following call fails. While that will
  // cause loading of this driver to fail, proceeding with ACPI and DT both
  // disabled will guarantee a failed boot, and so it is better to leave them
  // installed in that case.
  //
  return InstallHiiPages ();
}