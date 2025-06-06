//////////////////////////////////////////////////////////////////////////////
//
//  Create a process with a DLL (creatwth.cpp of detours.lib)
//
//  Microsoft Research Detours Package, Version 4.0.1
//
//  Copyright (c) Microsoft Corporation.  All rights reserved.
//

#ifdef SOUP_BUILD
module;
#endif

// #define DETOUR_DEBUG 1
#define DETOURS_INTERNAL
#include "detours.h"
#include <stddef.h>

#ifdef SOUP_BUILD
module Detours;
#endif

#if DETOURS_VERSION != 0x4c0c1   // 0xMAJORcMINORcPATCH
#error detours.h version mismatch
#endif

#define IMPORT_DIRECTORY OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
#define BOUND_DIRECTORY OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT]
#define CLR_DIRECTORY OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR]
#define IAT_DIRECTORY OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT]

//////////////////////////////////////////////////////////////////////////////
//
const GUID DETOUR_EXE_HELPER_GUID = { /* ea0251b9-5cde-41b5-98d0-2af4a26b0fee */
    0xea0251b9, 0x5cde, 0x41b5,
    { 0x98, 0xd0, 0x2a, 0xf4, 0xa2, 0x6b, 0x0f, 0xee }};

//////////////////////////////////////////////////////////////////////////////
//
// Enumerate through modules in the target process.
//
static PVOID LoadNtHeaderFromProcess(_In_ HANDLE hProcess,
                                     _In_ HMODULE hModule,
                                     _Out_ PIMAGE_NT_HEADERS32 pNtHeader)
{
    ZeroMemory(pNtHeader, sizeof(*pNtHeader));
    PBYTE pbModule = (PBYTE)hModule;

    if (pbModule == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    MEMORY_BASIC_INFORMATION mbi;
    ZeroMemory(&mbi, sizeof(mbi));

    if (VirtualQueryEx(hProcess, hModule, &mbi, sizeof(mbi)) == 0) {
        return NULL;
    }

    IMAGE_DOS_HEADER idh;
    if (!ReadProcessMemory(hProcess, pbModule, &idh, sizeof(idh), NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(idh@%p..%p) failed: %lu\n",
                      pbModule, pbModule + sizeof(idh), GetLastError()));
        return NULL;
    }

    if (idh.e_magic != IMAGE_DOS_SIGNATURE ||
        (DWORD)idh.e_lfanew > mbi.RegionSize ||
        (DWORD)idh.e_lfanew < sizeof(idh)) {

        SetLastError(ERROR_BAD_EXE_FORMAT);
        return NULL;
    }

    if (!ReadProcessMemory(hProcess, pbModule + idh.e_lfanew,
                           pNtHeader, sizeof(*pNtHeader), NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(inh@%p..%p:%p) failed: %lu\n",
                      pbModule + idh.e_lfanew,
                      pbModule + idh.e_lfanew + sizeof(*pNtHeader),
                      pbModule,
                      GetLastError()));
        return NULL;
    }

    if (pNtHeader->Signature != IMAGE_NT_SIGNATURE) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return NULL;
    }

    return pbModule + idh.e_lfanew;
}

static HMODULE EnumerateModulesInProcess(_In_ HANDLE hProcess,
                                         _In_opt_ HMODULE hModuleLast,
                                         _Out_ PIMAGE_NT_HEADERS32 pNtHeader,
                                         _Out_opt_ PVOID *pRemoteNtHeader)
{
    ZeroMemory(pNtHeader, sizeof(*pNtHeader));
    if (pRemoteNtHeader) {
        *pRemoteNtHeader = NULL;
    }

    PBYTE pbLast = (PBYTE)hModuleLast + MM_ALLOCATION_GRANULARITY;

    MEMORY_BASIC_INFORMATION mbi;
    ZeroMemory(&mbi, sizeof(mbi));

    // Find the next memory region that contains a mapped PE image.
    //

    for (;; pbLast = (PBYTE)mbi.BaseAddress + mbi.RegionSize) {
        if (VirtualQueryEx(hProcess, (PVOID)pbLast, &mbi, sizeof(mbi)) == 0) {
            break;
        }

        // Usermode address space has such an unaligned region size always at the
        // end and only at the end.
        //
        if ((mbi.RegionSize & 0xfff) == 0xfff) {
            break;
        }
        if (((PBYTE)mbi.BaseAddress + mbi.RegionSize) < pbLast) {
            break;
        }

        // Skip uncommitted regions and guard pages.
        //
        if ((mbi.State != MEM_COMMIT) ||
            ((mbi.Protect & 0xff) == PAGE_NOACCESS) ||
            (mbi.Protect & PAGE_GUARD)) {
            continue;
        }

        PVOID remoteHeader
            = LoadNtHeaderFromProcess(hProcess, (HMODULE)pbLast, pNtHeader);
        if (remoteHeader) {
            if (pRemoteNtHeader) {
                *pRemoteNtHeader = remoteHeader;
            }

            return (HMODULE)pbLast;
        }
    }
    return NULL;
}

//////////////////////////////////////////////////////////////////////////////
//
// Find payloads in target process.
//

static PVOID FindDetourSectionInRemoteModule(_In_ HANDLE hProcess,
                                             _In_ HMODULE hModule,
                                             _In_ const IMAGE_NT_HEADERS32 *pNtHeader,
                                             _In_ PVOID pRemoteNtHeader)
{
    if (pNtHeader->FileHeader.SizeOfOptionalHeader == 0) {
        SetLastError(ERROR_EXE_MARKED_INVALID);
        return NULL;
    }

    PIMAGE_SECTION_HEADER pRemoteSectionHeaders
        = (PIMAGE_SECTION_HEADER)((PBYTE)pRemoteNtHeader
                                  + sizeof(pNtHeader->Signature)
                                  + sizeof(pNtHeader->FileHeader)
                                  + pNtHeader->FileHeader.SizeOfOptionalHeader);

    IMAGE_SECTION_HEADER header;
    for (DWORD n = 0; n < pNtHeader->FileHeader.NumberOfSections; ++n) {
        if (!ReadProcessMemory(hProcess, pRemoteSectionHeaders + n, &header, sizeof(header), NULL)) {
            DETOUR_TRACE(("ReadProcessMemory(ish@%p..%p) failed: %lu\n",
                pRemoteSectionHeaders + n,
                (PBYTE)(pRemoteSectionHeaders + n) + sizeof(header),
                GetLastError()));

            return NULL;
        }

        if (strcmp((PCHAR)header.Name, ".detour") == 0) {
            if (header.VirtualAddress == 0 ||
                header.SizeOfRawData == 0) {

                break;
            }

            SetLastError(NO_ERROR);
            return (PBYTE)hModule + header.VirtualAddress;
        }
    }

    SetLastError(ERROR_EXE_MARKED_INVALID);
    return NULL;
}

static PVOID FindPayloadInRemoteDetourSection(_In_ HANDLE hProcess,
                                               _In_ REFGUID rguid,
                                               _Out_opt_ DWORD *pcbData,
                                               _In_ PVOID pvRemoteDetoursSection)
{
    if (pcbData) {
        *pcbData = 0;
    }

    PBYTE pbData = (PBYTE)pvRemoteDetoursSection;

    DETOUR_SECTION_HEADER header;
    if (!ReadProcessMemory(hProcess, pbData, &header, sizeof(header), NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(dsh@%p..%p) failed: %lu\n",
            pbData,
            pbData + sizeof(header),
            GetLastError()));
        return NULL;
    }

    if (header.cbHeaderSize < sizeof(DETOUR_SECTION_HEADER) ||
        header.nSignature != DETOUR_SECTION_HEADER_SIGNATURE) {
        SetLastError(ERROR_EXE_MARKED_INVALID);
        return NULL;
    }

    if (header.nDataOffset == 0) {
        header.nDataOffset = header.cbHeaderSize;
    }

    for (PVOID pvSection = pbData + header.nDataOffset; pvSection < pbData + header.cbDataSize;) {
        DETOUR_SECTION_RECORD section;
        if (!ReadProcessMemory(hProcess, pvSection, &section, sizeof(section), NULL)) {
            DETOUR_TRACE(("ReadProcessMemory(dsr@%p..%p) failed: %lu\n",
                pvSection,
                (PBYTE)pvSection + sizeof(section),
                GetLastError()));
            return NULL;
        }

        if (DetourAreSameGuid(section.guid, rguid)) {
            if (pcbData) {
                *pcbData = section.cbBytes - sizeof(section);
            }
            SetLastError(NO_ERROR);
            return (DETOUR_SECTION_RECORD *)pvSection + 1;
        }

        pvSection = (PBYTE)pvSection + section.cbBytes;
    }

    return NULL;
}

