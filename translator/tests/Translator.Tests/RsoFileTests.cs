using System.Buffers.Binary;
using System.Text;
using Translator.Core.Parsing.Rso;

namespace Translator.Tests;

public class RsoFileTests
{
    [Fact]
    public void ParsesStandardRsoStructureAndPreservesRawRecords()
    {
        var bytes = BuildRso();

        var rso = RsoFile.Parse(bytes);

        Assert.Equal((uint)RsoFile.HeaderSize, rso.Header.SectionTableOffset);
        Assert.Equal(4u, rso.Header.SectionCount);
        Assert.Equal(1u, rso.Header.Version);
        Assert.Equal(0x10u, rso.Header.BssSize);
        Assert.Equal("test.rso", rso.ModuleName);
        Assert.Equal(Encoding.Latin1.GetBytes("test.rso\0"), rso.RawModuleName.ToArray());

        Assert.Collection(
            rso.Sections,
            section =>
            {
                Assert.Equal(0, section.Index);
                Assert.Equal(0u, section.FileOffset);
                Assert.Equal(0u, section.Size);
                Assert.Null(section.Executable);
            },
            section =>
            {
                Assert.Equal(1, section.Index);
                Assert.Equal(0xA0u, section.RawOffset);
                Assert.Equal(0xA0u, section.FileOffset);
                Assert.Equal(0x20u, section.Size);
                Assert.True(section.HasFileData);
                Assert.Null(section.Executable);
            },
            section =>
            {
                Assert.Equal(2, section.Index);
                Assert.Equal(0xC0u, section.FileOffset);
                Assert.Equal(0x20u, section.Size);
            },
            section =>
            {
                Assert.Equal(3, section.Index);
                Assert.Equal(0u, section.FileOffset);
                Assert.Equal(0x10u, section.Size);
                Assert.False(section.HasFileData);
            });

        Assert.Collection(
            rso.InternalRelocations,
            relocation =>
            {
                Assert.Equal(RsoRelocationTableKind.Internal, relocation.TableKind);
                Assert.Equal(0xA4u, relocation.Offset);
                Assert.Equal(0x00000101u, relocation.RawInfo);
                Assert.Equal(1u, relocation.SymbolIndex);
                Assert.Equal(RsoRelocationType.R_PPC_ADDR32, relocation.Type);
                Assert.Equal(4u, relocation.Addend);
            },
            relocation =>
            {
                Assert.Equal(0xC8u, relocation.Offset);
                Assert.Equal(2u, relocation.SymbolIndex);
                Assert.Equal(RsoRelocationType.R_PPC_ADDR16_HA, relocation.Type);
                Assert.Equal(8u, relocation.Addend);
            });

        var external = Assert.Single(rso.ExternalRelocations);
        Assert.Equal(RsoRelocationTableKind.External, external.TableKind);
        Assert.Equal(0xACu, external.Offset);
        Assert.Equal(1u, external.SymbolIndex);
        Assert.Equal(RsoRelocationType.R_PPC_REL24, external.Type);
        Assert.Equal(0u, external.Addend);

        Assert.Collection(
            rso.Exports,
            export =>
            {
                Assert.Equal("Func", export.Name);
                Assert.Equal(Encoding.Latin1.GetBytes("Func"), export.RawName.ToArray());
                Assert.Equal(0u, export.NameOffset);
                Assert.Equal(4u, export.SymbolOffset);
                Assert.Equal(1u, export.SectionIndex);
                Assert.Equal(0x12345678u, export.ElfHash);
            },
            export =>
            {
                Assert.Equal("Data", export.Name);
                Assert.Equal(5u, export.NameOffset);
                Assert.Equal(8u, export.SymbolOffset);
                Assert.Equal(2u, export.SectionIndex);
                Assert.Equal(0x9ABCDEF0u, export.ElfHash);
            });

        Assert.Collection(
            rso.Imports,
            import =>
            {
                Assert.Equal("Ext", import.Name);
                Assert.Equal(Encoding.Latin1.GetBytes("Ext"), import.RawName.ToArray());
                Assert.Equal(0u, import.NameOffset);
                Assert.Equal(0x10u, import.CodeOffset);
                Assert.Equal(0x20u, import.EntryOffset);
            },
            import =>
            {
                Assert.Equal("Other", import.Name);
                Assert.Equal(4u, import.NameOffset);
                Assert.Equal(0x30u, import.CodeOffset);
                Assert.Equal(0x40u, import.EntryOffset);
            });
    }

