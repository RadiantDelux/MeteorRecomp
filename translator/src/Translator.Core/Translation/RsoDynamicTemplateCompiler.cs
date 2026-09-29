using System.Globalization;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Loading;
using Translator.Core.Parsing.Rso;

namespace Translator.Core.Translation;

public sealed record RsoDynamicTemplateCompileOptions(
    string TemplateId,
    uint CanonicalImageBase,
    IReadOnlySet<int> ExecutableSectionIndices,
    uint? CanonicalBssBase = null,
    IReadOnlySet<uint>? AdditionalFunctionOffsets = null,
    int MaxFunctions = 4096,
    int MaxFunctionBytes = 0x10000,
    bool IncludeExportRoots = true,
    bool IncludeRelocationRoots = true,
    bool PreserveUnresolvedImports = false,
    uint? RuntimeImageSize = null);

public sealed record RsoDynamicTemplateFunction(
    uint Address,
    uint Offset,
    string Name,
    FunctionTranslationResult Translation);

public sealed record RsoDynamicTemplateCompilation(
    RsoLinkedImage LinkedImage,
    DynamicModuleTemplateContext TemplateContext,
    IReadOnlyList<int> ExecutableSectionIndices,
    IReadOnlyList<RsoDynamicTemplateFunction> Functions);

