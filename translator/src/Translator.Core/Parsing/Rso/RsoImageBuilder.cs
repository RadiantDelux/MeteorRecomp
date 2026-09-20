using System.Buffers.Binary;
using System.Security.Cryptography;

namespace Translator.Core.Parsing.Rso;

public interface IRsoSymbolProvider
{
    bool TryResolve(string name, out uint address);
}

public sealed record RsoImageBuildOptions
{
    public bool PreserveBss { get; init; }
    public ReadOnlyMemory<byte> InitialBss { get; init; }
    public uint? UnresolvedSymbolAddress { get; init; }
    // Retail RSOLink permits imports that are absent from the selectable export
    // set. Their import address and relocation group are left untouched.
    public bool PreserveUnresolvedImports { get; init; }
}

public enum RsoRelocationTargetKind : byte
{
    InternalSection,
    ExternalImport,
}

public enum RsoRelocationSemantic : byte
{
    None,
    AbsoluteAddress,
    AddressFragment,
    BranchDisplacement,
}

public sealed record RsoRelocationProvenance(
    RsoRelocationTableKind TableKind,
    uint PatchAddress,
    RsoRelocationType Type,
    uint Addend,
    RsoRelocationTargetKind TargetKind,
    uint SymbolIndex,
    string? SymbolName,
    uint ResolvedTarget,
    RsoRelocationSemantic Semantic);

public sealed record RsoLinkedSection(int Index, uint Address, uint Size, bool IsBss);

public sealed record RsoLinkedImport(
    int Index,
    string Name,
    uint Address,
    uint ExternalRelocationOffset,
    bool IsResolved);

public sealed record RsoLinkedExport(string Name, uint ElfHash, uint Address);

public sealed record RsoLinkedImage(
    uint ImageBase,
    byte[] Image,
    uint BssBase,
    byte[] Bss,
    IReadOnlyList<RsoLinkedSection> Sections,
    IReadOnlyList<RsoLinkedImport> Imports,
    IReadOnlyList<RsoLinkedExport> Exports,
    IReadOnlyList<RsoRelocationProvenance> Relocations,
    uint? PrologAddress,
    uint? EpilogAddress,
    uint? UnresolvedAddress,
    byte[] PreLinkSha256);

/// <summary>
/// Deterministically constructs the linked address image used by the Wii RSO linker.
/// It operates only on caller-supplied guest addresses and never allocates guest memory.
/// </summary>
public static class RsoImageBuilder
{
    public static RsoLinkedImage Build(
        RsoFile file,
        uint imageBase,
        uint bssBase,
        IRsoSymbolProvider? symbolProvider = null,
        RsoImageBuildOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(file);
        options ??= new RsoImageBuildOptions();

        var image = file.RawData.ToArray();
        var linkedSections = LinkSections(file, image, imageBase, bssBase, out var bssSectionIndex);
        var bss = BuildBss(file.Header.BssSize, options);

        RebaseHeaderPointers(file.Header, image, imageBase);
        if (bssSectionIndex is { } discoveredBss)
        {
            image[0x23] = checked((byte)discoveredBss);
        }

        var prolog = LinkLifecycleAddress(file.Header.PrologSectionIndex, file.Header.PrologOffset, linkedSections);
        var epilog = LinkLifecycleAddress(file.Header.EpilogSectionIndex, file.Header.EpilogOffset, linkedSections);
        var unresolved = LinkLifecycleAddress(file.Header.UnresolvedSectionIndex, file.Header.UnresolvedOffset, linkedSections);
        WriteOptionalAddress(image, 0x24, prolog);
        WriteOptionalAddress(image, 0x28, epilog);
        WriteOptionalAddress(image, 0x2C, unresolved);

        var provenance = new List<RsoRelocationProvenance>(
            file.InternalRelocations.Count + file.ExternalRelocations.Count);
        ApplyInternalRelocations(file, image, imageBase, linkedSections, provenance);
        var imports = LinkImports(file, image, imageBase, symbolProvider, options, provenance);

        var exports = file.Exports.Select(export =>
        {
            var section = linkedSections[checked((int)export.SectionIndex)];
            return new RsoLinkedExport(
                export.Name,
                export.ElfHash,
                CheckedAdd(section.Address, export.SymbolOffset, $"export {export.Name}"));
        }).ToArray();

        return new RsoLinkedImage(
            imageBase,
            image,
            bssBase,
            bss,
            linkedSections,
            imports,
            exports,
            provenance,
            prolog,
            epilog,
            unresolved,
            SHA256.HashData(file.RawData.Span));
    }

