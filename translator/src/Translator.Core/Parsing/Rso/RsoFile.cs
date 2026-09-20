using System.Buffers.Binary;
using System.Collections.ObjectModel;
using System.Text;

namespace Translator.Core.Parsing.Rso;

/// <summary>
/// Standalone parser for the Wii Revolution Shared Object (RSO) file format.
/// It intentionally performs no relocation or runtime linking; callers get the exact structural
/// records needed to make those policy decisions elsewhere.
/// </summary>
public sealed class RsoFile
{
    public const int HeaderSize = 0x58;
    public const int SectionEntrySize = 0x08;
    public const int RelocationEntrySize = 0x0C;
    public const int ExportEntrySize = 0x10;
    public const int ImportEntrySize = 0x0C;

    private RsoFile(
        RsoHeader header,
        string moduleName,
        ReadOnlyMemory<byte> rawModuleName,
        IReadOnlyList<RsoSection> sections,
        IReadOnlyList<RsoRelocation> internalRelocations,
        IReadOnlyList<RsoRelocation> externalRelocations,
        IReadOnlyList<RsoExportSymbol> exports,
        IReadOnlyList<RsoImportSymbol> imports,
        ReadOnlyMemory<byte> rawData)
    {
        Header = header;
        ModuleName = moduleName;
        RawModuleName = rawModuleName;
        Sections = sections;
        InternalRelocations = internalRelocations;
        ExternalRelocations = externalRelocations;
        Exports = exports;
        Imports = imports;
        RawData = rawData;
    }

    public RsoHeader Header { get; }
    public string ModuleName { get; }
    public ReadOnlyMemory<byte> RawModuleName { get; }
    public IReadOnlyList<RsoSection> Sections { get; }
    public IReadOnlyList<RsoRelocation> InternalRelocations { get; }
    public IReadOnlyList<RsoRelocation> ExternalRelocations { get; }
    public IReadOnlyList<RsoExportSymbol> Exports { get; }
    public IReadOnlyList<RsoImportSymbol> Imports { get; }
    /// <summary>The immutable pre-link RSO image exactly as parsed.</summary>
    public ReadOnlyMemory<byte> RawData { get; }

    public static RsoFile Load(string path, RsoParseOptions? options = null) =>
        Parse(File.ReadAllBytes(path), options);

    public static RsoFile Parse(ReadOnlySpan<byte> data, RsoParseOptions? options = null)
    {
        if (data.Length < HeaderSize)
        {
            throw new InvalidDataException(
                $"RSO header requires 0x{HeaderSize:X} bytes, but input is only 0x{data.Length:X} bytes");
        }

        options ??= new RsoParseOptions();
        var raw = data.ToArray();
        var header = ParseHeader(raw);

        var sectionTableSize = checked((ulong)header.SectionCount * SectionEntrySize);
        ValidateRegion(raw.Length, header.SectionTableOffset, sectionTableSize, "section table");
        if (header.SectionCount > int.MaxValue)
        {
            throw new InvalidDataException($"RSO section count 0x{header.SectionCount:X8} is too large");
        }

        ValidateFixedTable(raw.Length, header.InternalRelocationTableOffset,
            header.InternalRelocationTableSize, RelocationEntrySize, "internal relocation table");
        ValidateFixedTable(raw.Length, header.ExternalRelocationTableOffset,
            header.ExternalRelocationTableSize, RelocationEntrySize, "external relocation table");
        ValidateFixedTable(raw.Length, header.ExportTableOffset,
            header.ExportTableSize, ExportEntrySize, "export table");
        ValidateFixedTable(raw.Length, header.ImportTableOffset,
            header.ImportTableSize, ImportEntrySize, "import table");

        var sections = ParseSections(raw, header, options);
        ValidateHeaderSectionReferences(header, sections);

        var rawModuleName = ParseModuleName(raw, header);
        var moduleName = DecodeFixedString(rawModuleName.Span);

        var imports = ParseImports(raw, header);
        var exports = ParseExports(raw, header, sections);
        var internalRelocations = ParseRelocations(
            raw,
            header.InternalRelocationTableOffset,
            header.InternalRelocationTableSize,
            RsoRelocationTableKind.Internal,
            sections.Count);
        var externalRelocations = ParseRelocations(
            raw,
            header.ExternalRelocationTableOffset,
            header.ExternalRelocationTableSize,
            RsoRelocationTableKind.External,
            imports.Count);

        return new RsoFile(
            header,
            moduleName,
            rawModuleName,
            new ReadOnlyCollection<RsoSection>(sections),
            new ReadOnlyCollection<RsoRelocation>(internalRelocations),
            new ReadOnlyCollection<RsoRelocation>(externalRelocations),
            new ReadOnlyCollection<RsoExportSymbol>(exports),
            new ReadOnlyCollection<RsoImportSymbol>(imports),
            raw);
    }

