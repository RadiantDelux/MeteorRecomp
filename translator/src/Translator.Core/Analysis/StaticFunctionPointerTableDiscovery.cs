using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;

namespace Translator.Core.Analysis;

public sealed record StaticFunctionPointerTable(
    uint Address,
    IReadOnlyList<uint> Targets,
    bool HasNullHeaderPreamble = false,
    bool HasCodeWarriorSentinelPreamble = false,
    bool HasDedicatedSectionCallbackShape = false);

public sealed record StaticFunctionPointerTableDiscoveryResult(
    IReadOnlyList<StaticFunctionPointerTable> Tables,
    IReadOnlyList<uint> IsolatedTargets,
    int PointerEntryCount);

/// <summary>
/// Finds dense initialized-data runs whose words all look like executable PPC addresses.
///
/// This deliberately does not claim that a run is a function table: switch tables have the same
/// byte shape. Callers must provide semantic evidence before promoting any target to a function
/// boundary (for example, multiple entries in the run are already proven function starts and the
/// new targets are outside all already-decoded control-flow coverage).
/// </summary>
public static class StaticFunctionPointerTableDiscovery
{
    private const int MinimumRunLength = 2;

    public static StaticFunctionPointerTableDiscoveryResult Discover(DolFile dol, ProgramImage image)
    {
        ArgumentNullException.ThrowIfNull(dol);
        ArgumentNullException.ThrowIfNull(image);

        var executableRanges = dol.Sections
            .Where(static section => section.IsExecutable && section.Size != 0)
            .Select(static section => section.Range)
            .ToArray();

        bool LooksLikeExecutableEntry(uint address)
        {
            if ((address & 3u) != 0 ||
                !executableRanges.Any(range => range.Contains(address)) ||
                !image.Contains(address, sizeof(uint)))
            {
                return false;
            }

            var offset = image.GetOffset(address, sizeof(uint));
            var word = BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(offset, sizeof(uint)));
            if (word is 0 or 0xFFFFFFFFu)
            {
                return false;
            }

            var instruction = PpcDecoder.Decode(address, word);
            return !instruction.Mnemonic.StartsWith("unk", StringComparison.OrdinalIgnoreCase);
        }

        var tables = new List<StaticFunctionPointerTable>();
        var isolatedTargets = new HashSet<uint>();
        var pointerEntries = 0;
        foreach (var section in dol.Sections.Where(static section =>
                     section.Kind == SectionKind.Data && section.HasData && section.Size >= 8))
        {
            var span = section.Data.Span;
            var cursor = 0;
            while (cursor + 4 <= span.Length)
            {
                var first = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(cursor, 4));
                if (!LooksLikeExecutableEntry(first))
                {
                    cursor += 4;
                    continue;
                }

                var start = cursor;
                var targets = new List<uint>();
                while (cursor + 4 <= span.Length)
                {
                    var candidate = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(cursor, 4));
                    if (!LooksLikeExecutableEntry(candidate))
                    {
                        break;
                    }

                    targets.Add(candidate);
                    cursor += 4;
                }

