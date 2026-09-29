using System.Buffers.Binary;
using System.Collections.ObjectModel;
using System.Text;
using Translator.Core.Parsing.Dol;

namespace Translator.Core.Parsing.Rso;

/// <summary>
/// Parser for CodeWarrior Wii selectable-export files (.sel). The container
/// shares the RSO header/export-table layout, but its on-disk section records
/// intentionally do not carry the live executable section bases. Those are
/// supplied by the executable at runtime and therefore by the caller here.
/// </summary>
public sealed class RsoSelectableFile
{
    // ELF SHN_ABS. CodeWarrior emits selectable exports such as _SDA_BASE_
    // and _SDA2_BASE_ with this section index; SymbolOffset is then already
    // the final absolute guest address rather than a section-relative offset.
    public const uint AbsoluteSectionIndex = 0xFFF1u;

    private RsoSelectableFile(uint sectionCount, IReadOnlyList<RsoExportSymbol> exports, ReadOnlyMemory<byte> rawData)
    {
        SectionCount = sectionCount;
        Exports = exports;
        RawData = rawData;
    }

    public uint SectionCount { get; }
    public IReadOnlyList<RsoExportSymbol> Exports { get; }
    public ReadOnlyMemory<byte> RawData { get; }

    public static RsoSelectableFile Load(string path) => Parse(File.ReadAllBytes(path));

    public static RsoSelectableFile Parse(ReadOnlySpan<byte> data)
    {
        if (data.Length < RsoFile.HeaderSize)
            throw new InvalidDataException("SEL header is truncated");

        var raw = data.ToArray();
        var sectionCount = ReadU32(raw, 0x08);
        var sectionTableOffset = ReadU32(raw, 0x0C);
        var exportTableOffset = ReadU32(raw, 0x40);
        var exportTableSize = ReadU32(raw, 0x44);
        var exportNameTableOffset = ReadU32(raw, 0x48);

        ValidateRegion(raw.Length, sectionTableOffset, checked((ulong)sectionCount * RsoFile.SectionEntrySize), "section table");
        if (exportTableSize % RsoFile.ExportEntrySize != 0)
            throw new InvalidDataException("SEL export table size is not a multiple of 0x10");
        ValidateRegion(raw.Length, exportTableOffset, exportTableSize, "export table");
        if (exportTableSize != 0 && exportNameTableOffset >= raw.Length)
            throw new InvalidDataException("SEL export name table is outside the file");

        var exportCount = checked((int)(exportTableSize / RsoFile.ExportEntrySize));
        var exports = new List<RsoExportSymbol>(exportCount);
        for (var index = 0; index < exportCount; ++index)
        {
            var entry = checked((int)((ulong)exportTableOffset + (ulong)index * RsoFile.ExportEntrySize));
            var nameOffset = ReadU32(raw, entry);
            var symbolOffset = ReadU32(raw, entry + 4);
            var sectionIndex = ReadU32(raw, entry + 8);
            var elfHash = ReadU32(raw, entry + 12);
            if (sectionIndex >= sectionCount && sectionIndex != AbsoluteSectionIndex)
                throw new InvalidDataException($"SEL export {index} references section {sectionIndex}, but only {sectionCount} exist");
            var rawName = ReadCString(raw, exportNameTableOffset, nameOffset, $"export {index}");
            exports.Add(new RsoExportSymbol(
                nameOffset,
                symbolOffset,
                sectionIndex,
                elfHash,
                Encoding.Latin1.GetString(rawName.Span),
                rawName));
        }

        return new RsoSelectableFile(
            sectionCount,
            new ReadOnlyCollection<RsoExportSymbol>(exports),
            raw);
    }

    private static ReadOnlyMemory<byte> ReadCString(byte[] raw, uint tableOffset, uint relativeOffset, string label)
    {
        var start64 = (ulong)tableOffset + relativeOffset;
        if (start64 >= (ulong)raw.Length)
            throw new InvalidDataException($"SEL {label} name offset is outside the file");
        var start = checked((int)start64);
        var end = Array.IndexOf(raw, (byte)0, start);
        if (end < 0)
            throw new InvalidDataException($"SEL {label} name is not NUL terminated");
        return raw.AsMemory(start, end - start).ToArray();
    }