    [Fact]
    public void LowBitExecutableSectionEncodingIsExplicitOptIn()
    {
        var bytes = BuildRso();
        WriteU32(bytes, 0x58 + 8, 0xA1);

        var standard = RsoFile.Parse(bytes);
        Assert.Equal(0xA1u, standard.Sections[1].FileOffset);
        Assert.Null(standard.Sections[1].Executable);

        var flagged = RsoFile.Parse(bytes, new RsoParseOptions
        {
            SectionOffsetLowBitIsExecutable = true,
        });
        Assert.Equal(0xA1u, flagged.Sections[1].RawOffset);
        Assert.Equal(0xA0u, flagged.Sections[1].FileOffset);
        Assert.True(flagged.Sections[1].Executable);
        Assert.False(flagged.Sections[2].Executable);
    }

    [Fact]
    public void UnknownRelocationTypeIsPreservedWithoutInventingSemantics()
    {
        var bytes = BuildRso();
        WriteU32(bytes, 0xE4, 0x000001FEu);

        var relocation = RsoFile.Parse(bytes).InternalRelocations[0];

        Assert.Equal(0x000001FEu, relocation.RawInfo);
        Assert.Equal(1u, relocation.SymbolIndex);
        Assert.Equal((RsoRelocationType)0xFE, relocation.Type);
    }