                if (targets.Count >= MinimumRunLength)
                {
                    // A sizeable family of CodeWarrior Wii vtables uses a null RTTI/header pair
                    // rather than [RTTI pointer, this adjustment]. Keep that shape as metadata only:
                    // it is not sufficient by itself to claim function boundaries because a jump
                    // table can also be preceded by zero-filled data. The later stored-header pass
                    // adds constructor/vptr evidence before any target is promoted.
                    var hasNullHeaderPreamble = start >= 8 &&
                                                BinaryPrimitives.ReadUInt32BigEndian(span.Slice(start - 8, 4)) == 0 &&
                                                BinaryPrimitives.ReadUInt32BigEndian(span.Slice(start - 4, 4)) == 0;
                    // CodeWarrior's global constructor/destructor arrays use a very
                    // distinctive [-1, function..., 0] shape. Unlike an ordinary
                    // dense executable-pointer run, this sentinel/terminator pair is
                    // strong enough to identify a callback table without requiring an
                    // already-translated member as an anchor.
                    var hasCodeWarriorSentinelPreamble =
                        start >= 4 &&
                        BinaryPrimitives.ReadUInt32BigEndian(span.Slice(start - 4, 4)) == 0xFFFFFFFFu &&
                        cursor + 4 <= span.Length &&
                        BinaryPrimitives.ReadUInt32BigEndian(span.Slice(cursor, 4)) == 0;
                    // DOL conversion can place CodeWarrior's .ctors/.dtors in their own data
                    // section and omit the ELF-side -1 sentinel from that section. In that form
                    // the section itself is the delimiter: it starts with a dense run of code
                    // pointers and every remaining word is zero padding. This is substantially
                    // stronger evidence than an arbitrary pointer run embedded in rodata and is
                    // safe to use as a startup/teardown callback root source.
                    var hasDedicatedSectionCallbackShape =
                        start == 0 &&
                        cursor < span.Length &&
                        (span.Length - cursor) % sizeof(uint) == 0;
                    if (hasDedicatedSectionCallbackShape)
                    {
                        for (var tail = cursor; tail + sizeof(uint) <= span.Length; tail += sizeof(uint))
                        {
                            if (BinaryPrimitives.ReadUInt32BigEndian(span.Slice(tail, sizeof(uint))) != 0)
                            {
                                hasDedicatedSectionCallbackShape = false;
                                break;
                            }
                        }
                    }
                    tables.Add(new StaticFunctionPointerTable(
                        checked(section.VirtualAddress + (uint)start),
                        targets.ToArray(),
                        hasNullHeaderPreamble,
                        hasCodeWarriorSentinelPreamble,
                        hasDedicatedSectionCallbackShape));
                    pointerEntries += targets.Count;
                }
                else if (targets.Count == 1)
                {
                    // An isolated executable pointer surrounded by scalar/data words is a common
                    // callback-in-struct shape. Keep it as a candidate only; the CLI still requires
                    // the much stronger proof that it equals a decoded function's exclusive end and
                    // lies outside every translated control-flow range before promoting it.
                    isolatedTargets.Add(targets[0]);
                    pointerEntries++;
                }

                // If the run was only one pointer, cursor already points at the first non-code word.
                // Otherwise it points there as well, so the outer loop can inspect that delimiter.
            }
        }

        return new StaticFunctionPointerTableDiscoveryResult(
            tables,
            isolatedTargets.OrderBy(static address => address).ToArray(),
            pointerEntries);
    }
}

public sealed record StoredNullHeaderVTableDiscoveryResult(
    IReadOnlyList<uint> Targets,
    int TableSegmentCount);

public sealed record StoredStridedCallbackDescriptorDiscoveryResult(
    IReadOnlyList<uint> Targets,
    int TableSegmentCount,
    int CandidateTableCount);

/// <summary>
/// Promotes siblings from null-header CodeWarrior vtables only when translated PPC actually stores
/// the table-header address to memory (constructor/vptr evidence). This is deliberately stricter
/// than ordinary vtable discovery: the dense run must also have class-like function-boundary density,
/// and each new candidate must be outside translated control-flow coverage and immediately follow a
/// terminal PPC transfer (return or non-link unconditional branch). CodeWarrior emits both ordinary
/// functions and packed tail-call thunks in vtables, and both forms have no fallthrough into the next
/// entry. Those constraints let null-RTTI tables participate without turning
/// switch labels or arbitrary executable-looking data words into function starts.
/// </summary>
public static class StoredNullHeaderVTableDiscovery
{
    private const int MinimumBoundaryEvidenceCount = 2;
    private const uint BlrInstruction = 0x4E800020u;

