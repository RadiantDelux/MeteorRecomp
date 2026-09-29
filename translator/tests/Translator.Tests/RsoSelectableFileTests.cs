using System.Buffers.Binary;
using System.Text;
using Translator.Core.Parsing.Dol;
using Translator.Core.Parsing.Rso;
using Xunit;

namespace Translator.Tests;

public class RsoSelectableFileTests
{
    [Fact]
    public void SelectableProviderResolvesExactExportAgainstCallerSectionBases()
    {
        var raw = BuildSelectable();
        var selectable = RsoSelectableFile.Parse(raw);
        var bases = new uint[13];
        bases[2] = 0x80008000u;
        bases[7] = 0x80700000u;
        var provider = new RsoSelectableSymbolProvider(selectable, bases);

        Assert.Equal(13u, selectable.SectionCount);
        Assert.Equal(2, selectable.Exports.Count);
        Assert.True(provider.TryResolve("Func", out var function));
        Assert.Equal(0x80008120u, function);
        Assert.True(provider.TryResolve("Data", out var data));
        Assert.Equal(0x80700040u, data);
        Assert.False(provider.TryResolve("func", out _));
    }

    [Fact]
    public void SelectableProviderResolvesCodeWarriorAbsoluteExportDirectly()
    {
        var selectable = RsoSelectableFile.Parse(BuildSelectableWithAbsoluteExport());
        var provider = new RsoSelectableSymbolProvider(selectable, new uint[13]);

        Assert.True(provider.TryResolve("_SDA_BASE_", out var sda));
        Assert.Equal(0x806C9CA0u, sda);
    }

    [Fact]
    public void DolSectionLayoutBuildsSelectableBasesWithoutAbsoluteAddressConstants()
    {
        var sections = new DolSection[]
        {
            Section(0, SectionKind.Text, 0x80004000u, 0x100u),
            Section(1, SectionKind.Text, 0x80008000u, 0x1000u),
            Section(4, SectionKind.Data, 0x80660000u, 0x200u),
            Section(5, SectionKind.Data, 0x80670000u, 0x300u),
            Section(6, SectionKind.Data, 0x80780000u, 0x80u),
            Section(7, SectionKind.Data, 0x80790000u, 0x40u),
        };

        var bases = RsoSelectableSectionBases.FromDolSections(sections, 0x806E0000u, 29u);

        Assert.Equal(0x80004000u, bases[1]);
        Assert.Equal(0x80008000u, bases[2]);
        Assert.Equal(0x80660000u, bases[5]);
        Assert.Equal(0x80670000u, bases[6]);
        Assert.Equal(0x806E0000u, bases[7]);
        Assert.Equal(0x80780000u, bases[8]);
        Assert.Equal(0x80790000u, bases[9]);
        Assert.Equal(0x80780080u, bases[11]);
        Assert.Equal(0x80790040u, bases[12]);
        Assert.Equal(0u, bases[10]);
        Assert.Equal(0u, bases[28]);
    }

    [Fact]
    public void SelectableParserRejectsTruncatedNameTable()
    {
        var raw = BuildSelectable();
        WriteU32(raw, 0xC0, 0xFFFFu);
        Assert.Throws<InvalidDataException>(() => RsoSelectableFile.Parse(raw));
    }

    [Fact]
    public void ExecutableExportDiscoveryKeepsOnlyMappedTextExports()
    {
        var selectable = RsoSelectableFile.Parse(BuildSelectableForRootDiscovery());
        var sections = new DolSection[]
        {
            Section(0, SectionKind.Text, 0x80004000u, 0x100u),
            Section(1, SectionKind.Text, 0x80008000u, 0x1000u),
            Section(5, SectionKind.Data, 0x80670000u, 0x300u),
            Section(6, SectionKind.Data, 0x80780000u, 0x80u),
        };
        var bases = new uint[13];
        bases[2] = 0x80008000u;
        bases[6] = 0x80670000u;
        bases[7] = 0x806E0000u;
        // Section 10 deliberately remains unmapped (base zero).

        var roots = RsoSelectableExecutableExportDiscovery.Discover(selectable, sections, bases);

        var root = Assert.Single(roots);
        Assert.Equal(0x80008120u, root.Address);
        Assert.Equal("Func", root.Name);
        Assert.Equal(2u, root.SectionIndex);
        Assert.Equal(0x120u, root.SymbolOffset);
    }