    private static uint ReadU32(byte[] raw, int offset) =>
        BinaryPrimitives.ReadUInt32BigEndian(raw.AsSpan(offset, 4));

    private static void ValidateRegion(int length, uint offset, ulong size, string label)
    {
        var end = (ulong)offset + size;
        if (end > (ulong)length)
            throw new InvalidDataException($"SEL {label} 0x{offset:X8}+0x{size:X} exceeds file size 0x{length:X}");
    }
}

/// <summary>Exact-name symbol resolver over a selectable file and its live section bases.</summary>
public sealed class RsoSelectableSymbolProvider : IRsoSymbolProvider
{
    private readonly Dictionary<string, uint> _symbols = new(StringComparer.Ordinal);

    public RsoSelectableSymbolProvider(RsoSelectableFile selectable, IReadOnlyList<uint> sectionBases)
    {
        ArgumentNullException.ThrowIfNull(selectable);
        ArgumentNullException.ThrowIfNull(sectionBases);
        if ((uint)sectionBases.Count < selectable.SectionCount)
            throw new ArgumentException("SEL section base table is shorter than the selectable section table", nameof(sectionBases));

        foreach (var export in selectable.Exports)
        {
            if (export.SectionIndex == RsoSelectableFile.AbsoluteSectionIndex)
            {
                var absoluteValue = export.SymbolOffset;
                if (_symbols.TryGetValue(export.Name, out var existingAbsolute) && existingAbsolute != absoluteValue)
                    throw new InvalidDataException($"SEL export '{export.Name}' resolves ambiguously to 0x{existingAbsolute:X8} and 0x{absoluteValue:X8}");
                _symbols[export.Name] = absoluteValue;
                continue;
            }
            var sectionBase = sectionBases[checked((int)export.SectionIndex)];
            if (sectionBase == 0)
                continue;
            var value64 = (ulong)sectionBase + export.SymbolOffset;
            if (value64 > uint.MaxValue)
                throw new InvalidDataException($"SEL export '{export.Name}' address overflows 32-bit guest space");
            var value = (uint)value64;
            if (_symbols.TryGetValue(export.Name, out var existing) && existing != value)
                throw new InvalidDataException($"SEL export '{export.Name}' resolves ambiguously to 0x{existing:X8} and 0x{value:X8}");
            _symbols[export.Name] = value;
        }
    }

    public bool TryResolve(string name, out uint address) => _symbols.TryGetValue(name, out address);
}

/// <summary>
/// A main-executable function exported through the CodeWarrior selectable file.
/// These are linker-authored function roots, not heuristic pointer candidates.
/// </summary>
public sealed record RsoSelectableExecutableExportRoot(
    uint Address,
    string Name,
    uint SectionIndex,
    uint SymbolOffset);

/// <summary>
/// Resolves selectable exports against the live DOL section layout and retains
/// only addresses that land inside initialized executable DOL sections. Data,
/// BSS and unmapped selectable exports are never promoted to function roots.
/// </summary>
public static class RsoSelectableExecutableExportDiscovery
{
    public static IReadOnlyList<RsoSelectableExecutableExportRoot> Discover(
        RsoSelectableFile selectable,
        DolFile dol)
    {
        ArgumentNullException.ThrowIfNull(dol);
        return Discover(
            selectable,
            dol.Sections,
            RsoSelectableSectionBases.FromDol(dol, selectable.SectionCount));
    }