    public static StoredNullHeaderVTableDiscoveryResult Discover(
        StaticFunctionPointerTableDiscoveryResult pointerTables,
        DolFile dol,
        ProgramImage image,
        IReadOnlySet<uint> provenFunctionStarts,
        IReadOnlySet<uint> coveredInstructions,
        IReadOnlySet<uint>? storedPointerValues = null)
    {
        ArgumentNullException.ThrowIfNull(pointerTables);
        ArgumentNullException.ThrowIfNull(dol);
        ArgumentNullException.ThrowIfNull(image);
        ArgumentNullException.ThrowIfNull(provenFunctionStarts);
        ArgumentNullException.ThrowIfNull(coveredInstructions);

        var executableRanges = dol.Sections
            .Where(static section => section.IsExecutable && section.Size != 0)
            .Select(static section => section.Range)
            .ToArray();
        var targets = new HashSet<uint>();
        var tableSegments = 0;
        foreach (var table in pointerTables.Tables)
        {
            if (!table.HasNullHeaderPreamble)
            {
                continue;
            }

            var headerAddress = table.Address - 8u;
            var hasStoredHeaderPointer = storedPointerValues?.Contains(headerAddress) == true;
            if (!hasStoredHeaderPointer)
            {
                continue;
            }

            // A constructor-like store of the table address is required, but it still
            // cannot make every executable-looking word a function boundary. Require
            // a class-like boundary pattern across the whole run: at least 80% of
            // distinct targets are either proven starts or sit immediately after a raw
            // blr. Real CodeWarrior null-header families have this shape; a stored
            // switch-table address does not gain carte blanche merely because it has
            // translated case-label anchors.
            var distinctTargets = table.Targets.Distinct().ToArray();
            var provenAnchorCount = distinctTargets.Count(provenFunctionStarts.Contains);
            var hasDenseProvenAnchors =
                provenAnchorCount >= MinimumBoundaryEvidenceCount &&
                provenAnchorCount * 5 >= distinctTargets.Length * 4;
            var boundaryEvidence = distinctTargets.Count(candidate =>
                provenFunctionStarts.Contains(candidate) ||
                ImmediatelyFollowsTerminalTransfer(image, executableRanges, candidate));
            if (boundaryEvidence < MinimumBoundaryEvidenceCount ||
                boundaryEvidence * 5 < distinctTargets.Length * 4)
            {
                continue;
            }

            // A second CodeWarrior shape is a compact mixed table where one method is
            // already proven and return-delimited, while its packed sibling thunks start
            // immediately after unconditional non-link tail branches. If every distinct
            // target in the exact stored-header table has terminal-boundary evidence, the
            // return-delimited proven method is strong enough to anchor those tail siblings.
            // This deliberately does not relax the all-tail case below: a table made only
            // of branch thunks still needs the existing dense proven-anchor rule.
            var hasReturnAnchoredAllTerminalFamily =
                distinctTargets.Any(candidate =>
                    provenFunctionStarts.Contains(candidate) &&
                    ImmediatelyFollowsReturn(image, executableRanges, candidate)) &&
                distinctTargets.All(candidate =>
                    ImmediatelyFollowsTerminalTransfer(image, executableRanges, candidate));

            var tableAdded = false;
            foreach (var candidate in table.Targets)
            {
                var followsReturn = ImmediatelyFollowsReturn(image, executableRanges, candidate);
                var followsTailBranch = !followsReturn &&
                                        ImmediatelyFollowsTerminalTransfer(image, executableRanges, candidate);
                if (provenFunctionStarts.Contains(candidate) ||
                    coveredInstructions.Contains(candidate) ||
                    (!followsReturn && !((hasDenseProvenAnchors || hasReturnAnchoredAllTerminalFamily) && followsTailBranch)))
                {
                    continue;
                }

                if (targets.Add(candidate))
                {
                    tableAdded = true;
                }
            }

            if (tableAdded)
            {
                tableSegments++;
            }
        }

        return new StoredNullHeaderVTableDiscoveryResult(
            targets.OrderBy(static address => address).ToArray(),
            tableSegments);
    }