    private static RsoLinkedSection[] LinkSections(
        RsoFile file,
        byte[] image,
        uint imageBase,
        uint bssBase,
        out int? bssSectionIndex)
    {
        var sections = new RsoLinkedSection[file.Sections.Count];
        bssSectionIndex = null;
        for (var index = 0; index < file.Sections.Count; ++index)
        {
            var section = file.Sections[index];
            uint address;
            var isBss = false;

            if (index == 0 || section.Size == 0)
            {
                address = section.FileOffset == 0
                    ? 0u
                    : CheckedAdd(imageBase, section.FileOffset, $"section {index}");
            }
            else if (section.FileOffset != 0)
            {
                address = CheckedAdd(imageBase, section.FileOffset, $"section {index}");
            }
            else
            {
                if (bssSectionIndex is not null)
                {
                    throw new InvalidDataException("RSO contains more than one non-empty BSS section");
                }
                if (section.Size != file.Header.BssSize)
                {
                    throw new InvalidDataException(
                        $"RSO BSS section {index} size 0x{section.Size:X8} does not match header BSS size 0x{file.Header.BssSize:X8}");
                }
                address = bssBase;
                isBss = true;
                bssSectionIndex = index;
            }

            sections[index] = new RsoLinkedSection(index, address, section.Size, isBss);
            if (index != 0)
            {
                var entryOffset = checked((int)((ulong)file.Header.SectionTableOffset + (ulong)index * RsoFile.SectionEntrySize));
                WriteU32(image, entryOffset, address);
            }
        }

        if (file.Header.BssSize != 0 && bssSectionIndex is null)
        {
            throw new InvalidDataException("RSO declares BSS bytes but has no non-empty BSS section");
        }
        return sections;
    }

    private static byte[] BuildBss(uint bssSize, RsoImageBuildOptions options)
    {
        if (bssSize > int.MaxValue)
        {
            throw new InvalidDataException($"RSO BSS size 0x{bssSize:X8} is too large");
        }
        var bss = new byte[checked((int)bssSize)];
        if (!options.PreserveBss)
        {
            return bss;
        }
        if (options.InitialBss.Length != bss.Length)
        {
            throw new InvalidDataException(
                $"Preserved BSS requires exactly 0x{bss.Length:X} initial bytes, got 0x{options.InitialBss.Length:X}");
        }
        options.InitialBss.Span.CopyTo(bss);
        return bss;
    }

    private static void RebaseHeaderPointers(RsoHeader header, byte[] image, uint imageBase)
    {
        foreach (var (fieldOffset, rawOffset, label) in new (int, uint, string)[]
        {
            (0x0C, header.SectionTableOffset, "section table"),
            (0x10, header.ModuleNameOffset, "module name"),
            (0x30, header.InternalRelocationTableOffset, "internal relocation table"),
            (0x38, header.ExternalRelocationTableOffset, "external relocation table"),
            (0x40, header.ExportTableOffset, "export table"),
            (0x48, header.ExportNameTableOffset, "export name table"),
            (0x4C, header.ImportTableOffset, "import table"),
            (0x54, header.ImportNameTableOffset, "import name table"),
        })
        {
            WriteU32(image, fieldOffset, rawOffset == 0 ? 0u : CheckedAdd(imageBase, rawOffset, label));
        }
    }

    private static uint? LinkLifecycleAddress(byte sectionIndex, uint sectionOffset, IReadOnlyList<RsoLinkedSection> sections)
    {
        if (sectionIndex == 0)
        {
            return null;
        }
        return CheckedAdd(sections[sectionIndex].Address, sectionOffset, "lifecycle function");
    }

    private static void ApplyInternalRelocations(
        RsoFile file,
        byte[] image,
        uint imageBase,
        IReadOnlyList<RsoLinkedSection> sections,
        ICollection<RsoRelocationProvenance> provenance)
    {
        for (var index = 0; index < file.InternalRelocations.Count; ++index)
        {
            var relocation = file.InternalRelocations[index];
            var patchAddress = CheckedAdd(imageBase, relocation.Offset, $"internal relocation {index} patch");
            var section = sections[checked((int)relocation.SymbolIndex)];
            var target = CheckedAdd(section.Address, relocation.Addend, $"internal relocation {index} target");
            ApplyRelocation(image, imageBase, relocation, patchAddress, target);
            WriteRelocationPatchAddress(file.Header.InternalRelocationTableOffset, index, image, patchAddress);
            provenance.Add(new RsoRelocationProvenance(
                relocation.TableKind,
                patchAddress,
                relocation.Type,
                relocation.Addend,
                RsoRelocationTargetKind.InternalSection,
                relocation.SymbolIndex,
                null,
                target,
                GetSemantic(relocation.Type)));
        }
    }

