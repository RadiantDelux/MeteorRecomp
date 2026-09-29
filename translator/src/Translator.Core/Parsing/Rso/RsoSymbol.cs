namespace Translator.Core.Parsing.Rso;

public sealed record RsoExportSymbol(
    uint NameOffset,
    uint SymbolOffset,
    uint SectionIndex,
    uint ElfHash,
    string Name,
    ReadOnlyMemory<byte> RawName);

public sealed record RsoImportSymbol(
    uint NameOffset,
    uint CodeOffset,
    uint EntryOffset,
    string Name,
    ReadOnlyMemory<byte> RawName);