_Success_(return != NULL)
PVOID WINAPI DetourFindRemotePayload(_In_ HANDLE hProcess,
                                     _In_ REFGUID rguid,
                                     _Out_opt_ DWORD *pcbData)
{
    if (hProcess == NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }

    IMAGE_NT_HEADERS32 header;
    PVOID pvRemoteHeader;
    for (HMODULE hMod = NULL; (hMod = EnumerateModulesInProcess(hProcess, hMod, &header, &pvRemoteHeader)) != NULL;) {
        PVOID pvData = FindDetourSectionInRemoteModule(hProcess, hMod, &header, pvRemoteHeader);
        if (pvData != NULL) {
            pvData = FindPayloadInRemoteDetourSection(hProcess, rguid, pcbData, pvData);
            if (pvData != NULL) {
                return pvData;
            }
        }
    }

    SetLastError(ERROR_MOD_NOT_FOUND);
    return NULL;
}

//////////////////////////////////////////////////////////////////////////////
//
// Find a region of memory in which we can create a replacement import table.
//
static PBYTE FindAndAllocateNearBase(HANDLE hProcess, PBYTE pbModule, PBYTE pbBase, DWORD cbAlloc)
{
    MEMORY_BASIC_INFORMATION mbi;
    ZeroMemory(&mbi, sizeof(mbi));

    PBYTE pbLast = pbBase;
    for (;; pbLast = (PBYTE)mbi.BaseAddress + mbi.RegionSize) {

        ZeroMemory(&mbi, sizeof(mbi));
        if (VirtualQueryEx(hProcess, (PVOID)pbLast, &mbi, sizeof(mbi)) == 0) {
            if (GetLastError() == ERROR_INVALID_PARAMETER) {
                break;
            }
            DETOUR_TRACE(("VirtualQueryEx(%p) failed: %lu\n",
                          pbLast, GetLastError()));
            break;
        }
        // Usermode address space has such an unaligned region size always at the
        // end and only at the end.
        //
        if ((mbi.RegionSize & 0xfff) == 0xfff) {
            break;
        }

        // Skip anything other than a pure free region.
        //
        if (mbi.State != MEM_FREE) {
            continue;
        }

        // Use the max of mbi.BaseAddress and pbBase, in case mbi.BaseAddress < pbBase.
        PBYTE pbAddress = (PBYTE)mbi.BaseAddress > pbBase ? (PBYTE)mbi.BaseAddress : pbBase;

        // Round pbAddress up to the nearest MM allocation boundary.
        const DWORD_PTR mmGranularityMinusOne = (DWORD_PTR)(MM_ALLOCATION_GRANULARITY -1);
        pbAddress = (PBYTE)(((DWORD_PTR)pbAddress + mmGranularityMinusOne) & ~mmGranularityMinusOne);

#ifdef _WIN64
        // The offset from pbModule to any replacement import must fit into 32 bits.
        // For simplicity, we check that the offset to the last byte fits into 32 bits,
        // instead of the largest offset we'll actually use. The values are very similar.
        const size_t GB4 = ((((size_t)1) << 32) - 1);
        if ((size_t)(pbAddress + cbAlloc - 1 - pbModule) > GB4) {
            DETOUR_TRACE(("FindAndAllocateNearBase(1) failing due to distance >4GB %p\n", pbAddress));
            return NULL;
        }
#else
        UNREFERENCED_PARAMETER(pbModule);
#endif

        DETOUR_TRACE(("Free region %p..%p\n",
                      mbi.BaseAddress,
                      (PBYTE)mbi.BaseAddress + mbi.RegionSize));

        for (; pbAddress < (PBYTE)mbi.BaseAddress + mbi.RegionSize; pbAddress += MM_ALLOCATION_GRANULARITY) {
            PBYTE pbAlloc = (PBYTE)VirtualAllocEx(hProcess, pbAddress, cbAlloc,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (pbAlloc == NULL) {
                DETOUR_TRACE(("VirtualAllocEx(%p) failed: %lu\n", pbAddress, GetLastError()));
                continue;
            }
#ifdef _WIN64
            // The offset from pbModule to any replacement import must fit into 32 bits.
            if ((size_t)(pbAddress + cbAlloc - 1 - pbModule) > GB4) {
                DETOUR_TRACE(("FindAndAllocateNearBase(2) failing due to distance >4GB %p\n", pbAddress));
                return NULL;
            }
#endif
            DETOUR_TRACE(("[%p..%p] Allocated for import table.\n",
                          pbAlloc, pbAlloc + cbAlloc));
            return pbAlloc;
        }
    }
    return NULL;
}

static inline DWORD PadToDword(DWORD dw)
{
    return (dw + 3) & ~3u;
}

static inline DWORD PadToDwordPtr(DWORD dw)
{
    return (dw + 7) & ~7u;
}

static inline HRESULT ReplaceOptionalSizeA(_Inout_z_count_(cchDest) LPSTR pszDest,
                                           _In_ size_t cchDest,
                                           _In_z_ LPCSTR pszSize)
{
    if (cchDest == 0 || pszDest == NULL || pszSize == NULL ||
        pszSize[0] == '\0' || pszSize[1] == '\0' || pszSize[2] != '\0') {

        // can not write into empty buffer or with string other than two chars.
        return ERROR_INVALID_PARAMETER;
    }

    for (; cchDest >= 2; cchDest--, pszDest++) {
        if (pszDest[0] == '?' && pszDest[1] == '?') {
            pszDest[0] = pszSize[0];
            pszDest[1] = pszSize[1];
            break;
        }
    }

    return S_OK;
}

static BOOL RecordExeRestore(HANDLE hProcess, HMODULE hModule, DETOUR_EXE_RESTORE& der)
{
    // Save the various headers for DetourRestoreAfterWith.
    ZeroMemory(&der, sizeof(der));
    der.cb = sizeof(der);

    der.pidh = (PBYTE)hModule;
    der.cbidh = sizeof(der.idh);
    if (!ReadProcessMemory(hProcess, der.pidh, &der.idh, sizeof(der.idh), NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(idh@%p..%p) failed: %lu\n",
                      der.pidh, der.pidh + der.cbidh, GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("IDH: %p..%p\n", der.pidh, der.pidh + der.cbidh));

    // We read the NT header in two passes to get the full size.
    // First we read just the Signature and FileHeader.
    der.pinh = der.pidh + der.idh.e_lfanew;
    der.cbinh = FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader);
    if (!ReadProcessMemory(hProcess, der.pinh, &der.inh, der.cbinh, NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(inh@%p..%p) failed: %lu\n",
                      der.pinh, der.pinh + der.cbinh, GetLastError()));
        return FALSE;
    }

    // Second we read the OptionalHeader and Section headers.
    der.cbinh = (FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) +
                 der.inh.FileHeader.SizeOfOptionalHeader +
                 der.inh.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER));

    if (der.cbinh > sizeof(der.raw)) {
        return FALSE;
    }

    if (!ReadProcessMemory(hProcess, der.pinh, &der.inh, der.cbinh, NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(inh@%p..%p) failed: %lu\n",
                      der.pinh, der.pinh + der.cbinh, GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("INH: %p..%p\n", der.pinh, der.pinh + der.cbinh));

    // Third, we read the CLR header

    if (der.inh.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        if (der.inh32.CLR_DIRECTORY.VirtualAddress != 0 &&
            der.inh32.CLR_DIRECTORY.Size != 0) {

            DETOUR_TRACE(("CLR32.VirtAddr=%08lx, CLR.Size=%lu\n",
                          der.inh32.CLR_DIRECTORY.VirtualAddress,
                          der.inh32.CLR_DIRECTORY.Size));

            der.pclr = ((PBYTE)hModule) + der.inh32.CLR_DIRECTORY.VirtualAddress;
        }
    }
    else if (der.inh.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (der.inh64.CLR_DIRECTORY.VirtualAddress != 0 &&
            der.inh64.CLR_DIRECTORY.Size != 0) {

            DETOUR_TRACE(("CLR64.VirtAddr=%08lx, CLR.Size=%lu\n",
                          der.inh64.CLR_DIRECTORY.VirtualAddress,
                          der.inh64.CLR_DIRECTORY.Size));

            der.pclr = ((PBYTE)hModule) + der.inh64.CLR_DIRECTORY.VirtualAddress;
        }
    }

    if (der.pclr != 0) {
        der.cbclr = sizeof(der.clr);
        if (!ReadProcessMemory(hProcess, der.pclr, &der.clr, der.cbclr, NULL)) {
            DETOUR_TRACE(("ReadProcessMemory(clr@%p..%p) failed: %lu\n",
                          der.pclr, der.pclr + der.cbclr, GetLastError()));
            return FALSE;
        }
        DETOUR_TRACE(("CLR: %p..%p\n", der.pclr, der.pclr + der.cbclr));
    }

    return TRUE;
}