    private static bool ImmediatelyFollowsReturn(
        ProgramImage image,
        IReadOnlyList<AddressRange> executableRanges,
        uint address)
    {
        if (address < sizeof(uint))
        {
            return false;
        }

        var previousAddress = address - sizeof(uint);
        if (!executableRanges.Any(range => range.Contains(previousAddress)) ||
            !image.Contains(previousAddress, sizeof(uint)))
        {
            return false;
        }

        var offset = image.GetOffset(previousAddress, sizeof(uint));
        return BinaryPrimitives.ReadUInt32BigEndian(
            image.Memory.AsSpan(offset, sizeof(uint))) == BlrInstruction;
    }

    private static bool ImmediatelyFollowsTerminalTransfer(
        ProgramImage image,
        IReadOnlyList<AddressRange> executableRanges,
        uint address)
    {
        if (address < sizeof(uint))
        {
            return false;
        }

        var previousAddress = address - sizeof(uint);
        if (!executableRanges.Any(range => range.Contains(previousAddress)) ||
            !image.Contains(previousAddress, sizeof(uint)))
        {
            return false;
        }

        var offset = image.GetOffset(previousAddress, sizeof(uint));
        var previousWord = BinaryPrimitives.ReadUInt32BigEndian(
            image.Memory.AsSpan(offset, sizeof(uint)));
        if (previousWord == BlrInstruction)
        {
            return true;
        }

        // A plain `b target` is just as terminal as blr for boundary purposes:
        // it has no architectural fallthrough and is the standard CodeWarrior
        // shape for tiny virtual-method adjustment/tail-call thunks. Conditional
        // branches and linked branches (calls) are deliberately excluded by the
        // decoder's IsUnconditionalBranch predicate.
        return PpcDecoder.Decode(previousAddress, previousWord).IsUnconditionalBranch;
    }
}

/// <summary>
/// Finds CodeWarrior-style callback/interface descriptor families laid out as
/// repeated {0, 0, function} triples.  The shape alone is intentionally not an
/// oracle: at least one descriptor base must have been materialized and stored
/// to an object field by already-translated PPC, and the family must have dense
/// real function-boundary evidence.  This admits tables addressed through
/// descriptor+8 (the ABI shape used by several Wii interfaces) without treating
/// ordinary zero-padded data as function starts.
/// </summary>
public static class StoredStridedCallbackDescriptorDiscovery
{
    private const int DescriptorSize = 12;
    private const int MinimumDescriptorCount = 3;
    private const int MinimumBoundaryEvidenceCount = 2;
    private const uint BlrInstruction = 0x4E800020u;

    private sealed record Candidate(
        uint Address,
        IReadOnlyList<uint> DescriptorAddresses,
        IReadOnlyList<uint> Targets);

    public static StoredStridedCallbackDescriptorDiscoveryResult Discover(
        DolFile dol,
        ProgramImage image,
        IReadOnlySet<uint> provenFunctionStarts,
        IReadOnlySet<uint> storedPointerValues)
    {
        ArgumentNullException.ThrowIfNull(dol);
        ArgumentNullException.ThrowIfNull(image);
        ArgumentNullException.ThrowIfNull(provenFunctionStarts);
        ArgumentNullException.ThrowIfNull(storedPointerValues);

        var executableRanges = dol.Sections
            .Where(static section => section.IsExecutable && section.Size != 0)
            .Select(static section => section.Range)
            .ToArray();

        bool LooksLikeExecutableEntry(uint address)
        {
            if ((address & 3u) != 0 ||
                !executableRanges.Any(range => range.Contains(address)) ||
                !image.Contains(address, sizeof(uint)))
            {
                return false;
            }

            var offset = image.GetOffset(address, sizeof(uint));
            var word = BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(offset, sizeof(uint)));
            if (word is 0 or 0xFFFFFFFFu)
            {
                return false;
            }

            return !PpcDecoder.Decode(address, word).Mnemonic.StartsWith(
                "unk", StringComparison.OrdinalIgnoreCase);
        }