    private static RsoHeader ParseHeader(IReadOnlyList<byte> raw)
    {
        return new RsoHeader(
            ReadUInt32(raw, 0x00),
            ReadUInt32(raw, 0x04),
            ReadUInt32(raw, 0x08),
            ReadUInt32(raw, 0x0C),
            ReadUInt32(raw, 0x10),
            ReadUInt32(raw, 0x14),
            ReadUInt32(raw, 0x18),
            ReadUInt32(raw, 0x1C),
            raw[0x20],
            raw[0x21],
            raw[0x22],
            raw[0x23],
            ReadUInt32(raw, 0x24),
            ReadUInt32(raw, 0x28),
            ReadUInt32(raw, 0x2C),
            ReadUInt32(raw, 0x30),
            ReadUInt32(raw, 0x34),
            ReadUInt32(raw, 0x38),
            ReadUInt32(raw, 0x3C),
            ReadUInt32(raw, 0x40),
            ReadUInt32(raw, 0x44),
            ReadUInt32(raw, 0x48),
            ReadUInt32(raw, 0x4C),
            ReadUInt32(raw, 0x50),
            ReadUInt32(raw, 0x54));
    }

    private static List<RsoSection> ParseSections(
        IReadOnlyList<byte> raw,
        RsoHeader header,
        RsoParseOptions options)
    {
        var sections = new List<RsoSection>((int)header.SectionCount);
        for (var index = 0; index < (int)header.SectionCount; ++index)
        {
            var entryOffset = checked((int)((ulong)header.SectionTableOffset + (ulong)index * SectionEntrySize));
            var rawOffset = ReadUInt32(raw, entryOffset);
            var size = ReadUInt32(raw, entryOffset + 4);
            var fileOffset = options.SectionOffsetLowBitIsExecutable ? rawOffset & ~1u : rawOffset;
            bool? executable = options.SectionOffsetLowBitIsExecutable ? (rawOffset & 1u) != 0 : null;

            // Offset zero denotes null/BSS sections and therefore has no file payload to bound.
            if (fileOffset != 0)
            {
                ValidateRegion(raw.Count, fileOffset, size, $"section {index} payload");
            }

            sections.Add(new RsoSection(index, rawOffset, fileOffset, size, executable));
        }
        return sections;
    }

    private static void ValidateHeaderSectionReferences(RsoHeader header, IReadOnlyList<RsoSection> sections)
    {
        ValidateOptionalFunctionSection(sections, header.PrologSectionIndex, header.PrologOffset, "prolog");
        ValidateOptionalFunctionSection(sections, header.EpilogSectionIndex, header.EpilogOffset, "epilog");
        ValidateOptionalFunctionSection(sections, header.UnresolvedSectionIndex, header.UnresolvedOffset, "unresolved");

        if (header.BssSectionIndex != 0 && header.BssSectionIndex >= sections.Count)
        {
            throw new InvalidDataException(
                $"RSO BSS section index {header.BssSectionIndex} is outside {sections.Count} sections");
        }
    }

    private static void ValidateOptionalFunctionSection(
        IReadOnlyList<RsoSection> sections,
        byte sectionIndex,
        uint sectionOffset,
        string label)
    {
        if (sectionIndex == 0)
        {
            return;
        }
        if (sectionIndex >= sections.Count)
        {
            throw new InvalidDataException(
                $"RSO {label} section index {sectionIndex} is outside {sections.Count} sections");
        }
        if (sectionOffset >= sections[sectionIndex].Size)
        {
            throw new InvalidDataException(
                $"RSO {label} offset 0x{sectionOffset:X8} is outside section {sectionIndex} size 0x{sections[sectionIndex].Size:X8}");
        }
    }

    private static ReadOnlyMemory<byte> ParseModuleName(byte[] raw, RsoHeader header)
    {
        if (header.ModuleNameSize == 0)
        {
            return ReadOnlyMemory<byte>.Empty;
        }
        if (header.ModuleNameOffset == 0)
        {
            throw new InvalidDataException("RSO module name has a non-zero size but a zero offset");
        }

        ValidateRegion(raw.Length, header.ModuleNameOffset, header.ModuleNameSize, "module name");
        var offset = checked((int)header.ModuleNameOffset);
        var length = checked((int)header.ModuleNameSize);
        return raw.AsMemory(offset, length).ToArray();
    }