    [Fact]
    public void ExecutableExportDiscoveryRejectsAddressOverflow()
    {
        var selectable = RsoSelectableFile.Parse(BuildSelectable());
        var sections = new DolSection[]
        {
            Section(1, SectionKind.Text, 0x80008000u, 0x1000u),
        };
        var bases = new uint[13];
        bases[2] = 0xFFFFFFF0u;

        var error = Assert.Throws<InvalidDataException>(() =>
            RsoSelectableExecutableExportDiscovery.Discover(selectable, sections, bases));
        Assert.Contains("overflows 32-bit guest space", error.Message, StringComparison.Ordinal);
    }

    private static DolSection Section(int index, SectionKind kind, uint address, uint size) =>
        new(index, $"s{index}", kind, 0, address, size, new byte[size]);

    private static byte[] BuildSelectable()
    {
        var raw = new byte[0x120];
        WriteU32(raw, 0x08, 13u);
        WriteU32(raw, 0x0C, 0x58u);
        WriteU32(raw, 0x40, 0xC0u);
        WriteU32(raw, 0x44, 0x20u);
        WriteU32(raw, 0x48, 0xE0u);

        WriteU32(raw, 0xC0, 0u);
        WriteU32(raw, 0xC4, 0x120u);
        WriteU32(raw, 0xC8, 2u);
        WriteU32(raw, 0xCC, 0x11111111u);
        WriteU32(raw, 0xD0, 5u);
        WriteU32(raw, 0xD4, 0x40u);
        WriteU32(raw, 0xD8, 7u);
        WriteU32(raw, 0xDC, 0x22222222u);
        Encoding.ASCII.GetBytes("Func\0Data\0").CopyTo(raw, 0xE0);
        return raw;
    }

    private static byte[] BuildSelectableForRootDiscovery()
    {
        var raw = new byte[0x180];
        WriteU32(raw, 0x08, 13u);
        WriteU32(raw, 0x0C, 0x58u);
        WriteU32(raw, 0x40, 0xC0u);
        WriteU32(raw, 0x44, 0x40u);
        WriteU32(raw, 0x48, 0x100u);

        WriteExport(raw, 0xC0, 0u, 0x120u, 2u, 0x11111111u);
        WriteExport(raw, 0xD0, 5u, 0x40u, 6u, 0x22222222u);
        WriteExport(raw, 0xE0, 10u, 0x20u, 7u, 0x33333333u);
        WriteExport(raw, 0xF0, 14u, 0x10u, 10u, 0x44444444u);
        Encoding.ASCII.GetBytes("Func\0Data\0Bss\0Unmapped\0").CopyTo(raw, 0x100);
        return raw;
    }

    private static byte[] BuildSelectableWithAbsoluteExport()
    {
        var raw = new byte[0x100];
        WriteU32(raw, 0x08, 13u);
        WriteU32(raw, 0x0C, 0x58u);
        WriteU32(raw, 0x40, 0xC0u);
        WriteU32(raw, 0x44, 0x10u);
        WriteU32(raw, 0x48, 0xD0u);
        WriteExport(raw, 0xC0, 0u, 0x806C9CA0u, RsoSelectableFile.AbsoluteSectionIndex, 0x11111111u);
        Encoding.ASCII.GetBytes("_SDA_BASE_\0").CopyTo(raw, 0xD0);
        return raw;
    }

    private static void WriteExport(
        byte[] raw,
        int offset,
        uint nameOffset,
        uint symbolOffset,
        uint sectionIndex,
        uint elfHash)
    {
        WriteU32(raw, offset, nameOffset);
        WriteU32(raw, offset + 4, symbolOffset);
        WriteU32(raw, offset + 8, sectionIndex);
        WriteU32(raw, offset + 12, elfHash);
    }

    private static void WriteU32(byte[] raw, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(raw.AsSpan(offset, 4), value);
}