    public static IReadOnlyList<RsoSelectableExecutableExportRoot> Discover(
        RsoSelectableFile selectable,
        IReadOnlyList<DolSection> dolSections,
        IReadOnlyList<uint> sectionBases)
    {
        ArgumentNullException.ThrowIfNull(selectable);
        ArgumentNullException.ThrowIfNull(dolSections);
        ArgumentNullException.ThrowIfNull(sectionBases);
        if ((uint)sectionBases.Count < selectable.SectionCount)
            throw new ArgumentException("SEL section base table is shorter than the selectable section table", nameof(sectionBases));

        var executableSections = dolSections
            .Where(static section => section.IsExecutable && section.Size != 0)
            .ToArray();
        var roots = new List<RsoSelectableExecutableExportRoot>();
        var seen = new HashSet<uint>();

        foreach (var export in selectable.Exports)
        {
            // Absolute linker symbols are valid RSO imports, but they are not
            // section-relative DOL functions and therefore cannot be roots.
            if (export.SectionIndex == RsoSelectableFile.AbsoluteSectionIndex)
                continue;
            var sectionBase = sectionBases[checked((int)export.SectionIndex)];
            if (sectionBase == 0)
                continue;

            var address64 = (ulong)sectionBase + export.SymbolOffset;
            if (address64 > uint.MaxValue)
                throw new InvalidDataException($"SEL export '{export.Name}' address overflows 32-bit guest space");
            var address = (uint)address64;
            if (!executableSections.Any(section => section.Range.Contains(address)) || !seen.Add(address))
                continue;

            roots.Add(new RsoSelectableExecutableExportRoot(
                address,
                export.Name,
                export.SectionIndex,
                export.SymbolOffset));
        }

        return new ReadOnlyCollection<RsoSelectableExecutableExportRoot>(roots);
    }
}

/// <summary>
/// Builds the CodeWarrior main-executable section-base vector consumed by SEL
/// export lookup. The mapping mirrors the standard DOL layout used by the Wii
/// RSO runtime: .init/.text, .rodata/.data, BSS, .sdata/.sdata2 and the starts
/// of .sbss/.sbss2 at the corresponding small-data ends.
/// </summary>
public static class RsoSelectableSectionBases
{
    public static uint[] FromDol(DolFile dol, uint selectableSectionCount)
    {
        ArgumentNullException.ThrowIfNull(dol);
        return FromDolSections(dol.Sections, dol.BssAddress, selectableSectionCount);
    }

    public static uint[] FromDolSections(
        IReadOnlyList<DolSection> dolSections,
        uint bssAddress,
        uint selectableSectionCount)
    {
        ArgumentNullException.ThrowIfNull(dolSections);
        if (selectableSectionCount > int.MaxValue)
            throw new InvalidDataException("SEL section count is too large");
        var bases = new uint[checked((int)selectableSectionCount)];

        DolSection? Find(SectionKind kind, int originalIndex) =>
            dolSections.FirstOrDefault(section => section.Kind == kind && section.Index == originalIndex);
        void Set(int selIndex, uint value)
        {
            if ((uint)selIndex < selectableSectionCount)
                bases[selIndex] = value;
        }
        uint AddressOf(SectionKind kind, int index, string name)
        {
            var section = Find(kind, index)
                ?? throw new InvalidDataException($"DOL is missing {name} required by the selectable layout");
            return section.VirtualAddress;
        }
        uint EndOf(SectionKind kind, int index, string name)
        {
            var section = Find(kind, index)
                ?? throw new InvalidDataException($"DOL is missing {name} required by the selectable layout");
            return checked(section.VirtualAddress + section.Size);
        }

        Set(1, AddressOf(SectionKind.Text, 0, ".init"));
        Set(2, AddressOf(SectionKind.Text, 1, ".text"));
        Set(5, AddressOf(SectionKind.Data, 4, ".rodata"));
        Set(6, AddressOf(SectionKind.Data, 5, ".data"));
        Set(7, bssAddress);
        Set(8, AddressOf(SectionKind.Data, 6, ".sdata"));
        Set(9, AddressOf(SectionKind.Data, 7, ".sdata2"));
        Set(11, EndOf(SectionKind.Data, 6, ".sdata/.sbss boundary"));
        Set(12, EndOf(SectionKind.Data, 7, ".sdata2/.sbss2 boundary"));
        return bases;
    }
}