    private static List<RsoImportSymbol> ParseImports(byte[] raw, RsoHeader header)
    {
        var count = CheckedEntryCount(header.ImportTableSize, ImportEntrySize, "import table");
        if (count != 0)
        {
            ValidateNameTableBase(raw.Length, header.ImportNameTableOffset, "import name table");
        }

        var result = new List<RsoImportSymbol>(count);
        for (var index = 0; index < count; ++index)
        {
            var entryOffset = checked((int)((ulong)header.ImportTableOffset + (ulong)index * ImportEntrySize));
            var nameOffset = ReadUInt32(raw, entryOffset);
            var codeOffset = ReadUInt32(raw, entryOffset + 4);
            var importEntryOffset = ReadUInt32(raw, entryOffset + 8);
            var rawName = ReadCString(raw, header.ImportNameTableOffset, nameOffset, $"import {index} name");
            result.Add(new RsoImportSymbol(
                nameOffset,
                codeOffset,
                importEntryOffset,
                Encoding.Latin1.GetString(rawName.Span),
                rawName));
        }
        return result;
    }

    private static List<RsoExportSymbol> ParseExports(
        byte[] raw,
        RsoHeader header,
        IReadOnlyList<RsoSection> sections)
    {
        var count = CheckedEntryCount(header.ExportTableSize, ExportEntrySize, "export table");
        if (count != 0)
        {
            ValidateNameTableBase(raw.Length, header.ExportNameTableOffset, "export name table");
        }

        var result = new List<RsoExportSymbol>(count);
        for (var index = 0; index < count; ++index)
        {
            var entryOffset = checked((int)((ulong)header.ExportTableOffset + (ulong)index * ExportEntrySize));
            var nameOffset = ReadUInt32(raw, entryOffset);
            var symbolOffset = ReadUInt32(raw, entryOffset + 4);
            var sectionIndex = ReadUInt32(raw, entryOffset + 8);
            var elfHash = ReadUInt32(raw, entryOffset + 12);
            if (sectionIndex >= (uint)sections.Count)
            {
                throw new InvalidDataException(
                    $"RSO export {index} references section {sectionIndex}, but only {sections.Count} sections exist");
            }
            if (symbolOffset > sections[(int)sectionIndex].Size)
            {
                throw new InvalidDataException(
                    $"RSO export {index} offset 0x{symbolOffset:X8} exceeds section {sectionIndex} size 0x{sections[(int)sectionIndex].Size:X8}");
            }

            var rawName = ReadCString(raw, header.ExportNameTableOffset, nameOffset, $"export {index} name");
            result.Add(new RsoExportSymbol(
                nameOffset,
                symbolOffset,
                sectionIndex,
                elfHash,
                Encoding.Latin1.GetString(rawName.Span),
                rawName));
        }
        return result;
    }

    private static List<RsoRelocation> ParseRelocations(
        byte[] raw,
        uint tableOffset,
        uint tableSize,
        RsoRelocationTableKind tableKind,
        int symbolCount)
    {
        var label = tableKind == RsoRelocationTableKind.Internal
            ? "internal relocation table"
            : "external relocation table";
        var count = CheckedEntryCount(tableSize, RelocationEntrySize, label);
        var result = new List<RsoRelocation>(count);
        for (var index = 0; index < count; ++index)
        {
            var entryOffset = checked((int)((ulong)tableOffset + (ulong)index * RelocationEntrySize));
            var offset = ReadUInt32(raw, entryOffset);
            var info = ReadUInt32(raw, entryOffset + 4);
            var addend = ReadUInt32(raw, entryOffset + 8);
            var symbolIndex = info >> 8;
            var type = (RsoRelocationType)(info & 0xFFu);

            if (offset >= (uint)raw.Length)
            {
                throw new InvalidDataException(
                    $"RSO {tableKind.ToString().ToLowerInvariant()} relocation {index} offset 0x{offset:X8} is outside file bounds");
            }
            if (symbolIndex >= (uint)symbolCount)
            {
                var targetKind = tableKind == RsoRelocationTableKind.Internal ? "section" : "import";
                throw new InvalidDataException(
                    $"RSO {tableKind.ToString().ToLowerInvariant()} relocation {index} references {targetKind} {symbolIndex}, but only {symbolCount} exist");
            }
            if (KnownRelocationPatchWidth(type) is { } patchWidth && patchWidth != 0)
            {
                ValidateRegion(
                    raw.Length,
                    offset,
                    (ulong)patchWidth,
                    $"{tableKind.ToString().ToLowerInvariant()} relocation {index} patch");
            }

            result.Add(new RsoRelocation(tableKind, offset, info, addend));
        }
        return result;
    }