    [Theory]
    [InlineData(0x34, 0x0D)]
    [InlineData(0x3C, 0x0D)]
    [InlineData(0x44, 0x11)]
    [InlineData(0x50, 0x0D)]
    public void RejectsMisalignedFixedTableSizes(int headerSizeOffset, uint malformedSize)
    {
        var bytes = BuildRso();
        WriteU32(bytes, headerSizeOffset, malformedSize);

        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(bytes));
    }

    [Fact]
    public void RejectsTruncationAndOverflowingRegions()
    {
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(new byte[RsoFile.HeaderSize - 1]));

        var sectionCountOverflow = BuildRso();
        WriteU32(sectionCountOverflow, 0x08, uint.MaxValue);
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(sectionCountOverflow));

        var tableRangeOverflow = BuildRso();
        WriteU32(tableRangeOverflow, 0x38, 0xFFFFFFF8u);
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(tableRangeOverflow));

        var sectionPayloadOverflow = BuildRso();
        WriteU32(sectionPayloadOverflow, 0x58 + 8, 0x1F0u);
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(sectionPayloadOverflow));

        var moduleNameOverflow = BuildRso();
        WriteU32(moduleNameOverflow, 0x10, 0x1FCu);
        WriteU32(moduleNameOverflow, 0x14, 0x10u);
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(moduleNameOverflow));
    }

    [Fact]
    public void RejectsInvalidSymbolNamesAndReferences()
    {
        var exportNameOutsideFile = BuildRso();
        WriteU32(exportNameOutsideFile, 0x110, 0x100u);
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(exportNameOutsideFile));

        var unterminatedImportName = BuildRso();
        WriteU32(unterminatedImportName, 0x148, 0x9Fu);
        for (var i = 0x1FF; i < unterminatedImportName.Length; ++i)
        {
            unterminatedImportName[i] = 0x41;
        }
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(unterminatedImportName));

        var badExportSection = BuildRso();
        WriteU32(badExportSection, 0x118, 4u);
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(badExportSection));

        var badExportOffset = BuildRso();
        WriteU32(badExportOffset, 0x114, 0x21u);
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(badExportOffset));

        var badInternalSection = BuildRso();
        WriteU32(badInternalSection, 0xE4, 0x00000401u);
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(badInternalSection));

        var badExternalImport = BuildRso();
        WriteU32(badExternalImport, 0xFC, 0x0000020Au);
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(badExternalImport));

        var relocationPatchPastEnd = BuildRso();
        WriteU32(relocationPatchPastEnd, 0xE0, 0x1FEu);
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(relocationPatchPastEnd));
    }

    [Fact]
    public void RejectsInvalidLifecycleSectionReferences()
    {
        var badSection = BuildRso();
        badSection[0x20] = 4;
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(badSection));

        var badOffset = BuildRso();
        WriteU32(badOffset, 0x24, 0x20u);
        Assert.Throws<InvalidDataException>(() => RsoFile.Parse(badOffset));
    }

    private static byte[] BuildRso()
    {
        var bytes = new byte[0x200];

        WriteU32(bytes, 0x08, 4u);
        WriteU32(bytes, 0x0C, RsoFile.HeaderSize);
        WriteU32(bytes, 0x10, 0x80u);
        WriteU32(bytes, 0x14, 9u);
        WriteU32(bytes, 0x18, 1u);
        WriteU32(bytes, 0x1C, 0x10u);
        bytes[0x20] = 1;
        bytes[0x21] = 1;
        bytes[0x22] = 1;
        bytes[0x23] = 3;
        WriteU32(bytes, 0x24, 0u);
        WriteU32(bytes, 0x28, 4u);
        WriteU32(bytes, 0x2C, 8u);
        WriteU32(bytes, 0x30, 0xE0u);
        WriteU32(bytes, 0x34, 0x18u);
        WriteU32(bytes, 0x38, 0xF8u);
        WriteU32(bytes, 0x3C, 0x0Cu);
        WriteU32(bytes, 0x40, 0x110u);
        WriteU32(bytes, 0x44, 0x20u);
        WriteU32(bytes, 0x48, 0x130u);
        WriteU32(bytes, 0x4C, 0x148u);
        WriteU32(bytes, 0x50, 0x18u);
        WriteU32(bytes, 0x54, 0x160u);

        // Standard RSO section entries are absolute file offsets plus size. Section 3 models BSS.
        WriteU32(bytes, 0x58 + 0x00, 0u);
        WriteU32(bytes, 0x58 + 0x04, 0u);
        WriteU32(bytes, 0x58 + 0x08, 0xA0u);
        WriteU32(bytes, 0x58 + 0x0C, 0x20u);
        WriteU32(bytes, 0x58 + 0x10, 0xC0u);
        WriteU32(bytes, 0x58 + 0x14, 0x20u);
        WriteU32(bytes, 0x58 + 0x18, 0u);
        WriteU32(bytes, 0x58 + 0x1C, 0x10u);

        Encoding.Latin1.GetBytes("test.rso\0").CopyTo(bytes, 0x80);

        WriteRelocation(bytes, 0xE0, 0xA4u, 1u, RsoRelocationType.R_PPC_ADDR32, 4u);
        WriteRelocation(bytes, 0xEC, 0xC8u, 2u, RsoRelocationType.R_PPC_ADDR16_HA, 8u);
        WriteRelocation(bytes, 0xF8, 0xACu, 1u, RsoRelocationType.R_PPC_REL24, 0u);

        WriteExport(bytes, 0x110, 0u, 4u, 1u, 0x12345678u);
        WriteExport(bytes, 0x120, 5u, 8u, 2u, 0x9ABCDEF0u);
        Encoding.Latin1.GetBytes("Func\0Data\0").CopyTo(bytes, 0x130);

        WriteImport(bytes, 0x148, 0u, 0x10u, 0x20u);
        WriteImport(bytes, 0x154, 4u, 0x30u, 0x40u);
        Encoding.Latin1.GetBytes("Ext\0Other\0").CopyTo(bytes, 0x160);

        return bytes;
    }

    private static void WriteRelocation(
        byte[] bytes,
        int offset,
        uint patchOffset,
        uint symbolIndex,
        RsoRelocationType type,
        uint addend)
    {
        WriteU32(bytes, offset, patchOffset);
        WriteU32(bytes, offset + 4, (symbolIndex << 8) | (byte)type);
        WriteU32(bytes, offset + 8, addend);
    }

    private static void WriteExport(
        byte[] bytes,
        int offset,
        uint nameOffset,
        uint symbolOffset,
        uint sectionIndex,
        uint elfHash)
    {
        WriteU32(bytes, offset, nameOffset);
        WriteU32(bytes, offset + 4, symbolOffset);
        WriteU32(bytes, offset + 8, sectionIndex);
        WriteU32(bytes, offset + 12, elfHash);
    }

    private static void WriteImport(
        byte[] bytes,
        int offset,
        uint nameOffset,
        uint codeOffset,
        uint entryOffset)
    {
        WriteU32(bytes, offset, nameOffset);
        WriteU32(bytes, offset + 4, codeOffset);
        WriteU32(bytes, offset + 8, entryOffset);
    }

    private static void WriteU32(byte[] bytes, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(offset, 4), value);
}