    private static RsoLinkedImport[] LinkImports(
        RsoFile file,
        byte[] image,
        uint imageBase,
        IRsoSymbolProvider? symbolProvider,
        RsoImageBuildOptions options,
        ICollection<RsoRelocationProvenance> provenance)
    {
        var imports = new RsoLinkedImport[file.Imports.Count];
        var importAddresses = new uint[file.Imports.Count];
        var resolved = new bool[file.Imports.Count];
        var applyRelocations = new bool[file.Imports.Count];

        for (var index = 0; index < file.Imports.Count; ++index)
        {
            var import = file.Imports[index];
            var entryOffset = checked((int)((ulong)file.Header.ImportTableOffset + (ulong)index * RsoFile.ImportEntrySize));
            if (symbolProvider is not null && symbolProvider.TryResolve(import.Name, out var address))
            {
                importAddresses[index] = address;
                resolved[index] = true;
                applyRelocations[index] = true;
            }
            else if (options.UnresolvedSymbolAddress is { } fallback)
            {
                importAddresses[index] = fallback;
                applyRelocations[index] = true;
            }
            else if (options.PreserveUnresolvedImports)
            {
                importAddresses[index] = ReadU32(image, entryOffset + 4);
            }
            else
            {
                throw new InvalidDataException($"RSO import '{import.Name}' is unresolved and no unresolved handler was supplied");
            }

            if (applyRelocations[index])
                WriteU32(image, entryOffset + 4, importAddresses[index]);
            imports[index] = new RsoLinkedImport(index, import.Name, importAddresses[index], import.EntryOffset, resolved[index]);
        }

        ValidateExternalRelocationGroups(file);
        for (var index = 0; index < file.ExternalRelocations.Count; ++index)
        {
            var relocation = file.ExternalRelocations[index];
            var importIndex = checked((int)relocation.SymbolIndex);
            if (!applyRelocations[importIndex])
                continue;
            var patchAddress = CheckedAdd(imageBase, relocation.Offset, $"external relocation {index} patch");
            var target = CheckedAdd(importAddresses[importIndex], relocation.Addend, $"external relocation {index} target");
            ApplyRelocation(image, imageBase, relocation, patchAddress, target);
            WriteRelocationPatchAddress(file.Header.ExternalRelocationTableOffset, index, image, patchAddress);
            provenance.Add(new RsoRelocationProvenance(
                relocation.TableKind,
                patchAddress,
                relocation.Type,
                relocation.Addend,
                RsoRelocationTargetKind.ExternalImport,
                relocation.SymbolIndex,
                file.Imports[importIndex].Name,
                target,
                GetSemantic(relocation.Type)));
        }
        return imports;
    }

    private static void ValidateExternalRelocationGroups(RsoFile file)
    {
        for (var importIndex = 0; importIndex < file.Imports.Count; ++importIndex)
        {
            var import = file.Imports[importIndex];
            if (import.EntryOffset % RsoFile.RelocationEntrySize != 0 || import.EntryOffset > file.Header.ExternalRelocationTableSize)
            {
                throw new InvalidDataException(
                    $"RSO import {importIndex} external relocation offset 0x{import.EntryOffset:X8} is invalid");
            }
            if (import.EntryOffset == file.Header.ExternalRelocationTableSize)
            {
                continue;
            }
            var firstIndex = checked((int)(import.EntryOffset / RsoFile.RelocationEntrySize));
            if (firstIndex >= file.ExternalRelocations.Count || file.ExternalRelocations[firstIndex].SymbolIndex != (uint)importIndex)
            {
                throw new InvalidDataException(
                    $"RSO import {importIndex} external relocation group does not start at declared offset 0x{import.EntryOffset:X8}");
            }
        }
    }

