namespace Translator.Core.Parsing.Rso;

public sealed record RsoSection(
    int Index,
    uint RawOffset,
    uint FileOffset,
    uint Size,
    bool? Executable)
{
    public bool HasFileData => FileOffset != 0 && Size != 0;
}