    private static int? KnownRelocationPatchWidth(RsoRelocationType type) => type switch
    {
        RsoRelocationType.R_PPC_NONE => 0,
        RsoRelocationType.R_PPC_ADDR16 or
        RsoRelocationType.R_PPC_ADDR16_LO or
        RsoRelocationType.R_PPC_ADDR16_HI or
        RsoRelocationType.R_PPC_ADDR16_HA => 2,
        RsoRelocationType.R_PPC_ADDR32 or
        RsoRelocationType.R_PPC_ADDR24 or
        RsoRelocationType.R_PPC_ADDR14 or
        RsoRelocationType.R_PPC_ADDR14_BRTAKEN or
        RsoRelocationType.R_PPC_ADDR14_BRNTAKEN or
        RsoRelocationType.R_PPC_REL24 or
        RsoRelocationType.R_PPC_REL14 or
        RsoRelocationType.R_PPC_REL14_BRTAKEN or
        RsoRelocationType.R_PPC_REL14_BRNTAKEN or
        RsoRelocationType.R_PPC_EMB_SDA21 => 4,
        _ => null,
    };

    private static ReadOnlyMemory<byte> ReadCString(
        byte[] raw,
        uint nameTableOffset,
        uint relativeOffset,
        string label)
    {
        var absolute = (ulong)nameTableOffset + relativeOffset;
        if (absolute >= (ulong)raw.Length)
        {
            throw new InvalidDataException(
                $"RSO {label} offset 0x{absolute:X} is outside file bounds");
        }

        var start = checked((int)absolute);
        var end = start;
        while (end < raw.Length && raw[end] != 0)
        {
            ++end;
        }
        if (end == raw.Length)
        {
            throw new InvalidDataException($"RSO {label} is not NUL-terminated before end of file");
        }

        return raw.AsMemory(start, end - start).ToArray();
    }

    private static string DecodeFixedString(ReadOnlySpan<byte> bytes)
    {
        var terminator = bytes.IndexOf((byte)0);
        if (terminator >= 0)
        {
            bytes = bytes[..terminator];
        }
        return Encoding.Latin1.GetString(bytes);
    }

    private static void ValidateNameTableBase(int fileLength, uint offset, string label)
    {
        if (offset >= (uint)fileLength)
        {
            throw new InvalidDataException($"RSO {label} offset 0x{offset:X8} is outside file bounds");
        }
    }

    private static void ValidateFixedTable(
        int fileLength,
        uint offset,
        uint size,
        int entrySize,
        string label)
    {
        if (size % (uint)entrySize != 0)
        {
            throw new InvalidDataException(
                $"RSO {label} size 0x{size:X8} is not a multiple of 0x{entrySize:X}");
        }
        ValidateRegion(fileLength, offset, size, label);
        _ = CheckedEntryCount(size, entrySize, label);
    }

    private static int CheckedEntryCount(uint tableSize, int entrySize, string label)
    {
        if (tableSize % (uint)entrySize != 0)
        {
            throw new InvalidDataException(
                $"RSO {label} size 0x{tableSize:X8} is not a multiple of 0x{entrySize:X}");
        }

        var count = tableSize / (uint)entrySize;
        if (count > int.MaxValue)
        {
            throw new InvalidDataException($"RSO {label} contains too many entries: 0x{count:X8}");
        }
        return (int)count;
    }

    private static void ValidateRegion(int fileLength, uint offset, ulong size, string label)
    {
        var end = (ulong)offset + size;
        if (end > (ulong)fileLength)
        {
            throw new InvalidDataException(
                $"RSO {label} range 0x{offset:X8}+0x{size:X} extends beyond file size 0x{fileLength:X}");
        }
    }

    private static uint ReadUInt32(IReadOnlyList<byte> data, int offset)
    {
        if (offset < 0 || (ulong)(uint)offset + 4u > (ulong)data.Count)
        {
            throw new InvalidDataException("Attempted to read past end of RSO payload");
        }

        Span<byte> buffer = stackalloc byte[4];
        buffer[0] = data[offset];
        buffer[1] = data[offset + 1];
        buffer[2] = data[offset + 2];
        buffer[3] = data[offset + 3];
        return BinaryPrimitives.ReadUInt32BigEndian(buffer);
    }
}