/// <summary>
/// Links and pretranslates one RSO as a canonical dynamic-module template.
/// Canonical addresses are analysis-only and are never registered in the fixed
/// guest dispatch namespace.
/// </summary>
public static class RsoDynamicTemplateCompiler
{
    public static RsoDynamicTemplateCompilation Compile(
        RsoFile file,
        IRsoSymbolProvider symbolProvider,
        RsoDynamicTemplateCompileOptions options,
        IGuestFunctionAbiProvider? guestAbiProvider = null)
    {
        ArgumentNullException.ThrowIfNull(file);
        ArgumentNullException.ThrowIfNull(symbolProvider);
        ArgumentNullException.ThrowIfNull(options);
        if (string.IsNullOrWhiteSpace(options.TemplateId))
            throw new ArgumentException("Dynamic RSO template id is required", nameof(options));
        if (options.ExecutableSectionIndices is not { Count: > 0 })
            throw new InvalidDataException("Dynamic RSO translation requires explicit executable section indices");
        if (options.MaxFunctions <= 0 || options.MaxFunctionBytes <= 0)
            throw new ArgumentOutOfRangeException(nameof(options), "Dynamic RSO translation budgets must be positive");

        var imageSize = checked((uint)file.RawData.Length);
        var runtimeImageSize = options.RuntimeImageSize ?? imageSize;
        if (runtimeImageSize == 0 || runtimeImageSize > imageSize)
            throw new InvalidDataException("Dynamic RSO runtime image size must be within the pre-link image");
        if (runtimeImageSize < 0x58u)
            throw new InvalidDataException("Dynamic RSO runtime image must retain the complete RSO header");
        if ((ulong)file.Header.SectionTableOffset + (ulong)file.Sections.Count * RsoFile.SectionEntrySize > runtimeImageSize)
            throw new InvalidDataException("Dynamic RSO runtime image must retain the complete section table");
        var canonicalImageEnd = checked(options.CanonicalImageBase + imageSize);
        if (options.CanonicalImageBase >= MemoryLayout.RamBase || canonicalImageEnd >= MemoryLayout.RamBase)
        {
            throw new InvalidDataException(
                $"Dynamic RSO canonical image 0x{options.CanonicalImageBase:X8}-0x{canonicalImageEnd:X8} " +
                $"must live entirely below guest RAM 0x{MemoryLayout.RamBase:X8}");
        }

        var bssBase = file.Header.BssSize == 0
            ? 0u
            : options.CanonicalBssBase ?? AlignUp(canonicalImageEnd, 0x20u);
        if (file.Header.BssSize != 0)
        {
            var bssEnd = checked(bssBase + file.Header.BssSize);
            if (bssBase < canonicalImageEnd || bssEnd >= MemoryLayout.RamBase)
                throw new InvalidDataException("Dynamic RSO canonical BSS must be non-overlapping and below guest RAM");
        }

        foreach (var sectionIndex in options.ExecutableSectionIndices)
        {
            if (sectionIndex <= 0 || sectionIndex >= file.Sections.Count)
                throw new InvalidDataException($"Dynamic RSO executable section {sectionIndex} is outside the section table");
            var section = file.Sections[sectionIndex];
            if (!section.HasFileData)
                throw new InvalidDataException($"Dynamic RSO executable section {sectionIndex} has no file-backed payload");
            if ((ulong)section.FileOffset + section.Size > runtimeImageSize)
                throw new InvalidDataException(
                    $"Dynamic RSO executable section {sectionIndex} is not fully retained by runtime image size 0x{runtimeImageSize:X8}");
        }

        var linked = RsoImageBuilder.Build(
            file,
            options.CanonicalImageBase,
            bssBase,
            symbolProvider,
            new RsoImageBuildOptions { PreserveUnresolvedImports = options.PreserveUnresolvedImports });
        ValidateBranchReachability(linked);
        if (!options.PreserveUnresolvedImports && linked.Imports.Any(static import => !import.IsResolved))
            throw new InvalidDataException("Dynamic RSO template contains unresolved imports");

        var context = DynamicModuleTemplateContext.FromRsoLinkedImage(options.TemplateId, linked, runtimeImageSize);
        var executableSections = options.ExecutableSectionIndices.OrderBy(static index => index).ToArray();
        var executableRanges = executableSections
            .Select(index => linked.Sections[index])
            .ToArray();

        bool IsExecutableAddress(uint address) => executableRanges.Any(section =>
            address >= section.Address && (ulong)address < (ulong)section.Address + section.Size);

        var rootNames = new Dictionary<uint, string>();
        void AddRoot(uint address, string reason)
        {
            if (!IsExecutableAddress(address))
                return;
            if (!rootNames.TryGetValue(address, out var existing))
                rootNames[address] = reason;
            else if (!existing.Contains(reason, StringComparison.Ordinal))
                rootNames[address] = existing + "; " + reason;
        }

        if (options.IncludeExportRoots)
        {
            foreach (var export in linked.Exports)
                AddRoot(export.Address, $"export {export.Name}");
        }
        if (linked.PrologAddress is { } prolog) AddRoot(prolog, "prolog");
        if (linked.EpilogAddress is { } epilog) AddRoot(epilog, "epilog");
        if (linked.UnresolvedAddress is { } unresolved) AddRoot(unresolved, "unresolved");
        if (options.IncludeRelocationRoots)
        {
            foreach (var relocation in linked.Relocations)
            {
                if (relocation.TargetKind == RsoRelocationTargetKind.InternalSection &&
                    relocation.Semantic is RsoRelocationSemantic.AbsoluteAddress or RsoRelocationSemantic.AddressFragment &&
                    IsExecutableAddress(relocation.ResolvedTarget))
                {
                    AddRoot(relocation.ResolvedTarget, $"relocation target @0x{relocation.PatchAddress:X8}");
                }
            }
        }
        if (options.AdditionalFunctionOffsets is not null)
        {
            foreach (var offset in options.AdditionalFunctionOffsets)
                AddRoot(checked(options.CanonicalImageBase + offset), "explicit root");
        }
        if (rootNames.Count == 0)
        {
            var requested = options.AdditionalFunctionOffsets is null
                ? "none"
                : string.Join(",", options.AdditionalFunctionOffsets.OrderBy(static value => value).Select(static value => $"0x{value:X8}"));
            var ranges = string.Join(",", executableRanges.Select(static range =>
                $"0x{range.Address:X8}+0x{range.Size:X}"));
            throw new InvalidDataException(
                $"Dynamic RSO template has no proven executable function roots (explicit offsets: {requested}; executable ranges: {ranges})");
        }

        var programImage = new ProgramImage(
            linked.Image,
            AddressRange.FromStartAndSize(options.CanonicalImageBase, imageSize),
            AddressRange.FromStartAndSize(options.CanonicalImageBase, imageSize),
            new AddressRange(0, 0),
            Convert.ToHexString(linked.PreLinkSha256),
            options.CanonicalImageBase);
        var translator = new FunctionTranslator(programImage, guestAbiProvider);

        // Discovery first, emission second. This gives every emitted body the
        // complete set of same-template function boundaries without ever placing
        // canonical addresses into the process-wide fixed function map.
        var known = new HashSet<uint>(rootNames.Keys);
        var queue = new Queue<uint>(known.OrderBy(static address => address));
        while (queue.Count != 0)
        {
            if (known.Count > options.MaxFunctions)
                throw new InvalidDataException($"Dynamic RSO template exceeds {options.MaxFunctions} discovered functions");
            var address = queue.Dequeue();
            var discovery = translator.Discover(
                address,
                TranslationOptions.Default with
                {
                    PreferredName = rootNames.TryGetValue(address, out var rootName) ? rootName : $"dynamic_{address:X8}",
                    MaxBytes = options.MaxFunctionBytes,
                    AllowUnsupportedInstructions = false,
                    KnownFunctionEntryPoints = known,
                    EnableLeafInlining = false,
                    DynamicModuleTemplate = context,
                });

            foreach (var target in DirectGuestCalls(discovery.LinearIr))
            {
                if (!IsExecutableAddress(target) || !known.Add(target))
                    continue;
                rootNames[target] = $"direct call from 0x{address:X8}";
                queue.Enqueue(target);
            }
        }

        var functions = new List<RsoDynamicTemplateFunction>(known.Count);
        foreach (var address in known.OrderBy(static address => address))
        {
            var offset = checked(address - options.CanonicalImageBase);
            var name = rootNames.TryGetValue(address, out var rootName)
                ? rootName
                : $"dynamic_{offset:X8}";
            var translation = translator.Translate(
                address,
                TranslationOptions.Default with
                {
                    PreferredName = name,
                    MaxBytes = options.MaxFunctionBytes,
                    AllowUnsupportedInstructions = false,
                    KnownFunctionEntryPoints = known,
                    EnableLeafInlining = false,
                    DynamicModuleTemplate = context,
                });
            functions.Add(new RsoDynamicTemplateFunction(address, offset, name, translation));
        }

        return new RsoDynamicTemplateCompilation(linked, context, executableSections, functions);
    }