    private static void ApplyRelocation(byte[] image, uint imageBase, RsoRelocation relocation, uint patchAddress, uint target)
    {
        var patchOffset = checked((int)(patchAddress - imageBase));
        switch (relocation.Type)
        {
            case RsoRelocationType.R_PPC_NONE:
                return;
            case RsoRelocationType.R_PPC_ADDR32:
                WriteU32(image, patchOffset, target);
                return;
            case RsoRelocationType.R_PPC_ADDR24:
                WriteU32(image, patchOffset, (ReadU32(image, patchOffset) & 0xFC000003u) | (target & 0x03FFFFFCu));
                return;
            case RsoRelocationType.R_PPC_ADDR16:
            case RsoRelocationType.R_PPC_ADDR16_LO:
                WriteU16(image, patchOffset, (ushort)target);
                return;
            case RsoRelocationType.R_PPC_ADDR16_HI:
                WriteU16(image, patchOffset, (ushort)(target >> 16));
                return;
            case RsoRelocationType.R_PPC_ADDR16_HA:
                WriteU16(image, patchOffset, (ushort)((target + 0x8000u) >> 16));
                return;
            case RsoRelocationType.R_PPC_ADDR14:
            case RsoRelocationType.R_PPC_ADDR14_BRTAKEN:
            case RsoRelocationType.R_PPC_ADDR14_BRNTAKEN:
                WriteU32(image, patchOffset, (ReadU32(image, patchOffset) & 0xFFFF0003u) | (target & 0x0000FFFCu));
                return;
            case RsoRelocationType.R_PPC_REL24:
            {
                var displacement = unchecked((int)(target - patchAddress));
                var field = displacement < -0x02000000 || displacement > 0x01FFFFFC
                    ? 0x03FFFFFCu
                    : unchecked((uint)displacement) & 0x03FFFFFCu;
                WriteU32(image, patchOffset, (ReadU32(image, patchOffset) & 0xFC000003u) | field);
                return;
            }
            case RsoRelocationType.R_PPC_REL14:
            case RsoRelocationType.R_PPC_REL14_BRTAKEN:
            case RsoRelocationType.R_PPC_REL14_BRNTAKEN:
            {
                var displacement = target - patchAddress;
                WriteU32(image, patchOffset, (ReadU32(image, patchOffset) & 0xFFFF0003u) | (displacement & 0x0000FFFCu));
                return;
            }
            case RsoRelocationType.R_PPC_EMB_SDA21:
                throw new NotSupportedException("R_PPC_EMB_SDA21 requires Wii SDA context and is intentionally not approximated");
            default:
                throw new NotSupportedException($"Unsupported RSO relocation type {(byte)relocation.Type}");
        }
    }

    private static RsoRelocationSemantic GetSemantic(RsoRelocationType type) => type switch
    {
        RsoRelocationType.R_PPC_NONE => RsoRelocationSemantic.None,
        RsoRelocationType.R_PPC_ADDR32 or RsoRelocationType.R_PPC_ADDR24 or
            RsoRelocationType.R_PPC_ADDR14 or RsoRelocationType.R_PPC_ADDR14_BRTAKEN or
            RsoRelocationType.R_PPC_ADDR14_BRNTAKEN => RsoRelocationSemantic.AbsoluteAddress,
        RsoRelocationType.R_PPC_ADDR16 or RsoRelocationType.R_PPC_ADDR16_LO or
            RsoRelocationType.R_PPC_ADDR16_HI or RsoRelocationType.R_PPC_ADDR16_HA => RsoRelocationSemantic.AddressFragment,
        RsoRelocationType.R_PPC_REL24 or RsoRelocationType.R_PPC_REL14 or
            RsoRelocationType.R_PPC_REL14_BRTAKEN or RsoRelocationType.R_PPC_REL14_BRNTAKEN => RsoRelocationSemantic.BranchDisplacement,
        _ => RsoRelocationSemantic.None,
    };

    private static void WriteRelocationPatchAddress(uint tableOffset, int index, byte[] image, uint patchAddress)
    {
        var recordOffset = checked((int)((ulong)tableOffset + (ulong)index * RsoFile.RelocationEntrySize));
        WriteU32(image, recordOffset, patchAddress);
    }

    private static void WriteOptionalAddress(byte[] image, int offset, uint? address) => WriteU32(image, offset, address ?? 0u);

    private static uint CheckedAdd(uint left, uint right, string label)
    {
        var result = (ulong)left + right;
        if (result > uint.MaxValue)
        {
            throw new InvalidDataException($"RSO {label} address overflows 32-bit guest space");
        }
        return (uint)result;
    }

    private static uint ReadU32(byte[] data, int offset) =>
        BinaryPrimitives.ReadUInt32BigEndian(data.AsSpan(offset, 4));

    private static void WriteU16(byte[] data, int offset, ushort value) =>
        BinaryPrimitives.WriteUInt16BigEndian(data.AsSpan(offset, 2), value);

    private static void WriteU32(byte[] data, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(offset, 4), value);
}
