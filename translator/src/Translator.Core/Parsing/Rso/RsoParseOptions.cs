namespace Translator.Core.Parsing.Rso;

public sealed record RsoParseOptions
{
    /// <summary>
    /// The standard RSO section entry is only an absolute file offset and size, so executable state
    /// is unknown by default. Some Wii producers reuse the REL low-bit executable marker. Enable
    /// this only when that encoding is known for the input; RawOffset always preserves the file word.
    /// </summary>
    public bool SectionOffsetLowBitIsExecutable { get; init; }
}