//////////////////////////////////////////////////////////////////////////////
//
#if DETOURS_32BIT
#define DWORD_XX                        DWORD32
#define IMAGE_NT_HEADERS_XX             IMAGE_NT_HEADERS32
#define IMAGE_NT_OPTIONAL_HDR_MAGIC_XX  IMAGE_NT_OPTIONAL_HDR32_MAGIC
#define IMAGE_ORDINAL_FLAG_XX           IMAGE_ORDINAL_FLAG32
#define IMAGE_THUNK_DATAXX              IMAGE_THUNK_DATA32
#define UPDATE_IMPORTS_XX               UpdateImports32
#define DETOURS_BITS_XX                 32
#include "uimports.cpp"
#undef DETOUR_EXE_RESTORE_FIELD_XX
#undef DWORD_XX
#undef IMAGE_NT_HEADERS_XX
#undef IMAGE_NT_OPTIONAL_HDR_MAGIC_XX
#undef IMAGE_ORDINAL_FLAG_XX
#undef UPDATE_IMPORTS_XX
#endif // DETOURS_32BIT

#if DETOURS_64BIT
#define DWORD_XX                        DWORD64
#define IMAGE_NT_HEADERS_XX             IMAGE_NT_HEADERS64
#define IMAGE_NT_OPTIONAL_HDR_MAGIC_XX  IMAGE_NT_OPTIONAL_HDR64_MAGIC
#define IMAGE_ORDINAL_FLAG_XX           IMAGE_ORDINAL_FLAG64
#define IMAGE_THUNK_DATAXX              IMAGE_THUNK_DATA64
#define UPDATE_IMPORTS_XX               UpdateImports64
#define DETOURS_BITS_XX                 64
#include "uimports.cpp"
#undef DETOUR_EXE_RESTORE_FIELD_XX
#undef DWORD_XX
#undef IMAGE_NT_HEADERS_XX
#undef IMAGE_NT_OPTIONAL_HDR_MAGIC_XX
#undef IMAGE_ORDINAL_FLAG_XX
#undef UPDATE_IMPORTS_XX
#endif // DETOURS_64BIT

//////////////////////////////////////////////////////////////////////////////
//
#if DETOURS_64BIT

C_ASSERT(sizeof(IMAGE_NT_HEADERS64) == sizeof(IMAGE_NT_HEADERS32) + 16);

