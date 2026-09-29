namespace Translator.Core.Parsing.Rso;

public enum RsoRelocationType : byte
{
    R_PPC_NONE = 0,
    R_PPC_ADDR32 = 1,
    R_PPC_ADDR24 = 2,
    R_PPC_ADDR16 = 3,
    R_PPC_ADDR16_LO = 4,
    R_PPC_ADDR16_HI = 5,
    R_PPC_ADDR16_HA = 6,
    R_PPC_ADDR14 = 7,
    R_PPC_ADDR14_BRTAKEN = 8,
    R_PPC_ADDR14_BRNTAKEN = 9,
    R_PPC_REL24 = 10,
    R_PPC_REL14 = 11,
    R_PPC_REL14_BRTAKEN = 12,
    R_PPC_REL14_BRNTAKEN = 13,
    R_PPC_EMB_SDA21 = 109,
}

public enum RsoRelocationTableKind : byte
{
    Internal,
    External,
}

public sealed record RsoRelocation(
    RsoRelocationTableKind TableKind,
    uint Offset,
    uint RawInfo,
    uint Addend)
{
    public uint SymbolIndex => RawInfo >> 8;
    public RsoRelocationType Type => (RsoRelocationType)(RawInfo & 0xFFu);
}