        var candidates = new List<Candidate>();
        foreach (var section in dol.Sections.Where(static section =>
                     section.Kind == SectionKind.Data && section.HasData && section.Size >= DescriptorSize * MinimumDescriptorCount))
        {
            var span = section.Data.Span;
            var cursor = 0;
            while (cursor + DescriptorSize <= span.Length)
            {
                if (!IsDescriptor(span, cursor, LooksLikeExecutableEntry))
                {
                    cursor += sizeof(uint);
                    continue;
                }

                var start = cursor;
                var descriptorAddresses = new List<uint>();
                var targets = new List<uint>();
                while (cursor + DescriptorSize <= span.Length &&
                       IsDescriptor(span, cursor, LooksLikeExecutableEntry))
                {
                    descriptorAddresses.Add(checked(section.VirtualAddress + (uint)cursor));
                    targets.Add(BinaryPrimitives.ReadUInt32BigEndian(span.Slice(cursor + 8, 4)));
                    cursor += DescriptorSize;
                }

                if (targets.Count >= MinimumDescriptorCount)
                {
                    candidates.Add(new Candidate(
                        checked(section.VirtualAddress + (uint)start),
                        descriptorAddresses.ToArray(),
                        targets.ToArray()));
                }
                else
                {
                    // A short false start may overlap a real family four/eight bytes
                    // later. Resume one word after its original beginning rather than
                    // skipping the whole partial run.
                    cursor = start + sizeof(uint);
                }
            }
        }

        var targetsToPromote = new HashSet<uint>();
        var admittedTables = 0;
        foreach (var candidate in candidates)
        {
            if (!candidate.DescriptorAddresses.Any(storedPointerValues.Contains))
            {
                continue;
            }

            var distinctTargets = candidate.Targets.Distinct().ToArray();
            var boundaryEvidence = distinctTargets.Count(target =>
                provenFunctionStarts.Contains(target) ||
                ImmediatelyFollowsTerminalTransfer(image, executableRanges, target));
            if (boundaryEvidence < MinimumBoundaryEvidenceCount ||
                boundaryEvidence * 5 < distinctTargets.Length * 4)
            {
                continue;
            }

            var tableAdded = false;
            foreach (var target in distinctTargets)
            {
                if (provenFunctionStarts.Contains(target) ||
                    !ImmediatelyFollowsTerminalTransfer(image, executableRanges, target))
                {
                    continue;
                }

                if (targetsToPromote.Add(target))
                {
                    tableAdded = true;
                }
            }

            if (tableAdded)
            {
                admittedTables++;
            }
        }

        return new StoredStridedCallbackDescriptorDiscoveryResult(
            targetsToPromote.OrderBy(static address => address).ToArray(),
            admittedTables,
            candidates.Count);
    }

    private static bool IsDescriptor(
        ReadOnlySpan<byte> span,
        int offset,
        Func<uint, bool> looksLikeExecutableEntry)
    {
        return BinaryPrimitives.ReadUInt32BigEndian(span.Slice(offset, 4)) == 0 &&
               BinaryPrimitives.ReadUInt32BigEndian(span.Slice(offset + 4, 4)) == 0 &&
               looksLikeExecutableEntry(BinaryPrimitives.ReadUInt32BigEndian(span.Slice(offset + 8, 4)));
    }

    private static bool ImmediatelyFollowsTerminalTransfer(
        ProgramImage image,
        IReadOnlyList<AddressRange> executableRanges,
        uint address)
    {
        if (address < sizeof(uint))
        {
            return false;
        }

        var previousAddress = address - sizeof(uint);
        if (!executableRanges.Any(range => range.Contains(previousAddress)) ||
            !image.Contains(previousAddress, sizeof(uint)))
        {
            return false;
        }

        var offset = image.GetOffset(previousAddress, sizeof(uint));
        var previousWord = BinaryPrimitives.ReadUInt32BigEndian(
            image.Memory.AsSpan(offset, sizeof(uint)));
        if (previousWord == BlrInstruction)
        {
            return true;
        }

        return PpcDecoder.Decode(previousAddress, previousWord).IsUnconditionalBranch;
    }
}