static BOOL UpdateFrom32To64(HANDLE hProcess, HMODULE hModule, WORD machine,
                             DETOUR_EXE_RESTORE& der)
{
    IMAGE_DOS_HEADER idh;
    IMAGE_NT_HEADERS32 inh32;
    IMAGE_NT_HEADERS64 inh64;
    IMAGE_SECTION_HEADER sects[32];
    PBYTE pbModule = (PBYTE)hModule;
    DWORD n;

    ZeroMemory(&inh32, sizeof(inh32));
    ZeroMemory(&inh64, sizeof(inh64));
    ZeroMemory(sects, sizeof(sects));

    DETOUR_TRACE(("UpdateFrom32To64(%04x)\n", machine));
    //////////////////////////////////////////////////////// Read old headers.
    //
    if (!ReadProcessMemory(hProcess, pbModule, &idh, sizeof(idh), NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(idh@%p..%p) failed: %lu\n",
                      pbModule, pbModule + sizeof(idh), GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("ReadProcessMemory(idh@%p..%p)\n",
                  pbModule, pbModule + sizeof(idh)));

    PBYTE pnh = pbModule + idh.e_lfanew;
    if (!ReadProcessMemory(hProcess, pnh, &inh32, sizeof(inh32), NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(inh@%p..%p) failed: %lu\n",
                      pnh, pnh + sizeof(inh32), GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("ReadProcessMemory(inh@%p..%p)\n", pnh, pnh + sizeof(inh32)));

    if (inh32.FileHeader.NumberOfSections > (sizeof(sects)/sizeof(sects[0]))) {
        return FALSE;
    }

    PBYTE psects = pnh +
        FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) +
        inh32.FileHeader.SizeOfOptionalHeader;
    ULONG cb = inh32.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (!ReadProcessMemory(hProcess, psects, &sects, cb, NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(ish@%p..%p) failed: %lu\n",
                      psects, psects + cb, GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("ReadProcessMemory(ish@%p..%p)\n", psects, psects + cb));

    ////////////////////////////////////////////////////////// Convert header.
    //
    inh64.Signature = inh32.Signature;
    inh64.FileHeader = inh32.FileHeader;
    inh64.FileHeader.Machine = machine;
    inh64.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);

    inh64.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    inh64.OptionalHeader.MajorLinkerVersion = inh32.OptionalHeader.MajorLinkerVersion;
    inh64.OptionalHeader.MinorLinkerVersion = inh32.OptionalHeader.MinorLinkerVersion;
    inh64.OptionalHeader.SizeOfCode = inh32.OptionalHeader.SizeOfCode;
    inh64.OptionalHeader.SizeOfInitializedData = inh32.OptionalHeader.SizeOfInitializedData;
    inh64.OptionalHeader.SizeOfUninitializedData = inh32.OptionalHeader.SizeOfUninitializedData;
    inh64.OptionalHeader.AddressOfEntryPoint = inh32.OptionalHeader.AddressOfEntryPoint;
    inh64.OptionalHeader.BaseOfCode = inh32.OptionalHeader.BaseOfCode;
    inh64.OptionalHeader.ImageBase = inh32.OptionalHeader.ImageBase;
    inh64.OptionalHeader.SectionAlignment = inh32.OptionalHeader.SectionAlignment;
    inh64.OptionalHeader.FileAlignment = inh32.OptionalHeader.FileAlignment;
    inh64.OptionalHeader.MajorOperatingSystemVersion
        = inh32.OptionalHeader.MajorOperatingSystemVersion;
    inh64.OptionalHeader.MinorOperatingSystemVersion
        = inh32.OptionalHeader.MinorOperatingSystemVersion;
    inh64.OptionalHeader.MajorImageVersion = inh32.OptionalHeader.MajorImageVersion;
    inh64.OptionalHeader.MinorImageVersion = inh32.OptionalHeader.MinorImageVersion;
    inh64.OptionalHeader.MajorSubsystemVersion = inh32.OptionalHeader.MajorSubsystemVersion;
    inh64.OptionalHeader.MinorSubsystemVersion = inh32.OptionalHeader.MinorSubsystemVersion;
    inh64.OptionalHeader.Win32VersionValue = inh32.OptionalHeader.Win32VersionValue;
    inh64.OptionalHeader.SizeOfImage = inh32.OptionalHeader.SizeOfImage;
    inh64.OptionalHeader.SizeOfHeaders = inh32.OptionalHeader.SizeOfHeaders;
    inh64.OptionalHeader.CheckSum = inh32.OptionalHeader.CheckSum;
    inh64.OptionalHeader.Subsystem = inh32.OptionalHeader.Subsystem;
    inh64.OptionalHeader.DllCharacteristics = inh32.OptionalHeader.DllCharacteristics;
    inh64.OptionalHeader.SizeOfStackReserve = inh32.OptionalHeader.SizeOfStackReserve;
    inh64.OptionalHeader.SizeOfStackCommit = inh32.OptionalHeader.SizeOfStackCommit;
    inh64.OptionalHeader.SizeOfHeapReserve = inh32.OptionalHeader.SizeOfHeapReserve;
    inh64.OptionalHeader.SizeOfHeapCommit = inh32.OptionalHeader.SizeOfHeapCommit;
    inh64.OptionalHeader.LoaderFlags = inh32.OptionalHeader.LoaderFlags;
    inh64.OptionalHeader.NumberOfRvaAndSizes = inh32.OptionalHeader.NumberOfRvaAndSizes;
    for (n = 0; n < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; n++) {
        inh64.OptionalHeader.DataDirectory[n] = inh32.OptionalHeader.DataDirectory[n];
    }

    /////////////////////////////////////////////////////// Write new headers.
    //
    DWORD dwProtect = 0;
    if (!DetourVirtualProtectSameExecuteEx(hProcess, pbModule, inh64.OptionalHeader.SizeOfHeaders,
                                           PAGE_EXECUTE_READWRITE, &dwProtect)) {
        return FALSE;
    }

    if (!WriteProcessMemory(hProcess, pnh, &inh64, sizeof(inh64), NULL)) {
        DETOUR_TRACE(("WriteProcessMemory(inh@%p..%p) failed: %lu\n",
                      pnh, pnh + sizeof(inh64), GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("WriteProcessMemory(inh@%p..%p)\n", pnh, pnh + sizeof(inh64)));

    psects = pnh +
        FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) +
        inh64.FileHeader.SizeOfOptionalHeader;
    cb = inh64.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (!WriteProcessMemory(hProcess, psects, &sects, cb, NULL)) {
        DETOUR_TRACE(("WriteProcessMemory(ish@%p..%p) failed: %lu\n",
                      psects, psects + cb, GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("WriteProcessMemory(ish@%p..%p)\n", psects, psects + cb));

    // Record the updated headers.
    if (!RecordExeRestore(hProcess, hModule, der)) {
        return FALSE;
    }

    // Remove the import table.
    if (der.pclr != NULL && (der.clr.Flags & COMIMAGE_FLAGS_ILONLY)) {
        inh64.IMPORT_DIRECTORY.VirtualAddress = 0;
        inh64.IMPORT_DIRECTORY.Size = 0;

        if (!WriteProcessMemory(hProcess, pnh, &inh64, sizeof(inh64), NULL)) {
            DETOUR_TRACE(("WriteProcessMemory(inh@%p..%p) failed: %lu\n",
                          pnh, pnh + sizeof(inh64), GetLastError()));
            return FALSE;
        }
    }

    DWORD dwOld = 0;
    if (!VirtualProtectEx(hProcess, pbModule, inh64.OptionalHeader.SizeOfHeaders,
                          dwProtect, &dwOld)) {
        return FALSE;
    }

    return TRUE;
}
#endif // DETOURS_64BIT

typedef BOOL(WINAPI *LPFN_ISWOW64PROCESS)(HANDLE, PBOOL);

static BOOL IsWow64ProcessHelper(HANDLE hProcess,
                                 PBOOL Wow64Process)
{
#ifdef _X86_
    if (Wow64Process == NULL) {
        return FALSE;
    }

    // IsWow64Process is not available on all supported versions of Windows.
    //
    HMODULE hKernel32 = LoadLibraryW(L"KERNEL32.DLL");
    if (hKernel32 == NULL) {
        DETOUR_TRACE(("LoadLibraryW failed: %lu\n", GetLastError()));
        return FALSE;
    }

    LPFN_ISWOW64PROCESS pfnIsWow64Process = (LPFN_ISWOW64PROCESS)GetProcAddress(
        hKernel32, "IsWow64Process");

    if (pfnIsWow64Process == NULL) {
        DETOUR_TRACE(("GetProcAddress failed: %lu\n", GetLastError()));
        return FALSE;
    }
    return pfnIsWow64Process(hProcess, Wow64Process);
#else
    return IsWow64Process(hProcess, Wow64Process);
#endif
}

//////////////////////////////////////////////////////////////////////////////
//
BOOL WINAPI DetourUpdateProcessWithDll(_In_ HANDLE hProcess,
                                       _In_reads_(nDlls) LPCSTR *rlpDlls,
                                       _In_ DWORD nDlls)
{
    // Find the next memory region that contains a mapped PE image.
    //
    BOOL bIs32BitProcess;
    BOOL bIs64BitOS = FALSE;
    HMODULE hModule = NULL;
    HMODULE hLast = NULL;

    DETOUR_TRACE(("DetourUpdateProcessWithDll(%p,dlls=%lu)\n", hProcess, nDlls));

    for (;;) {
        IMAGE_NT_HEADERS32 inh;

        if ((hLast = EnumerateModulesInProcess(hProcess, hLast, &inh, NULL)) == NULL) {
            break;
        }

        DETOUR_TRACE(("%p  machine=%04x magic=%04x\n",
                      hLast, inh.FileHeader.Machine, inh.OptionalHeader.Magic));

        if ((inh.FileHeader.Characteristics & IMAGE_FILE_DLL) == 0) {
            hModule = hLast;
            DETOUR_TRACE(("%p  Found EXE\n", hLast));
        }
    }

    if (hModule == NULL) {
        SetLastError(ERROR_INVALID_OPERATION);
        return FALSE;
    }

    // Determine if the target process is 32bit or 64bit. This is a two-stop process:
    //
    // 1. First, determine if we're running on a 64bit operating system.
    //   - If we're running 64bit code (i.e. _WIN64 is defined), this is trivially true.
    //   - If we're running 32bit code (i.e. _WIN64 is not defined), test if
    //   we're running under Wow64. If so, it implies that the operating system
    //   is 64bit.
    //
#ifdef _WIN64
    bIs64BitOS = TRUE;
#else
    if (!IsWow64ProcessHelper(GetCurrentProcess(), &bIs64BitOS)) {
        return FALSE;
    }
#endif

    // 2. With the operating system bitness known, we can now consider the target process:
    //   - If we're running on a 64bit OS, the target process is 32bit in case
    //   it is running under Wow64. Otherwise, it's 64bit, running natively
    //   (without Wow64).
    //   - If we're running on a 32bit OS, the target process must be 32bit, too.
    //
    if (bIs64BitOS) {
        if (!IsWow64ProcessHelper(hProcess, &bIs32BitProcess)) {
            return FALSE;
        }
    } else {
        bIs32BitProcess = TRUE;
    }

    DETOUR_TRACE(("    32BitProcess=%d\n", bIs32BitProcess));

    return DetourUpdateProcessWithDllEx(hProcess,
                                        hModule,
                                        bIs32BitProcess,
                                        rlpDlls,
                                        nDlls);
}

BOOL WINAPI DetourUpdateProcessWithDllEx(_In_ HANDLE hProcess,
                                         _In_ HMODULE hModule,
                                         _In_ BOOL bIs32BitProcess,
                                         _In_reads_(nDlls) LPCSTR *rlpDlls,
                                         _In_ DWORD nDlls)
{
    // Find the next memory region that contains a mapped PE image.
    //
    BOOL bIs32BitExe = FALSE;

    DETOUR_TRACE(("DetourUpdateProcessWithDllEx(%p,%p,dlls=%lu)\n", hProcess, hModule, nDlls));

    IMAGE_NT_HEADERS32 inh;

    if (hModule == NULL || !LoadNtHeaderFromProcess(hProcess, hModule, &inh)) {
        SetLastError(ERROR_INVALID_OPERATION);
        return FALSE;
    }

    if (inh.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC
        && inh.FileHeader.Machine != 0) {

        bIs32BitExe = TRUE;
    }

    DETOUR_TRACE(("    32BitExe=%d\n", bIs32BitExe));

    if (hModule == NULL) {
        SetLastError(ERROR_INVALID_OPERATION);
        return FALSE;
    }

    // Save the various headers for DetourRestoreAfterWith.
    //
    DETOUR_EXE_RESTORE der;

    if (!RecordExeRestore(hProcess, hModule, der)) {
        return FALSE;
    }

#if defined(DETOURS_64BIT)
    // Try to convert a neutral 32-bit managed binary to a 64-bit managed binary.
    if (bIs32BitExe && !bIs32BitProcess) {
        if (!der.pclr                       // Native binary
            || (der.clr.Flags & COMIMAGE_FLAGS_ILONLY) == 0     // Or mixed-mode MSIL
            || (der.clr.Flags & COMIMAGE_FLAGS_32BITREQUIRED) != 0) {  // Or 32BIT Required MSIL

            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }

        if (!UpdateFrom32To64(hProcess, hModule,
#if defined(DETOURS_X64)
                              IMAGE_FILE_MACHINE_AMD64,
#elif defined(DETOURS_IA64)
                              IMAGE_FILE_MACHINE_IA64,
#elif defined(DETOURS_ARM64)
                              IMAGE_FILE_MACHINE_ARM64,
#else
#error Must define one of DETOURS_X64 or DETOURS_IA64 or DETOURS_ARM64 on 64-bit.
#endif
                              der)) {
            return FALSE;
        }
        bIs32BitExe = FALSE;
    }
#endif // DETOURS_64BIT

    // Now decide if we can insert the detour.

#if defined(DETOURS_32BIT)
    if (bIs32BitProcess) {
        // 32-bit native or 32-bit managed process on any platform.
        if (!UpdateImports32(hProcess, hModule, rlpDlls, nDlls)) {
            return FALSE;
        }
    }
    else {
        // 64-bit native or 64-bit managed process.
        //
        // Can't detour a 64-bit process with 32-bit code.
        // Note: This happens for 32-bit PE binaries containing only
        // manage code that have been marked as 64-bit ready.
        //
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
#elif defined(DETOURS_64BIT)
    if (bIs32BitProcess || bIs32BitExe) {
        // Can't detour a 32-bit process with 64-bit code.
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    else {
        // 64-bit native or 64-bit managed process on any platform.
        if (!UpdateImports64(hProcess, hModule, rlpDlls, nDlls)) {
            return FALSE;
        }
    }
#else
#pragma Must define one of DETOURS_32BIT or DETOURS_64BIT.
#endif // DETOURS_64BIT

    /////////////////////////////////////////////////// Update the CLR header.
    //
    if (der.pclr != NULL) {
        DETOUR_CLR_HEADER clr;
        CopyMemory(&clr, &der.clr, sizeof(clr));
        clr.Flags &= ~COMIMAGE_FLAGS_ILONLY;    // Clear the IL_ONLY flag.

        DWORD dwProtect;
        if (!DetourVirtualProtectSameExecuteEx(hProcess, der.pclr, sizeof(clr), PAGE_READWRITE, &dwProtect)) {
            DETOUR_TRACE(("VirtualProtectEx(clr) write failed: %lu\n", GetLastError()));
            return FALSE;
        }

        if (!WriteProcessMemory(hProcess, der.pclr, &clr, sizeof(clr), NULL)) {
            DETOUR_TRACE(("WriteProcessMemory(clr) failed: %lu\n", GetLastError()));
            return FALSE;
        }

        if (!VirtualProtectEx(hProcess, der.pclr, sizeof(clr), dwProtect, &dwProtect)) {
            DETOUR_TRACE(("VirtualProtectEx(clr) restore failed: %lu\n", GetLastError()));
            return FALSE;
        }
        DETOUR_TRACE(("CLR: %p..%p\n", der.pclr, der.pclr + der.cbclr));

#if DETOURS_64BIT
        if (der.clr.Flags & COMIMAGE_FLAGS_32BITREQUIRED) { // Is the 32BIT Required Flag set?
            // X64 never gets here because the process appears as a WOW64 process.
            // However, on IA64, it doesn't appear to be a WOW process.
            DETOUR_TRACE(("CLR Requires 32-bit\n"));
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
#endif // DETOURS_64BIT
    }

    //////////////////////////////// Save the undo data to the target process.
    //
    if (!DetourCopyPayloadToProcess(hProcess, DETOUR_EXE_RESTORE_GUID, &der, sizeof(der))) {
        DETOUR_TRACE(("DetourCopyPayloadToProcess failed: %lu\n", GetLastError()));
        return FALSE;
    }
    return TRUE;
}

//////////////////////////////////////////////////////////////////////////////
//
BOOL WINAPI DetourCreateProcessWithDllA(_In_opt_ LPCSTR lpApplicationName,
                                        _Inout_opt_ LPSTR lpCommandLine,
                                        _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                        _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                        _In_ BOOL bInheritHandles,
                                        _In_ DWORD dwCreationFlags,
                                        _In_opt_ LPVOID lpEnvironment,
                                        _In_opt_ LPCSTR lpCurrentDirectory,
                                        _In_ LPSTARTUPINFOA lpStartupInfo,
                                        _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                        _In_ LPCSTR lpDllName,
                                        _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA)
{
    DWORD dwMyCreationFlags = (dwCreationFlags | CREATE_SUSPENDED);
    PROCESS_INFORMATION pi;
    BOOL fResult = FALSE;

    if (pfCreateProcessA == NULL) {
        pfCreateProcessA = CreateProcessA;
    }

    fResult = pfCreateProcessA(lpApplicationName,
                               lpCommandLine,
                               lpProcessAttributes,
                               lpThreadAttributes,
                               bInheritHandles,
                               dwMyCreationFlags,
                               lpEnvironment,
                               lpCurrentDirectory,
                               lpStartupInfo,
                               &pi);

    if (lpProcessInformation != NULL) {
        CopyMemory(lpProcessInformation, &pi, sizeof(pi));
    }

    if (!fResult) {
        return FALSE;
    }

    LPCSTR rlpDlls[2];
    DWORD nDlls = 0;
    if (lpDllName != NULL) {
        rlpDlls[nDlls++] = lpDllName;
    }

    if (!DetourUpdateProcessWithDll(pi.hProcess, rlpDlls, nDlls)) {
        TerminateProcess(pi.hProcess, ~0u);
        return FALSE;
    }

    if (!(dwCreationFlags & CREATE_SUSPENDED)) {
        ResumeThread(pi.hThread);
    }
    return TRUE;
}


BOOL WINAPI DetourCreateProcessWithDllW(_In_opt_ LPCWSTR lpApplicationName,
                                        _Inout_opt_ LPWSTR lpCommandLine,
                                        _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                        _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                        _In_ BOOL bInheritHandles,
                                        _In_ DWORD dwCreationFlags,
                                        _In_opt_ LPVOID lpEnvironment,
                                        _In_opt_ LPCWSTR lpCurrentDirectory,
                                        _In_ LPSTARTUPINFOW lpStartupInfo,
                                        _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                        _In_ LPCSTR lpDllName,
                                        _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW)
{
    DWORD dwMyCreationFlags = (dwCreationFlags | CREATE_SUSPENDED);
    PROCESS_INFORMATION pi;

    if (pfCreateProcessW == NULL) {
        pfCreateProcessW = CreateProcessW;
    }

    BOOL fResult = pfCreateProcessW(lpApplicationName,
                                    lpCommandLine,
                                    lpProcessAttributes,
                                    lpThreadAttributes,
                                    bInheritHandles,
                                    dwMyCreationFlags,
                                    lpEnvironment,
                                    lpCurrentDirectory,
                                    lpStartupInfo,
                                    &pi);

    if (lpProcessInformation) {
        CopyMemory(lpProcessInformation, &pi, sizeof(pi));
    }

    if (!fResult) {
        return FALSE;
    }

    LPCSTR rlpDlls[2];
    DWORD nDlls = 0;
    if (lpDllName != NULL) {
        rlpDlls[nDlls++] = lpDllName;
    }

    if (!DetourUpdateProcessWithDll(pi.hProcess, rlpDlls, nDlls)) {
        TerminateProcess(pi.hProcess, ~0u);
        return FALSE;
    }

    if (!(dwCreationFlags & CREATE_SUSPENDED)) {
        ResumeThread(pi.hThread);
    }
    return TRUE;
}

BOOL WINAPI DetourCopyPayloadToProcess(_In_ HANDLE hProcess,
                                       _In_ REFGUID rguid,
                                       _In_reads_bytes_(cbData) LPCVOID pvData,
                                       _In_ DWORD cbData)
{
    return DetourCopyPayloadToProcessEx(hProcess, rguid, pvData, cbData) != NULL;
}

_Success_(return != NULL)
PVOID WINAPI DetourCopyPayloadToProcessEx(_In_ HANDLE hProcess,
                                          _In_ REFGUID rguid,
                                          _In_reads_bytes_(cbData) LPCVOID pvData,
                                          _In_ DWORD cbData)
{
    if (hProcess == NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }

    DWORD cbTotal = (sizeof(IMAGE_DOS_HEADER) +
                     sizeof(IMAGE_NT_HEADERS) +
                     sizeof(IMAGE_SECTION_HEADER) +
                     sizeof(DETOUR_SECTION_HEADER) +
                     sizeof(DETOUR_SECTION_RECORD) +
                     cbData);

    PBYTE pbBase = (PBYTE)VirtualAllocEx(hProcess, NULL, cbTotal,
                                         MEM_COMMIT, PAGE_READWRITE);
    if (pbBase == NULL) {
        DETOUR_TRACE(("VirtualAllocEx(%lu) failed: %lu\n", cbTotal, GetLastError()));
        return NULL;
    }

    // As you can see in the following code,
    // the memory layout of the payload range "[pbBase, pbBase+cbTotal]" is a PE executable file,
    // so DetourFreePayload can use "DetourGetContainingModule(Payload pointer)" to get the above "pbBase" pointer,
    // pbBase: the memory block allocated by VirtualAllocEx will be released in DetourFreePayload by VirtualFree.

    PBYTE pbTarget = pbBase;
    IMAGE_DOS_HEADER idh;
    IMAGE_NT_HEADERS inh;
    IMAGE_SECTION_HEADER ish;
    DETOUR_SECTION_HEADER dsh;
    DETOUR_SECTION_RECORD dsr;
    SIZE_T cbWrote = 0;

    ZeroMemory(&idh, sizeof(idh));
    idh.e_magic = IMAGE_DOS_SIGNATURE;
    idh.e_lfanew = sizeof(idh);
    if (!WriteProcessMemory(hProcess, pbTarget, &idh, sizeof(idh), &cbWrote) ||
        cbWrote != sizeof(idh)) {
        DETOUR_TRACE(("WriteProcessMemory(idh) failed: %lu\n", GetLastError()));
        return NULL;
    }
    pbTarget += sizeof(idh);

    ZeroMemory(&inh, sizeof(inh));
    inh.Signature = IMAGE_NT_SIGNATURE;
    inh.FileHeader.SizeOfOptionalHeader = sizeof(inh.OptionalHeader);
    inh.FileHeader.Characteristics = IMAGE_FILE_DLL;
    inh.FileHeader.NumberOfSections = 1;
    inh.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR_MAGIC;
    if (!WriteProcessMemory(hProcess, pbTarget, &inh, sizeof(inh), &cbWrote) ||
        cbWrote != sizeof(inh)) {
        return NULL;
    }
    pbTarget += sizeof(inh);

    ZeroMemory(&ish, sizeof(ish));
    memcpy(ish.Name, ".detour", sizeof(ish.Name));
    ish.VirtualAddress = (DWORD)((pbTarget + sizeof(ish)) - pbBase);
    ish.SizeOfRawData = (sizeof(DETOUR_SECTION_HEADER) +
                         sizeof(DETOUR_SECTION_RECORD) +
                         cbData);
    if (!WriteProcessMemory(hProcess, pbTarget, &ish, sizeof(ish), &cbWrote) ||
        cbWrote != sizeof(ish)) {
        return NULL;
    }
    pbTarget += sizeof(ish);

    ZeroMemory(&dsh, sizeof(dsh));
    dsh.cbHeaderSize = sizeof(dsh);
    dsh.nSignature = DETOUR_SECTION_HEADER_SIGNATURE;
    dsh.nDataOffset = sizeof(DETOUR_SECTION_HEADER);
    dsh.cbDataSize = (sizeof(DETOUR_SECTION_HEADER) +
                      sizeof(DETOUR_SECTION_RECORD) +
                      cbData);
    if (!WriteProcessMemory(hProcess, pbTarget, &dsh, sizeof(dsh), &cbWrote) ||
        cbWrote != sizeof(dsh)) {
        return NULL;
    }
    pbTarget += sizeof(dsh);

    ZeroMemory(&dsr, sizeof(dsr));
    dsr.cbBytes = cbData + sizeof(DETOUR_SECTION_RECORD);
    dsr.nReserved = 0;
    dsr.guid = rguid;
    if (!WriteProcessMemory(hProcess, pbTarget, &dsr, sizeof(dsr), &cbWrote) ||
        cbWrote != sizeof(dsr)) {
        return NULL;
    }
    pbTarget += sizeof(dsr);

    if (!WriteProcessMemory(hProcess, pbTarget, pvData, cbData, &cbWrote) ||
        cbWrote != cbData) {
        return NULL;
    }

    DETOUR_TRACE(("Copied %lu byte payload into target process at %p\n",
                  cbData, pbTarget));
    
    SetLastError(NO_ERROR);
    return pbTarget;
}

static BOOL s_fSearchedForHelper = FALSE;
static PDETOUR_EXE_HELPER s_pHelper = NULL;

VOID CALLBACK DetourFinishHelperProcess(_In_ HWND,
                                        _In_ HINSTANCE,
                                        _In_ LPSTR,
                                        _In_ INT)
{
    LPCSTR * rlpDlls = NULL;
    DWORD Result = 9900;
    DWORD cOffset = 0;
    DWORD cSize = 0;
    HANDLE hProcess = NULL;

    if (s_pHelper == NULL) {
        DETOUR_TRACE(("DetourFinishHelperProcess called with s_pHelper = NULL.\n"));
        Result = 9905;
        goto Cleanup;
    }

    hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, s_pHelper->pid);
    if (hProcess == NULL) {
        DETOUR_TRACE(("OpenProcess(pid=%lu) failed: %lu\n",
                      s_pHelper->pid, GetLastError()));
        Result = 9901;
        goto Cleanup;
    }

    rlpDlls = new NOTHROW LPCSTR [s_pHelper->nDlls];
    cSize = s_pHelper->cb - sizeof(DETOUR_EXE_HELPER);
    for (DWORD n = 0; n < s_pHelper->nDlls; n++) {
        size_t cchDest = 0;
        HRESULT hr = StringCchLengthA(&s_pHelper->rDlls[cOffset], cSize - cOffset, &cchDest);
        if (!SUCCEEDED(hr)) {
            Result = 9902;
            goto Cleanup;
        }

        rlpDlls[n] = &s_pHelper->rDlls[cOffset];
        cOffset += (DWORD)cchDest + 1;
    }

    if (!DetourUpdateProcessWithDll(hProcess, rlpDlls, s_pHelper->nDlls)) {
        DETOUR_TRACE(("DetourUpdateProcessWithDll(pid=%lu) failed: %lu\n",
                      s_pHelper->pid, GetLastError()));
        Result = 9903;
        goto Cleanup;
    }
    Result = 0;

  Cleanup:
    if (rlpDlls != NULL) {
        delete[] rlpDlls;
        rlpDlls = NULL;
    }

    // Note: s_pHelper is allocated as part of injecting the payload in DetourCopyPayloadToProcess(..),
    // it's a fake section and not data allocated by the system PE loader.

    // Delete the payload after execution to release the memory occupied by it
    if (s_pHelper != NULL) {
        DetourFreePayload(s_pHelper);
        s_pHelper = NULL;
    }

    ExitProcess(Result);
}

BOOL WINAPI DetourIsHelperProcess(VOID)
{
    PVOID pvData;
    DWORD cbData;

    if (s_fSearchedForHelper) {
        return (s_pHelper != NULL);
    }

    s_fSearchedForHelper = TRUE;
    pvData = DetourFindPayloadEx(DETOUR_EXE_HELPER_GUID, &cbData);

    if (pvData == NULL || cbData < sizeof(DETOUR_EXE_HELPER)) {
        return FALSE;
    }

    s_pHelper = (PDETOUR_EXE_HELPER)pvData;
    if (s_pHelper->cb < sizeof(*s_pHelper)) {
        s_pHelper = NULL;
        return FALSE;
    }

    return TRUE;
}

static
BOOL WINAPI AllocExeHelper(_Out_ PDETOUR_EXE_HELPER *pHelper,
                           _In_ DWORD dwTargetPid,
                           _In_ DWORD nDlls,
                           _In_reads_(nDlls) LPCSTR *rlpDlls)
{
    PDETOUR_EXE_HELPER Helper = NULL;
    BOOL Result = FALSE;
    _Field_range_(0, cSize - 4) DWORD cOffset = 0;
    DWORD cSize = 4;

    if (pHelper == NULL) {
        goto Cleanup;
    }
    *pHelper = NULL;

    if (nDlls < 1 || nDlls > 4096) {
        SetLastError(ERROR_INVALID_PARAMETER);
        goto Cleanup;
    }

    for (DWORD n = 0; n < nDlls; n++) {
        HRESULT hr;
        size_t cchDest = 0;

        hr = StringCchLengthA(rlpDlls[n], 4096, &cchDest);
        if (!SUCCEEDED(hr)) {
            goto Cleanup;
        }

        cSize += (DWORD)cchDest + 1;
    }

    Helper = (PDETOUR_EXE_HELPER) new NOTHROW BYTE[sizeof(DETOUR_EXE_HELPER) + cSize];
    if (Helper == NULL) {
        goto Cleanup;
    }

    Helper->cb = sizeof(DETOUR_EXE_HELPER) + cSize;
    Helper->pid = dwTargetPid;
    Helper->nDlls = nDlls;

    for (DWORD n = 0; n < nDlls; n++) {
        HRESULT hr;
        size_t cchDest = 0;

        if (cOffset > 0x10000 || cSize > 0x10000 || cOffset + 2 >= cSize) {
            goto Cleanup;
        }

        if (cOffset + 2 >= cSize || cOffset + 65536 < cSize) {
            goto Cleanup;
        }

        _Analysis_assume_(cOffset + 1 < cSize);
        _Analysis_assume_(cOffset < 0x10000);
        _Analysis_assume_(cSize < 0x10000);

        PCHAR psz = &Helper->rDlls[cOffset];

        hr = StringCchCopyA(psz, cSize - cOffset, rlpDlls[n]);
        if (!SUCCEEDED(hr)) {
            goto Cleanup;
        }

// REVIEW 28020 The expression '1<=_Param_(2)& &_Param_(2)<=2147483647' is not true at this call.
// REVIEW 28313 Analysis will not proceed past this point because of annotation evaluation. The annotation expression *_Param_(3)<_Param_(2)&&*_Param_(3)<=stringLength$(_Param_(1)) cannot be true under any assumptions at this point in the program.
#pragma warning(suppress:28020 28313)
        hr = StringCchLengthA(psz, cSize - cOffset, &cchDest);
        if (!SUCCEEDED(hr)) {
            goto Cleanup;
        }

        // Replace "32." with "64." or "64." with "32."

        for (DWORD c = (DWORD)cchDest + 1; c > 3; c--) {
#if DETOURS_32BIT
            if (psz[c - 3] == '3' && psz[c - 2] == '2' && psz[c - 1] == '.') {
                psz[c - 3] = '6'; psz[c - 2] = '4';
                break;
            }
#else
            if (psz[c - 3] == '6' && psz[c - 2] == '4' && psz[c - 1] == '.') {
                psz[c - 3] = '3'; psz[c - 2] = '2';
                break;
            }
#endif
        }

        cOffset += (DWORD)cchDest + 1;
    }

    *pHelper = Helper;
    Helper = NULL;
    Result = TRUE;

  Cleanup:
    if (Helper != NULL) {
        delete[] (PBYTE)Helper;
        Helper = NULL;
    }
    return Result;
}

static
VOID WINAPI FreeExeHelper(PDETOUR_EXE_HELPER *pHelper)
{
    if (*pHelper != NULL) {
        delete[] (PBYTE)*pHelper;
        *pHelper = NULL;
    }
}

BOOL WINAPI DetourProcessViaHelperA(_In_ DWORD dwTargetPid,
                                    _In_ LPCSTR lpDllName,
                                    _In_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA)
{
    return DetourProcessViaHelperDllsA(dwTargetPid, 1, &lpDllName, pfCreateProcessA);
}


BOOL WINAPI DetourProcessViaHelperDllsA(_In_ DWORD dwTargetPid,
                                        _In_ DWORD nDlls,
                                        _In_reads_(nDlls) LPCSTR *rlpDlls,
                                        _In_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA)
{
    BOOL Result = FALSE;
    PROCESS_INFORMATION pi;
    STARTUPINFOA si;
    CHAR szExe[MAX_PATH];
    CHAR szCommand[MAX_PATH];
    PDETOUR_EXE_HELPER helper = NULL;
    HRESULT hr;
    DWORD nLen = GetEnvironmentVariableA("WINDIR", szExe, ARRAYSIZE(szExe));

    DETOUR_TRACE(("DetourProcessViaHelperDlls(pid=%lu,dlls=%lu)\n", dwTargetPid, nDlls));
    if (nDlls < 1 || nDlls > 4096) {
        SetLastError(ERROR_INVALID_PARAMETER);
        goto Cleanup;
    }
    if (!AllocExeHelper(&helper, dwTargetPid, nDlls, rlpDlls)) {
        goto Cleanup;
    }

    if (nLen == 0 || nLen >= ARRAYSIZE(szExe)) {
        goto Cleanup;
    }

#if DETOURS_OPTION_BITS
#if DETOURS_32BIT
    hr = StringCchCatA(szExe, ARRAYSIZE(szExe), "\\sysnative\\rundll32.exe");
#else // !DETOURS_32BIT
    hr = StringCchCatA(szExe, ARRAYSIZE(szExe), "\\syswow64\\rundll32.exe");
#endif // !DETOURS_32BIT
#else // DETOURS_OPTIONS_BITS
    hr = StringCchCatA(szExe, ARRAYSIZE(szExe), "\\system32\\rundll32.exe");
#endif // DETOURS_OPTIONS_BITS
    if (!SUCCEEDED(hr)) {
        goto Cleanup;
    }

    //for East Asia languages and so on, like Chinese, print format with "%hs" can not work fine before user call _tsetlocale(LC_ALL,_T(".ACP"));
    //so we can't use "%hs" in format string, because the dll that contain this code would inject to any process, even not call _tsetlocale(LC_ALL,_T(".ACP")) before
    hr = StringCchPrintfA(szCommand, ARRAYSIZE(szCommand),
                          "rundll32.exe \"%s\",#1", &helper->rDlls[0]);
    if (!SUCCEEDED(hr)) {
        goto Cleanup;
    }

    ZeroMemory(&pi, sizeof(pi));
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    DETOUR_TRACE(("DetourProcessViaHelperDlls(\"%hs\", \"%hs\")\n", szExe, szCommand));
    if (pfCreateProcessA(szExe, szCommand, NULL, NULL, FALSE, CREATE_SUSPENDED,
                         NULL, NULL, &si, &pi)) {

        if (!DetourCopyPayloadToProcess(pi.hProcess,
                                        DETOUR_EXE_HELPER_GUID,
                                        helper, helper->cb)) {
            DETOUR_TRACE(("DetourCopyPayloadToProcess failed: %lu\n", GetLastError()));
            TerminateProcess(pi.hProcess, ~0u);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            goto Cleanup;
        }

        ResumeThread(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD dwResult = 500;
        GetExitCodeProcess(pi.hProcess, &dwResult);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (dwResult != 0) {
            DETOUR_TRACE(("Rundll32.exe failed: result=%lu\n", dwResult));
            goto Cleanup;
        }
        Result = TRUE;
    }
    else {
        DETOUR_TRACE(("CreateProcess failed: %lu\n", GetLastError()));
        goto Cleanup;
    }

  Cleanup:
    FreeExeHelper(&helper);
    return Result;
}

BOOL WINAPI DetourProcessViaHelperW(_In_ DWORD dwTargetPid,
                                    _In_ LPCSTR lpDllName,
                                    _In_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW)
{
    return DetourProcessViaHelperDllsW(dwTargetPid, 1, &lpDllName, pfCreateProcessW);
}

BOOL WINAPI DetourProcessViaHelperDllsW(_In_ DWORD dwTargetPid,
                                        _In_ DWORD nDlls,
                                        _In_reads_(nDlls) LPCSTR *rlpDlls,
                                        _In_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW)
{
    BOOL Result = FALSE;
    PROCESS_INFORMATION pi;
    STARTUPINFOW si;
    WCHAR szExe[MAX_PATH];
    WCHAR szCommand[MAX_PATH];
    PDETOUR_EXE_HELPER helper = NULL;
    HRESULT hr;
    WCHAR szDllName[MAX_PATH];
    int cchWrittenWideChar;
    DWORD nLen = GetEnvironmentVariableW(L"WINDIR", szExe, ARRAYSIZE(szExe));

    DETOUR_TRACE(("DetourProcessViaHelperDlls(pid=%lu,dlls=%lu)\n", dwTargetPid, nDlls));
    if (nDlls < 1 || nDlls > 4096) {
        SetLastError(ERROR_INVALID_PARAMETER);
        goto Cleanup;
    }
    if (!AllocExeHelper(&helper, dwTargetPid, nDlls, rlpDlls)) {
        goto Cleanup;
    }

    if (nLen == 0 || nLen >= ARRAYSIZE(szExe)) {
        goto Cleanup;
    }

#if DETOURS_OPTION_BITS
#if DETOURS_32BIT
    hr = StringCchCatW(szExe, ARRAYSIZE(szExe), L"\\sysnative\\rundll32.exe");
#else // !DETOURS_32BIT
    hr = StringCchCatW(szExe, ARRAYSIZE(szExe), L"\\syswow64\\rundll32.exe");
#endif // !DETOURS_32BIT
#else // DETOURS_OPTIONS_BITS
    hr = StringCchCatW(szExe, ARRAYSIZE(szExe), L"\\system32\\rundll32.exe");
#endif // DETOURS_OPTIONS_BITS
    if (!SUCCEEDED(hr)) {
        goto Cleanup;
    }

    //for East Asia languages and so on, like Chinese, print format with "%hs" can not work fine before user call _tsetlocale(LC_ALL,_T(".ACP"));
    //so we can't use "%hs" in format string, because the dll that contain this code would inject to any process, even not call _tsetlocale(LC_ALL,_T(".ACP")) before
    
    cchWrittenWideChar = MultiByteToWideChar(CP_ACP, 0, &helper->rDlls[0], -1, szDllName, ARRAYSIZE(szDllName));
    if (cchWrittenWideChar >= ARRAYSIZE(szDllName) || cchWrittenWideChar <= 0) {
        goto Cleanup;
    }
    hr = StringCchPrintfW(szCommand, ARRAYSIZE(szCommand),
        L"rundll32.exe \"%s\",#1", szDllName);
    if (!SUCCEEDED(hr)) {
        goto Cleanup;
    }

    ZeroMemory(&pi, sizeof(pi));
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    DETOUR_TRACE(("DetourProcessViaHelperDlls(\"%ls\", \"%ls\")\n", szExe, szCommand));
    if (pfCreateProcessW(szExe, szCommand, NULL, NULL, FALSE, CREATE_SUSPENDED,
                         NULL, NULL, &si, &pi)) {

        if (!DetourCopyPayloadToProcess(pi.hProcess,
                                        DETOUR_EXE_HELPER_GUID,
                                        helper, helper->cb)) {
            DETOUR_TRACE(("DetourCopyPayloadToProcess failed: %lu\n", GetLastError()));
            TerminateProcess(pi.hProcess, ~0u);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            goto Cleanup;
        }

        ResumeThread(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD dwResult = 500;
        GetExitCodeProcess(pi.hProcess, &dwResult);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (dwResult != 0) {
            DETOUR_TRACE(("Rundll32.exe failed: result=%lu\n", dwResult));
            goto Cleanup;
        }
        Result = TRUE;
    }
    else {
        DETOUR_TRACE(("CreateProcess failed: %lu\n", GetLastError()));
        goto Cleanup;
    }

  Cleanup:
    FreeExeHelper(&helper);
    return Result;
}

BOOL WINAPI DetourCreateProcessWithDllExA(_In_opt_ LPCSTR lpApplicationName,
                                          _Inout_opt_ LPSTR lpCommandLine,
                                          _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                          _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                          _In_ BOOL bInheritHandles,
                                          _In_ DWORD dwCreationFlags,
                                          _In_opt_ LPVOID lpEnvironment,
                                          _In_opt_ LPCSTR lpCurrentDirectory,
                                          _In_ LPSTARTUPINFOA lpStartupInfo,
                                          _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                          _In_ LPCSTR lpDllName,
                                          _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA)
{
    if (pfCreateProcessA == NULL) {
        pfCreateProcessA = CreateProcessA;
    }

    PROCESS_INFORMATION backup;
    if (lpProcessInformation == NULL) {
        lpProcessInformation = &backup;
        ZeroMemory(&backup, sizeof(backup));
    }

    if (!pfCreateProcessA(lpApplicationName,
                          lpCommandLine,
                          lpProcessAttributes,
                          lpThreadAttributes,
                          bInheritHandles,
                          dwCreationFlags | CREATE_SUSPENDED,
                          lpEnvironment,
                          lpCurrentDirectory,
                          lpStartupInfo,
                          lpProcessInformation)) {
        return FALSE;
    }

    LPCSTR szDll = lpDllName;

    if (!DetourUpdateProcessWithDll(lpProcessInformation->hProcess, &szDll, 1) &&
        !DetourProcessViaHelperA(lpProcessInformation->dwProcessId,
                                 lpDllName,
                                 pfCreateProcessA)) {

        TerminateProcess(lpProcessInformation->hProcess, ~0u);
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
        return FALSE;
    }

    if (!(dwCreationFlags & CREATE_SUSPENDED)) {
        ResumeThread(lpProcessInformation->hThread);
    }

    if (lpProcessInformation == &backup) {
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
    }

    return TRUE;
}

BOOL WINAPI DetourCreateProcessWithDllExW(_In_opt_ LPCWSTR lpApplicationName,
                                          _Inout_opt_  LPWSTR lpCommandLine,
                                          _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                          _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                          _In_ BOOL bInheritHandles,
                                          _In_ DWORD dwCreationFlags,
                                          _In_opt_ LPVOID lpEnvironment,
                                          _In_opt_ LPCWSTR lpCurrentDirectory,
                                          _In_ LPSTARTUPINFOW lpStartupInfo,
                                          _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                          _In_ LPCSTR lpDllName,
                                          _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW)
{
    if (pfCreateProcessW == NULL) {
        pfCreateProcessW = CreateProcessW;
    }

    PROCESS_INFORMATION backup;
    if (lpProcessInformation == NULL) {
        lpProcessInformation = &backup;
        ZeroMemory(&backup, sizeof(backup));
    }

    if (!pfCreateProcessW(lpApplicationName,
                          lpCommandLine,
                          lpProcessAttributes,
                          lpThreadAttributes,
                          bInheritHandles,
                          dwCreationFlags | CREATE_SUSPENDED,
                          lpEnvironment,
                          lpCurrentDirectory,
                          lpStartupInfo,
                          lpProcessInformation)) {
        return FALSE;
    }


    LPCSTR sz = lpDllName;

    if (!DetourUpdateProcessWithDll(lpProcessInformation->hProcess, &sz, 1) &&
        !DetourProcessViaHelperW(lpProcessInformation->dwProcessId,
                                 lpDllName,
                                 pfCreateProcessW)) {

        TerminateProcess(lpProcessInformation->hProcess, ~0u);
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
        return FALSE;
    }

    if (!(dwCreationFlags & CREATE_SUSPENDED)) {
        ResumeThread(lpProcessInformation->hThread);
    }

    if (lpProcessInformation == &backup) {
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
    }
    return TRUE;
}

BOOL WINAPI DetourCreateProcessWithDllsA(_In_opt_ LPCSTR lpApplicationName,
                                         _Inout_opt_ LPSTR lpCommandLine,
                                         _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                         _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                         _In_ BOOL bInheritHandles,
                                         _In_ DWORD dwCreationFlags,
                                         _In_opt_ LPVOID lpEnvironment,
                                         _In_opt_ LPCSTR lpCurrentDirectory,
                                         _In_ LPSTARTUPINFOA lpStartupInfo,
                                         _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                         _In_ DWORD nDlls,
                                         _In_reads_(nDlls) LPCSTR *rlpDlls,
                                         _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA)
{
    if (pfCreateProcessA == NULL) {
        pfCreateProcessA = CreateProcessA;
    }

    PROCESS_INFORMATION backup;
    if (lpProcessInformation == NULL) {
        lpProcessInformation = &backup;
        ZeroMemory(&backup, sizeof(backup));
    }

    if (!pfCreateProcessA(lpApplicationName,
                          lpCommandLine,
                          lpProcessAttributes,
                          lpThreadAttributes,
                          bInheritHandles,
                          dwCreationFlags | CREATE_SUSPENDED,
                          lpEnvironment,
                          lpCurrentDirectory,
                          lpStartupInfo,
                          lpProcessInformation)) {
        return FALSE;
    }

    if (!DetourUpdateProcessWithDll(lpProcessInformation->hProcess, rlpDlls, nDlls) &&
        !DetourProcessViaHelperDllsA(lpProcessInformation->dwProcessId,
                                     nDlls,
                                     rlpDlls,
                                     pfCreateProcessA)) {

        TerminateProcess(lpProcessInformation->hProcess, ~0u);
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
        return FALSE;
    }

    if (!(dwCreationFlags & CREATE_SUSPENDED)) {
        ResumeThread(lpProcessInformation->hThread);
    }

    if (lpProcessInformation == &backup) {
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
    }

    return TRUE;
}

BOOL WINAPI DetourCreateProcessWithDllsW(_In_opt_ LPCWSTR lpApplicationName,
                                         _Inout_opt_ LPWSTR lpCommandLine,
                                         _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                         _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                         _In_ BOOL bInheritHandles,
                                         _In_ DWORD dwCreationFlags,
                                         _In_opt_ LPVOID lpEnvironment,
                                         _In_opt_ LPCWSTR lpCurrentDirectory,
                                         _In_ LPSTARTUPINFOW lpStartupInfo,
                                         _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                         _In_ DWORD nDlls,
                                         _In_reads_(nDlls) LPCSTR *rlpDlls,
                                         _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW)
{
    if (pfCreateProcessW == NULL) {
        pfCreateProcessW = CreateProcessW;
    }

    PROCESS_INFORMATION backup;
    if (lpProcessInformation == NULL) {
        lpProcessInformation = &backup;
        ZeroMemory(&backup, sizeof(backup));
    }

    if (!pfCreateProcessW(lpApplicationName,
                          lpCommandLine,
                          lpProcessAttributes,
                          lpThreadAttributes,
                          bInheritHandles,
                          dwCreationFlags | CREATE_SUSPENDED,
                          lpEnvironment,
                          lpCurrentDirectory,
                          lpStartupInfo,
                          lpProcessInformation)) {
        return FALSE;
    }


    if (!DetourUpdateProcessWithDll(lpProcessInformation->hProcess, rlpDlls, nDlls) &&
        !DetourProcessViaHelperDllsW(lpProcessInformation->dwProcessId,
                                     nDlls,
                                     rlpDlls,
                                     pfCreateProcessW)) {

        TerminateProcess(lpProcessInformation->hProcess, ~0u);
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
        return FALSE;
    }

    if (!(dwCreationFlags & CREATE_SUSPENDED)) {
        ResumeThread(lpProcessInformation->hThread);
    }

    if (lpProcessInformation == &backup) {
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
    }
    return TRUE;
}

//
///////////////////////////////////////////////////////////////// End of File.