    private static IEnumerable<uint> DirectGuestCalls(IrFunction function)
    {
        foreach (var call in function.Blocks.SelectMany(static block => block.Instructions).OfType<IrCall>())
        {
            if (TryParseGuestAddress(call.Target, out var address))
                yield return address;
        }
    }

    private static bool TryParseGuestAddress(string target, out uint address)
    {
        address = 0;
        var text = target.StartsWith("func_", StringComparison.OrdinalIgnoreCase)
            ? target.AsSpan(5)
            : target.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
                ? target.AsSpan(2)
                : default;
        return !text.IsEmpty && uint.TryParse(text, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out address);
    }

    private static void ValidateBranchReachability(RsoLinkedImage linked)
    {
        foreach (var relocation in linked.Relocations.Where(static relocation => relocation.Type == RsoRelocationType.R_PPC_REL24))
        {
            var displacement = (long)relocation.ResolvedTarget - relocation.PatchAddress;
            if (displacement < -0x02000000L || displacement > 0x01FFFFFCL)
            {
                throw new InvalidDataException(
                    $"Dynamic RSO canonical base makes REL24 @0x{relocation.PatchAddress:X8} to " +
                    $"0x{relocation.ResolvedTarget:X8} unreachable");
            }
        }
    }

    private static uint AlignUp(uint value, uint alignment)
    {
        var mask = checked(alignment - 1u);
        return checked((value + mask) & ~mask);
    }
}
